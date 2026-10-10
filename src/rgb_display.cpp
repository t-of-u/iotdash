#include "rgb_display.h"

#include <esp32s3/rom/cache.h>
#include <esp_cache.h>
#include <esp_cpu.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <hal/gdma_ll.h>
#include <soc/gdma_struct.h>

#include "rt_base.h"

extern volatile bool gBlitActive;
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "board.h"

static const char *TAG = "rgb_display";
static const int kLcdIsrCore = 1;  // the panel's (and fill DMA's) interrupts run on this core
static const int kBounceRows = 12;  // 2 x 19 KB in SRAM; GDMA refills one in ~0.08 ms of its 0.6 ms

// ease-out cubic in 1/1024 units for frame f of n
static IRAM_ATTR int easeOut1024(uint32_t f, uint32_t n) {
  if (f >= n) return 1024;
  int t = (int)(f * 1024 / n);
  int u = 1024 - t;
  return 1024 - (int)(((int64_t)u * u * u) >> 20);
}

void IRAM_ATTR RgbDisplay::fill(uint16_t *out, int y0, int rows) {
  const Cfg &c = active_;
  const int W = w_;
  if (!c.kind) {
    // the page is written by DMA behind the cache: drop any stale lines first
    Cache_Invalidate_Addr((uint32_t)(c.page + y0 * W), rows * W * 2);
    memcpy(out, c.page + y0 * W, rows * W * 2);
    return;
  }
  for (int r = 0; r < rows; r++) {
    int y = y0 + r;
    uint16_t *o = out + r * W;
    if (y < c.ay || y >= c.ay + c.ah) {  // outside the sliding area: the new page
      memcpy(o, c.to + y * W, W * 2);
      continue;
    }
    if (c.kind == SLIDE_UP || c.kind == SLIDE_DOWN) {
      // vertical push: rows inside the area come from old/new pages at an offset
      int off = c.offset;
      int sy, from;
      if (c.kind == SLIDE_UP) {
        int rel = y - c.ay + off;  // position within the stacked [old; new]
        from = rel < c.ah;
        sy = c.ay + (from ? rel : rel - c.ah);
      } else {
        int rel = y - c.ay - off;
        from = rel >= 0;
        sy = c.ay + (from ? rel : rel + c.ah);
      }
      const uint16_t *src = from ? c.from : c.to;
      // columns outside the area still show the new page
      if (c.ax > 0) memcpy(o, c.to + y * W, c.ax * 2);
      memcpy(o + c.ax, src + sy * W + c.ax, c.aw * 2);
      int right = c.ax + c.aw;
      if (right < W) memcpy(o + right, c.to + y * W + right, (W - right) * 2);
      continue;
    }
    // horizontal push
    const uint16_t *oldRow = c.from + y * W, *newRow = c.to + y * W;
    if (c.ax > 0) memcpy(o, newRow, c.ax * 2);
    int off = c.offset;  // 0..aw
    int aw = c.aw, ax = c.ax;
    if (c.kind == SLIDE_LEFT) {
      // old page moves left by off, new page enters from the right
      int oldW = aw - off;
      if (oldW > 0) memcpy(o + ax, oldRow + ax + off, oldW * 2);
      if (off > 0) memcpy(o + ax + oldW, newRow + ax, off * 2);
    } else {
      // old page moves right, new page enters from the left
      if (off > 0) memcpy(o + ax, newRow + ax + aw - off, off * 2);
      int oldW = aw - off;
      if (oldW > 0) memcpy(o + ax + off, oldRow + ax, oldW * 2);
    }
    int right = ax + aw;
    if (right < W) memcpy(o + right, newRow + right, (W - right) * 2);
  }
}

bool IRAM_ATTR RgbDisplay::onFillEof(gdma_channel_handle_t, gdma_event_data_t *, void *ctx) {
  static_cast<RgbDisplay *>(ctx)->fillBusy_ = false;
  return false;
}

