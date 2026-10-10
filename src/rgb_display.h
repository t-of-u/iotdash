#pragma once

#include <Arduino.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_private/gdma.h>
#include <esp_private/gdma_link.h>
#include <freertos/semphr.h>

struct Rect {
  int16_t x, y, w, h;
};

// RGB565 driver for the 800x480 panel with a scan-out compositor.
//
// The panel runs without driver frame buffers: we own two page buffers in PSRAM and fill
// the bounce buffers (internal SRAM) ourselves from the interrupt. Normally that is one
// memcpy per bounce buffer, exactly like the stock driver; during a page transition each
// row is composed from the old and the new page at a sliding offset, so a full-screen slide
// costs no extra drawing and no PSRAM writes - it runs at the panel's 39 Hz.
//
//  - draw into back() (through DmaBlitter), then present(): the switch happens at the next
//    frame start, so there is no tearing; present() returns once the old front is free
//  - slide(): animate from the current front to back() over `ms`, then back() is shown
//  - the scan-out interrupt lives on core 0; draw from core 1
class RgbDisplay {
 public:
  enum Slide : uint8_t { SLIDE_LEFT = 1, SLIDE_RIGHT = 2, SLIDE_UP = 3, SLIDE_DOWN = 4 };

  bool begin(uint32_t pclkHz);

  int width() const { return w_; }
  int height() const { return h_; }

  // Page buffers are used in rotation: front() is the newest presented frame, back() the
  // oldest one (bufferCount() - 1 frames old), which is the next to draw into.
  uint16_t *front() { return fb_[last_]; }
  uint16_t *back() { return fb_[(last_ + 1) % nbuf_]; }
  // the page the scan-out is reading now (null during a transition)
  const uint16_t *scanPage() const { return active_.kind ? nullptr : active_.page; }
  int bufferCount() const { return nbuf_; }

  // Queue back() for display at the next frame start; returns at once (with three buffers
  // the next frame can be drawn while this one waits for its frame start).
  void present();
  // Block until back() may be drawn into (it is neither on screen nor queued).
  void waitPresented();
  // Pages are written by DMA only; call before reading them with the CPU.
  void invalidatePages();
  // Blocking page transition from front() to back() within `area` (rows/cols outside it
  // come from back()). Afterwards back() is the front buffer.
  void slide(Slide kind, int ms, const Rect &area);

  uint32_t waitVsync();
  bool setPclk(uint32_t hz);
  uint32_t pclk() const { return pclkHz_; }
  float refreshHz() const;
  uint32_t frames() const { return frames_; }

  // Scan-out health: bounce-buffer fills that took longer than the buffer lasts on screen
  // (each one is a visible glitch) and the slowest fill seen.
  uint32_t lateFills() const { return late_; }
  uint32_t maxFillUs() const { return maxFillCycles_ / 240; }
  uint32_t avgFillUs() const { return avgFillCycles_ / 240; }
  uint32_t budgetUs() const { return budgetCycles_ / 240; }
  void resetStats() {
    late_ = 0;
    maxFillCycles_ = 0;
  }
  bool preload = false;  // CPU path: prefetch the next chunk into the data cache after each fill
  bool useDma = true;    // fill bounce buffers with GDMA (bypasses the cache, frees the CPU)
  uint32_t dmaFallbacks() const { return dmaFallbacks_; }
  // Tears: the panel started sending a bounce buffer whose refill had not finished (the DMA
  // descriptor still belonged to the DMA) -- what the eye sees as a glitch band. Counted per
  // rt::Phase of the engine at that moment.
  uint32_t tears() const { return tears_; }
  uint32_t tearsIn(int phase) const { return tearByPhase_[phase & 7]; }
  uint32_t tearsWithBlit() const { return tearsWithBlit_; }  // the band blitter was writing too
  uint32_t maxTearAgeUs() const { return maxTearAgeUs_; }    // refill age when the panel needed it
  // interrupt latency: the bounce interrupt arriving late leaves the refill no time
  uint32_t isrLateMaxUs() const { return isrLateMaxUs_; }
  uint32_t isrLate200() const { return isrLate200_; }
  uint32_t isrBackToBack() const { return isrBackToBack_; }
  uint32_t maxDmaIsrUs() const { return maxDmaIsrCycles_ / 240; }
  int isrCore() const { return isrCore_; }
  // For other DMA traffic to PSRAM (the band blitter): wait until a transfer of `needUs` fits
  // between two refills, so the two never compete for the bus. Returns at once in a slide.
  void waitBusWindow(uint32_t needUs);

