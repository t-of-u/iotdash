#include "raster.h"

#include "prof.h"

namespace rt {

// ============================================================ span shading

namespace {

inline uint16_t to565(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }

// Blend a straight-alpha colour onto dst with coverage (0..255).
inline void blendPx(uint16_t *d, uint16_t c565, uint32_t a255) {
  if (a255 >= 255) {
    *d = c565;
    return;
  }
  if (a255 == 0) return;
  *d = blend565(*d, c565, (a255 + 4) >> 3);
}


inline int div255(int v) { return (v + 128 + ((v + 128) >> 8)) >> 8; }

// ---- dithered blending in 565 units with 8 fractional bits
//
// Translucent fills and shadows are blended at 8-bit precision and dithered back to 565
// (4x4 Bayer), so faint shadows and gradients do not band. Staying in 565 channel units
// (5.8 / 6.8 fixed point) avoids the 565 -> 888 -> 565 round trip: ~25 integer ops a pixel.

// Dither thresholds (1/256 units: bayer * 16 + 8) for the 4 phases of row y.
inline const uint8_t *ditherRow(int y) {
  static const uint8_t T[16] = {8, 136, 40, 168, 200, 72, 232, 104, 56, 184, 24, 152, 248, 120, 216, 88};
  return T + ((y & 3) << 2);
}

// Colour in 565 channel units with 8 fractional bits (r, b: 0..7935, g: 0..16127).
struct Fix565 {
  int r, g, b;
  Fix565(int r8, int g8, int b8) : r((r8 * 7967) >> 8), g((g8 * 16191) >> 8), b((b8 * 7967) >> 8) {}
};

inline int alpha256(uint32_t a255) { return (int)a255 + ((int)a255 >> 7); }

// s over d with alpha a (0..256), dithered with threshold t (0..255).
inline uint16_t blendFix(uint16_t d, int sr, int sg, int sb, int a, int t) {
  int dr = (d >> 11) << 8, dg = ((d >> 5) & 63) << 8, db = (d & 31) << 8;
  int r = (dr + (((sr - dr) * a) >> 8) + t) >> 8;
  int g = (dg + (((sg - dg) * a) >> 8) + t) >> 8;
  int b = (db + (((sb - db) * a) >> 8) + t) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}
inline uint16_t blendFix(uint16_t d, const Fix565 &s, int a, int t) { return blendFix(d, s.r, s.g, s.b, a, t); }

// Opaque fixed-point colour -> dithered 565.
inline uint16_t ditherFix(int r, int g, int b, int t) {
  return (uint16_t)((((r + t) >> 8) << 11) | (((g + t) >> 8) << 5) | ((b + t) >> 8));
}

// Ordered-dithered 8-bit RGB -> 565.
inline uint16_t dither565(int r, int g, int b, int x, int y) {
  return ditherFix((r * 7967) >> 8, (g * 16191) >> 8, (b * 7967) >> 8, ditherRow(y)[x & 3]);
}

inline void blendDither(uint16_t *d, int sr, int sg, int sb, uint32_t a255, int x, int y) {
  if (a255 == 0) return;
  *d = blendFix(*d, Fix565(sr, sg, sb), alpha256(a255), ditherRow(y)[x & 3]);
}

// One colour at one alpha over a run. Works in groups of four pixels (one dither phase
// cycle): a group whose background equals the previous group's reuses its result, so a
// uniform background costs a compare and two stores per four pixels.
void blendRunFix(uint16_t *d, int x, int y, int n, const Fix565 &s, int a) {
  if (n <= 0 || a <= 0) return;
  const uint8_t *tr = ditherRow(y);
  int i = 0;
  for (; i < n && ((x + i) & 3); i++) d[i] = blendFix(d[i], s, a, tr[(x + i) & 3]);
  if ((uintptr_t)(d + i) & 3) {  // odd band stride: no word access
    for (; i < n; i++) d[i] = blendFix(d[i], s, a, tr[(x + i) & 3]);
    return;
  }
  uint32_t in0 = 0, in1 = 0, out0 = 0, out1 = 0;
  bool have = false;
  const int t0 = tr[0], t1 = tr[1], t2 = tr[2], t3 = tr[3];
  for (; i + 4 <= n; i += 4) {
    uint32_t *q = (uint32_t *)(d + i);
    uint32_t v0 = q[0], v1 = q[1];
    if (have && v0 == in0 && v1 == in1) {
      q[0] = out0;
      q[1] = out1;
      continue;
    }
    in0 = v0;
    in1 = v1;
    // little endian: low half is the left pixel
    out0 = blendFix((uint16_t)v0, s, a, t0) | (uint32_t)blendFix((uint16_t)(v0 >> 16), s, a, t1) << 16;
    out1 = blendFix((uint16_t)v1, s, a, t2) | (uint32_t)blendFix((uint16_t)(v1 >> 16), s, a, t3) << 16;
    have = true;
    q[0] = out0;
    q[1] = out1;
  }
  for (; i < n; i++) d[i] = blendFix(d[i], s, a, tr[(x + i) & 3]);
}

struct Shader {
  const Paint &p;
  uint16_t solid565 = 0;
  uint8_t solidA = 255;
  explicit Shader(const Paint &pp) : p(pp) {
    if (p.kind == Paint::SOLID) {
      solid565 = p.color.to565();
      solidA = (uint8_t)((p.color.a * p.alpha + 127) / 255);
    }
  }
  bool opaqueSolid() const { return p.kind == Paint::SOLID && solidA == 255; }

  // Sample the paint at pixel (x, y): returns 565 colour and alpha (0..255).
  inline void sample(int x, int y, uint16_t &c, uint32_t &a) const {
    switch (p.kind) {
      case Paint::SOLID:
        c = solid565;
        a = solidA;
        return;
      case Paint::SIDES: {
        float bx, by;
        p.sidesInv.apply(x + 0.5f, y + 0.5f, bx, by);
        const float eps = 1e-4f;
        float d[4] = {by / std::max(p.bw[0], eps), (p.boxW - bx) / std::max(p.bw[1], eps), (p.boxH - by) / std::max(p.bw[2], eps),
                      bx / std::max(p.bw[3], eps)};
        int side = -1;
        float best = 1e30f;
        for (int k = 0; k < 4; k++)
          if (p.bw[k] > 0 && d[k] < best) {
            best = d[k];
            side = k;
          }
        if (side < 0) {
          a = 0;
          return;
        }
        const Color &col = p.sideColor[side];
        c = col.to565();
        a = (uint32_t)col.a * p.alpha / 255;
        return;
      }
      case Paint::GRADIENT: {
        float t = p.grad->tAt(x + 0.5f, y + 0.5f);
        if (p.grad->repeating) t -= (float)ifloor(t);
        int i = (int)(clampf(t, 0, 1) * 255 + 0.5f);
        const int16_t *f = p.grad->lutFix[i];
        c = ditherFix(f[0], f[1], f[2], ditherRow(y)[x & 3]);
        a = (uint32_t)(p.grad->lutA[i] * p.alpha + 127) / 255;
        return;
      }
      case Paint::IMAGE: {
        float u, v;
        p.imgInv.apply(x + 0.5f, y + 0.5f, u, v);
        const ImageView &im = p.img;
        if (p.imgRepeatX) {
          u = std::fmod(u, (float)im.w);
          if (u < 0) u += im.w;
        }
        if (p.imgRepeatY) {
          v = std::fmod(v, (float)im.h);
          if (v < 0) v += im.h;
        }
        if (!p.smooth) {
          int iu = ifloor(u), iv = ifloor(v);
          if (iu < 0 || iv < 0 || iu >= im.w || iv >= im.h) {
            a = 0;
            return;
          }
          c = im.px[iv * im.stride + iu];
          a = im.alpha ? im.alpha[iv * im.stride + iu] : 255;
          a = (a * p.alpha + 127) / 255;
          return;
        }
        // bilinear
        u -= 0.5f;
        v -= 0.5f;
        int u0 = ifloor(u), v0 = ifloor(v);
        float fu = u - u0, fv = v - v0;
        if (u0 < -1 || v0 < -1 || u0 >= im.w || v0 >= im.h) {
          a = 0;
          return;
        }
        int u1 = std::min(u0 + 1, im.w - 1), v1 = std::min(v0 + 1, im.h - 1);
        u0 = std::max(u0, 0);
        v0 = std::max(v0, 0);
        uint32_t wu = (uint32_t)(fu * 32), wv = (uint32_t)(fv * 32);
        uint32_t c00 = expand565(im.px[v0 * im.stride + u0]), c10 = expand565(im.px[v0 * im.stride + u1]);
        uint32_t c01 = expand565(im.px[v1 * im.stride + u0]), c11 = expand565(im.px[v1 * im.stride + u1]);
        uint32_t top = (c00 + (((c10 - c00) * wu) >> 5)) & 0x07E0F81F;
        uint32_t bot = (c01 + (((c11 - c01) * wu) >> 5)) & 0x07E0F81F;
        uint32_t m = (top + (((bot - top) * wv) >> 5)) & 0x07E0F81F;
        c = pack565(m);
        if (im.alpha) {
          float a00 = im.alpha[v0 * im.stride + u0], a10 = im.alpha[v0 * im.stride + u1];
          float a01 = im.alpha[v1 * im.stride + u0], a11 = im.alpha[v1 * im.stride + u1];
          float at = a00 + (a10 - a00) * fu, ab = a01 + (a11 - a01) * fu;
          a = (uint32_t)(at + (ab - at) * fv);
        } else {
          a = 255;
        }
        a = (a * p.alpha + 127) / 255;
        return;
      }
    }
  }

