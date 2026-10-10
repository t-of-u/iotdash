#include "ui.h"

static bool overlaps(const Rect &a, const Rect &b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static Rect bbox(const Rect &a, const Rect &b) {
  int x0 = min(a.x, b.x), y0 = min(a.y, b.y);
  int x1 = max(a.x + a.w, b.x + b.w), y1 = max(a.y + a.h, b.y + b.h);
  return {(int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)};
}

// Add a rect, merging it with any it overlaps (so no pixel is drawn twice per frame).
void Ui::List::add(Rect a) {
  bool merged = true;
  while (merged) {
    merged = false;
    for (int i = 0; i < n; i++) {
      if (overlaps(a, r[i])) {
        a = bbox(a, r[i]);
        r[i] = r[--n];
        merged = true;
        break;
      }
    }
  }
  if (n == MAX_RECTS) {  // too many pieces: fall back to one bounding box
    for (int i = 0; i < n; i++) a = bbox(a, r[i]);
    n = 0;
  }
  r[n++] = a;
}

void Ui::invalidate(Rect r) {
  const int A = DmaBlitter::ALIGN_PX;
  int x0 = max(0, (int)r.x) & ~(A - 1);
  int x1 = min(lcd_->width(), (r.x + r.w + A - 1) & ~(A - 1));
  int y0 = max(0, (int)r.y), y1 = min(lcd_->height(), r.y + r.h);
  if (x1 <= x0 || y1 <= y0) return;
  cur_.add({(int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)});
}

bool Ui::frame(const DrawFn &draw) {
  List todo = cur_;
  for (int i = 0; i < prev_.n; i++) todo.add(prev_.r[i]);
  if (todo.n == 0) {
    lcd_->waitVsync();
    return false;
  }

  uint32_t t0 = micros();
  uint16_t *fb = lcd_->back();
  for (int i = 0; i < todo.n; i++) {
    const Rect a = todo.r[i];
    dma_->render(fb, a, [&](uint16_t *band, int y, int rows) {
      draw(gfx::Canvas{band, a.w, rows, a.x, y}, Rect{a.x, (int16_t)y, a.w, (int16_t)rows});
    });
  }
  drawUs_ = micros() - t0;

  lcd_->present();
  prev_ = cur_;
  cur_.n = 0;
  return true;
}