bool RgbDisplay::initFillDma() {
  gdma_channel_alloc_config_t txCfg = {};
  txCfg.direction = GDMA_CHANNEL_DIRECTION_TX;
  txCfg.flags.reserve_sibling = 1;
  if (gdma_new_ahb_channel(&txCfg, &ftx_) != ESP_OK) return false;
  gdma_channel_alloc_config_t rxCfg = {};
  rxCfg.direction = GDMA_CHANNEL_DIRECTION_RX;
  rxCfg.sibling_chan = ftx_;
  if (gdma_new_ahb_channel(&rxCfg, &frx_) != ESP_OK) return false;
  gdma_trigger_t trig = GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_M2M, 0);
  uint32_t freeMask = 0;
  gdma_get_free_m2m_trig_id_mask(ftx_, &freeMask);
  if (!freeMask) return false;
  trig.instance_id = __builtin_ctz(freeMask);
  if (gdma_connect(frx_, trig) != ESP_OK || gdma_connect(ftx_, trig) != ESP_OK) return false;
  // the scan-out fill must win over the band blitter (both read/write PSRAM); a fill that
  // is still running at the next bounce interrupt falls back to a slow CPU copy
  // below the panel's own DMA (see begin()), above the band blitter
  gdma_set_priority(ftx_, GDMA_LL_CHANNEL_MAX_PRIORITY - 1);
  gdma_set_priority(frx_, GDMA_LL_CHANNEL_MAX_PRIORITY - 1);
  gdma_strategy_config_t strategy = {};
  strategy.owner_check = true;
  strategy.auto_update_desc = true;
  strategy.eof_till_data_popped = true;
  gdma_apply_strategy(ftx_, &strategy);
  gdma_apply_strategy(frx_, &strategy);
  gdma_transfer_config_t transfer = {};
  transfer.max_data_burst_size = 64;
  transfer.access_ext_mem = true;
  if (gdma_config_transfer(ftx_, &transfer) != ESP_OK || gdma_config_transfer(frx_, &transfer) != ESP_OK) return false;
  gdma_rx_event_callbacks_t cbs = {};
  cbs.on_recv_eof = onFillEof;
  if (gdma_register_rx_event_callbacks(frx_, &cbs, this) != ESP_OK) return false;
  size_t bytes = (size_t)w_ * bbRows_ * 2;
  for (int i = 0; i < 2; i++) {
    gdma_link_list_config_t lc = {};
    lc.num_items = bytes / 4032 + 2;
    lc.item_alignment = 4;
    if (gdma_new_link_list(&lc, &ftxList_[i]) != ESP_OK) return false;
    if (gdma_new_link_list(&lc, &frxList_[i]) != ESP_OK) return false;
  }
  dmaReady_ = true;
  return true;
}

// Start a PSRAM -> bounce buffer copy and return; it completes well within the time the
// other bounce buffer is on screen.
// The refill started last is still being written (its last descriptor still belongs to the
// DMA). Read from the descriptor itself: the EOF interrupt can be late.
bool IRAM_ATTR RgbDisplay::fillRunning() {
  int s = lastFillSlot_;
  if (s < 0) return false;
  gdma_lli_owner_t owner = GDMA_LLI_OWNER_CPU;
  gdma_link_get_owner(frxList_[s], fillEnd_[s], &owner);
  return owner == GDMA_LLI_OWNER_DMA;
}

void RgbDisplay::waitBusWindow(uint32_t needUs) {
  if (!dmaReady_ || !useDma) return;
  const int64_t t0 = esp_timer_get_time();
  const uint32_t period = budgetUs();
  constexpr uint32_t kMarginUs = 60;
  while (true) {
    int64_t now = esp_timer_get_time();
    if (active_.kind || now - t0 > 2 * period) return;  // a slide (no refills), or never wait long
    if (!fillRunning()) {
      int64_t since = now - lastBounceUs_;
      if (since + needUs + kMarginUs < period) return;  // ends before the next refill starts
      // the gap over the vertical blank is longer than a period: the next refill is later
      if (since > period + kMarginUs && since + needUs + kMarginUs < 2 * period) return;
    }
    esp_rom_delay_us(8);
  }
}

