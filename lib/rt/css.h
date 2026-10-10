#pragma once
// CSS: tokenizer, value types, declaration/selector/stylesheet parsing.

#include "dom.h"

namespace rt {

// ---------------- tokens ----------------
enum TokType : uint8_t {
  TK_EOF, TK_IDENT, TK_FUNC, TK_AT, TK_HASH, TK_STRING, TK_NUM, TK_PCT, TK_DIM, TK_WS, TK_COLON,
  TK_SEMI, TK_COMMA, TK_LBRACK, TK_RBRACK, TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_DELIM, TK_URL
};
struct Tok {
  TokType t = TK_EOF;
  std::string s;  // ident / function name / string / unit / url / hash
  float n = 0;
  char c = 0;     // delim
  bool isInt = false;
};
void cssTokenize(const char *s, size_t n, std::vector<Tok> &out);
std::string tokensToString(const std::vector<Tok> &t, size_t a, size_t b);

// ---------------- values ----------------
enum LenKind : uint8_t { LK_LEN, LK_AUTO, LK_NONE, LK_MIN_CONTENT, LK_MAX_CONTENT, LK_FIT_CONTENT, LK_NORMAL, LK_CONTENT };

struct CalcNode;
// Specified length: linear combination of units (calc() of + and - folds into this).
struct LenSpec {
  float px = 0, pct = 0, em = 0, rem = 0, vw = 0, vh = 0, ch = 0;
  LenKind kind = LK_LEN;
  std::shared_ptr<CalcNode> calc;  // min()/max()/clamp() and other non-linear calc
  static LenSpec Px(float v) { LenSpec l; l.px = v; return l; }
  static LenSpec Pct(float v) { LenSpec l; l.pct = v; return l; }
  static LenSpec Kind(LenKind k) { LenSpec l; l.kind = k; return l; }
  bool isZero() const { return kind == LK_LEN && !calc && px == 0 && pct == 0 && em == 0 && rem == 0 && vw == 0 && vh == 0 && ch == 0; }
};

// Computed length: em/rem/vw/vh resolved; percentage kept until layout.
struct Len {
  float px = 0, pct = 0;
  LenKind kind = LK_LEN;
  std::shared_ptr<CalcNode> calc;  // resolved-unit calc tree (needs the % reference at layout)
  static Len Px(float v) { Len l; l.px = v; return l; }
  static Len Kind(LenKind k) { Len l; l.kind = k; return l; }
  bool isAuto() const { return kind == LK_AUTO; }
  bool isLen() const { return kind == LK_LEN; }
  bool hasPct() const { return kind == LK_LEN && (pct != 0 || calc); }
  // Resolve against a reference size (for %). Returns fallback when not a length.
  float resolve(float ref, float fallback = 0) const {
    if (kind != LK_LEN) return fallback;
    return calc ? evalCalc(ref) : px + pct * 0.01f * ref;
  }
  float evalCalc(float ref) const;
  bool operator==(const Len &o) const { return px == o.px && pct == o.pct && kind == o.kind && calc == o.calc; }
  bool operator!=(const Len &o) const { return !(*this == o); }
};

struct ResolveCtx {  // what em/rem/vw/vh mean while computing a style
  float em = 16, rem = 16, vw = 800, vh = 480, ch = 8;
};

struct CalcNode {
  enum Op : uint8_t { LEAF, ADD, SUB, MUL, DIV, MIN, MAX, CLAMP } op = LEAF;
  LenSpec leaf;   // for LEAF
  float num = 0;  // plain number leaf (for MUL/DIV) when isNum
  bool isNum = false;
  std::vector<std::shared_ptr<CalcNode>> kids;
};
Len computeLen(const LenSpec &s, const ResolveCtx &c);

struct ColorSpec {
  Color c;
  bool current = false;  // currentColor
};

struct GradStop {
  ColorSpec color;
  float pos = -1;     // fraction 0..1 (or px when isPx); -1 = auto
  bool isPx = false;
  bool hint = false;  // colour hint (transition midpoint) - treated as a stop
};
struct Gradient {
  enum Kind : uint8_t { LINEAR, RADIAL, CONIC } kind = LINEAR;
  bool repeating = false;
  float angleDeg = 180;  // linear: CSS angle (180 = to bottom); conic: start angle
  bool toCorner = false;
  int cornerX = 0, cornerY = 0;  // -1/1 for "to top left" etc.
  bool circle = false;            // radial shape
  LenSpec cx = LenSpec::Pct(50), cy = LenSpec::Pct(50);
  std::vector<GradStop> stops;
};

struct BgLayer {
  std::shared_ptr<Gradient> grad;
  std::string url;  // image path
};

struct Shadow {
  LenSpec x, y, blur, spread;
  ColorSpec color{{0, 0, 0, 64}, false};
  bool inset = false;
  bool hasColor = false;
};

struct TransformFn {
  enum Kind : uint8_t { TRANSLATE, SCALE, ROTATE, SKEW, MATRIX } kind;
  LenSpec a, b;       // translate x/y
  float v[6] = {0, 0, 0, 0, 0, 0};  // scale x,y / rotate rad / skew rad x,y / matrix
};

struct TimingFn {
  enum Kind : uint8_t { BEZIER, STEPS, LINEAR } kind = BEZIER;
  float x1 = 0.25f, y1 = 0.1f, x2 = 0.25f, y2 = 1.0f;  // "ease"
  int steps = 1;
  bool jumpStart = false;
  float eval(float t) const;
};

struct TransitionSpec {
  Atom prop = 0;  // 0 = all
  float duration = 0, delay = 0;  // seconds
  TimingFn timing;
};

struct AnimationSpec {
  Atom name = 0;
  float duration = 0, delay = 0;
  TimingFn timing;
  float iterations = 1;  // INFINITY for infinite
  uint8_t direction = 0;  // 0 normal 1 reverse 2 alternate 3 alternate-reverse
  uint8_t fill = 0;       // 0 none 1 forwards 2 backwards 3 both
  bool paused = false;
};

struct TrackSize {
  enum Kind : uint8_t { FIXED, FR, AUTO, MIN_CONTENT, MAX_CONTENT, MINMAX } kind = AUTO;
  LenSpec len;
  float fr = 0;
  std::shared_ptr<TrackSize> mn, mx;  // minmax
};
struct TrackList {
  std::vector<TrackSize> tracks;
  int autoRepeat = 0;  // 0 none, 1 auto-fill, 2 auto-fit (tracks = the repeated pattern)
  std::vector<TrackSize> before, after;  // fixed tracks around an auto repeat
};

struct GridLine {
  int line = 0;  // 0 = auto
  int span = 0;  // >0 means "span n"
};

struct ContentPart {
  enum Kind : uint8_t { STR, ATTR, OPEN_QUOTE, CLOSE_QUOTE, COUNTER } kind = STR;
  std::string s;
};

// A parsed property value.
enum ValType : uint8_t {
  VT_NONE, VT_INHERIT, VT_INITIAL, VT_UNSET, VT_KW, VT_LEN, VT_NUM, VT_COLOR, VT_STR, VT_RAW, VT_OBJ, VT_LEN2
};
struct ValueExt {
  LenSpec len2;                 // the second length of a pair
  std::string str;              // raw text (custom properties, var()), keyword text of inline styles
  std::shared_ptr<void> obj;    // typed by property
};
struct Value {
  ValType type = VT_NONE;
  int32_t kw = 0;   // keyword enum / integer value
  float num = 0;
  LenSpec len;
  ColorSpec color;
  // Few values have the rest: it lives out of line (a sheet holds thousands of Values), shared by
  // copies until one of them writes through ext()
  const LenSpec &len2() const { return x().len2; }
  const std::string &str() const { return x().str; }
  const std::shared_ptr<void> &obj() const { return x().obj; }
  ValueExt &ext();

