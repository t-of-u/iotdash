#pragma once
// Shared basics for the HTML runtime ("rt"). Portable C++17: the same sources build for
// the ESP32-S3 player and for the desktop simulator.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// RT_HOT: code placed in internal RAM on the device. The firmware runs from PSRAM through a
// 32 KB instruction cache that a full band pass over the paint list overflows; the innermost
// paint paths are kept out of it.
#if defined(ESP_PLATFORM)
#include <esp_attr.h>
#define RT_HOT IRAM_ATTR
#else
#define RT_HOT
#endif

namespace rt {

// ---- platform hooks (implemented by the player / simulator) ----
uint64_t nowUs();
void logWrite(const char *s);

// Debug switches for A/B checks of fast paths (simulator "dbg N"): 1 = no glyph table,
// 2 = no rounded-rect row fast path, 4 = no cached layers (canvas/svg).
extern int gDebugFlags;
constexpr int DBG_INPUT = 0x10;  // log every pointer event and what took it

// What the engine thread is doing (for diagnostics: e.g. the panel tags a scan-out tear with it)
enum Phase : uint8_t { PH_IDLE, PH_LOAD, PH_SCRIPT, PH_OS, PH_LAYOUT, PH_PAINT, PH_IO, PH_INPUT, PH_COUNT };
extern volatile uint8_t gPhase;
extern uint32_t gRestyleCount;  // elements cascaded (style recalc stats)
// Some loaded rule has a sibling combinator (+, ~): a state or attribute change of an element
// can then restyle its following siblings. Without one, only the element's own subtree.
extern bool gSiblingSelectors;
// Some rule tests a pseudo-class (or :not/:is/:has) left of its last compound, e.g.
// ".key:active .label": a state change of an element can then restyle its descendants.
// Without one (".key:active"), a state change restyles only the element itself.
extern bool gStateDescendants;
extern const char *const kPhaseNames[PH_COUNT];

// Guards the few lazily filled caches the painter touches (shadow masks, canvas screen
// paths, svg trees) when bands are painted on two cores at once.
struct PaintLock {
  PaintLock();
  ~PaintLock();
  PaintLock(const PaintLock &) = delete;
  PaintLock &operator=(const PaintLock &) = delete;
};
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- geometry ----
struct RectI {
  int x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
  int right() const { return x + w; }
  int bottom() const { return y + h; }
  bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
  bool intersects(const RectI &o) const {
    return x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h;
  }
  RectI intersect(const RectI &o) const {
    int x0 = std::max(x, o.x), y0 = std::max(y, o.y);
    int x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
    if (x1 <= x0 || y1 <= y0) return {x0, y0, 0, 0};
    return {x0, y0, x1 - x0, y1 - y0};
  }
  RectI unite(const RectI &o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    int x0 = std::min(x, o.x), y0 = std::min(y, o.y);
    int x1 = std::max(right(), o.right()), y1 = std::max(bottom(), o.bottom());
    return {x0, y0, x1 - x0, y1 - y0};
  }
  bool operator==(const RectI &o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
  bool operator!=(const RectI &o) const { return !(*this == o); }
};

struct RectF {
  float x = 0, y = 0, w = 0, h = 0;
  float right() const { return x + w; }
  float bottom() const { return y + h; }
  bool empty() const { return w <= 0 || h <= 0; }
  bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
  RectF intersect(const RectF &o) const {
    float x0 = std::max(x, o.x), y0 = std::max(y, o.y);
    float x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
    if (x1 <= x0 || y1 <= y0) return {x0, y0, 0, 0};
    return {x0, y0, x1 - x0, y1 - y0};
  }
  RectF unite(const RectF &o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    float x0 = std::min(x, o.x), y0 = std::min(y, o.y);
    float x1 = std::max(right(), o.right()), y1 = std::max(bottom(), o.bottom());
    return {x0, y0, x1 - x0, y1 - y0};
  }
  RectF inset(float l, float t, float r, float b) const { return {x + l, y + t, w - l - r, h - t - b}; }
  RectF translated(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
  // Smallest integer rect covering this one.
  RectI outer() const {
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    int x1 = (int)std::ceil(x + w), y1 = (int)std::ceil(y + h);
    return {x0, y0, x1 - x0, y1 - y0};
  }
  bool operator==(const RectF &o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
  bool operator!=(const RectF &o) const { return !(*this == o); }
};

// 2D affine transform: x' = a*x + c*y + e, y' = b*x + d*y + f (canvas/CSS matrix order).
struct Affine {
  float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  static Affine translate(float x, float y) { return {1, 0, 0, 1, x, y}; }
  static Affine scale(float sx, float sy) { return {sx, 0, 0, sy, 0, 0}; }
  static Affine rotate(float rad) {
    float s = std::sin(rad), co = std::cos(rad);
    return {co, s, -s, co, 0, 0};
  }
  // this * o (apply o first, then this)
  Affine operator*(const Affine &o) const {
    return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d,
            b * o.c + d * o.d, a * o.e + c * o.f + e, b * o.e + d * o.f + f};
  }
  void apply(float x, float y, float &ox, float &oy) const {
    ox = a * x + c * y + e;
    oy = b * x + d * y + f;
  }
  bool isTranslate() const { return a == 1 && b == 0 && c == 0 && d == 1; }
  bool isIdentity() const { return isTranslate() && e == 0 && f == 0; }
  bool invert(Affine &out) const {
    float det = a * d - b * c;
    if (std::fabs(det) < 1e-12f) return false;
    float id = 1.0f / det;
    out = {d * id, -b * id, -c * id, a * id, (c * f - d * e) * id, (b * e - a * f) * id};
    return true;
  }
  RectF mapRect(const RectF &r) const {
    float xs[4], ys[4];
    apply(r.x, r.y, xs[0], ys[0]);
    apply(r.right(), r.y, xs[1], ys[1]);
    apply(r.x, r.bottom(), xs[2], ys[2]);
    apply(r.right(), r.bottom(), xs[3], ys[3]);
    float x0 = std::min({xs[0], xs[1], xs[2], xs[3]}), x1 = std::max({xs[0], xs[1], xs[2], xs[3]});
    float y0 = std::min({ys[0], ys[1], ys[2], ys[3]}), y1 = std::max({ys[0], ys[1], ys[2], ys[3]});
    return {x0, y0, x1 - x0, y1 - y0};
  }
};

// ---- colour ----
struct Color {
  uint8_t r = 0, g = 0, b = 0, a = 0;  // straight (not premultiplied) alpha
  bool operator==(const Color &o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
  bool operator!=(const Color &o) const { return !(*this == o); }
  bool opaque() const { return a == 255; }
  bool transparent() const { return a == 0; }
  // rounded (not truncated) conversion: truncation makes every colour slightly too bright
  uint16_t to565() const {
    return (uint16_t)((((r * 31 + 127) / 255) << 11) | (((g * 63 + 127) / 255) << 5) | ((b * 31 + 127) / 255));
  }
  Color withAlpha(uint8_t na) const { return {r, g, b, na}; }
  Color scaleAlpha(int k /*0..255*/) const { return {r, g, b, (uint8_t)((a * k + 127) / 255)}; }
};
constexpr Color kTransparent{0, 0, 0, 0};
constexpr Color kBlack{0, 0, 0, 255};
constexpr Color kWhite{255, 255, 255, 255};

inline Color lerpColor(Color a, Color b, float t) {
  auto L = [t](uint8_t x, uint8_t y) { return (uint8_t)std::lround(x + (y - x) * t); };
  // interpolate premultiplied so transparent endpoints don't darken the mix
  float aa = a.a / 255.f, ba = b.a / 255.f;
  float oa = aa + (ba - aa) * t;
  if (oa <= 0.0001f) return kTransparent;
  auto P = [&](uint8_t x, uint8_t y) {
    float v = (x * aa + (y * ba - x * aa) * t) / oa;
    return (uint8_t)std::lround(std::min(255.f, std::max(0.f, v)));
  };
  (void)L;
  return {P(a.r, b.r), P(a.g, b.g), P(a.b, b.b), (uint8_t)std::lround(oa * 255)};
}

// ---- small helpers ----
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Inline float helpers for hot loops: on the ESP32-S3 sqrtf/floorf/ceilf are library calls.
// fastSqrt: bit-trick reciprocal root + two Newton steps (relative error < 1e-5).
inline float fastSqrt(float x) {
  if (!(x > 0)) return 0;
  union {
    float f;
    uint32_t i;
  } u{x};
  u.i = 0x5f375a86u - (u.i >> 1);
  float y = u.f, hx = 0.5f * x;
  y = y * (1.5f - hx * y * y);
  y = y * (1.5f - hx * y * y);
  return x * y;
}
inline int ifloor(float v) {
  int i = (int)v;
  return i - (v < (float)i);
}
inline int iceil(float v) {
  int i = (int)v;
  return i + (v > (float)i);
}

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
inline char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
std::string toLower(const std::string &s);
std::string trim(const std::string &s);
bool startsWith(const std::string &s, const char *p);
bool iequals(const std::string &a, const char *b);
std::vector<std::string> split(const std::string &s, char sep);

// UTF-8
uint32_t utf8Next(const char *s, size_t len, size_t &i);  // decode at i, advance i
void utf8Append(std::string &out, uint32_t cp);
size_t utf8PrevBoundary(const std::string &s, size_t i);   // start of the code point before i

// Minimal JSON reader for manifests (no JS needed to load an app).
struct Json {
  enum Type : uint8_t { Null, Bool, Num, Str, Arr, Obj } type = Null;
  double num = 0;
  bool b = false;
  std::string str;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;
  const Json *get(const char *key) const;
  double numOr(const char *key, double d) const {
    const Json *j = get(key);
    return j && j->type == Num ? j->num : d;
  }
  std::string strOr(const char *key, const char *d) const {
    const Json *j = get(key);
    return j && j->type == Str ? j->str : std::string(d);
  }
  static bool parse(const char *s, size_t n, Json &out);
  std::string dump() const;                     // compact JSON text
  static std::string quote(const std::string &s);  // JSON string literal
};

// ---- program images
// Where an archive's bytes are. In RAM it is used in place (direct()); in slow storage (the
// device's flash partitions) it is read on demand, in paced pieces, by the kernel -- never
// mapped: reading flash while drawing starves the panel's scan-out (see slow memory below).
struct ImageSource {
  virtual ~ImageSource() = default;
  virtual size_t size() const = 0;
  virtual const uint8_t *direct() const { return nullptr; }
  virtual bool read(size_t off, void *dst, size_t n) const = 0;
};

// An archive in RAM; `keep` keeps its buffer alive. inPlace = false makes users go through
// read() as with slow storage (the simulator checks that path with it).
class RamSource : public ImageSource {
 public:
  RamSource(const uint8_t *p, size_t n, std::shared_ptr<void> keep = nullptr, bool inPlace = true)
      : p_(p), n_(n), keep_(std::move(keep)), inPlace_(inPlace) {}
  size_t size() const override { return n_; }
  const uint8_t *direct() const override { return inPlace_ ? p_ : nullptr; }
  bool read(size_t off, void *dst, size_t n) const override {
    if (off > n_ || n > n_ - off) return false;
    memcpy(dst, p_ + off, n);
    return true;
  }