bool IRAM_ATTR RgbDisplay::dmaFill(void *bounce, int pos_px, int len_bytes) {
  if (!dmaReady_ || !useDma) return false;
  if (fillRunning()) {
    // the previous refill still holds the channel: give it a moment rather than adding a CPU
    // copy to the bus it is already short of
    uint32_t t0 = esp_cpu_get_cycle_count();
    while (fillRunning() && esp_cpu_get_cycle_count() - t0 < 240 * 250) {
    }
    if (fillRunning()) {
      dmaFallbacks_ = dmaFallbacks_ + 1;
      return false;
    }
  }
  int slot;
  if (bounce == bounceSeen_[0]) slot = 0;
  else if (bounce == bounceSeen_[1]) slot = 1;
  else if (!bounceSeen_[0]) {
    bounceSeen_[0] = bounce;
    slot = 0;
  } else if (!bounceSeen_[1]) {
    bounceSeen_[1] = bounce;
    slot = 1;
  } else {
    return false;
  }
  gdma_buffer_mount_config_t tx = {};
  tx.buffer = (void *)(active_.page + pos_px);
  tx.buffer_alignment = 64;
  tx.length = len_bytes;
  tx.flags.mark_eof = 1;
  tx.flags.mark_final = GDMA_FINAL_LINK_TO_NULL;
  if (gdma_link_mount_buffers(ftxList_[slot], 0, &tx, 1, nullptr) != ESP_OK) return false;
  gdma_buffer_mount_config_t rx = {};
  rx.buffer = bounce;
  rx.buffer_alignment = 4;
  rx.length = len_bytes;
  rx.flags.mark_final = GDMA_FINAL_LINK_TO_NULL;
  if (gdma_link_mount_buffers(frxList_[slot], 0, &rx, 1, &fillEnd_[slot]) != ESP_OK) return false;
  fillBusy_ = true;
  lastFillSlot_ = slot;
  fillStartUs_ = esp_timer_get_time();
  gdma_start(frx_, gdma_link_get_head_addr(frxList_[slot]));
  gdma_start(ftx_, gdma_link_get_head_addr(ftxList_[slot]));
  return true;
}