  // Shade a horizontal run with uniform coverage cov (0..255).
  void span(const Band &b, int x, int y, int n, uint32_t cov) const {
    if (n <= 0 || cov == 0) return;
    uint16_t *d = b.at(x, y);
    if (p.kind == Paint::SOLID) {
      uint32_t a = (solidA * cov + 127) / 255;
      if (a >= 255) {
        // fast fill (32-bit stores)
        if ((uintptr_t)d & 2) {
          *d++ = solid565;
          n--;
        }
        uint32_t c2 = (uint32_t)solid565 << 16 | solid565;
        uint32_t *q = (uint32_t *)d;
        int pairs = n >> 1;
        while (pairs >= 4) {
          q[0] = c2; q[1] = c2; q[2] = c2; q[3] = c2;
          q += 4;
          pairs -= 4;
        }
        while (pairs--) *q++ = c2;
        if (n & 1) *(uint16_t *)q = solid565;
        return;
      }
      if (a == 0) return;
      blendRunFix(d, x, y, n, Fix565(p.color.r, p.color.g, p.color.b), alpha256(a));
      return;
    }
    if (p.kind == Paint::GRADIENT && p.grad->kind == 0 && !p.grad->repeating) {
      RT_PROF(PF_GRAD);
      // linear: t is affine in x along the row
      float t0 = p.grad->tAt(x + 0.5f, y + 0.5f), t1 = p.grad->tAt(x + 1.5f, y + 0.5f);
      // t in 1/65536 of the LUT range, stepped in fixed point
      int64_t k = (int64_t)(clampf(t0, -1e4f, 1e4f) * (255.f * 65536.f)) + 32768;
      const int64_t kstep = (int64_t)(clampf(t1 - t0, -1e4f, 1e4f) * (255.f * 65536.f));
      const int am = div255((int)p.alpha * (int)cov);  // span-wide alpha, 0..255
      const uint8_t *tr = ditherRow(y);
      const GradientPaint &g = *p.grad;
      for (int i = 0; i < n; i++, k += kstep) {
        int64_t kk = k >> 16;
        int ki = kk < 0 ? 0 : kk > 255 ? 255 : (int)kk;
        const int16_t *f = g.lutFix[ki];
        int la = g.lutA[ki];
        int tt = tr[(x + i) & 3];
        if (la == 255 && am == 255) {
          d[i] = ditherFix(f[0], f[1], f[2], tt);
        } else {
          int a = (la * am + 128) >> 8;
          if (a) d[i] = blendFix(d[i], f[0], f[1], f[2], a + (a >> 7), tt);
        }
      }
      return;
    }
    for (int i = 0; i < n; i++) {
      uint16_t c;
      uint32_t a;
      sample(x + i, y, c, a);
      blendPx(d + i, c, (uint32_t)div255((int)(a * cov)));
    }
  }

