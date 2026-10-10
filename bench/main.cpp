// iotdash: display bring-up and realtime benchmark for the Waveshare ESP32-S3-Touch-LCD-7.
//
// All drawing goes through Ui (ui.h): areas are drawn in SRAM bands and copied to the
// PSRAM frame buffer by DMA, then shown tear-free at the next frame boundary.
//
// 1. Benchmark: redraws a centred region every frame at several sizes; reports fps and
//    drawing time per frame.
// 2. Touch mode: a box follows your finger, redrawing only what moved.
// Serial (115200 on the UART port):
//   'b' benchmark again   't' touch mode   'd' 5 s non-stop DMA drawing (stability check)
//   '[' / ']' DMA write-rate cap -/+ 2 MB/s   '-' / '+' pixel clock -/+ 2 MHz

#include <Arduino.h>
#include <Wire.h>
#include <esp_psram.h>

#include "board.h"
#include "ch422g.h"
#include "dma_blit.h"
#include "gfx.h"
#include "gt911.h"
#include "rgb_display.h"
#include "ui.h"

using namespace gfx;

static RgbDisplay lcd;
static DmaBlitter dma;
static Ui ui;
static CH422G io;
static GT911 touch;
static bool touchOk = false;
static uint32_t dmaRate = 26 * 1000 * 1000;

static const int W = board::LCD_W, H = board::LCD_H;
static const uint32_t PHASE_MS = 4000;
static const uint16_t BG = rgb(16, 20, 32);

// Region sizes (widths multiples of 32 for DMA). 448x480 is about the largest physically
// square area: the panel is ~154 x 86 mm (6.1 x 3.4 in), so 4 x 4 in does not fit.
struct Size {
  int16_t w, h;
};
static const Size SIZES[] = {{192, 192}, {288, 288}, {384, 384}, {448, 480}, {800, 480}};
static const int NSIZES = sizeof(SIZES) / sizeof(SIZES[0]);

struct Result {
  Size size;
  float fps, drawMs;
};
static Result results[NSIZES];

// Centred, with x on a 32-pixel boundary.
static Rect centred(Size s) {
  return {(int16_t)(((W - s.w) / 2) & ~(DmaBlitter::ALIGN_PX - 1)), (int16_t)((H - s.h) / 2), s.w, s.h};
}