bool IRAM_ATTR RgbDisplay::onBounceEmpty(esp_lcd_panel_handle_t, void *bounce, int pos_px, int len_bytes, void *ctx) {
  auto *self = static_cast<RgbDisplay *>(ctx);
  uint32_t t0 = esp_cpu_get_cycle_count();
  int64_t nowUs = esp_timer_get_time();
  // how late this interrupt is: bounce buffers drain every period (longer over the vertical blank)
  if (self->lastBounceUs_ && pos_px != 0) {
    int64_t gap = nowUs - self->lastBounceUs_;
    int64_t late = gap - (int64_t)(self->budgetCycles_ / 240);
    if (late > (int64_t)self->isrLateMaxUs_) self->isrLateMaxUs_ = (uint32_t)late;
    if (late > 200) self->isrLate200_ = self->isrLate200_ + 1;
    if (gap < 300) self->isrBackToBack_ = self->isrBackToBack_ + 1;
  }
  self->isrCore_ = xPortGetCoreID();
  self->lastBounceUs_ = nowUs;
  // the other bounce buffer goes on screen now: was its refill done?
  bool busy = self->fillRunning();
  {
    Ev &e = self->ev_[self->evHead_ & (kEv - 1)];
    e.us = (uint32_t)nowUs;
    e.pos = pos_px;
    e.flags = (busy ? 1 : 0) | (busy && !self->active_.kind ? 4 : 0);
    e.phase = rt::gPhase;
    self->evHead_ = self->evHead_ + 1;
    if (busy && !self->active_.kind && self->evArmed_) {
      for (int i = 0; i < kEv; i++) self->frozen_[i] = self->ev_[i];
      self->frozenHead_ = self->evHead_;
      self->evArmed_ = false;
    }
  }
  if (!self->active_.kind && busy) {
    self->tears_ = self->tears_ + 1;
    uint8_t ph = rt::gPhase & 7;
    self->tearByPhase_[ph] = self->tearByPhase_[ph] + 1;
    if (gBlitActive) self->tearsWithBlit_ = self->tearsWithBlit_ + 1;
    uint32_t age = (uint32_t)(self->lastBounceUs_ - self->fillStartUs_);
    if (age > self->maxTearAgeUs_) self->maxTearAgeUs_ = age;
  }
  bool yield = false;
  if (pos_px == 0) {
    // frame start: latch a pending page switch / advance a transition
    BaseType_t woken = pdFALSE;
    if (self->hasPending_) {
      self->active_ = self->pending_;
      self->hasPending_ = false;
      if (self->active_.kind) {  // a transition starts with this frame
        self->slideStart_ = self->frames_;
        self->sliding_ = true;
      }
      xSemaphoreGiveFromISR(self->latchSem_, &woken);
    }
    if (self->sliding_) {
      uint32_t f = self->frames_ - self->slideStart_;
      Cfg &c = self->active_;
      int span = (self->slideKind_ == SLIDE_UP || self->slideKind_ == SLIDE_DOWN) ? c.ah : c.aw;
      c.offset = (int16_t)((span * easeOut1024(f, self->slideFrames_)) >> 10);
      if (f >= self->slideFrames_) {
        c.kind = 0;
        c.page = c.to;
        self->sliding_ = false;
        xSemaphoreGiveFromISR(self->doneSem_, &woken);
      }
    }
    self->frames_ = self->frames_ + 1;
    yield = woken == pdTRUE;
  }
  int y0 = pos_px / self->w_;
  int rows = len_bytes / (self->w_ * 2);
  bool viaDma = !self->active_.kind && self->dmaFill(bounce, pos_px, len_bytes);
  if (!viaDma) self->fill((uint16_t *)bounce, y0, rows);
  // prefetch the next chunk of the shown page into the cache
  int next = pos_px + rows * self->w_;
  if (!viaDma && self->preload && !self->active_.kind && next < self->w_ * self->h_)
    Cache_Start_DCache_Preload((uint32_t)(self->active_.page + next), len_bytes, 0);
  uint32_t dt = esp_cpu_get_cycle_count() - t0;
  if (dt > self->maxFillCycles_) self->maxFillCycles_ = dt;
  if (viaDma && dt > self->maxDmaIsrCycles_) self->maxDmaIsrCycles_ = dt;
  self->avgFillCycles_ = self->avgFillCycles_ + ((int32_t)dt - (int32_t)self->avgFillCycles_) / 16;
  if (self->budgetCycles_ && dt > self->budgetCycles_) self->late_ = self->late_ + 1;
  return yield;
}

bool IRAM_ATTR RgbDisplay::onVsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *ctx) {
  auto *self = static_cast<RgbDisplay *>(ctx);
  uint32_t now = (uint32_t)esp_timer_get_time();
  {
    Ev &e = self->ev_[self->evHead_ & (kEv - 1)];
    e.us = now;
    e.pos = -1;
    e.flags = 8;
    e.phase = rt::gPhase;
    self->evHead_ = self->evHead_ + 1;
  }
  if (self->vsyncs_ == 0) self->firstVsyncUs_ = now;
  self->lastVsyncUs_ = now;
  self->vsyncs_ = self->vsyncs_ + 1;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(self->vsyncSem_, &woken);
  return woken == pdTRUE;
}

