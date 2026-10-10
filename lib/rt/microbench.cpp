// Raster micro benchmarks: the same primitives in a tight loop on one band, so the device and
// the host can be compared without the page (cache and code-fetch effects of a full frame
// are left out on purpose).
#include "microbench.h"

#include "prof.h"
#include "raster.h"

namespace rt {

std::string microBench(uint16_t *buf, int w, int rows) {
  Band band{buf, w, 0, 0, w, rows};
  std::string out;
  char line[128];
  auto run = [&](const char *name, int iters, int px, const std::function<void(int)> &fn) {
    fn(0);  // warm caches
    uint32_t t0 = cycles();
    for (int i = 0; i < iters; i++) fn(i);
    uint32_t dt = cycles() - t0;
    double perCall = (double)dt / iters;
    snprintf(line, sizeof line, "%-14s %8.0f cyc/call %6.1f cyc/px\n", name, perCall, px ? perCall / px : 0.0);
    out += line;
  };
  RasterTarget t{&band, {0, 0, w, rows}};
  const int n = w * rows;
  run("fill-solid", 50, n, [&](int i) { fillRectSolid(t, {0, 0, w, rows}, Color{(uint8_t)i, 200, 100, 255}); });
  run("fill-alpha", 20, n, [&](int) {
    Paint p;
    p.color = Color{20, 40, 200, 100};
    fillRRect(t, {0, 0, (float)w, (float)rows}, nullptr, p);
  });
  fillRectSolid(t, {0, 0, w, rows}, Color{255, 255, 255, 255});
  float rad[4] = {14, 14, 14, 14};
  // a card's shadow: rows through the middle (left/right edges only) and across the top edge
  RectF card{40, -100, 513, 165};
  RectF hole = card;
  hole.y -= 2;
  run("shadow-side", 50, 0, [&](int) { fillShadow(t, card, rad, 10, Color{0, 0, 0, 15}, &hole, rad); });
  RectF cardTop{40, 12, 513, 165};
  RectF holeTop = cardTop;
  holeTop.y -= 2;
  run("shadow-top", 50, 0, [&](int) { fillShadow(t, cardTop, rad, 10, Color{0, 0, 0, 15}, &holeTop, rad); });
  run("rrect-card", 50, 0, [&](int) {
    Paint p;
    p.color = Color{255, 255, 255, 255};
    fillRRect(t, {40, -4, 513, 165}, rad, p);
  });
  run("rect-800", 50, n, [&](int) {
    Paint p;
    p.color = Color{238, 242, 247, 255};
    fillRRect(t, {0, 0, (float)w, (float)rows}, nullptr, p);
  });
  run("rrect-mid", 50, 513 * rows, [&](int) {
    Paint p;
    p.color = Color{255, 255, 255, 255};
    fillRRect(t, {40.4f, -60, 513, 165}, rad, p);
  });
  Path circle;
  circle.ellipse(200, 5, 60, 60, 0, 0, 6.2831853f, false);
  circle.close();
  run("path-circle", 50, 0, [&](int) {
    Paint p;
    p.color = Color{200, 40, 40, 255};
    fillPath(t, circle, FR_NONZERO, p);
  });
  static uint8_t glyph[16 * 16];
  for (int i = 0; i < 256; i++) glyph[i] = (uint8_t)((i * 37) & 255);
  run("glyph16", 200, 256, [&](int i) { blitMask(t, glyph, 16, 16, (i * 17) % (w - 16), -3, Color{30, 30, 30, 255}); });
  // pure ALU loop (no memory): calibrates the core itself
  volatile uint32_t sink = 0;
  run("alu-1k", 20, 1000, [&](int) {
    uint32_t a = 1;
    for (int i = 0; i < 1000; i++) a = a * 1664525u + 1013904223u;
    sink = a;
  });
  (void)sink;
  return out;
}

}  // namespace rt

namespace rt {
// Paint the same primitives into one band twice - whole, and as two clipped halves - and
// report the first differing pixel (results must not depend on the clip).
std::string clipTest() {
  const int w = 64, rows = 10, y0 = 200;
  std::vector<uint16_t> a(w * rows, 0xFFFF), b(w * rows, 0xFFFF);
  Band ba{a.data(), w, 360, y0, w, rows}, bb{b.data(), w, 360, y0, w, rows};
  float rr[4] = {11, 11, 11, 11};
  RectF sh{369.3f, 181.f, 22, 22};
  auto draw = [&](const Band &band, RectI clip) {
    RasterTarget t{&band, clip};
    fillShadow(t, sh, rr, 4, Color{0, 0, 0, 70}, nullptr, nullptr);
    Paint p;
    p.color = Color{255, 255, 255, 255};
    fillRRect(t, {369.3f, 180, 22, 22}, rr, p);
    float ri[4] = {10, 10, 10, 10};
    p.color = Color{0, 0, 0, 30};
    fillRRectRing(t, {369.3f, 180, 22, 22}, rr, {370.3f, 181, 20, 20}, ri, p);
  };
  draw(ba, ba.rect());
  draw(bb, {360, y0, w, 5});
  draw(bb, {360, y0 + 5, w, 5});
  std::string out;
  for (int y = 0; y < rows; y++)
    for (int x = 0; x < w; x++)
      if (a[y * w + x] != b[y * w + x]) {
        char buf[96];
        snprintf(buf, sizeof buf, "diff at %d,%d: %04x vs %04x\n", 360 + x, y0 + y, a[y * w + x], b[y * w + x]);
        out += buf;
        if (out.size() > 600) return out;
      }
  return out.empty() ? "clip test ok\n" : out;
}
}  // namespace rt
