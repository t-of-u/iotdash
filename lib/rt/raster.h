#pragma once
// Rasterizer for RGB565 bands. Everything is drawn into a small band buffer in internal SRAM
// (y range [y0, y0 + rows)), clipped to a rect, then DMA'd to the frame buffer.
// Anti-aliased rounded rects, gradients (ordered dither), shadows, glyph masks, images and
// arbitrary paths (canvas / SVG) with an exact-area coverage scanline rasterizer.

#include "rt_base.h"

namespace rt {

struct Band {
  uint16_t *buf;  // rows * stride pixels
  int stride;     // pixels per row (== band width)
  int x0, y0;     // screen position of buf[0]
  int w, rows;
  RectI rect() const { return {x0, y0, w, rows}; }
  uint16_t *at(int x, int y) const { return buf + (y - y0) * stride + (x - x0); }
};

// 565 helpers
inline uint32_t expand565(uint16_t c) { return (c | ((uint32_t)c << 16)) & 0x07E0F81F; }
inline uint16_t pack565(uint32_t x) { return (uint16_t)((x & 0xF81F) | ((x >> 16) & 0x07E0)); }
// blend src over dst with alpha a (0..32)
inline uint16_t blend565(uint16_t dst, uint16_t src, uint32_t a32) {
  uint32_t d = expand565(dst), s = expand565(src);
  d += ((s - d) * a32) >> 5;
  return pack565(d & 0x07E0F81F);
}

// Paint source for fills: solid colour, gradient or image.
struct Gradient;
struct ImageView {
  const uint16_t *px = nullptr;  // RGB565
  const uint8_t *alpha = nullptr;  // optional A8
  int w = 0, h = 0, stride = 0;
};

struct GradientPaint {
  // linear: t = (x*dx + y*dy) - t0 ; radial: t = dist((x,y),(cx,cy)) / r ; conic: angle
  uint8_t kind = 0;  // 0 linear 1 radial 2 conic
  float ax = 0, ay = 0, bx = 0, by = 0;  // linear: start/end points (screen)
  float cx = 0, cy = 0, rx = 1, ry = 1;  // radial / conic centre and radii
  float startAngle = 0;                 // conic (radians, 0 = up)
  bool repeating = false;
  struct Stop {
    float t;
    Color c;
  };
  std::vector<Stop> stops;
  // 256-entry lookup: colour in 565 units with 8 fractional bits (r, g, b) and alpha
  uint16_t lut565[256];
  uint8_t lutA[256];
  uint8_t lutRGB[256][3];
  int16_t lutFix[256][3];
  void build();
  float tAt(float x, float y) const;
};

struct Paint {
  enum Kind : uint8_t { SOLID, GRADIENT, IMAGE, SIDES } kind = SOLID;
  Color color = kBlack;
  const GradientPaint *grad = nullptr;
  ImageView img;
  Affine imgInv;           // screen -> image pixel transform
  bool imgRepeatX = false, imgRepeatY = false;
  bool smooth = true;      // bilinear sampling
  uint8_t alpha = 255;     // global alpha multiplier
  // SIDES: border colour chosen per pixel by the nearest box edge (corner wedges split along
  // the outer->inner corner diagonals, like CSS). Geometry in box space via sidesInv.
  Affine sidesInv;         // screen -> box space
  float boxW = 0, boxH = 0, bw[4] = {0, 0, 0, 0};
  Color sideColor[4];
};

// Clip + band target for one drawing operation.
struct RasterTarget {
  const Band *band;
  RectI clip;  // already intersected with the band rect
};

// ---- primitives ----
void fillRectSolid(const RasterTarget &t, const RectI &r, Color c);
void fillRectPaint(const RasterTarget &t, const RectI &r, const Paint &p);
// Rounded rect with per-corner radii (x radius only = circular corners), AA edges.
// Fractional rect coordinates are honoured (sub-pixel edges get partial coverage).
void fillRRect(const RasterTarget &t, const RectF &r, const float radii[4], const Paint &p);
// Ring between outer and inner rounded rects (borders).
void fillRRectRing(const RasterTarget &t, const RectF &outer, const float ro[4], const RectF &inner, const float ri[4],
                   const Paint &p);
// Blurred rounded-rect shadow (Gaussian approximation).
void fillShadow(const RasterTarget &t, const RectF &r, const float radii[4], float blur, Color c, const RectF *holeOut,
                const float *holeRadii);
// A8 mask (glyph) at integer position, coloured.
void blitMask(const RasterTarget &t, const uint8_t *mask, int mw, int mh, int x, int y, Color c);
// A8 mask scaled/transformed (inv maps screen -> mask pixel); bbox is the screen bounds to scan.
void blitMaskTransformed(const RasterTarget &t, const uint8_t *mask, int mw, int mh, const Affine &inv, const RectI &bbox,
                         Color c);
// Image into a destination rect (scaled), optionally clipped by a rounded rect.
void drawImage(const RasterTarget &t, const ImageView &img, const RectF &dst, const RectF &src, uint8_t alpha, bool smooth,
               const RectF *clipRR = nullptr, const float *clipRadii = nullptr);

// ---- paths ----
struct Path {
  struct Pt {
    float x, y;
  };
  std::vector<Pt> pts;
  std::vector<uint32_t> contourEnds;  // index past the end of each finished contour
  std::vector<uint32_t> openEnds;     // those of contourEnds whose contour was not closed (strokes
                                      // leave them open; fills close every contour anyway)
  void moveTo(float x, float y);
  void lineTo(float x, float y);
  void close();
  void quadTo(float cx, float cy, float x, float y, float tol = 0.25f);
  void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y, float tol = 0.25f);
  void arc(float cx, float cy, float r, float a0, float a1, bool ccw);
  void ellipse(float cx, float cy, float rx, float ry, float rot, float a0, float a1, bool ccw);
  void rect(float x, float y, float w, float h);
  void roundRect(float x, float y, float w, float h, const float radii[4]);
  void clear() {
    pts.clear();
    contourEnds.clear();
    openEnds.clear();
    open_ = false;
  }
  bool empty() const { return pts.empty(); }
  RectF bounds() const;
  void transform(const Affine &m);
  bool open_ = false;  // a contour is in progress
  uint32_t start_ = 0;
  void finishContour(bool closed = false);
};

enum FillRule : uint8_t { FR_NONZERO, FR_EVENODD };
enum LineCap : uint8_t { CAP_BUTT, CAP_ROUND, CAP_SQUARE };
enum LineJoin : uint8_t { JOIN_MITER, JOIN_ROUND, JOIN_BEVEL };

struct StrokeStyle {
  float width = 1;
  LineCap cap = CAP_BUTT;
  LineJoin join = JOIN_MITER;
  float miter = 10;
  std::vector<float> dash;
  float dashOffset = 0;
};

// Convert a stroke to a fillable outline (nonzero).
void strokePath(const Path &in, const StrokeStyle &st, Path &out, bool closedFlags = false);
// Fill a path (screen coordinates) with AA.
void fillPath(const RasterTarget &t, const Path &p, FillRule rule, const Paint &paint);

// Ordered dither threshold (4x4 Bayer) for gradients: 0..15
inline int bayer4(int x, int y) {
  static const uint8_t m[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
  return m[((y & 3) << 2) | (x & 3)];
}

}  // namespace rt