  inline void pixel(const Band &b, int x, int y, uint32_t cov) const {
    if (cov == 0) return;
    uint16_t c;
    uint32_t a;
    sample(x, y, c, a);
    blendPx(b.at(x, y), c, (uint32_t)div255((int)(a * cov)));
  }
};

}  // namespace

// ============================================================ gradients

void GradientPaint::build() {
  if (stops.empty()) stops.push_back({0, kTransparent});
  for (int i = 0; i < 256; i++) {
    float t = i / 255.f;
    Color c;
    if (t <= stops.front().t) c = stops.front().c;
    else if (t >= stops.back().t) c = stops.back().c;
    else {
      size_t k = 1;
      while (k < stops.size() && stops[k].t < t) k++;
      const Stop &a = stops[k - 1], &b = stops[k];
      float f = b.t > a.t ? (t - a.t) / (b.t - a.t) : 1;
      c = lerpColor(a.c, b.c, f);
    }
    lutRGB[i][0] = c.r;
    lutRGB[i][1] = c.g;
    lutRGB[i][2] = c.b;
    lutA[i] = c.a;
    lut565[i] = c.to565();
    lutFix[i][0] = (int16_t)((c.r * 7967) >> 8);
    lutFix[i][1] = (int16_t)((c.g * 16191) >> 8);
    lutFix[i][2] = (int16_t)((c.b * 7967) >> 8);
  }
}

float GradientPaint::tAt(float x, float y) const {
  if (kind == 0) {
    float dx = bx - ax, dy = by - ay;
    float len2 = dx * dx + dy * dy;
    if (len2 <= 1e-9f) return 0;
    return ((x - ax) * dx + (y - ay) * dy) / len2;
  }
  if (kind == 1) {
    float dx = (x - cx) / rx, dy = (y - cy) / ry;
    return fastSqrt(dx * dx + dy * dy);
  }
  // conic: 0 at the top, clockwise
  float a = std::atan2(x - cx, -(y - cy)) - startAngle;
  float t = a / 6.2831853f;
  t -= std::floor(t);
  return t;
}

// ============================================================ rects

void fillRectSolid(const RasterTarget &t, const RectI &r, Color c) {
  Paint p;
  p.color = c;
  fillRectPaint(t, r, p);
}

void fillRectPaint(const RasterTarget &t, const RectI &r, const Paint &p) {
  RectI a = r.intersect(t.clip);
  if (a.empty()) return;
  Shader sh(p);
  for (int y = a.y; y < a.bottom(); y++) sh.span(*t.band, a.x, y, a.w, 255);
}

namespace {

struct RRect {
  float l, t, r, b;
  float rad[4];  // tl tr br bl (already clamped)
  void set(const RectF &rc, const float radii[4]) {
    l = rc.x;
    t = rc.y;
    r = rc.right();
    b = rc.bottom();
    float w = rc.w, h = rc.h;
    for (int i = 0; i < 4; i++) rad[i] = radii ? std::max(0.f, radii[i]) : 0;
    // CSS: scale all radii down if adjacent radii overflow the box
    float f = 1;
    auto chk = [&](float sum, float len) {
      if (sum > len && sum > 0) f = std::min(f, len / sum);
    };
    chk(rad[0] + rad[1], w);
    chk(rad[3] + rad[2], w);
    chk(rad[0] + rad[3], h);
    chk(rad[1] + rad[2], h);
    if (f < 1)
      for (auto &v : rad) v *= f;
  }
  bool empty() const { return r <= l || b <= t; }
  // Coverage (0..1) of the pixel centred at (px, py).
  float cov(float px, float py) const {
    float cx = px, cy = py;
    // corners
    if (cx < l + rad[0] && cy < t + rad[0]) return circ(px, py, l + rad[0], t + rad[0], rad[0]);
    if (cx > r - rad[1] && cy < t + rad[1]) return circ(px, py, r - rad[1], t + rad[1], rad[1]);
    if (cx > r - rad[2] && cy > b - rad[2]) return circ(px, py, r - rad[2], b - rad[2], rad[2]);
    if (cx < l + rad[3] && cy > b - rad[3]) return circ(px, py, l + rad[3], b - rad[3], rad[3]);
    float a = clampf(px - l + 0.5f, 0, 1) * clampf(r - px + 0.5f, 0, 1);
    return a * clampf(py - t + 0.5f, 0, 1) * clampf(b - py + 0.5f, 0, 1);
  }
  static float circ(float px, float py, float ccx, float ccy, float rr) {
    if (rr <= 0) return 1;
    float dx = px - ccx, dy = py - ccy;
    float d = fastSqrt(dx * dx + dy * dy);
    return clampf(rr + 0.5f - d, 0, 1);
  }
  // Row span (pixel indices) that is fully inside for row y (pixel centre cy) - may be empty.
  void innerSpan(float cy, int &x0, int &x1) const {
    if (cy - 0.5f < t || cy + 0.5f > b) {
      x0 = 1;
      x1 = 0;
      return;
    }
    float li = l, ri = r;
    if (cy < t + rad[0]) li = std::max(li, l + rad[0] - fastSqrt(std::max(0.f, rad[0] * rad[0] - sq(t + rad[0] - (cy - 0.5f)))));
    if (cy > b - rad[3]) li = std::max(li, l + rad[3] - fastSqrt(std::max(0.f, rad[3] * rad[3] - sq((cy + 0.5f) - (b - rad[3])))));
    if (cy < t + rad[1]) ri = std::min(ri, r - rad[1] + fastSqrt(std::max(0.f, rad[1] * rad[1] - sq(t + rad[1] - (cy - 0.5f)))));
    if (cy > b - rad[2]) ri = std::min(ri, r - rad[2] + fastSqrt(std::max(0.f, rad[2] * rad[2] - sq((cy + 0.5f) - (b - rad[2])))));
    // corners reached by the row's top/bottom: be conservative by one pixel
    x0 = iceil(li + 0.001f);
    x1 = ifloor(ri - 0.001f);  // exclusive
    if (rad[0] > 0 || rad[3] > 0) x0 += (cy < t + rad[0] + 1 || cy > b - rad[3] - 1) ? 1 : 0;
    if (rad[1] > 0 || rad[2] > 0) x1 -= (cy < t + rad[1] + 1 || cy > b - rad[2] - 1) ? 1 : 0;
  }
  // Pixels of row cy whose centre lies inside: [x0, x1) (may be empty).
  void centerSpan(float cy, int &x0, int &x1) const {
    if (cy <= t || cy >= b) {
      x0 = 1;
      x1 = 0;
      return;
    }
    float li = l, ri = r;
    auto inset = [&](float rr, float dy) { return rr - fastSqrt(std::max(0.f, rr * rr - dy * dy)); };
    if (cy < t + rad[0]) li = l + inset(rad[0], t + rad[0] - cy);
    else if (cy > b - rad[3]) li = l + inset(rad[3], cy - (b - rad[3]));
    if (cy < t + rad[1]) ri = r - inset(rad[1], t + rad[1] - cy);
    else if (cy > b - rad[2]) ri = r - inset(rad[2], cy - (b - rad[2]));
    x0 = iceil(li - 0.5f);
    x1 = iceil(ri - 0.5f);
  }
  static float sq(float v) { return v * v; }
};

}  // namespace

void fillRRect(const RasterTarget &t, const RectF &rc, const float radii[4], const Paint &p) {
  RRect rr;
  rr.set(rc, radii);
  if (rr.empty()) return;
  RectI bb = rc.outer().intersect(t.clip);
  if (bb.empty()) return;
  Shader sh(p);
  bool noRad = rr.rad[0] == 0 && rr.rad[1] == 0 && rr.rad[2] == 0 && rr.rad[3] == 0;
  // pixel-aligned rect without radii: plain spans
  if (noRad && rc.x == std::floor(rc.x) && rc.y == std::floor(rc.y) && rc.w == std::floor(rc.w) && rc.h == std::floor(rc.h)) {
    for (int y = bb.y; y < bb.bottom(); y++) sh.span(*t.band, bb.x, y, bb.w, 255);
    return;
  }
  // rows clear of the corners and fully covered vertically all look the same: edge pixels
  // with fixed partial coverage around one solid span
  const float topRad = std::max(rr.rad[0], rr.rad[1]), botRad = std::max(rr.rad[2], rr.rad[3]);
  const int midY0 = iceil(rr.t + topRad), midY1 = ifloor(rr.b - botRad);  // rows [midY0, midY1)
  const int fl = iceil(rr.l), fr = ifloor(rr.r);                          // full columns [fl, fr)
  const uint32_t covL = (uint32_t)((fl - rr.l) * 255 + 0.5f), covR = (uint32_t)((rr.r - fr) * 255 + 0.5f);
  const int s0 = std::max(fl, bb.x), s1 = std::min(fr, bb.right());
  const bool edgeL = covL && fl - 1 >= bb.x && fl - 1 < bb.right(), edgeR = covR && fr >= bb.x && fr < bb.right();
  for (int y = bb.y; y < bb.bottom(); y++) {
    if (y >= midY0 && y < midY1 && fr > fl && !(gDebugFlags & 2)) {
      if (edgeL) sh.pixel(*t.band, fl - 1, y, covL);
      if (s1 > s0) sh.span(*t.band, s0, y, s1 - s0, 255);
      if (edgeR) sh.pixel(*t.band, fr, y, covR);
      continue;
    }
    float cy = y + 0.5f;
    int i0, i1;
    rr.innerSpan(cy, i0, i1);
    i0 = std::max(i0, bb.x);
    i1 = std::min(i1, bb.right());
    if (i1 <= i0) {
      for (int x = bb.x; x < bb.right(); x++) sh.pixel(*t.band, x, y, (uint32_t)(rr.cov(x + 0.5f, cy) * 255 + 0.5f));
      continue;
    }
    for (int x = bb.x; x < i0; x++) sh.pixel(*t.band, x, y, (uint32_t)(rr.cov(x + 0.5f, cy) * 255 + 0.5f));
    sh.span(*t.band, i0, y, i1 - i0, 255);
    for (int x = i1; x < bb.right(); x++) sh.pixel(*t.band, x, y, (uint32_t)(rr.cov(x + 0.5f, cy) * 255 + 0.5f));
  }
}

void fillRRectRing(const RasterTarget &t, const RectF &outer, const float ro[4], const RectF &inner, const float ri[4],
                   const Paint &p) {
  RRect o, in;
  o.set(outer, ro);
  if (o.empty()) return;
  in.set(inner, ri);
  bool hasInner = !in.empty();
  RectI bb = outer.outer().intersect(t.clip);
  if (bb.empty()) return;
  Shader sh(p);
  for (int y = bb.y; y < bb.bottom(); y++) {
    float cy = y + 0.5f;
    int o0, o1, n0 = 1, n1 = 0;
    o.innerSpan(cy, o0, o1);
    if (hasInner) {
      // pixels fully inside the inner shape are skipped; widen by one for AA
      in.innerSpan(cy, n0, n1);
    }
    for (int x = bb.x; x < bb.right(); x++) {
      if (x >= n0 && x < n1) {
        x = n1 - 1;
        continue;
      }
      float c;
      if (x >= o0 && x < o1) c = 1;
      else c = o.cov(x + 0.5f, cy);
      if (hasInner && c > 0) c = std::max(0.f, c - in.cov(x + 0.5f, cy));
      if (c > 0) {
        // run-length fill when fully covered
        if (c >= 0.999f) {
          int x2 = x + 1;
          while (x2 < bb.right() && x2 >= o0 && x2 < o1 && !(x2 >= n0 && x2 < n1) &&
                 (!hasInner || in.cov(x2 + 0.5f, cy) <= 0.0f))
            x2++;
          sh.span(*t.band, x, y, x2 - x, 255);
          x = x2 - 1;
        } else {
          sh.pixel(*t.band, x, y, (uint32_t)(c * 255 + 0.5f));
        }
      }
    }
  }
}

// ============================================================ shadows

static float erfApprox(float x) {
  // Abramowitz-Stegun 7.1.26 style approximation
  float s = x < 0 ? -1.f : 1.f;
  x = std::fabs(x);
  float tt = 1.f / (1.f + 0.3275911f * x);
  float y = 1.f - (((((1.061405429f * tt - 1.453152027f) * tt) + 1.421413741f) * tt - 0.284496736f) * tt + 0.254829592f) * tt * std::exp(-x * x);
  return s * y;
}

namespace {
// Blurred rounded-rect shadow geometry, computed once per (size, radii, blur, colour alpha):
// four corner patches plus a 1-D edge profile, already scaled by the colour's alpha so
// pixels that round to nothing are known up front. Away from the corners the alpha only
// depends on the distance to the nearest edge, so a row is: corner/edge pixels on the
// left, one constant run in the middle, corner/edge pixels on the right.
struct ShadowMask {
  float ext = 0, sigma = 0, rad[4] = {0, 0, 0, 0};
  int cs[4] = {0, 0, 0, 0};  // corner patch sizes (ext + radius, rounded up)
  uint8_t *corner[4] = {};   // cs x cs alpha, outermost pixel at index 0
  uint8_t *prof = nullptr;   // alpha by distance into the shape: index 0 = ext outside
  int plen = 0;
  size_t bytes = 0;
  uint8_t *mem = nullptr;    // one block (bulk memory: PSRAM on the device) for all of the above
  float w = 0, h = 0, peak = 1;
  uint32_t ca = 255;
  ~ShadowMask() { free(mem); }
};

// One cache per painting thread (bands may be painted on two cores): no locking. Bounded by
// size, most recently used last: a fading shadow (its alpha changes every frame) must not
// pile up masks.
constexpr size_t kShadowCacheBytes = 96 * 1024;
struct ShadowCache {
  std::vector<std::shared_ptr<ShadowMask>> v;
  size_t bytes = 0;
};
ShadowCache &shadowCache() {
  static thread_local ShadowCache *c = nullptr;
  if (!c) c = new ShadowCache();
  return *c;
}

const ShadowMask *getShadowMask(float w, float h, const float rad[4], float blur, uint32_t ca) {
  float sigma = blur / 2;
  ca = std::min<uint32_t>(255, (ca + 2) & ~3u);  // steps of 4/255: a fade reuses masks
  ShadowCache &sc = shadowCache();
  for (size_t i = 0; i < sc.v.size(); i++) {
    auto &m = sc.v[i];
    if (m->ca == ca && std::fabs(m->sigma - sigma) < 0.01f && std::fabs(m->w - w) < 0.5f && std::fabs(m->h - h) < 0.5f &&
        m->rad[0] == rad[0] && m->rad[1] == rad[1] && m->rad[2] == rad[2] && m->rad[3] == rad[3]) {
      if (i + 1 < sc.v.size()) {  // most recently used last
        auto keep = m;
        sc.v.erase(sc.v.begin() + i);
        sc.v.push_back(keep);
      }
      return sc.v.back().get();
    }
  }
  RT_PROF(PF_SHADOW_MASK);
  auto m = std::make_shared<ShadowMask>();
  m->sigma = sigma;
  m->ext = sigma * 3;
  m->w = w;
  m->h = h;
  m->ca = ca;
  for (int i = 0; i < 4; i++) m->rad[i] = rad[i];
  float inv = 1.f / (sigma * 1.41421356f);
  m->peak = clampf(erfApprox(w * 0.5f * inv) * erfApprox(h * 0.5f * inv), 0, 1);
  const float scale = (float)ca;
  int ext = (int)std::ceil(m->ext);
  // profile: distance d from the edge (negative inside) sampled at pixel centres
  int plen = 2 * ext + 2;
  m->plen = plen;
  size_t total = plen;
  for (int k = 0; k < 4; k++) {
    m->cs[k] = (int)std::ceil(m->ext + rad[k]) + 1;
    total += (size_t)m->cs[k] * m->cs[k];
  }
  m->mem = (uint8_t *)gAllocBulk(total);
  if (!m->mem) m->mem = (uint8_t *)malloc(total);
  if (!m->mem) {  // out of memory: this shadow is not drawn (better than a crash)
    static int said = 0;
    if (said++ < 3)
      logf("shadow: no memory for a %u B mask (%.0fx%.0f blur %.0f, %u KB left)", (unsigned)total, w, h, blur,
           (unsigned)(gLargeFree() / 1024));
    return nullptr;
  }
  m->bytes = total + sizeof(ShadowMask);
  m->prof = m->mem;
  uint8_t *at = m->mem + plen;
  for (int k = 0; k < 4; k++) {
    m->corner[k] = at;
    at += (size_t)m->cs[k] * m->cs[k];
  }
  for (int i = 0; i < plen; i++) {
    float d = (ext - i) - 0.5f + 1;  // i = 0 is the outermost pixel
    m->prof[i] = (uint8_t)std::lround(scale * m->peak * 0.5f * (1 - erfApprox(d * inv)));
  }
  for (int k = 0; k < 4; k++) {
    float rr = rad[k];
    int csz = m->cs[k];
    // corner patch in "top-left" orientation: pixel (i, j) from the outer corner
    float cx = m->ext + rr, cy = m->ext + rr;  // circle centre within the patch
    for (int j = 0; j < csz; j++)
      for (int i = 0; i < csz; i++) {
        float px = i + 0.5f, py = j + 0.5f;
        float d;
        if (px < cx && py < cy) d = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy)) - rr;
        else d = std::max(cx - rr - px, cy - rr - py);
        float a = 0.5f * (1 - erfApprox(d * inv)) * m->peak;
        m->corner[k][(size_t)j * csz + i] = (uint8_t)std::lround(clampf(a, 0, 1) * scale);
      }
  }
  sc.bytes += m->bytes;
  sc.v.push_back(m);
  while (sc.bytes > kShadowCacheBytes && sc.v.size() > 1) {  // least recently used first
    sc.bytes -= sc.v.front()->bytes;
    sc.v.erase(sc.v.begin());
  }
  return m.get();
}

}  // namespace

