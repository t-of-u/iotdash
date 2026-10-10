#pragma once

#include <functional>

#include "dma_blit.h"
#include "gfx.h"
#include "rgb_display.h"

// Frame loop for a retained UI on the RGB panel.
//
//   ui.invalidate(rect);           // whatever changed since the last frame
//   ui.frame([](const gfx::Canvas &c, const Rect &area) {
//     ...paint every pixel of `area` on c (background first, then widgets)...
//   });
//
// frame() redraws only invalid areas, through DMA, into the back buffer and presents it
// at the next frame boundary (tear-free). With two buffers, an area changed last frame
// is missing from the current back buffer, so it is redrawn again: nothing is ever copied
// PSRAM to PSRAM, and the CPU never stores to PSRAM (both would starve the scan-out).
class Ui {
 public:
  using DrawFn = std::function<void(const gfx::Canvas &c, const Rect &area)>;

  void begin(RgbDisplay &lcd, DmaBlitter &dma) {
    lcd_ = &lcd;
    dma_ = &dma;
  }

  // Mark r for redraw. x and w are widened to 32-pixel boundaries (DMA requirement).
  void invalidate(Rect r);
  void invalidateAll() { invalidate({0, 0, (int16_t)lcd_->width(), (int16_t)lcd_->height()}); }

  // Redraw and present. Returns false (after waiting one vsync) when nothing was invalid.
  bool frame(const DrawFn &draw);

  uint32_t lastDrawUs() const { return drawUs_; }  // drawing + DMA time of the last frame

 private:
  static constexpr int MAX_RECTS = 16;
  struct List {
    Rect r[MAX_RECTS];
    int n = 0;
    void add(Rect a);
  };

  RgbDisplay *lcd_ = nullptr;
  DmaBlitter *dma_ = nullptr;
  List cur_, prev_;
  uint32_t drawUs_ = 0;
};