 private:
  const uint8_t *p_;
  size_t n_;
  std::shared_ptr<void> keep_;
  bool inPlace_;
};

// One file of an archive.
struct ImageSlice {
  const uint8_t *data = nullptr;  // in RAM: read in place; nullptr: read() through src
  size_t size = 0;
  std::shared_ptr<const ImageSource> src;  // keeps the archive alive
  uint32_t off = 0;                        // in the archive
  bool read(size_t at, void *dst, size_t n) const {
    if (at > size || n > size - at) return false;
    if (data) {
      memcpy(dst, data + at, n);
      return true;
    }
    return src && src->read(off + at, dst, n);
  }
};

// ---- slow memory
// On the device, program images are mapped from flash. Flash and the PSRAM the panel scans out
// from share one bus, and a stream of flash reads starves the scan-out (the screen glitches).
// So what is read while drawing (font atlases, images) gets a RAM copy, made once, and other
// bulk reads (page sources) are paced. The platform sets the hooks; the defaults (simulator)
// treat all memory as fast.
extern bool (*gIsSlowMemory)(const void *p);
extern void (*gBulkCopy)(void *dst, const void *src, size_t n);  // paced for slow sources
extern size_t (*gLargeFree)();                                   // memory left for big buffers
extern void (*gIoPause)();  // between 4 KB file reads (storage in flash: lets the scan-out in)
extern void (*gSlowPause)();               // after ~4 KB read from slow memory in small pieces
extern void *(*gAllocBulk)(size_t n);      // copies of slow data (device: PSRAM; free())
// A RAM copy of [src, src+len) when src is slow memory (shared, and kept until its image is
// dropped), else src itself; also src when memory is short.
const uint8_t *residentCopy(const uint8_t *src, size_t len);
// The image at [begin, begin+len) goes away: its copies are freed by collectResident(), once
// no document can still be using them.
void dropResident(const uint8_t *begin, size_t len);
void collectResident();
size_t residentBytes();

}  // namespace rt