// Shadow pixels [x0, x1) of one row with alpha arr[k], k = k0, k0 + step, ...
static void shadowPixels(uint16_t *row, int x0, int x1, const uint8_t *arr, int k0, int step, const Fix565 &s,
                         const uint8_t *tr) {
  const int sr = s.r, sg = s.g, sb = s.b;
  const uint8_t *ap = arr + k0;
  for (int x = x0; x < x1; x++, ap += step) {
    int a = *ap;
    if (a) row[x] = blendFix(row[x], sr, sg, sb, a + (a >> 7), tr[x & 3]);
  }
}

void fillShadow(const RasterTarget &t, const RectF &r, const float radii[4], float blur, Color c, const RectF *hole,
                const float *holeRadii) {
  float sigma = blur / 2;
  if (sigma < 0.5f) {
    Paint p;
    p.color = c;
    if (hole) {
      float hr[4] = {0, 0, 0, 0};
      if (holeRadii)
        for (int i = 0; i < 4; i++) hr[i] = holeRadii[i];
      fillRRectRing(t, r, radii, *hole, hr, p);
    } else {
      fillRRect(t, r, radii, p);
    }
    return;
  }
  if (c.a == 0) return;
  RRect shape;
  float rad[4];
  for (int i = 0; i < 4; i++) rad[i] = radii ? radii[i] : 0;
  shape.set(r, rad);  // clamps radii
  // integer placement (shadow offsets are whole pixels in practice)
  int ext = (int)std::ceil(sigma * 3);
  int X0 = (int)std::lround(r.x) - ext, Y0 = (int)std::lround(r.y) - ext;
  int W = (int)std::lround(r.w) + 2 * ext, H = (int)std::lround(r.h) + 2 * ext;
  RectI bb = RectI{X0, Y0, W, H}.intersect(t.clip);
  if (bb.empty()) return;
  const ShadowMask *m = getShadowMask(r.w, r.h, shape.rad, blur, c.a);
  if (!m) return;
  RT_PROF(PF_SHADOW_PX);
  RRect holeR;
  if (hole) holeR.set(*hole, holeRadii);
  const uint8_t *prof = m->prof;
  const int plen = m->plen;
  const Fix565 col(c.r, c.g, c.b);
  const int xa = bb.x, xb = bb.right();
  for (int y = bb.y; y < bb.bottom(); y++) {
    const int ly = y - Y0, fromBottom = H - 1 - ly;
    const int rowD = std::min(ly, fromBottom);  // distance from the top/bottom edge
    // Alpha along the row: arrL[lx] for lx < lenL, arrR[fromRight] for fromRight < lenR,
    // the constant `mid` in between (corner patch rows use the patch, others the profile).
    const uint8_t *arrL = prof, *arrR = prof;
    int lenL = std::min(rowD, plen - 1), lenR = lenL;
    if (ly < m->cs[0]) { lenL = m->cs[0]; arrL = m->corner[0] + (size_t)ly * lenL; }
    else if (fromBottom < m->cs[3]) { lenL = m->cs[3]; arrL = m->corner[3] + (size_t)fromBottom * lenL; }
    if (ly < m->cs[1]) { lenR = m->cs[1]; arrR = m->corner[1] + (size_t)ly * lenR; }
    else if (fromBottom < m->cs[2]) { lenR = m->cs[2]; arrR = m->corner[2] + (size_t)fromBottom * lenR; }
    const int mid = prof[std::min(rowD, plen - 1)];
    // pixels whose centre is under the element get no shadow (CSS clips it to the outside)
    int c0 = 1, c1 = 0;
    if (hole) holeR.centerSpan(y + 0.5f, c0, c1);
    uint16_t *row = t.band->at(0, y);
    const uint8_t *tr = ditherRow(y);
    // calls fn on [a0, a1) clipped to the band and minus the hole
    auto each = [&](int a0, int a1, auto &&fn) {
      a0 = std::max(a0, xa);
      a1 = std::min(a1, xb);
      if (a1 <= a0) return;
      if (c1 > c0 && c0 < a1 && c1 > a0) {
        if (c0 > a0) fn(a0, c0);
        if (a1 > c1) fn(c1, a1);
      } else {
        fn(a0, a1);
      }
    };
    if (lenL + lenR > W) {
      // narrow shape: the two sides meet, take the smaller distance per pixel
      each(X0, X0 + W, [&](int x0, int x1) {
        for (int x = x0; x < x1; x++) {
          int lx = x - X0, fr = W - 1 - lx, a;
          if (lx < lenL && (arrL != prof || lx <= fr)) a = arrL[lx];
          else if (fr < lenR) a = arrR[fr];
          else a = mid;
          if (a) row[x] = blendFix(row[x], col, a + (a >> 7), tr[x & 3]);
        }
      });
      continue;
    }
    each(X0, X0 + lenL, [&](int x0, int x1) { shadowPixels(row, x0, x1, arrL, x0 - X0, 1, col, tr); });
    if (mid) each(X0 + lenL, X0 + W - lenR, [&](int x0, int x1) { blendRunFix(row + x0, x0, y, x1 - x0, col, mid + (mid >> 7)); });
    each(X0 + W - lenR, X0 + W, [&](int x0, int x1) { shadowPixels(row, x0, x1, arrR, W - 1 - (x0 - X0), -1, col, tr); });
  }
}

