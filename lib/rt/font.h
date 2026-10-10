#pragma once
// Pre-rasterized fonts (glyph atlases made by the host compiler) and text shaping:
// kerning, letter-spacing and Thai mark placement.

#include <atomic>

#include "rt_base.h"

namespace rt {

#pragma pack(push, 1)
struct FntHeader {
  char magic[4];  // "FNT1"
  uint16_t version, flags;
  float size, ascent, descent, lineGap, xHeight, capHeight, underlinePos, underlineThick;
  float fbAscent, fbDescent;  // metrics of the fallback (Thai) face merged into this atlas
  uint32_t glyphCount, kernCount, thaiCount;
  uint32_t glyphOff, kernOff, thaiOff, bitmapOff;
  uint16_t weight;
  uint8_t italic, reserved;
  char family[32];
};
struct FntGlyph {
  uint32_t cp;      // code point; >= 0x110000 for contextual variants
  int16_t adv64;    // advance, 1/64 px
  int16_t bx, by;   // bitmap left / top relative to pen and baseline (top = baseline - by)
  uint16_t w, h;
  uint16_t flags;   // GF_*
  uint32_t off;     // offset of A8 bitmap in the bitmap block
};
struct FntKern {
  uint32_t pair;  // (a << 16) | b, BMP only
  int16_t adj64;
  uint16_t pad;
};
struct FntThai {     // mark placement after a Thai base consonant (from HarfBuzz)
  uint16_t base;     // base code point
  uint16_t mark;     // mark code point | TK_* context bits (0xFFFF = base variant)
  uint32_t glyph;    // glyph to draw (variant code point; 0 = the mark itself)
  int16_t dx64, dy64;  // offset from the pen after the base, 1/64 px, y up
};
#pragma pack(pop)

enum GlyphFlags : uint16_t { GF_FALLBACK = 1, GF_MARK = 2, GF_COLOR = 4 };
enum ThaiKey : uint16_t {
  TK_AFTER_UPPER = 0x8000,  // tone mark stacked above an upper vowel
  TK_AFTER_AM = 0x4000,     // tone mark followed by SARA AM (nikhahit below the tone)
  TK_BASE_VARIANT = 0xFFFF, // base glyph used above a lower vowel (ญ ฐ without tail)
};

struct FaceResident;

struct FontFace {
  // The header is read at load. An atlas in RAM is used in place; one in slow storage (flash)
  // gets its tables read into RAM by ensure() (FontManager::get), and the bitmap of each glyph
  // shapeText produces is read by touch(): drawing never reads flash, and only the glyphs pages
  // show take RAM. bitmap() is null for a glyph that could not be read (memory short): skip it.
  mutable FntHeader hdr;  // (ensure() empties the counts of a face it cannot load)
  const FntHeader *h = &hdr;
  mutable const FntGlyph *glyphs = nullptr;
  mutable const FntKern *kerns = nullptr;
  mutable const FntThai *thai = nullptr;
  mutable const uint8_t *bitmaps = nullptr;
  std::string family;
  // direct lookup for the common ranges
  mutable int16_t asciiIdx[128];
  mutable int16_t thaiIdx[128];  // U+0E00..0E7F
  bool load(const ImageSlice &file);
  void ensure() const;  // tables usable (FontManager::get calls it)
  const FntGlyph *glyph(uint32_t cp) const;
  float kern(uint32_t a, uint32_t b) const;
  const FntThai *thaiPlace(uint32_t base, uint32_t key) const;  // falls back to ก (U+0E01)
  const uint8_t *bitmap(const FntGlyph *g) const;
  void touch(const FntGlyph *g) const;  // its bitmap in RAM from now on (when possible)
  // Unloaded again: the tables and glyphs read from slow storage leave RAM. Nothing may use
  // text shaped with this face any more (FontManager::releaseResident).
  void release() const;
  float size() const { return h->size; }

 private:
  ImageSlice file_;
  mutable FaceResident *res_ = nullptr;
  mutable std::atomic<bool> ready_{false};
};

// RAM copies of atlases in slow storage: the archive `src` goes away (its copies are freed by
// fontsCollectResident, once no document can draw with them).
void fontsDropSource(const ImageSource *src);
void fontsCollectResident();
size_t fontsResidentBytes();

// A face at a requested size: scale != 1 when the atlas size differs from the CSS size.
struct FontRef {
  const FontFace *face = nullptr;
  float scale = 1;
  float size() const { return face ? face->h->size * scale : 0; }
  float ascent() const { return face ? face->h->ascent * scale : 0; }
  float descent() const { return face ? face->h->descent * scale : 0; }
  float lineGap() const { return face ? face->h->lineGap * scale : 0; }
  float fbAscent() const { return face ? face->h->fbAscent * scale : 0; }
  float fbDescent() const { return face ? face->h->fbDescent * scale : 0; }
  float xHeight() const { return face ? face->h->xHeight * scale : 0; }
  bool operator==(const FontRef &o) const { return face == o.face && scale == o.scale; }
};

class FontManager {
 public:
  void clear();
  // Register an atlas (in RAM it must stay valid while the manager lives).
  bool add(const ImageSlice &file);
  // Family aliases (e.g. "sans-serif" -> "Inter")
  void alias(const std::string &name, const std::string &target);
  FontRef get(const std::vector<std::string> *families, int weight, bool italic, float size) const;
  size_t faceCount() const { return faces_.size(); }
  uint32_t generation() const { return gen_; }  // changes whenever faces/aliases change
  // Every face unloaded: between documents, so a page holds the fonts it draws with and not
  // those of the pages before it (they come back from flash when used again).
  void releaseResident();

 private:
  int familyId(const std::string &lower) const;
  struct Alias {
    std::string name, target;  // lower case
    int id;                    // family id of target, -1 until such a face is added
  };
  std::vector<std::unique_ptr<FontFace>> faces_;
  std::vector<int> faceFam_;            // family id per face
  std::vector<std::string> famNames_;   // lower-case family names by id
  std::vector<Alias> aliases_;
  int defaultFam_ = -1;
  uint32_t gen_ = 0;
};

// ---- shaping ----
struct ShapedGlyph {
  const FntGlyph *g;  // nullptr for spaces with no glyph
  float x;            // pen x relative to the run start (before letter-spacing of this glyph)
  float dy;           // vertical offset (Thai marks), px, positive = up
  float adv;          // advance contributed (includes letter/word spacing)
  uint32_t src;       // byte offset in the source text
  uint8_t flags;      // SG_*
};
enum ShapedFlags : uint8_t {
  SG_BREAK_AFTER = 1,    // a line may break after this glyph
  SG_SPACE = 2,          // collapsible/visible space (hangs at line end)
  SG_CLUSTER_START = 4,  // grapheme cluster boundary before this glyph
  SG_HARD_BREAK = 8,     // forced line break after this glyph (pre / <br>)
  SG_FALLBACK = 16,      // glyph from the fallback face
  SG_THAI = 32,          // part of a Thai run (break anywhere between clusters if needed)
  SG_SOFT_HYPHEN = 64,
};

struct ShapeOpts {
  float letterSpacing = 0;
  float wordSpacing = 0;
  bool tabular = false;
};

// Shape UTF-8 text (already white-space processed). Appends to out; returns total advance.
float shapeText(const FontRef &f, const char *s, size_t n, const ShapeOpts &o, std::vector<ShapedGlyph> &out,
                uint32_t srcBase = 0);
// Measure only.
float measureText(const FontRef &f, const std::string &s, const ShapeOpts &o = ShapeOpts());

bool isThaiMark(uint32_t cp);
bool isThai(uint32_t cp);

}  // namespace rt