bool RgbDisplay::create() {
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = pclkHz_;
  cfg.timings.h_res = board::LCD_W;
  cfg.timings.v_res = board::LCD_H;
  cfg.timings.hsync_pulse_width = board::LCD_HPW;
  cfg.timings.hsync_back_porch = board::LCD_HBP;
  cfg.timings.hsync_front_porch = board::LCD_HFP;
  cfg.timings.vsync_pulse_width = board::LCD_VPW;
  cfg.timings.vsync_back_porch = board::LCD_VBP;
  cfg.timings.vsync_front_porch = board::LCD_VFP;
  cfg.timings.flags.pclk_active_neg = board::LCD_PCLK_ACTIVE_NEG;
  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 0;
  cfg.flags.no_fb = 1;  // we fill the bounce buffers ourselves (compositor)
  cfg.bounce_buffer_size_px = board::LCD_W * kBounceRows;
  cfg.dma_burst_size = 64;
  cfg.hsync_gpio_num = board::LCD_HSYNC;
  cfg.vsync_gpio_num = board::LCD_VSYNC;
  cfg.de_gpio_num = board::LCD_DE;
  cfg.pclk_gpio_num = board::LCD_PCLK;
  cfg.disp_gpio_num = -1;
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = board::LCD_DATA[i];

  esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &panel_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_lcd_new_rgb_panel: %s", esp_err_to_name(err));
    return false;
  }
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = onVsync;
  cbs.on_bounce_empty = onBounceEmpty;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel_, &cbs, this));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
  return true;
}

void RgbDisplay::createTask(void *arg) {
  auto *self = static_cast<RgbDisplay *>(arg);
  self->createOk_ = self->create();
  xSemaphoreGive(self->latchSem_);
  vTaskDelete(nullptr);
}

bool RgbDisplay::begin(uint32_t pclkHz) {
  w_ = board::LCD_W;
  h_ = board::LCD_H;
  pclkHz_ = pclkHz;
  bbRows_ = kBounceRows;
  vsyncSem_ = xSemaphoreCreateBinary();
  latchSem_ = xSemaphoreCreateBinary();
  doneSem_ = xSemaphoreCreateBinary();
  size_t bytes = (size_t)w_ * h_ * 2;
  // Two page buffers. A third makes a frame that takes a little longer than a refresh cost
  // one refresh instead of two, but the OS needs its 750 KB: font atlases are kept in PSRAM
  // (reading them from flash glitches the scan-out). RGB_PAGES=3 in the build flags brings
  // it back when PSRAM allows.
#ifndef RGB_PAGES
#define RGB_PAGES 2
#endif
  nbuf_ = 2;
  for (int i = 0; i < RGB_PAGES; i++) {
    if (i == 2 && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < bytes + 1536 * 1024) break;
    fb_[i] = (uint16_t *)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM);
    if (!fb_[i]) {
      if (i < 2) return false;
      break;
    }
    memset(fb_[i], 0, bytes);
    Cache_WriteBack_Addr((uint32_t)fb_[i], bytes);
    nbuf_ = i + 1;
  }
  last_ = shown_ = 0;
  pendingIdx_ = -1;
  active_ = Cfg{fb_[0], nullptr, nullptr, 0, 0, 0, 0, 0, 0};
  // time one bounce buffer stays on screen: rows * (h_res + porches) / pclk
  uint32_t lineCycles = (uint32_t)((uint64_t)(w_ + board::LCD_HPW + board::LCD_HBP + board::LCD_HFP) * 240000000ULL / pclkHz_);
  budgetCycles_ = lineCycles * bbRows_;
  // the panel's interrupt is installed on the creating core
  xTaskCreatePinnedToCore(createTask, "lcd_init", 4096, this, 5, nullptr, kLcdIsrCore);
  xSemaphoreTake(latchSem_, portMAX_DELAY);
  if (createOk_ && !initFillDma()) ESP_LOGW(TAG, "bounce fill DMA unavailable: CPU copies");
  // The panel's DMA (bounce buffers -> LCD FIFO) must never wait for another channel: when it
  // does, the end of a bounce buffer slips past VSYNC, the driver counts one short, restarts
  // the transfer and refills a buffer out of turn (a frame shifted by a bounce buffer). The
  // driver keeps its channel to itself: find it by its peripheral (LCD_CAM = 5).
  for (int i = 0; i < 5; i++)
    if (GDMA.channel[i].out.peri_sel.sel == 5) GDMA.channel[i].out.pri.tx_pri = GDMA_LL_CHANNEL_MAX_PRIORITY;
  return createOk_;
}