// ============================================================ masks & images

// Glyph blending is (a + 4) >> 3 in 33 levels; text sits on a uniform background almost
// everywhere, so the 33 results for that background are tabulated once (per thread, per
// background/colour) and a pixel costs a compare and a lookup.
struct GlyphLut {
  uint16_t dst = 0, src = 0;
  bool valid = false;
  uint16_t out[33];
  void build(uint16_t d, uint16_t s) {
    uint32_t dd = expand565(d), ss = expand565(s);
    for (uint32_t a32 = 0; a32 <= 32; a32++) out[a32] = pack565((dd + (((ss - dd) * a32) >> 5)) & 0x07E0F81F);
    dst = d;
    src = s;
    valid = true;
  }
};

RT_HOT void blitMask(const RasterTarget &t, const uint8_t *mask, int mw, int mh, int x, int y, Color c) {
  if (!mask) return;  // e.g. a glyph that could not be read
  RectI r = RectI{x, y, mw, mh}.intersect(t.clip);
  if (r.empty()) return;
  RT_PROF(PF_GLYPH);
  const uint16_t c565 = c.to565();
  const uint32_t ca = c.a;
  const uint32_t s = expand565(c565);
  static thread_local GlyphLut *lutp = nullptr;
  if (!lutp) lutp = new GlyphLut();
  GlyphLut &lut = *lutp;
  for (int yy = r.y; yy < r.bottom(); yy++) {
    const uint8_t *m = mask + (yy - y) * mw + (r.x - x);
    uint16_t *d = t.band->at(r.x, yy);
    for (int xx = 0; xx < r.w; xx++) {
      uint32_t a = m[xx];
      if (!a) continue;
      if (ca != 255) a = (a * ca + 128) >> 8;
      uint16_t v = d[xx];
      if (a >= 250) {
        d[xx] = c565;
        continue;
      }
      uint32_t a32 = (a + 4) >> 3;
      if ((gDebugFlags & 1) || !lut.valid || lut.dst != v || lut.src != c565) {
        // a new background: tabulate it if the next pixels are likely to share it
        if (!(gDebugFlags & 1) && xx + 1 < r.w && d[xx + 1] == v) {
          lut.build(v, c565);
        } else {
          uint32_t dd = expand565(v);
          dd += ((s - dd) * a32) >> 5;
          d[xx] = pack565(dd & 0x07E0F81F);
          continue;
        }
      }
      d[xx] = lut.out[a32];
    }
  }
}

void blitMaskTransformed(const RasterTarget &t, const uint8_t *mask, int mw, int mh, const Affine &inv, const RectI &bbox,
                         Color c) {
  if (!mask) return;
  RectI r = bbox.intersect(t.clip);
  if (r.empty()) return;
  uint16_t c565 = c.to565();
  for (int y = r.y; y < r.bottom(); y++) {
    for (int x = r.x; x < r.right(); x++) {
      float u, v;
      inv.apply(x + 0.5f, y + 0.5f, u, v);
      u -= 0.5f;
      v -= 0.5f;
      int u0 = ifloor(u), v0 = ifloor(v);
      if (u0 < -1 || v0 < -1 || u0 >= mw || v0 >= mh) continue;
      float fu = u - u0, fv = v - v0;
      auto M = [&](int uu, int vv) -> float { return (uu < 0 || vv < 0 || uu >= mw || vv >= mh) ? 0.f : (float)mask[vv * mw + uu]; };
      float a = (M(u0, v0) * (1 - fu) + M(u0 + 1, v0) * fu) * (1 - fv) + (M(u0, v0 + 1) * (1 - fu) + M(u0 + 1, v0 + 1) * fu) * fv;
      uint32_t aa = (uint32_t)(a * c.a / 255 + 0.5f);
      if (aa) blendPx(t.band->at(x, y), c565, aa);
    }
  }
}

