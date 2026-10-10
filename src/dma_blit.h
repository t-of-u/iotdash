#pragma once

#include <Arduino.h>
#include <esp_private/gdma.h>
#include <esp_private/gdma_link.h>
#include <freertos/semphr.h>

#include <functional>

#include "rgb_display.h"

// Draws a rect of a PSRAM frame buffer through internal-SRAM bands that GDMA copies out.
//
// Why: a CPU store to PSRAM goes through the data cache, which first reads the whole
// cache line from PSRAM, so drawing costs about twice its size in PSRAM traffic and
// competes with the panel's scan-out (the image then shifts). Here the CPU draws into
// fast SRAM and the DMA only writes to PSRAM. While the DMA copies one band, the CPU
// draws the next one.
//
// Rects must be 32-pixel aligned horizontally (x and w multiples of 32) so every row
// starts and ends on a 64-byte cache line / DMA burst boundary.
class DmaBlitter {
 public:
  static constexpr int ALIGN_PX = 32;
  // draw(band, y, rows): fill rows [y, y+rows) of the rect; band row i, column j is
  // screen pixel (r.x + j, y + i), band stride = r.w.
  using DrawFn = std::function<void(uint16_t *band, int y, int rows)>;

  bool begin(int fbWidth, size_t bandBytes = 16 * 1024);
  bool render(uint16_t *fb, const Rect &r, const DrawFn &draw);

  // Cap the average PSRAM write rate (bytes/s, 0 = no cap). Spreading a big redraw over
  // the frame instead of bursting it keeps the panel's scan-out from being starved.
  void setMaxRate(uint32_t bytesPerSec) { maxRate_ = bytesPerSec; }
  // Start each band only in a gap between the panel's bounce-buffer refills (see
  // RgbDisplay::waitBusWindow): the scan-out never competes with the blitter for PSRAM.
  void setScanOut(RgbDisplay *d) { scanOut_ = d; }

 private:
  static constexpr int MAX_ROWS = 64;  // RX descriptors per band (one per row)
  static bool IRAM_ATTR onRxEof(gdma_channel_handle_t, gdma_event_data_t *, void *ctx);
  bool start(int slot, uint16_t *fb, const Rect &r, int y, int rows);
  bool wait();

  gdma_channel_handle_t tx_ = nullptr, rx_ = nullptr;
  gdma_link_list_handle_t txList_[2] = {}, rxList_[2] = {};
  uint16_t *band_[2] = {};
  size_t bandBytes_ = 0;
  int fbW_ = 0;
  uint32_t maxRate_ = 40 * 1000 * 1000;  // with 31 MB/s of scan-out: no late fills measured up to 60 MB/s
  SemaphoreHandle_t done_ = nullptr;
  RgbDisplay *scanOut_ = nullptr;
};