// Learn whether the queued page has been latched by the scan-out (it then is the one shown).
void RgbDisplay::syncLatch(bool block) {
  if (pendingIdx_ < 0) return;
  if (xSemaphoreTake(latchSem_, block ? pdMS_TO_TICKS(100) : 0) == pdTRUE || block) {
    shown_ = pendingIdx_;
    pendingIdx_ = -1;
  }
}

void RgbDisplay::present() {
  syncLatch(pendingIdx_ >= 0);  // one page can wait for a frame start at a time
  int b = (last_ + 1) % nbuf_;
  pending_ = Cfg{fb_[b], nullptr, nullptr, 0, 0, 0, 0, 0, 0};
  xSemaphoreTake(latchSem_, 0);
  pendingIdx_ = b;
  last_ = b;
  hasPending_ = true;
}

void RgbDisplay::waitPresented() {
  syncLatch(false);
  int b = (last_ + 1) % nbuf_;
  while (b == shown_ || b == pendingIdx_) syncLatch(true);  // two buffers: wait for the swap
}

void RgbDisplay::slide(Slide kind, int ms, const Rect &area) {
  syncLatch(true);  // start from the newest frame on screen
  waitPresented();
  // the transition reads both pages with the CPU (through the cache); they were written by DMA
  invalidatePages();
  float hz = refreshHz();
  if (hz < 10) hz = (float)pclkHz_ / ((w_ + 20) * (h_ + 20));
  uint32_t frames = (uint32_t)std::max(2.f, ms * hz / 1000.f);
  Cfg c{back(), front(), back(), area.x, area.y, area.w, area.h, 0, (uint8_t)kind};
  xSemaphoreTake(doneSem_, 0);
  xSemaphoreTake(latchSem_, 0);
  // the ISR starts the transition at the frame that latches it
  slideKind_ = kind;
  slideFrames_ = frames;
  pending_ = c;
  hasPending_ = true;
  xSemaphoreTake(doneSem_, pdMS_TO_TICKS(ms + 500));
  // the transition ends showing the new page; the old one is free once that frame starts
  int b = (last_ + 1) % nbuf_;
  last_ = shown_ = b;
  pendingIdx_ = -1;
  xSemaphoreTake(latchSem_, 0);
  waitVsync();
}

void RgbDisplay::invalidatePages() {
  size_t bytes = (size_t)w_ * h_ * 2;
  for (auto *p : fb_)
    if (p) esp_cache_msync(p, bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
}

uint32_t RgbDisplay::waitVsync() {
  uint32_t t0 = micros();
  xSemaphoreTake(vsyncSem_, 0);
  xSemaphoreTake(vsyncSem_, pdMS_TO_TICKS(100));
  return micros() - t0;
}

bool RgbDisplay::setPclk(uint32_t hz) {
  if (esp_lcd_rgb_panel_set_pclk(panel_, hz) != ESP_OK) return false;
  pclkHz_ = hz;
  vsyncs_ = 0;
  return true;
}

float RgbDisplay::refreshHz() const {
  uint32_t n = vsyncs_;
  if (n < 2) return 0;
  return (n - 1) * 1e6f / (float)(lastVsyncUs_ - firstVsyncUs_);
}