void drawImage(const RasterTarget &t, const ImageView &img, const RectF &dst, const RectF &src, uint8_t alpha, bool smooth,
               const RectF *clipRR, const float *clipRadii) {
  if (!img.px || dst.w <= 0 || dst.h <= 0 || src.w <= 0 || src.h <= 0) return;
  RectI bb = dst.outer().intersect(t.clip);
  if (bb.empty()) return;
  float sx = src.w / dst.w, sy = src.h / dst.h;
  RRect clip;
  if (clipRR) clip.set(*clipRR, clipRadii);
  bool hasClipR = clipRR && (clip.rad[0] > 0 || clip.rad[1] > 0 || clip.rad[2] > 0 || clip.rad[3] > 0);
  // 1:1 fast path
  bool oneToOne = std::fabs(sx - 1) < 1e-4f && std::fabs(sy - 1) < 1e-4f && dst.x == std::floor(dst.x) &&
                  dst.y == std::floor(dst.y) && src.x == std::floor(src.x) && src.y == std::floor(src.y);
  if (oneToOne && !hasClipR) {
    int ox = (int)src.x - (int)dst.x, oy = (int)src.y - (int)dst.y;
    for (int y = bb.y; y < bb.bottom(); y++) {
      int iy = y + oy;
      if (iy < 0 || iy >= img.h) continue;
      const uint16_t *s = img.px + iy * img.stride;
      const uint8_t *as = img.alpha ? img.alpha + iy * img.stride : nullptr;
      uint16_t *d = t.band->at(bb.x, y);
      int x0 = bb.x + ox;
      for (int x = 0; x < bb.w; x++) {
        int ix = x0 + x;
        if (ix < 0 || ix >= img.w) continue;
        uint32_t a = as ? as[ix] : 255;
        if (alpha != 255) a = a * alpha / 255;
        blendPx(d + x, s[ix], a);
      }
    }
    return;
  }
  Paint p;
  p.kind = Paint::IMAGE;
  p.img = img;
  p.smooth = smooth && !(sx > 0.99f && sx < 1.01f && sy > 0.99f && sy < 1.01f);
  p.alpha = alpha;
  // screen -> image
  p.imgInv = Affine{sx, 0, 0, sy, src.x - dst.x * sx, src.y - dst.y * sy};
  Shader sh(p);
  for (int y = bb.y; y < bb.bottom(); y++) {
    for (int x = bb.x; x < bb.right(); x++) {
      uint32_t cov = 255;
      if (hasClipR || clipRR) {
        float cv = clip.cov(x + 0.5f, y + 0.5f);
        if (cv <= 0) continue;
        cov = (uint32_t)(cv * 255 + 0.5f);
      }
      // edge coverage for fractional destination rects
      float ex = clampf(x + 1 - dst.x, 0, 1) * clampf(dst.right() - x, 0, 1);
      float ey = clampf(y + 1 - dst.y, 0, 1) * clampf(dst.bottom() - y, 0, 1);
      cov = (uint32_t)(cov * ex * ey);
      sh.pixel(*t.band, x, y, cov);
    }
  }
}

// ============================================================ paths

void Path::finishContour(bool closed) {
  if (open_) {
    if (pts.size() - start_ >= 2) {
      contourEnds.push_back((uint32_t)pts.size());
      if (!closed) openEnds.push_back((uint32_t)pts.size());
    } else {
      pts.resize(start_);
    }
    open_ = false;
  }
}

void Path::moveTo(float x, float y) {
  finishContour();
  start_ = (uint32_t)pts.size();
  pts.push_back({x, y});
  open_ = true;
}

void Path::lineTo(float x, float y) {
  if (!open_) {
    moveTo(x, y);
    return;
  }
  const Pt &l = pts.back();
  if (l.x == x && l.y == y) return;
  pts.push_back({x, y});
}

void Path::close() { finishContour(true); }

void Path::quadTo(float cx, float cy, float x, float y, float tol) {
  if (!open_) moveTo(cx, cy);
  Pt p0 = pts.back();
  float dd = std::fabs(p0.x - 2 * cx + x) + std::fabs(p0.y - 2 * cy + y);
  int n = std::max(1, std::min(64, (int)std::ceil(std::sqrt(dd / (4 * tol)))));
  for (int i = 1; i <= n; i++) {
    float tt = (float)i / n, mt = 1 - tt;
    lineTo(mt * mt * p0.x + 2 * mt * tt * cx + tt * tt * x, mt * mt * p0.y + 2 * mt * tt * cy + tt * tt * y);
  }
}

void Path::cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y, float tol) {
  if (!open_) moveTo(c1x, c1y);
  Pt p0 = pts.back();
  float d1 = std::fabs(p0.x - 2 * c1x + c2x) + std::fabs(p0.y - 2 * c1y + c2y);
  float d2 = std::fabs(c1x - 2 * c2x + x) + std::fabs(c1y - 2 * c2y + y);
  float dd = std::max(d1, d2);
  int n = std::max(1, std::min(100, (int)std::ceil(std::sqrt(dd * 0.75f / tol))));
  for (int i = 1; i <= n; i++) {
    float tt = (float)i / n, mt = 1 - tt;
    float a = mt * mt * mt, b = 3 * mt * mt * tt, c = 3 * mt * tt * tt, d = tt * tt * tt;
    lineTo(a * p0.x + b * c1x + c * c2x + d * x, a * p0.y + b * c1y + c * c2y + d * y);
  }
}

void Path::ellipse(float cx, float cy, float rx, float ry, float rot, float a0, float a1, bool ccw) {
  const float TAU = 6.28318531f;
  float sweep = a1 - a0;
  if (!ccw) {
    if (sweep < 0) sweep = std::fmod(sweep, TAU) + TAU;
    if (a1 - a0 >= TAU) sweep = TAU;
  } else {
    if (sweep > 0) sweep = std::fmod(sweep, TAU) - TAU;
    if (a0 - a1 >= TAU) sweep = -TAU;
  }
  float r = std::max(rx, ry);
  int n = std::max(4, std::min(256, (int)std::ceil(std::fabs(sweep) * std::sqrt(r) * 1.5f)));
  float cr = std::cos(rot), sr = std::sin(rot);
  for (int i = 0; i <= n; i++) {
    float a = a0 + sweep * i / n;
    float ex = std::cos(a) * rx, ey = std::sin(a) * ry;
    float px = cx + ex * cr - ey * sr, py = cy + ex * sr + ey * cr;
    if (i == 0 && !open_) moveTo(px, py);
    else lineTo(px, py);
  }
}

void Path::arc(float cx, float cy, float r, float a0, float a1, bool ccw) { ellipse(cx, cy, r, r, 0, a0, a1, ccw); }

void Path::rect(float x, float y, float w, float h) {
  moveTo(x, y);
  lineTo(x + w, y);
  lineTo(x + w, y + h);
  lineTo(x, y + h);
  close();
}

void Path::roundRect(float x, float y, float w, float h, const float radii[4]) {
  float r[4];
  for (int i = 0; i < 4; i++) r[i] = std::min(std::max(0.f, radii[i]), std::min(std::fabs(w), std::fabs(h)) / 2);
  const float PI = 3.14159265f;
  moveTo(x + r[0], y);
  lineTo(x + w - r[1], y);
  if (r[1] > 0) arc(x + w - r[1], y + r[1], r[1], -PI / 2, 0, false);
  lineTo(x + w, y + h - r[2]);
  if (r[2] > 0) arc(x + w - r[2], y + h - r[2], r[2], 0, PI / 2, false);
  lineTo(x + r[3], y + h);
  if (r[3] > 0) arc(x + r[3], y + h - r[3], r[3], PI / 2, PI, false);
  lineTo(x, y + r[0]);
  if (r[0] > 0) arc(x + r[0], y + r[0], r[0], PI, PI * 1.5f, false);
  close();
}

