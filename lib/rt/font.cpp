#include "font.h"

#include <mutex>

namespace rt {

bool FontFace::load(const ImageSlice &file) {
  if (file.size < sizeof(FntHeader) || !file.read(0, &hdr, sizeof hdr)) return false;
  if (memcmp(hdr.magic, "FNT1", 4) != 0 || hdr.bitmapOff > file.size) return false;
  family = std::string(hdr.family, strnlen(hdr.family, sizeof(hdr.family)));
  file_ = file;
  ready_ = false;
  return true;
}

// ---- residency (atlases mapped from flash)

struct FaceResident {
  const ImageSource *src;  // identity (the face's ImageSlice keeps the archive alive)
  uint32_t off;
  uint8_t *tables = nullptr;  // copy of [0, bitmapOff): header, glyphs, kerning, Thai rows
  std::unique_ptr<std::atomic<const uint8_t *>[]> bm;  // per glyph: its bitmap in RAM
  size_t bytes = 0;
  bool dropped = false;
};

static std::mutex gFaceMu;
static std::vector<std::unique_ptr<FaceResident>> gFaceRes;  // guarded by gFaceMu
static size_t gFaceResBytes = 0, gPaceBytes = 0;
static constexpr size_t kFontReserve = 256 * 1024;  // memory left to everything else

static void freeResident(FaceResident &r, uint32_t glyphCount) {
  for (uint32_t i = 0; i < glyphCount; i++) free((void *)r.bm[i].load());
  free(r.tables);
  gFaceResBytes -= r.bytes;
}

void fontsDropSource(const ImageSource *src) {
  std::lock_guard<std::mutex> lock(gFaceMu);
  for (auto &r : gFaceRes)
    if (r->src == src) r->dropped = true;
}

void fontsCollectResident() {
  std::lock_guard<std::mutex> lock(gFaceMu);
  for (auto it = gFaceRes.begin(); it != gFaceRes.end();) {
    if ((*it)->dropped) {
      freeResident(**it, ((const FntHeader *)(*it)->tables)->glyphCount);
      it = gFaceRes.erase(it);
    } else {
      ++it;
    }
  }
}

size_t fontsResidentBytes() {
  std::lock_guard<std::mutex> lock(gFaceMu);
  return gFaceResBytes;
}

void FontFace::ensure() const {
  if (ready_.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lock(gFaceMu);
  if (ready_.load(std::memory_order_relaxed)) return;
  const uint8_t *data = file_.data;  // in RAM: in place
  bitmaps = data ? data + h->bitmapOff : nullptr;
  if (!data) {
    // slow storage: the tables in RAM (shared by every face of this atlas), glyphs by touch()
    for (auto &r : gFaceRes)
      if (r->src == file_.src.get() && r->off == file_.off && !r->dropped) res_ = r.get();
    size_t tl = h->bitmapOff;
    if (!res_) {
      auto r = std::make_unique<FaceResident>();
      r->src = file_.src.get();
      r->off = file_.off;
      r->tables = (uint8_t *)gAllocBulk(tl);
      if (r->tables && file_.read(0, r->tables, tl)) {
        r->bm.reset(new std::atomic<const uint8_t *>[h->glyphCount]);
        for (uint32_t i = 0; i < h->glyphCount; i++) r->bm[i].store(nullptr, std::memory_order_relaxed);
        r->bytes = tl;
        gFaceResBytes += tl;
        res_ = r.get();
        gFaceRes.push_back(std::move(r));
      } else {
        free(r->tables);
      }
    }
    if (res_) {
      data = res_->tables;
    } else {
      // no memory for even the tables: an empty face (text in it is not drawn) rather than flash reads
      hdr.glyphCount = hdr.kernCount = hdr.thaiCount = 0;
      static const uint8_t empty[16] = {};
      glyphs = (const FntGlyph *)empty;
      kerns = (const FntKern *)empty;
      thai = (const FntThai *)empty;
      for (int i = 0; i < 128; i++) asciiIdx[i] = thaiIdx[i] = -1;
      ready_.store(true, std::memory_order_release);
      return;
    }
  }
  glyphs = (const FntGlyph *)(data + h->glyphOff);
  kerns = (const FntKern *)(data + h->kernOff);
  thai = (const FntThai *)(data + h->thaiOff);
  for (int i = 0; i < 128; i++) asciiIdx[i] = thaiIdx[i] = -1;
  for (uint32_t i = 0; i < h->glyphCount; i++) {
    uint32_t cp = glyphs[i].cp;
    if (cp < 128) asciiIdx[cp] = (int16_t)i;
    else if (cp >= 0x0E00 && cp < 0x0E80) thaiIdx[cp - 0x0E00] = (int16_t)i;
  }
  ready_.store(true, std::memory_order_release);
}

void FontFace::release() const {
  std::lock_guard<std::mutex> lock(gFaceMu);
  if (!ready_.load(std::memory_order_relaxed) || file_.data) return;  // in RAM: used in place
  if (res_) res_->dropped = true;
  else file_.read(0, &hdr, sizeof hdr);  // loaded empty (memory was short): the real counts back
  res_ = nullptr;
  glyphs = nullptr;
  kerns = nullptr;
  thai = nullptr;
  bitmaps = nullptr;
  ready_.store(false, std::memory_order_release);
}

const uint8_t *FontFace::bitmap(const FntGlyph *g) const {
  if (res_) return res_->bm[g - glyphs].load(std::memory_order_acquire);  // null: not read
  return bitmaps ? bitmaps + g->off : nullptr;
}

void FontFace::touch(const FntGlyph *g) const {
  if (!res_ || !g || !g->w || !g->h) return;
  size_t i = g - glyphs;
  if (res_->bm[i].load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lock(gFaceMu);
  if (res_->bm[i].load(std::memory_order_relaxed)) return;
  // bitmaps are stored in glyph order: this one ends where the next begins
  size_t end = i + 1 < h->glyphCount ? glyphs[i + 1].off : file_.size - h->bitmapOff;
  size_t n = end - g->off;
  if (gLargeFree() < n + kFontReserve) return;
  auto *p = (uint8_t *)gAllocBulk(n);
  if (!p) return;
  if (!file_.read(h->bitmapOff + g->off, p, n)) {
    free(p);
    return;
  }
  if ((gPaceBytes += n) >= 4096) {  // many glyphs in a row: leave the bus to the scan-out now and then
    gPaceBytes = 0;
    gSlowPause();
  }
  res_->bytes += n;
  gFaceResBytes += n;
  res_->bm[i].store(p, std::memory_order_release);
}

const FntGlyph *FontFace::glyph(uint32_t cp) const {
  if (cp < 128) return asciiIdx[cp] >= 0 ? &glyphs[asciiIdx[cp]] : nullptr;
  if (cp >= 0x0E00 && cp < 0x0E80) return thaiIdx[cp - 0x0E00] >= 0 ? &glyphs[thaiIdx[cp - 0x0E00]] : nullptr;
  uint32_t lo = 0, hi = h->glyphCount;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (glyphs[mid].cp < cp) lo = mid + 1;
    else hi = mid;
  }
  return lo < h->glyphCount && glyphs[lo].cp == cp ? &glyphs[lo] : nullptr;
}

float FontFace::kern(uint32_t a, uint32_t b) const {
  if (!h->kernCount || a > 0xFFFF || b > 0xFFFF) return 0;
  uint32_t key = (a << 16) | b;
  uint32_t lo = 0, hi = h->kernCount;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (kerns[mid].pair < key) lo = mid + 1;
    else hi = mid;
  }
  return lo < h->kernCount && kerns[lo].pair == key ? kerns[lo].adj64 / 64.f : 0;
}

const FntThai *FontFace::thaiPlace(uint32_t base, uint32_t key) const {
  for (int pass = 0; pass < 2; pass++) {
    uint32_t b = pass == 0 ? base : 0x0E01;
    uint32_t want = (b << 16) | (key & 0xFFFF);
    uint32_t lo = 0, hi = h->thaiCount;
    while (lo < hi) {
      uint32_t mid = (lo + hi) / 2;
      uint32_t k = ((uint32_t)thai[mid].base << 16) | thai[mid].mark;
      if (k < want) lo = mid + 1;
      else hi = mid;
    }
    if (lo < h->thaiCount && thai[lo].base == b && thai[lo].mark == (key & 0xFFFF)) return &thai[lo];
    if (key == TK_BASE_VARIANT) return nullptr;  // no generic base variant
  }
  return nullptr;
}

// ============================================================ manager

// generations are unique across managers (a new manager may reuse an old one's address)
static uint32_t nextFontGen() {
  static uint32_t g = 1;
  return ++g;
}

void FontManager::clear() {
  gen_ = nextFontGen();
  for (auto &f : faces_) f->release();  // what they read into RAM goes too (not to the next faces)
  fontsCollectResident();
  faces_.clear();
  faceFam_.clear();
  famNames_.clear();
  aliases_.clear();
  defaultFam_ = -1;
}

int FontManager::familyId(const std::string &lower) const {
  for (size_t i = 0; i < famNames_.size(); i++)
    if (famNames_[i] == lower) return (int)i;
  return -1;
}

bool FontManager::add(const ImageSlice &file) {
  auto f = std::make_unique<FontFace>();
  if (!f->load(file)) return false;
  gen_ = nextFontGen();
  std::string lower = toLower(f->family);
  int id = familyId(lower);
  if (id < 0) {
    id = (int)famNames_.size();
    famNames_.push_back(lower);
  }
  if (defaultFam_ < 0) defaultFam_ = id;
  faces_.push_back(std::move(f));
  faceFam_.push_back(id);
  // aliases registered before this family existed
  for (auto &al : aliases_)
    if (al.target == lower) al.id = id;
  return true;
}

void FontManager::releaseResident() {
  gen_ = nextFontGen();  // remembered FontRefs (LayoutEngine::fontFor) go back through get()
  for (auto &f : faces_) f->release();
  fontsCollectResident();
}

void FontManager::alias(const std::string &name, const std::string &target) {
  gen_ = nextFontGen();
  Alias al{toLower(name), toLower(target), -1};
  al.id = familyId(al.target);
  if (al.name == "sans-serif" && al.id >= 0) defaultFam_ = al.id;
  aliases_.push_back(al);
}

// ASCII case-insensitive equality against an already lower-case name (no allocation).
static bool ieqLower(const std::string &s, const std::string &lower) {
  if (s.size() != lower.size()) return false;
  for (size_t i = 0; i < s.size(); i++)
    if (lowerAscii(s[i]) != lower[i]) return false;
  return true;
}

FontRef FontManager::get(const std::vector<std::string> *families, int weight, bool italic, float size) const {
  if (faces_.empty() || size <= 0) {
    if (faces_.empty()) return FontRef{nullptr, 1};
    faces_[0]->ensure();
    return FontRef{faces_[0].get(), 1};
  }
  int fam = -1;
  if (families) {
    for (auto &n : *families) {
      for (auto &al : aliases_)
        if (al.id >= 0 && ieqLower(n, al.name)) {
          fam = al.id;
          break;
        }
      if (fam >= 0) break;
      for (size_t i = 0; i < famNames_.size(); i++)
        if (ieqLower(n, famNames_[i])) {
          fam = (int)i;
          break;
        }
      if (fam >= 0) break;
    }
  }
  if (fam < 0) fam = defaultFam_;
  const size_t nf = faces_.size();
  // weight: closest, preferring the CSS search direction
  int bestW = -1;
  float bestCost = 1e9f;
  for (size_t i = 0; i < nf; i++) {
    if (faceFam_[i] != fam) continue;
    const FontFace &face = *faces_[i];
    int w = face.h->weight;
    float cost = (float)std::abs(w - weight);
    if (weight <= 500 ? w > weight : w < weight) cost += 0.5f;  // wrong direction
    if ((face.h->italic != 0) != italic) cost += 1000;
    if (cost < bestCost) {
      bestCost = cost;
      bestW = w;
    }
  }
  // size: smallest atlas >= requested (downscaling looks better), else the largest
  const FontFace *best = nullptr;
  const FontFace *largest = nullptr;
  for (size_t i = 0; i < nf; i++) {
    const FontFace *face = faces_[i].get();
    if (faceFam_[i] != fam || face->h->weight != bestW) continue;
    if ((face->h->italic != 0) != italic && bestCost < 1000) continue;
    float s = face->h->size;
    if (std::fabs(s - size) < 0.01f) {
      face->ensure();
      return FontRef{face, 1};
    }
    if (s >= size && (!best || s < best->h->size)) best = face;
    if (!largest || s > largest->h->size) largest = face;
  }
  // Prefer a slightly smaller atlas over a much larger one (sharper small text)
  if (best && largest) {
    const FontFace *below = nullptr;
    for (size_t i = 0; i < nf; i++) {
      const FontFace *face = faces_[i].get();
      if (faceFam_[i] != fam || face->h->weight != bestW) continue;
      float s = face->h->size;
      if (s < size && (!below || s > below->h->size)) below = face;
    }
    if (below && (size - below->h->size) < (best->h->size - size) * 0.5f) best = below;
  }
  const FontFace *f = best ? best : largest;
  if (!f) f = faces_[0].get();
  f->ensure();
  return FontRef{f, size / f->h->size};
}

// ============================================================ shaping

bool isThai(uint32_t cp) { return cp >= 0x0E00 && cp <= 0x0E7F; }

bool isThaiMark(uint32_t cp) {
  return cp == 0x0E31 || (cp >= 0x0E34 && cp <= 0x0E3A) || (cp >= 0x0E47 && cp <= 0x0E4E);
}

static bool isThaiUpperVowel(uint32_t cp) { return cp == 0x0E31 || (cp >= 0x0E34 && cp <= 0x0E37) || cp == 0x0E47 || cp == 0x0E4D || cp == 0x0E4E; }
static bool isThaiTone(uint32_t cp) { return (cp >= 0x0E48 && cp <= 0x0E4C); }
static bool isThaiLower(uint32_t cp) { return cp >= 0x0E38 && cp <= 0x0E3A; }

static bool isCJK(uint32_t cp) {
  return (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

static float shapeRun(const FontRef &f, const char *s, size_t n, const ShapeOpts &o, std::vector<ShapedGlyph> &out,
                      uint32_t srcBase);

float shapeText(const FontRef &f, const char *s, size_t n, const ShapeOpts &o, std::vector<ShapedGlyph> &out,
                uint32_t srcBase) {
  size_t first = out.size();
  float adv = shapeRun(f, s, n, o, out, srcBase);
  if (f.face)
    for (size_t i = first; i < out.size(); i++) f.face->touch(out[i].g);  // drawn later: in RAM
  return adv;
}

static float shapeRun(const FontRef &f, const char *s, size_t n, const ShapeOpts &o, std::vector<ShapedGlyph> &out,
                      uint32_t srcBase) {
  if (!f.face) return 0;
  const FontFace &face = *f.face;
  const float sc = f.scale;
  float pen = 0;
  uint32_t prevCp = 0;
  int clusterBaseIdx = -1;       // index in out of the cluster's base glyph
  uint32_t clusterBase = 0;      // base code point
  bool clusterHasUpper = false;
  int clusterToneIdx = -1;
  uint32_t clusterToneCp = 0;
  float markPen = 0;  // pen after the cluster's base (unscaled)
  float tabAdv = 0;
  if (o.tabular) {
    for (char c = '0'; c <= '9'; c++) {
      const FntGlyph *g = face.glyph((uint32_t)c);
      if (g) tabAdv = std::max(tabAdv, g->adv64 / 64.f);
    }
  }
  const FntGlyph *spaceG = face.glyph(' ');
  float spaceAdv = spaceG ? spaceG->adv64 / 64.f : face.h->size * 0.25f;

  auto placeAt = [&](ShapedGlyph &sg, uint32_t cp, uint32_t key) {
    const FntGlyph *g = face.glyph(cp);
    float dx = 0, dy = 0;
    if (const FntThai *t = face.thaiPlace(clusterBase ? clusterBase : 0x0E01, key)) {
      if (t->glyph) {
        const FntGlyph *vg = face.glyph(t->glyph);
        if (vg) g = vg;
      }
      dx = t->dx64 / 64.f;
      dy = t->dy64 / 64.f;
    }
    sg.g = g;
    sg.x = (markPen + dx) * sc;
    sg.dy = dy * sc;
  };
  auto placeMark = [&](uint32_t cp, uint32_t key, uint32_t src) {
    ShapedGlyph sg;
    sg.adv = 0;
    sg.src = src;
    placeAt(sg, cp, key);
    sg.flags = SG_THAI | (sg.g && (sg.g->flags & GF_FALLBACK) ? SG_FALLBACK : 0);
    out.push_back(sg);
    return (int)out.size() - 1;
  };

  size_t i = 0;
  while (i < n) {
    size_t at = i;
    uint32_t cp = utf8Next(s, n, i);
    uint32_t src = srcBase + (uint32_t)at;
    // ---- Thai combining marks (positions measured with HarfBuzz by the compiler)
    if (isThaiMark(cp) && clusterBaseIdx >= 0) {
      if (isThaiUpperVowel(cp)) {
        placeMark(cp, cp, src);
        clusterHasUpper = true;
        if (clusterToneIdx >= 0) placeAt(out[clusterToneIdx], clusterToneCp, clusterToneCp | TK_AFTER_UPPER);
      } else if (isThaiTone(cp)) {
        clusterToneCp = cp;
        clusterToneIdx = placeMark(cp, clusterHasUpper ? (cp | TK_AFTER_UPPER) : cp, src);
      } else if (isThaiLower(cp)) {
        placeMark(cp, cp, src);
        // ญ / ฐ lose their tail above a lower vowel
        if (const FntThai *t = face.thaiPlace(clusterBase, TK_BASE_VARIANT)) {
          const FntGlyph *vg = face.glyph(t->glyph);
          if (vg && clusterBaseIdx < (int)out.size()) out[clusterBaseIdx].g = vg;
        }
      }
      prevCp = cp;
      continue;
    }
    if (cp == 0x0E33 && clusterBaseIdx >= 0) {  // SARA AM = NIKHAHIT (mark) + SARA AA
      placeMark(0x0E4D, 0x0E4D, src);
      clusterHasUpper = true;
      if (clusterToneIdx >= 0) placeAt(out[clusterToneIdx], clusterToneCp, clusterToneCp | TK_AFTER_AM);
      cp = 0x0E32;
    }

    ShapedGlyph sg;
    sg.src = src;
    sg.dy = 0;
    sg.flags = SG_CLUSTER_START;
    if (cp == '\n') {
      sg.g = nullptr;
      sg.x = pen;
      sg.adv = 0;
      sg.flags |= SG_HARD_BREAK | SG_BREAK_AFTER;
      out.push_back(sg);
      prevCp = 0;
      clusterBaseIdx = -1;
      clusterBase = 0;
      continue;
    }
    if (cp == 0x200B || cp == 0xAD || cp == 0x200C || cp == 0x200D || cp == 0xFEFF) {
      if (!out.empty() && cp != 0x200D && cp != 0xFEFF) out.back().flags |= SG_BREAK_AFTER | (cp == 0xAD ? SG_SOFT_HYPHEN : 0);
      continue;
    }
    if (cp == '\t') cp = ' ';
    bool space = cp == ' ' || cp == 0xA0 || cp == 0x3000 || (cp >= 0x2000 && cp <= 0x200A);
    const FntGlyph *g = face.glyph(cp == 0xA0 ? ' ' : cp);
    if (!g && !space) g = face.glyph(0xFFFD);
    float adv;
    if (space) {
      adv = (g ? g->adv64 / 64.f : spaceAdv) + o.wordSpacing / sc;
      if (cp == 0x3000) adv = face.h->size;
      sg.g = nullptr;
    } else if (g) {
      adv = g->adv64 / 64.f;
      if (o.tabular && cp >= '0' && cp <= '9') {
        float extra = tabAdv - adv;
        pen += extra / 2;
        adv = tabAdv - extra / 2;
      }
      sg.g = g;
    } else {
      adv = face.h->size * 0.5f;
      sg.g = nullptr;
    }
    // kerning with the previous spacing glyph (folded into its advance, like HarfBuzz)
    if (prevCp && g && !space) {
      float k = face.kern(prevCp, cp);
      if (k != 0) {
        pen += k;
        if (!out.empty()) out.back().adv += k * sc;
      }
    }
    sg.x = pen * sc;
    float advPx = adv * sc + o.letterSpacing;
    sg.adv = advPx;
    if (space) {
      sg.flags |= SG_SPACE;
      if (cp != 0xA0) sg.flags |= SG_BREAK_AFTER;
    }
    if (g && (g->flags & GF_FALLBACK)) sg.flags |= SG_FALLBACK;
    if (isThai(cp)) sg.flags |= SG_THAI;
    if (isCJK(cp)) {
      sg.flags |= SG_BREAK_AFTER;
      if (!out.empty()) out.back().flags |= SG_BREAK_AFTER;
    }
    if ((cp == '-' || cp == 0x2013 || cp == 0x2014 || cp == '/') && prevCp && !(out.empty()) && !(out.back().flags & SG_SPACE))
      sg.flags |= SG_BREAK_AFTER;
    out.push_back(sg);
    pen += adv + o.letterSpacing / sc;
    markPen = pen - o.letterSpacing / sc;
    // marks of this cluster are positioned relative to the pen after the base
    if (isThai(cp) && !space) {
      clusterBaseIdx = (int)out.size() - 1;
      clusterBase = cp;
      clusterHasUpper = false;
      clusterToneIdx = -1;
    } else {
      clusterBaseIdx = (int)out.size() - 1;
      clusterBase = 0;
      clusterHasUpper = false;
      clusterToneIdx = -1;
    }
    prevCp = space ? 0 : cp;
  }
  return pen * sc;
}

float measureText(const FontRef &f, const std::string &s, const ShapeOpts &o) {
  std::vector<ShapedGlyph> g;
  return shapeRun(f, s.data(), s.size(), o, g, 0);  // nothing is drawn: no bitmaps needed
}

}  // namespace rt