 private:
  struct Cfg {
    const uint16_t *page;  // shown page (no transition)
    const uint16_t *from, *to;
    int16_t ax, ay, aw, ah;  // transition area
    int16_t offset;          // pixels the old page has moved
    uint8_t kind;            // 0 = none
  };
  static bool IRAM_ATTR onBounceEmpty(esp_lcd_panel_handle_t, void *bounce, int pos_px, int len_bytes, void *ctx);
  static bool IRAM_ATTR onVsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *ctx);
  static void createTask(void *arg);
  static bool IRAM_ATTR onFillEof(gdma_channel_handle_t, gdma_event_data_t *, void *ctx);
  bool create();
  bool initFillDma();
  bool IRAM_ATTR dmaFill(void *bounce, int pos_px, int len_bytes);
  void IRAM_ATTR fill(uint16_t *out, int y0, int rows);

  esp_lcd_panel_handle_t panel_ = nullptr;
  uint16_t *fb_[3] = {nullptr, nullptr, nullptr};
  int nbuf_ = 2;
  int last_ = 0;       // newest presented buffer
  int shown_ = 0;      // buffer the scan-out reads (as far as the task side knows)
  int pendingIdx_ = -1;  // presented, waiting for its frame start
  void syncLatch(bool block);
  int w_ = 0, h_ = 0, bbRows_ = 0;
  uint32_t pclkHz_ = 0;
  SemaphoreHandle_t vsyncSem_ = nullptr, latchSem_ = nullptr, doneSem_ = nullptr;
  volatile bool createOk_ = false;
  // compositor state (ISR side)
  Cfg active_ = {}, pending_ = {};
  volatile bool hasPending_ = false;
  volatile uint32_t frames_ = 0;
  volatile uint32_t vsyncs_ = 0, firstVsyncUs_ = 0, lastVsyncUs_ = 0;
  // running transition (advanced by the ISR once per frame)
  volatile bool sliding_ = false;
  uint32_t slideStart_ = 0, slideFrames_ = 0;
  uint8_t slideKind_ = 0;
  // GDMA bounce fill
  gdma_channel_handle_t ftx_ = nullptr, frx_ = nullptr;
  gdma_link_list_handle_t ftxList_[2] = {}, frxList_[2] = {};
  void *bounceSeen_[2] = {nullptr, nullptr};
  volatile bool fillBusy_ = false, dmaReady_ = false;
  volatile uint32_t dmaFallbacks_ = 0;
  int fillEnd_[2] = {0, 0};      // last descriptor of each slot's refill
  volatile int lastFillSlot_ = -1;
  volatile int64_t lastBounceUs_ = 0;
  volatile uint32_t tears_ = 0, tearByPhase_[8] = {};
  volatile uint32_t tearsWithBlit_ = 0, maxTearAgeUs_ = 0;
  volatile uint32_t isrLateMaxUs_ = 0, isrLate200_ = 0, isrBackToBack_ = 0, maxDmaIsrCycles_ = 0;
  volatile int isrCore_ = -1;
 public:
  // Timeline of the last bounce callbacks, frozen at the first tear after arm(): "T" text cmd
  struct Ev {
    uint32_t us;    // low 32 bits of esp_timer
    int32_t pos;    // pos_px
    uint8_t flags;  // 1 dma busy at entry, 2 via dma, 4 tear, 8 vsync
    uint8_t phase;
  };
  static constexpr int kEv = 64;
  Ev ev_[kEv];
  Ev frozen_[kEv];
  volatile int evHead_ = 0, frozenHead_ = -1;
  volatile bool evArmed_ = false;
  void armTimeline() { frozenHead_ = -1; evArmed_ = true; }
 private:
  volatile int64_t fillStartUs_ = 0;
  bool IRAM_ATTR fillRunning();
  // stats
  volatile uint32_t late_ = 0, maxFillCycles_ = 0, budgetCycles_ = 0, avgFillCycles_ = 0;
};