RectF Path::bounds() const {
  if (pts.empty()) return {};
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  for (auto &p : pts) {
    x0 = std::min(x0, p.x);
    y0 = std::min(y0, p.y);
    x1 = std::max(x1, p.x);
    y1 = std::max(y1, p.y);
  }
  return {x0, y0, x1 - x0, y1 - y0};
}

void Path::transform(const Affine &m) {
  for (auto &p : pts) {
    float x, y;
    m.apply(p.x, p.y, x, y);
    p.x = x;
    p.y = y;
  }
}

// ---- scanline coverage rasterizer (exact area accumulation, after font-rs) ----

namespace {
struct Edge {
  float x0, y0, x1, y1;  // y0 < y1
  float dir;
};

// Per-thread scratch for the path filler (bands may be painted on two cores).
struct PathScratch {
  std::vector<float> acc;  // kept all-zero between rows
  std::vector<Edge> edges;
  std::vector<const Edge *> active;
  std::vector<std::pair<int, int>> spans;
};
static PathScratch &pathScratch() {
  static thread_local PathScratch *s = nullptr;
  if (!s) s = new PathScratch();
  return *s;
}

// Accumulate one edge's contribution for pixel row [y, y+1) into acc (x already relative).
// Returns false if nothing was added, else the touched cells [lo, hi].
inline bool accumulateRow(float *a, int w, const Edge &e, float y, int &lo, int &hi) {
  float ya = std::max(e.y0, y), yb = std::min(e.y1, y + 1);
  if (yb <= ya) return false;
  float dxdy = (e.x1 - e.x0) / (e.y1 - e.y0);
  float x = e.x0 + (ya - e.y0) * dxdy;
  float xnext = e.x0 + (yb - e.y0) * dxdy;
  float d = (yb - ya) * e.dir;
  float x0 = std::min(x, xnext), x1 = std::max(x, xnext);
  // clamp into the buffer (area left of 0 accumulates at 0, right of w is dropped)
  if (x1 <= 0) {
    a[0] += d;
    lo = hi = 0;
    return true;
  }
  if (x0 >= w) return false;
  if (x0 < 0 || x1 > w) {
    // split coarsely: clamp endpoints (keeps the winding contribution exact)
    float cx0 = clampf(x0, 0, (float)w - 0.001f), cx1 = clampf(x1, 0, (float)w - 0.001f);
    x0 = cx0;
    x1 = cx1;
    x = (x < xnext) ? x0 : x1;
    xnext = (x == x0) ? x1 : x0;
  }
  int x0i = ifloor(x0);
  float x0floor = (float)x0i;
  int x1i = iceil(x1);
  float x1ceil = (float)x1i;
  lo = x0i;
  if (x1i <= x0i + 1) {
    float xmf = 0.5f * (x + xnext) - x0floor;
    a[x0i] += d - d * xmf;
    a[x0i + 1] += d * xmf;
    hi = x0i + 1;
  } else {
    float s = 1.f / (x1 - x0);
    float x0f = x0 - x0floor;
    float a0 = 0.5f * s * (1 - x0f) * (1 - x0f);
    float x1f = x1 - x1ceil + 1;
    float am = 0.5f * s * x1f * x1f;
    a[x0i] += d * a0;
    if (x1i == x0i + 2) {
      a[x0i + 1] += d * (1 - a0 - am);
    } else {
      float a1 = s * (1.5f - x0f);
      a[x0i + 1] += d * (a1 - a0);
      for (int xi = x0i + 2; xi < x1i - 1; xi++) a[xi] += d * s;
      float a2 = a1 + (x1i - x0i - 3) * s;
      a[x1i - 1] += d * (1 - a2 - am);
    }
    a[x1i] += d * am;
    hi = x1i;
  }
  return true;
}
}  // namespace

void fillPath(const RasterTarget &t, const Path &path, FillRule rule, const Paint &paint) {
  if (path.pts.empty()) return;
  RectF b = path.bounds();
  RectI bb = RectI{(int)std::floor(b.x), (int)std::floor(b.y), (int)std::ceil(b.right()) - (int)std::floor(b.x) + 1,
                   (int)std::ceil(b.bottom()) - (int)std::floor(b.y) + 1}
                 .intersect(t.clip);
  if (bb.empty()) return;
  RT_PROF(PF_PATH);
  // edges (all contours implicitly closed)
  PathScratch &S = pathScratch();
  std::vector<Edge> &edges = S.edges;
  edges.clear();
  uint32_t start = 0;
  const uint32_t npts = (uint32_t)path.pts.size();
  auto addContour = [&](uint32_t end) {
    if (end > npts) return;
    if (end - start >= 2) {
      for (uint32_t i = start; i < end; i++) {
        const Path::Pt &p0 = path.pts[i];
        const Path::Pt &p1 = path.pts[i + 1 < end ? i + 1 : start];
        if (p0.y == p1.y) continue;
        Edge e;
        if (p0.y < p1.y) e = {p0.x - bb.x, p0.y, p1.x - bb.x, p1.y, 1};
        else e = {p1.x - bb.x, p1.y, p0.x - bb.x, p0.y, -1};
        if (e.y1 <= bb.y || e.y0 >= bb.bottom()) continue;
        edges.push_back(e);
      }
    }
    start = end;
  };
  for (uint32_t end : path.contourEnds) addContour(end);
  if (path.open_ && npts - path.start_ >= 2) addContour(npts);
  if (edges.empty()) return;
  std::sort(edges.begin(), edges.end(), [](const Edge &a, const Edge &c) { return a.y0 < c.y0; });
  const int w = bb.w;
  if ((int)S.acc.size() < w + 3) S.acc.assign(w + 3, 0.f);
  float *acc = S.acc.data();
  Shader sh(paint);
  auto covOf = [rule](float sum) -> uint32_t {
    float c = std::fabs(sum);
    if (rule == FR_EVENODD) {
      c -= 2.f * (float)(int)(c * 0.5f);
      if (c > 1) c = 2 - c;
    } else if (c > 1) {
      c = 1;
    }
    return (uint32_t)(c * 255 + 0.5f);
  };
  size_t first = 0;
  std::vector<const Edge *> &active = S.active;
  active.clear();
  auto &spans = S.spans;
  for (int y = bb.y; y < bb.bottom(); y++) {
    float fy = (float)y;
    while (first < edges.size() && edges[first].y0 < fy + 1) active.push_back(&edges[first++]);
    if (active.empty()) continue;
    // accumulate, remembering which cells each edge touched
    spans.clear();
    size_t k = 0;
    for (size_t i = 0; i < active.size(); i++) {
      const Edge *e = active[i];
      if (e->y1 <= fy) continue;
      active[k++] = e;
      int lo, hi;
      if (accumulateRow(acc, w + 1, *e, fy, lo, hi)) spans.push_back({lo, hi});
    }
    active.resize(k);
    if (spans.empty()) continue;
    // touched cells in x order (insertion sort: a handful per row), merged
    for (size_t i = 1; i < spans.size(); i++)
      for (size_t j = i; j > 0 && spans[j].first < spans[j - 1].first; j--) std::swap(spans[j], spans[j - 1]);
    size_t nm = 0;
    for (size_t i = 0; i < spans.size(); i++) {
      if (nm && spans[i].first <= spans[nm - 1].second + 1) spans[nm - 1].second = std::max(spans[nm - 1].second, spans[i].second);
      else spans[nm++] = spans[i];
    }
    // coverage is constant between touched cells: those runs are shaded in one go
    float sum = 0;
    int x = 0;
    for (size_t i = 0; i < nm; i++) {
      int lo = spans[i].first, hi = spans[i].second;
      if (lo > x) {
        uint32_t cov = covOf(sum);
        if (cov) sh.span(*t.band, bb.x + x, y, std::min(lo, w) - x, cov);
      }
      for (int xi = lo; xi <= hi; xi++) {
        sum += acc[xi];
        acc[xi] = 0;
        if (xi < w) {
          uint32_t cov = covOf(sum);
          if (cov) sh.pixel(*t.band, bb.x + xi, y, cov);
        }
      }
      x = hi + 1;
    }
    if (x < w) {
      uint32_t cov = covOf(sum);
      if (cov) sh.span(*t.band, bb.x + x, y, w - x, cov);
    }
  }
}