 private:
  std::shared_ptr<ValueExt> x_;
  const ValueExt &x() const { return x_ ? *x_ : kNoExt; }
  static const ValueExt kNoExt;
};

// ---------------- properties ----------------
#define RT_PROPS(X)                                                                                    \
  X(display) X(position) X(top) X(right) X(bottom) X(left) X(z_index) X(float_) X(clear)                   \
  X(width) X(height) X(min_width) X(min_height) X(max_width) X(max_height)                                 \
  X(margin_top) X(margin_right) X(margin_bottom) X(margin_left)                                           \
  X(padding_top) X(padding_right) X(padding_bottom) X(padding_left)                                       \
  X(border_top_width) X(border_right_width) X(border_bottom_width) X(border_left_width)                   \
  X(border_top_style) X(border_right_style) X(border_bottom_style) X(border_left_style)                   \
  X(border_top_color) X(border_right_color) X(border_bottom_color) X(border_left_color)                   \
  X(border_top_left_radius) X(border_top_right_radius) X(border_bottom_right_radius)                      \
  X(border_bottom_left_radius) X(box_sizing) X(overflow_x) X(overflow_y) X(visibility) X(opacity)         \
  X(background_color) X(background_image) X(background_size) X(background_position)                      \
  X(background_repeat) X(background_clip) X(color) X(font_family) X(font_size) X(font_weight)              \
  X(font_style) X(line_height) X(letter_spacing) X(word_spacing) X(text_align)                             \
  X(text_decoration_line) X(text_decoration_color) X(text_transform) X(text_overflow) X(white_space)       \
  X(word_break) X(overflow_wrap) X(vertical_align) X(text_indent) X(text_shadow) X(line_clamp)            \
  X(box_shadow) X(outline_width) X(outline_style) X(outline_color) X(outline_offset) X(transform)         \
  X(transform_origin) X(filter) X(flex_direction) X(flex_wrap) X(justify_content) X(align_items)          \
  X(align_self) X(align_content) X(justify_items) X(justify_self) X(flex_grow) X(flex_shrink)             \
  X(flex_basis) X(order) X(row_gap) X(column_gap) X(grid_template_columns) X(grid_template_rows)         \
  X(grid_auto_rows) X(grid_auto_columns) X(grid_auto_flow) X(grid_column_start) X(grid_column_end)       \
  X(grid_row_start) X(grid_row_end) X(transition_property) X(transition_duration)                        \
  X(transition_timing_function) X(transition_delay) X(animation_name) X(animation_duration)              \
  X(animation_timing_function) X(animation_delay) X(animation_iteration_count) X(animation_direction)    \
  X(animation_fill_mode) X(animation_play_state) X(content) X(list_style_type) X(list_style_position)    \
  X(border_collapse) X(border_spacing) X(table_layout) X(pointer_events) X(accent_color) X(caret_color)  \
  X(appearance) X(object_fit) X(object_position) X(aspect_ratio) X(image_rendering) X(text_align_last)   \
  X(scroll_snap_type) X(scroll_snap_align) X(user_select) X(cursor) X(font_variant_numeric) X(fill) X(stroke) \
  X(stroke_width) X(stroke_linecap) X(stroke_linejoin)

enum Prop : uint16_t {
#define RT_PROP_ENUM(n) P_##n,
  RT_PROPS(RT_PROP_ENUM)
#undef RT_PROP_ENUM
  P__COUNT,
  P_CUSTOM = 0xFFFE,  // --name
};

const char *propName(Prop p);           // CSS name ("margin-top")
int propByName(const std::string &name);  // -1 unknown
bool propInherited(Prop p);

struct Decl {
  uint16_t prop;
  bool important = false;
  Atom customName = 0;  // for P_CUSTOM
  Value v;
};

struct DeclBlock {
  std::vector<Decl> decls;
  void set(Decl d);  // replace same property
  const Decl *find(uint16_t prop, Atom custom = 0) const;
  bool remove(uint16_t prop, Atom custom = 0);
};

// Parse "a: b; c: d" (style attribute / rule body). Shorthands are expanded. Keyword text is
// kept for reading back (element.style.x) unless `sheet`.
void parseDeclarations(const char *s, size_t n, DeclBlock &out, bool sheet = false);
// Parse a single property value (for element.style.foo = "..."). Returns false if invalid.
bool parsePropertyValue(const std::string &name, const std::string &value, std::vector<Decl> &out, bool important = false);
// Parse a value for a longhand from tokens (used after var() substitution).
bool parseLonghand(Prop p, const std::vector<Tok> &toks, Value &out);
std::string serializeValue(Prop p, const Value &v);

bool parseColorStr(const std::string &s, Color &out);
bool parseTimingFn(const std::vector<Tok> &t, size_t &i, TimingFn &out);

// ---------------- selectors ----------------
enum SelMatch : uint8_t {
  SM_TAG, SM_UNIVERSAL, SM_ID, SM_CLASS, SM_ATTR_EXISTS, SM_ATTR_EQ, SM_ATTR_INCLUDES, SM_ATTR_DASH,
  SM_ATTR_PREFIX, SM_ATTR_SUFFIX, SM_ATTR_SUBSTR, SM_PSEUDO, SM_NOT, SM_IS, SM_NTH_CHILD,
  SM_NTH_LAST_CHILD, SM_NTH_OF_TYPE, SM_NTH_LAST_OF_TYPE, SM_HAS
};
enum PseudoClass : uint8_t {
  PC_ACTIVE, PC_HOVER, PC_FOCUS, PC_FOCUS_WITHIN, PC_FOCUS_VISIBLE, PC_CHECKED, PC_DISABLED, PC_ENABLED,
  PC_FIRST_CHILD, PC_LAST_CHILD, PC_ONLY_CHILD, PC_FIRST_OF_TYPE, PC_LAST_OF_TYPE, PC_ONLY_OF_TYPE,
  PC_ROOT, PC_EMPTY, PC_TARGET, PC_PLACEHOLDER_SHOWN, PC_INVALID, PC_VALID, PC_REQUIRED, PC_OPTIONAL,
  PC_READ_ONLY, PC_READ_WRITE, PC_LINK, PC_VISITED, PC_OPEN, PC_INDETERMINATE, PC_DEFAULT, PC_SCOPE,
  PC_NEVER
};
enum PseudoElem : uint8_t { PE_NONE, PE_BEFORE, PE_AFTER, PE_PLACEHOLDER, PE_SELECTION, PE_MARKER, PE_BACKDROP, PE_THUMB, PE_TRACK, PE_UNKNOWN };

struct SelectorList;
struct SimpleSel {
  SelMatch m;
  Atom a = 0;       // tag / id / class / attribute name
  std::string val;  // attribute value
  bool icase = false;
  uint8_t pc = 0;   // pseudo-class
  int nthA = 0, nthB = 0;
  std::shared_ptr<SelectorList> sub;  // :not() / :is() / :where() / :has() / nth "of S"
};
enum Combinator : uint8_t { CB_NONE, CB_DESC, CB_CHILD, CB_ADJ, CB_SIB };
struct Compound {
  std::vector<SimpleSel> simple;
  Combinator comb = CB_NONE;  // relation to the compound on the LEFT (previous in the vector)
};
struct Selector {
  std::vector<Compound> parts;  // left to right
  uint32_t specificity = 0;     // a<<20 | b<<10 | c
  PseudoElem pseudo = PE_NONE;
};
struct SelectorList {
  std::vector<Selector> sels;
};
bool parseSelectorList(const std::string &s, SelectorList &out);
bool matchSelector(const Selector &s, Element *e, Element *scope = nullptr);
bool matchesAny(const SelectorList &l, Element *e, Element *scope = nullptr);

// ---------------- stylesheets ----------------
struct StyleRule {
  Selector sel;  // one selector per rule entry (lists are split)
  std::shared_ptr<DeclBlock> decls;
  uint32_t order = 0;  // source order
  uint8_t layer = 0;   // 0 = UA, 1 = author
};
struct Keyframes {
  Atom name;
  struct Frame {
    float offset;  // 0..1
    std::shared_ptr<DeclBlock> decls;
    std::shared_ptr<TimingFn> timing;
  };
  std::vector<Frame> frames;
};
struct StyleSheet {
  std::vector<StyleRule> rules;
  std::vector<Keyframes> keyframes;
  std::vector<std::pair<std::string, std::string>> fontFaces;  // family -> src url
};
struct MediaEnv {
  float width = 800, height = 480;
  bool dark = false;  // prefers-color-scheme (the OS registry's darkMode)
};
void parseStyleSheet(const std::string &css, StyleSheet &out, uint8_t layer, uint32_t &order, const MediaEnv &env);

}  // namespace rt