static bool intersects(const Rect &a, const Rect &b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// Background colour of a row: a colour sweep that moves with the frame number.
static inline uint16_t rowColour(int y, uint32_t f) {
  uint8_t a = (uint8_t)(y + f * 3);
  return rgb(a, (uint8_t)(255 - a), (uint8_t)(a * 2));
}

// Benchmark scene inside r: gradient, bouncing box, sweeping bar (a tear would show as a
// broken bar) and the frame rate. Paints only what falls on the canvas.
static void drawScene(const Canvas &c, Rect r, uint32_t f, float fps) {
  int y0 = max((int)r.y, c.oy), y1 = min(r.y + r.h, c.oy + c.h);
  for (int y = y0; y < y1; y++) c.row(r.x, y, r.w, rowColour(y - r.y, f));

  int box = r.w / 5;
  int spanX = r.w - box, spanY = r.h - box;
  int bx = (int)(f * 7) % (2 * spanX), by = (int)(f * 5) % (2 * spanY);
  if (bx > spanX) bx = 2 * spanX - bx;
  if (by > spanY) by = 2 * spanY - by;
  c.fill({(int16_t)(r.x + bx), (int16_t)(r.y + by), (int16_t)box, (int16_t)box}, WHITE);
  c.fill({(int16_t)(r.x + (int)(f * 9) % r.w), r.y, 6, r.h}, BLACK);

  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", fps);
  c.fill({(int16_t)(r.x + 4), (int16_t)(r.y + 4), 120, 36}, BLACK);
  c.text(r.x + 10, r.y + 8, 3, buf, rgb(255, 210, 0));
}

// Draw a full-screen static page into both buffers.
static void showPage(const Ui::DrawFn &draw) {
  ui.invalidateAll();
  ui.frame(draw);
  ui.frame(draw);  // the other buffer
}

static void runBenchmark() {
  Serial.printf("\nPanel refresh %.1f Hz (pclk %u MHz), DMA write cap %u MB/s\n", lcd.refreshHz(),
                (unsigned)(lcd.pclk() / 1000000), (unsigned)(dmaRate / 1000000));
  Serial.println("size        px     fps  draw ms");

  for (int i = 0; i < NSIZES; i++) {
    const Rect r = centred(SIZES[i]);
    results[i].size = SIZES[i];
    uint32_t frames = 0, drawUs = 0, start = millis();
    float fps = 0;
    ui.invalidateAll();  // first frames: clear the screen around the region
    while (millis() - start < PHASE_MS) {
      ui.invalidate(r);
      ui.frame([&](const Canvas &c, const Rect &area) {
        c.fill(area, BG);
        if (intersects(area, r)) drawScene(c, r, frames, fps);
      });
      drawUs += ui.lastDrawUs();
      frames++;
      fps = frames * 1000.0f / (millis() - start + 1);
    }
    results[i].fps = frames * 1000.0f / (millis() - start);
    results[i].drawMs = drawUs / 1000.0f / frames;
    Serial.printf("%3dx%-3d %7d  %5.1f  %7.2f\n", r.w, r.h, r.w * r.h, results[i].fps, results[i].drawMs);
  }

  // Summary page: bar = fps (scale 40), blue = drawing ms per frame.
  showPage([](const Canvas &c, const Rect &area) {
    c.fill(area, BG);
    for (int i = 0; i < NSIZES; i++) {
      int y = 30 + i * 88;
      char buf[24];
      snprintf(buf, sizeof(buf), "%dx%d", results[i].size.w, results[i].size.h);
      c.text(20, y + 14, 3, buf, WHITE);
      int bw = (int)(results[i].fps / 40.0f * 360);
      c.fill({224, (int16_t)y, (int16_t)bw, 44}, rgb(60, 200, 110));
      snprintf(buf, sizeof(buf), "%.1f", results[i].fps);
      int x = c.text(236 + bw, y + 10, 2, buf, WHITE);
      snprintf(buf, sizeof(buf), "%.1f", results[i].drawMs);
      c.text(x + 24, y + 10, 2, buf, rgb(120, 200, 255));
    }
  });
  Serial.println("Summary on screen: bar = fps, blue = draw ms. 't' touch, 'd' stress, '['/']' rate cap.");
}

// Touch mode: only the box's old and new positions and the labels are redrawn.
static void touchMode() {
  const Rect area = centred({384, 384});
  const int16_t BOX = 80;
  Rect box = {(int16_t)(area.x + (area.w - BOX) / 2), (int16_t)(area.y + (area.h - BOX) / 2), BOX, BOX};
  const Rect label = {(int16_t)(area.x + area.w + 24), area.y, 192, 100};
  bool pressed = false;
  float fps = 0, latencyMs = 0;

  auto draw = [&](const Canvas &c, const Rect &a) {
    c.fill(a, BG);
    if (intersects(a, area)) {
      for (int y = max((int)area.y, c.oy); y < min(area.y + area.h, c.oy + c.h); y++)
        c.row(area.x, y, area.w, rowColour(y - area.y, 0));
      c.fill(box, pressed ? rgb(255, 210, 0) : WHITE);
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", fps);
    c.text(label.x, label.y, 3, buf, WHITE);
    snprintf(buf, sizeof(buf), "%.1f", latencyMs);
    c.text(label.x, label.y + 50, 3, buf, rgb(120, 200, 255));
  };
  showPage(draw);
  Serial.println("Touch mode: drag inside the square (top number = updates/s, bottom = touch-to-screen ms).");

  uint32_t updates = 0, windowStart = millis();
  while (!Serial.available()) {
    GT911::Point p[1];
    int n = touchOk ? touch.read(p, 1) : 0;
    uint32_t touchUs = micros();
    bool nowPressed = n > 0;
    Rect old = box;
    if (nowPressed) {
      box.x = constrain((int)p[0].x - BOX / 2, (int)area.x, area.x + area.w - BOX);
      box.y = constrain((int)p[0].y - BOX / 2, (int)area.y, area.y + area.h - BOX);
    }
    if (box.x != old.x || box.y != old.y || nowPressed != pressed) {
      pressed = nowPressed;
      ui.invalidate(old);
      ui.invalidate(box);
    }
    if (millis() - windowStart >= 1000) {
      fps = updates * 1000.0f / (millis() - windowStart);
      updates = 0;
      windowStart = millis();
      ui.invalidate(label);
    }
    if (ui.frame(draw)) {
      updates++;
      if (nowPressed) {
        latencyMs = (micros() - touchUs) / 1000.0f;
        ui.invalidate(label);
      }
    }
  }
}

// Stress: DMA-draw into the visible buffer non-stop (no vsync wait), at the rate cap.
static void stressTest() {
  const Rect r = centred({448, 480});
  showPage([](const Canvas &c, const Rect &a) { c.fill(a, BG); });
  uint32_t frames = 0, start = millis();
  float fps = 0;
  Serial.printf("Stress: 5 s of non-stop DMA drawing at %u MB/s...\n", (unsigned)(dmaRate / 1000000));
  while (millis() - start < 5000) {
    dma.render(lcd.front(), r, [&](uint16_t *band, int y, int rows) {
      drawScene(Canvas{band, r.w, rows, r.x, y}, r, frames, fps);
    });
    frames++;
    fps = frames * 1000.0f / (millis() - start + 1);
    if ((frames & 15) == 0) delay(1);  // let the idle task feed the watchdog
  }
  Serial.printf("Stress done: %.1f frames/s. Did the image move? 'b' or 't' to continue.\n", fps);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== iotdash display test ===\n%s rev %d, %d MHz, flash %u MB, PSRAM %u MB\n", ESP.getChipModel(),
                ESP.getChipRevision(), getCpuFrequencyMhz(), ESP.getFlashChipSize() / (1024 * 1024),
                (unsigned)(esp_psram_get_size() / (1024 * 1024)));

  Wire.begin(board::I2C_SDA, board::I2C_SCL, board::I2C_HZ);
  // LCD out of reset, SD deselected, touch held in reset, backlight off, GPIO19/20 -> USB port.
  if (!io.begin(Wire, board::EXIO_LCD_RST | board::EXIO_SD_CS)) Serial.println("CH422G not responding on I2C");
  touchOk = touch.begin(Wire, io, board::EXIO_TP_RST, board::TOUCH_INT);
  Serial.printf("Touch: %s %s\n", touchOk ? "GT" : "not found", touchOk ? touch.productId() : "");

  if (!lcd.begin(board::LCD_PCLK_HZ)) {
    Serial.println("LCD init failed (is PSRAM enabled?)");
    return;
  }
  if (!dma.begin(W)) {
    Serial.println("DMA init failed");
    return;
  }
  dma.setMaxRate(dmaRate);
  ui.begin(lcd, dma);
  io.set(board::EXIO_BL, true);
  Serial.printf("LCD ready. Free PSRAM %u KB, free SRAM %u KB\n", ESP.getFreePsram() / 1024,
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
  delay(200);  // let a few vsyncs pass so the refresh rate can be measured

  runBenchmark();
}

void loop() {
  if (!Serial.available()) {
    delay(20);
    return;
  }
  int c = Serial.read();
  if (c == 'b') runBenchmark();
  if (c == 't') touchMode();
  if (c == 'd') stressTest();
  if (c == '[' || c == ']') {
    int mb = (int)(dmaRate / 1000000) + (c == ']' ? 2 : -2);
    dmaRate = constrain(mb, 6, 60) * 1000000u;
    dma.setMaxRate(dmaRate);
    Serial.printf("DMA write cap %u MB/s (full-screen redraw takes %.1f ms)\n", (unsigned)(dmaRate / 1000000),
                  W * H * 2 * 1000.0f / dmaRate);
  }
  if (c == '+' || c == '-') {
    uint32_t hz = constrain(lcd.pclk() + (c == '+' ? 2000000 : -2000000), 8000000u, 30000000u);
    bool ok = lcd.setPclk(hz);
    delay(500);
    Serial.printf("pclk %u MHz %s, refresh %.1f Hz\n", (unsigned)(hz / 1000000), ok ? "set" : "FAILED",
                  lcd.refreshHz());
  }
}