// ---- stroking ----

static void addPoly(Path &out, std::initializer_list<Path::Pt> pts) {
  // keep a consistent (positive) orientation so nonzero unions overlap correctly
  std::vector<Path::Pt> v(pts);
  float area = 0;
  for (size_t i = 0; i < v.size(); i++) {
    const auto &a = v[i], &b = v[(i + 1) % v.size()];
    area += a.x * b.y - b.x * a.y;
  }
  if (area < 0) std::reverse(v.begin(), v.end());
  out.moveTo(v[0].x, v[0].y);
  for (size_t i = 1; i < v.size(); i++) out.lineTo(v[i].x, v[i].y);
  out.close();
}

static void addCircle(Path &out, float cx, float cy, float r) {
  int n = std::max(8, std::min(64, (int)(r * 2.5f)));
  out.moveTo(cx + r, cy);
  for (int i = 1; i < n; i++) {
    float a = 6.2831853f * i / n;
    out.lineTo(cx + r * std::cos(a), cy + r * std::sin(a));
  }
  out.close();
}

static void strokePolyline(const std::vector<Path::Pt> &p, bool closed, const StrokeStyle &st, Path &out) {
  size_t n = p.size();
  if (n < 2) {
    if (n == 1 && st.cap == CAP_ROUND) addCircle(out, p[0].x, p[0].y, st.width / 2);
    return;
  }
  float hw = st.width / 2;
  size_t segs = closed ? n : n - 1;
  for (size_t i = 0; i < segs; i++) {
    Path::Pt a = p[i], b = p[(i + 1) % n];
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6f) continue;
    float nx = -dy / len * hw, ny = dx / len * hw;
    float ex = 0, ey = 0;  // extension for square caps
    Path::Pt a2 = a, b2 = b;
    if (!closed && st.cap == CAP_SQUARE) {
      ex = dx / len * hw;
      ey = dy / len * hw;
      if (i == 0) {
        a2.x -= ex;
        a2.y -= ey;
      }
      if (i == segs - 1) {
        b2.x += ex;
        b2.y += ey;
      }
    }
    addPoly(out, {{a2.x + nx, a2.y + ny}, {b2.x + nx, b2.y + ny}, {b2.x - nx, b2.y - ny}, {a2.x - nx, a2.y - ny}});
  }
  // joins
  size_t jstart = closed ? 0 : 1, jend = closed ? n : n - 1;
  for (size_t i = jstart; i < jend; i++) {
    Path::Pt prev = p[(i + n - 1) % n], cur = p[i], next = p[(i + 1) % n];
    float d0x = cur.x - prev.x, d0y = cur.y - prev.y, d1x = next.x - cur.x, d1y = next.y - cur.y;
    float l0 = std::sqrt(d0x * d0x + d0y * d0y), l1 = std::sqrt(d1x * d1x + d1y * d1y);
    if (l0 < 1e-6f || l1 < 1e-6f) continue;
    d0x /= l0; d0y /= l0; d1x /= l1; d1y /= l1;
    float cross = d0x * d1y - d0y * d1x;
    if (std::fabs(cross) < 1e-4f && d0x * d1x + d0y * d1y > 0) continue;  // straight
    if (st.join == JOIN_ROUND) {
      addCircle(out, cur.x, cur.y, hw);
      continue;
    }
    // outer side normals
    float s = cross > 0 ? -1.f : 1.f;
    Path::Pt o0{cur.x - d0y * hw * s, cur.y + d0x * hw * s};
    Path::Pt o1{cur.x - d1y * hw * s, cur.y + d1x * hw * s};
    if (st.join == JOIN_MITER) {
      float cosT = d0x * d1x + d0y * d1y;
      float miterLen = 1 / std::sqrt(std::max(1e-6f, (1 + cosT) / 2));
      if (miterLen <= st.miter) {
        float mx = (-d0y - d1y) * s, my = (d0x + d1x) * s;
        float ml = std::sqrt(mx * mx + my * my);
        if (ml > 1e-6f) {
          mx = mx / ml * hw * miterLen;
          my = my / ml * hw * miterLen;
          addPoly(out, {cur, o0, {cur.x + mx, cur.y + my}, o1});
          continue;
        }
      }
    }
    addPoly(out, {cur, o0, o1});
  }
  if (!closed && st.cap == CAP_ROUND) {
    addCircle(out, p[0].x, p[0].y, hw);
    addCircle(out, p[n - 1].x, p[n - 1].y, hw);
  }
}

void strokePath(const Path &in, const StrokeStyle &st, Path &out, bool) {
  uint32_t start = 0;
  std::vector<uint32_t> ends = in.contourEnds;
  std::vector<bool> closedFlags(ends.size(), true);
  // a subpath left open (another moveTo came first) is stroked open: "M4 8h14l-3-3M20 16H6"
  // is two arrows, not two triangles
  for (size_t ci = 0, oi = 0; ci < ends.size() && oi < in.openEnds.size(); ci++) {
    while (oi < in.openEnds.size() && in.openEnds[oi] < ends[ci]) oi++;
    if (oi < in.openEnds.size() && in.openEnds[oi] == ends[ci]) closedFlags[ci] = false;
  }
  if (in.open_ && in.pts.size() - in.start_ >= 1) {
    ends.push_back((uint32_t)in.pts.size());
    closedFlags.push_back(false);
  }
  for (size_t ci = 0; ci < ends.size(); ci++) {
    uint32_t end = ends[ci];
    std::vector<Path::Pt> poly(in.pts.begin() + start, in.pts.begin() + end);
    bool closed = closedFlags[ci];
    start = end;
    if (st.dash.empty()) {
      strokePolyline(poly, closed, st, out);
      continue;
    }
    // dashes
    float total = 0;
    for (float d : st.dash) total += d;
    if (total <= 0) {
      strokePolyline(poly, closed, st, out);
      continue;
    }
    if (closed) poly.push_back(poly[0]);
    size_t di = 0;
    float dpos = std::fmod(st.dashOffset, total);
    if (dpos < 0) dpos += total;
    while (dpos >= st.dash[di]) {
      dpos -= st.dash[di];
      di = (di + 1) % st.dash.size();
    }
    bool on = (di % 2) == 0;
    float remain = st.dash[di] - dpos;
    std::vector<Path::Pt> cur;
    if (on) cur.push_back(poly[0]);
    for (size_t i = 0; i + 1 < poly.size(); i++) {
      Path::Pt a = poly[i], b = poly[i + 1];
      float dx = b.x - a.x, dy = b.y - a.y;
      float len = std::sqrt(dx * dx + dy * dy);
      float pos = 0;
      while (len - pos > remain) {
        pos += remain;
        Path::Pt m{a.x + dx * pos / len, a.y + dy * pos / len};
        if (on) {
          cur.push_back(m);
          strokePolyline(cur, false, st, out);
          cur.clear();
        } else {
          cur.push_back(m);
        }
        on = !on;
        di = (di + 1) % st.dash.size();
        remain = st.dash[di];
      }
      remain -= len - pos;
      if (on) cur.push_back(b);
    }
    if (on && cur.size() >= 2) strokePolyline(cur, false, st, out);
  }
}

}  // namespace rt
