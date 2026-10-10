#pragma once

#include <Arduino.h>

#include "rgb_display.h"

// Tiny drawing helpers for an RGB565 frame buffer with a fixed stride.
namespace gfx {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t BLACK = 0x0000, WHITE = 0xFFFF;

inline Rect clip(Rect r, int w, int h) {
  if (r.x < 0) { r.w += r.x; r.x = 0; }
  if (r.y < 0) { r.h += r.y; r.y = 0; }
  if (r.x + r.w > w) r.w = w - r.x;
  if (r.y + r.h > h) r.h = h - r.y;
  if (r.w < 0) r.w = 0;
  if (r.h < 0) r.h = 0;
  return r;
}

// Fill one row with 32-bit stores (two pixels per store).
inline void fillRow(uint16_t *p, int n, uint16_t c) {
  if (n <= 0) return;
  if ((uintptr_t)p & 2) { *p++ = c; n--; }
  uint32_t c2 = (uint32_t)c << 16 | c;
  uint32_t *q = reinterpret_cast<uint32_t *>(p);
  int pairs = n >> 1;
  while (pairs >= 4) { q[0] = c2; q[1] = c2; q[2] = c2; q[3] = c2; q += 4; pairs -= 4; }
  while (pairs--) *q++ = c2;
  if (n & 1) *reinterpret_cast<uint16_t *>(q) = c;
}

inline void fillRect(uint16_t *fb, int stride, int fbH, Rect r, uint16_t c) {
  r = clip(r, stride, fbH);
  for (int y = r.y; y < r.y + r.h; y++) fillRow(fb + y * stride + r.x, r.w, c);
}

inline void frame(uint16_t *fb, int stride, int fbH, Rect r, int t, uint16_t c) {
  fillRect(fb, stride, fbH, {r.x, r.y, r.w, (int16_t)t}, c);
  fillRect(fb, stride, fbH, {r.x, (int16_t)(r.y + r.h - t), r.w, (int16_t)t}, c);
  fillRect(fb, stride, fbH, {r.x, r.y, (int16_t)t, r.h}, c);
  fillRect(fb, stride, fbH, {(int16_t)(r.x + r.w - t), r.y, (int16_t)t, r.h}, c);
}

// Seven-segment digits: enough for numbers on a benchmark screen without a font.
// Glyph cell is 6s wide and 10s tall (s = scale); returns the x after the glyph.
inline int digit(uint16_t *fb, int stride, int fbH, int x, int y, int s, char ch, uint16_t c) {
  static const uint8_t SEG[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  int w = 4 * s, h = 9 * s, t = s;
  if (ch == '.') {
    fillRect(fb, stride, fbH, {(int16_t)x, (int16_t)(y + h - t), (int16_t)t, (int16_t)t}, c);
    return x + 2 * s;
  }
  if (ch == '-') {
    fillRect(fb, stride, fbH, {(int16_t)x, (int16_t)(y + h / 2), (int16_t)w, (int16_t)t}, c);
    return x + 6 * s;
  }
  if (ch == 'x') {  // small multiplication sign
    for (int i = 0; i < 3 * s; i++) {
      fillRect(fb, stride, fbH, {(int16_t)(x + s / 2 + i), (int16_t)(y + 3 * s + i), (int16_t)t, (int16_t)t}, c);
      fillRect(fb, stride, fbH, {(int16_t)(x + s / 2 + 3 * s - i), (int16_t)(y + 3 * s + i), (int16_t)t, (int16_t)t}, c);
    }
    return x + 5 * s;
  }
  if (ch < '0' || ch > '9') return x + 6 * s;
  uint8_t m = SEG[ch - '0'];
  int hy = h / 2;
  if (m & 0x01) fillRect(fb, stride, fbH, {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)t}, c);                    // a
  if (m & 0x02) fillRect(fb, stride, fbH, {(int16_t)(x + w - t), (int16_t)y, (int16_t)t, (int16_t)(hy + 1)}, c);   // b
  if (m & 0x04) fillRect(fb, stride, fbH, {(int16_t)(x + w - t), (int16_t)(y + hy), (int16_t)t, (int16_t)(h - hy)}, c); // c
  if (m & 0x08) fillRect(fb, stride, fbH, {(int16_t)x, (int16_t)(y + h - t), (int16_t)w, (int16_t)t}, c);          // d
  if (m & 0x10) fillRect(fb, stride, fbH, {(int16_t)x, (int16_t)(y + hy), (int16_t)t, (int16_t)(h - hy)}, c);      // e
  if (m & 0x20) fillRect(fb, stride, fbH, {(int16_t)x, (int16_t)y, (int16_t)t, (int16_t)(hy + 1)}, c);             // f
  if (m & 0x40) fillRect(fb, stride, fbH, {(int16_t)x, (int16_t)(y + hy), (int16_t)w, (int16_t)t}, c);             // g
  return x + 6 * s;
}

inline int text(uint16_t *fb, int stride, int fbH, int x, int y, int s, const char *str, uint16_t c) {
  while (*str) x = digit(fb, stride, fbH, x, y, s, *str++, c);
  return x;
}

// A drawing surface in screen coordinates: screen pixel (x, y) is
// buf[(y - oy) * stride + (x - ox)]. A whole frame buffer is {fb, 800, 480, 0, 0};
// a DMA band covering rect r from line y is {band, r.w, rows, r.x, y}.
struct Canvas {
  uint16_t *buf;
  int stride, h, ox, oy;

  Rect bounds() const { return {(int16_t)ox, (int16_t)oy, (int16_t)stride, (int16_t)h}; }
  void fill(Rect r, uint16_t c) const {
    fillRect(buf, stride, h, {(int16_t)(r.x - ox), (int16_t)(r.y - oy), r.w, r.h}, c);
  }
  // Fill row y (screen) between x and x + w; rows outside the canvas are ignored.
  void row(int x, int y, int w, uint16_t c) const {
    if (y < oy || y >= oy + h) return;
    int x0 = max(x, ox), x1 = min(x + w, ox + stride);
    if (x1 > x0) fillRow(buf + (y - oy) * stride + (x0 - ox), x1 - x0, c);
  }
  int text(int x, int y, int s, const char *str, uint16_t c) const {
    return gfx::text(buf, stride, h, x - ox, y - oy, s, str, c) + ox;
  }
};

}  // namespace gfx
