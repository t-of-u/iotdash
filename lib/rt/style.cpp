// Cascade, inheritance and computed values.
#include "style.h"

#include "extra.h"

namespace rt {

static const uint16_t P_SHORTHAND_RAW = 0xFFFD;
bool expandShorthandPublic(const std::string &name, const std::vector<Tok> &toks, std::vector<Decl> &out, bool imp);

const char *kUserAgentCss = R"CSS(
html, address, blockquote, body, center, dialog, div, figure, figcaption, footer, form, header, hr,
legend, main, p, pre, search, article, aside, h1, h2, h3, h4, h5, h6, hgroup, nav, section, dd, dl,
dt, menu, ol, ul, fieldset, details, summary, optgroup { display: block; }
li { display: list-item; }
head, script, style, title, meta, link, template, datalist, noscript, param, base, area, slot, option { display: none; }
[hidden] { display: none !important; }
html { color: #000; font-family: sans-serif; font-size: 16px; line-height: normal; }
body { margin: 8px; }
p, blockquote, dl, menu { margin-top: 1em; margin-bottom: 1em; }
figure, blockquote { margin: 1em 40px; }
ul, ol { margin-top: 1em; margin-bottom: 1em; padding-left: 40px; }
ul { list-style-type: disc; }
ol { list-style-type: decimal; }
ul ul, ol ul { list-style-type: circle; margin-top: 0; margin-bottom: 0; }
ol ol, ul ol { margin-top: 0; margin-bottom: 0; }
dd { margin-left: 40px; }
h1 { font-size: 2em; margin-top: 0.67em; margin-bottom: 0.67em; font-weight: bold; }
h2 { font-size: 1.5em; margin-top: 0.83em; margin-bottom: 0.83em; font-weight: bold; }
h3 { font-size: 1.17em; margin-top: 1em; margin-bottom: 1em; font-weight: bold; }
h4 { font-size: 1em; margin-top: 1.33em; margin-bottom: 1.33em; font-weight: bold; }
h5 { font-size: 0.83em; margin-top: 1.67em; margin-bottom: 1.67em; font-weight: bold; }
h6 { font-size: 0.67em; margin-top: 2.33em; margin-bottom: 2.33em; font-weight: bold; }
b, strong, th, summary, legend { font-weight: bold; }
i, em, cite, var, dfn, address { font-style: italic; }
small { font-size: smaller; }
sub { vertical-align: sub; font-size: smaller; }
sup { vertical-align: super; font-size: smaller; }
code, kbd, samp, pre, tt { font-family: monospace; }
pre { white-space: pre; margin-top: 1em; margin-bottom: 1em; }
a:link { color: #0000ee; text-decoration: underline; }
u, ins { text-decoration: underline; }
s, strike, del { text-decoration: line-through; }
mark { background-color: yellow; color: black; }
hr { color: gray; border-style: inset; border-width: 1px; margin: 0.5em auto; overflow: hidden; }
center { text-align: center; }
table { display: table; border-collapse: separate; border-spacing: 2px; box-sizing: border-box; border-color: gray; }
caption { display: table-caption; text-align: center; }
thead { display: table-header-group; vertical-align: middle; }
tbody { display: table-row-group; vertical-align: middle; }
tfoot { display: table-footer-group; vertical-align: middle; }
tr { display: table-row; }
td, th { display: table-cell; padding: 1px; }
th { text-align: center; }
col { display: table-column; }
colgroup { display: table-column-group; }
img, canvas, svg, video { display: inline; }
svg:not([width]):not([height]) { width: 300px; height: 150px; }
iframe, video, audio, object, embed { display: none; }
fieldset { margin: 0 2px; padding: 0.35em 0.75em 0.625em; border: 2px groove #c0c0c0; }
legend { padding: 0 2px; }
details > :not(summary) { display: none; }
details[open] > :not(summary) { display: block; }
summary { display: list-item; list-style-type: disc; }
dialog { display: none; position: absolute; left: 0; right: 0; margin: auto; width: fit-content; height: fit-content;
  border: none; padding: 1em; background: #fff; color: #000; }
dialog[open] { display: block; position: fixed; top: 0; bottom: 0; z-index: 1000; }

/* ---- form controls (touch-friendly defaults) ---- */
input, select, textarea, button { display: inline-block; font-family: inherit; font-size: 16px;
  line-height: normal; margin: 0; color: #111827; box-sizing: border-box; letter-spacing: normal; text-align: start; }
input, textarea, select { background-color: #fff; border: 1px solid #c8ccd4; border-radius: 8px; padding: 8px 12px; }
input { width: 200px; }
textarea { width: 260px; height: 88px; white-space: pre-wrap; overflow-y: auto; vertical-align: top; }
select { padding-right: 32px; min-width: 120px; }
input[type=checkbox], input[type=radio] { width: 20px; height: 20px; padding: 0; margin: 3px 4px; border: 2px solid #9ca3af;
  vertical-align: middle; }
input[type=checkbox] { border-radius: 5px; }
input[type=radio] { border-radius: 50%; }
input[type=range] { width: 200px; height: 24px; padding: 0; border: none; background: transparent; vertical-align: middle; }
input[type=color] { width: 56px; height: 36px; padding: 4px; }
input[type=button], input[type=submit], input[type=reset], button { width: auto; padding: 8px 16px; border: none;
  border-radius: 8px; background-color: #e5e7eb; color: #111827; text-align: center; }
input[type=hidden] { display: none; }
input[type=file] { border: none; padding: 0; width: auto; }
input:focus, textarea:focus, select:focus { border-color: #2563eb; outline: 2px solid rgba(37, 99, 235, 0.25); outline-offset: 0; }
input:disabled, textarea:disabled, select:disabled, button:disabled { opacity: 0.5; }
input::placeholder, textarea::placeholder { color: #9ca3af; }
progress, meter { display: inline-block; width: 160px; height: 10px; vertical-align: middle; border-radius: 5px; background-color: #e5e7eb; }
label { cursor: default; }
)CSS";

// ============================================================ helpers

bool ComputedStyle::isBlockLevel() const {
  switch (display) {
    case D_BLOCK: case D_FLEX: case D_GRID: case D_TABLE: case D_LIST_ITEM: case D_FLOW_ROOT:
    case D_TABLE_ROW: case D_TABLE_CELL: case D_TABLE_ROW_GROUP: case D_TABLE_HEADER_GROUP:
    case D_TABLE_FOOTER_GROUP: case D_TABLE_CAPTION:
      return true;
    default:
      return false;
  }
}

static Display blockify(Display d) {
  switch (d) {
    case D_INLINE: case D_INLINE_BLOCK: return D_BLOCK;
    case D_INLINE_FLEX: return D_FLEX;
    case D_INLINE_GRID: return D_GRID;
    case D_INLINE_TABLE: return D_TABLE;
    case D_TABLE_ROW: case D_TABLE_CELL: case D_TABLE_ROW_GROUP: case D_TABLE_HEADER_GROUP:
    case D_TABLE_FOOTER_GROUP: case D_TABLE_CAPTION: case D_TABLE_COLUMN: case D_TABLE_COLUMN_GROUP:
      return D_BLOCK;
    default: return d;
  }
}

static void copyInherited(ComputedStyle &d, const ComputedStyle &s) {
  d.color = s.color;
  d.fontFamily = s.fontFamily;
  d.fontSize = s.fontSize;
  d.fontWeight = s.fontWeight;
  d.italic = s.italic;
  d.lineHeight = s.lineHeight;
  d.lineHeightNum = s.lineHeightNum;
  d.lineHeightIsNum = s.lineHeightIsNum;
  d.letterSpacing = s.letterSpacing;
  d.wordSpacing = s.wordSpacing;
  d.textAlign = s.textAlign;
  d.textTransform = s.textTransform;
  d.whiteSpace = s.whiteSpace;
  d.wordBreak = s.wordBreak;
  d.overflowWrap = s.overflowWrap;
  d.textIndent = s.textIndent;
  d.textShadow = s.textShadow;
  d.visibility = s.visibility;
  d.listStyleType = s.listStyleType;
  d.listStyleInside = s.listStyleInside;
  d.borderCollapse = s.borderCollapse;
  d.borderSpacingX = s.borderSpacingX;
  d.borderSpacingY = s.borderSpacingY;
  d.caretColor = s.caretColor;
  d.caretAuto = s.caretAuto;
  d.accentColor = s.accentColor;
  d.accentAuto = s.accentAuto;
  d.pointerEvents = s.pointerEvents;
  d.tabularNums = s.tabularNums;
  d.customProps = s.customProps;
  d.svgFill = s.svgFill;
  d.svgStroke = s.svgStroke;
  d.svgFillNone = s.svgFillNone;
  d.svgStrokeNone = s.svgStrokeNone;
  d.svgStrokeWidth = s.svgStrokeWidth;
  d.svgCap = s.svgCap;
  d.svgJoin = s.svgJoin;
}

static bool inheritedDiffers(const ComputedStyle &a, const ComputedStyle &b) {
  auto famEq = [](const std::shared_ptr<std::vector<std::string>> &x, const std::shared_ptr<std::vector<std::string>> &y) {
    if (x == y) return true;
    if (!x || !y) return false;
    return *x == *y;
  };
  return a.color != b.color || !famEq(a.fontFamily, b.fontFamily) || a.fontSize != b.fontSize ||
         a.fontWeight != b.fontWeight || a.italic != b.italic || a.lineHeight != b.lineHeight ||
         a.lineHeightNum != b.lineHeightNum || a.lineHeightIsNum != b.lineHeightIsNum ||
         a.letterSpacing != b.letterSpacing || a.wordSpacing != b.wordSpacing || a.textAlign != b.textAlign ||
         a.textTransform != b.textTransform || a.whiteSpace != b.whiteSpace || a.wordBreak != b.wordBreak ||
         a.overflowWrap != b.overflowWrap || a.textIndent != b.textIndent || a.textShadow != b.textShadow ||
         a.visibility != b.visibility || a.listStyleType != b.listStyleType ||
         a.listStyleInside != b.listStyleInside || a.borderCollapse != b.borderCollapse ||
         a.borderSpacingX != b.borderSpacingX || a.borderSpacingY != b.borderSpacingY ||
         a.caretColor != b.caretColor || a.accentColor != b.accentColor || a.pointerEvents != b.pointerEvents ||
         !(a.customProps == b.customProps || (a.customProps && b.customProps && *a.customProps == *b.customProps)) ||
         a.tabularNums != b.tabularNums;
}

template <class T>
static bool ptrEq(const std::shared_ptr<T> &a, const std::shared_ptr<T> &b) {
  return a == b;
}

uint8_t diffStyles(const ComputedStyle &a, const ComputedStyle &b) {
  uint8_t d = SD_NONE;
  // layout-affecting
  bool layout =
      a.display != b.display || a.position != b.position || a.top != b.top || a.right != b.right ||
      a.bottom != b.bottom || a.left != b.left || a.width != b.width || a.height != b.height ||
      a.minWidth != b.minWidth || a.minHeight != b.minHeight || a.maxWidth != b.maxWidth ||
      a.maxHeight != b.maxHeight || a.boxSizing != b.boxSizing || a.overflowX != b.overflowX ||
      a.overflowY != b.overflowY || a.fontSize != b.fontSize || a.fontWeight != b.fontWeight ||
      a.italic != b.italic || a.fontFamily != b.fontFamily || a.lineHeight != b.lineHeight ||
      a.lineHeightNum != b.lineHeightNum || a.lineHeightIsNum != b.lineHeightIsNum ||
      a.letterSpacing != b.letterSpacing || a.wordSpacing != b.wordSpacing || a.textAlign != b.textAlign ||
      a.textTransform != b.textTransform || a.whiteSpace != b.whiteSpace || a.wordBreak != b.wordBreak ||
      a.overflowWrap != b.overflowWrap || a.textIndent != b.textIndent || a.verticalAlign != b.verticalAlign ||
      a.verticalAlignLen != b.verticalAlignLen || a.lineClamp != b.lineClamp || a.textOverflow != b.textOverflow ||
      a.flexDirection != b.flexDirection || a.flexWrap != b.flexWrap || a.justifyContent != b.justifyContent ||
      a.alignItems != b.alignItems || a.alignSelf != b.alignSelf || a.alignContent != b.alignContent ||
      a.justifyItems != b.justifyItems || a.justifySelf != b.justifySelf || a.flexGrow != b.flexGrow ||
      a.flexShrink != b.flexShrink || a.flexBasis != b.flexBasis || a.order != b.order || a.rowGap != b.rowGap ||
      a.columnGap != b.columnGap || a.gridCols != b.gridCols || a.gridRows != b.gridRows ||
      a.gridAutoRows != b.gridAutoRows || a.gridAutoCols != b.gridAutoCols || a.gridFlow != b.gridFlow ||
      a.gridColStart.line != b.gridColStart.line || a.gridColStart.span != b.gridColStart.span ||
      a.gridColEnd.line != b.gridColEnd.line || a.gridColEnd.span != b.gridColEnd.span ||
      a.gridRowStart.line != b.gridRowStart.line || a.gridRowStart.span != b.gridRowStart.span ||
      a.gridRowEnd.line != b.gridRowEnd.line || a.gridRowEnd.span != b.gridRowEnd.span ||
      a.content != b.content || a.contentNone != b.contentNone || a.listStyleType != b.listStyleType ||
      a.listStyleInside != b.listStyleInside || a.borderCollapse != b.borderCollapse ||
      a.borderSpacingX != b.borderSpacingX || a.borderSpacingY != b.borderSpacingY ||
      a.tableLayout != b.tableLayout || a.aspectRatio != b.aspectRatio || a.tabularNums != b.tabularNums ||
      a.appearance != b.appearance;
  for (int i = 0; i < 4 && !layout; i++) {
    layout = a.margin[i] != b.margin[i] || a.padding[i] != b.padding[i] || a.borderW[i] != b.borderW[i] ||
             (a.borderStyle[i] == BS_NONE) != (b.borderStyle[i] == BS_NONE);
  }
  if (layout) d |= SD_LAYOUT | SD_PAINT;
  if (a.zIndex != b.zIndex || a.zAuto != b.zAuto || a.makesStackingContext() != b.makesStackingContext())
    d |= SD_STACKING | SD_PAINT;
  if (!(d & SD_PAINT)) {
    bool paint = a.color != b.color || a.bgColor != b.bgColor || a.bgImages != b.bgImages ||
                 a.bgSizeKind != b.bgSizeKind || a.bgSizeX != b.bgSizeX || a.bgSizeY != b.bgSizeY ||
                 a.bgPosX != b.bgPosX || a.bgPosY != b.bgPosY || a.bgRepeat != b.bgRepeat ||
                 a.bgClip != b.bgClip || a.visibility != b.visibility || a.opacity != b.opacity ||
                 a.textShadow != b.textShadow || a.boxShadow != b.boxShadow || a.outlineWidth != b.outlineWidth ||
                 a.outlineStyle != b.outlineStyle || a.outlineColor.c != b.outlineColor.c ||
                 a.outlineColor.current != b.outlineColor.current || a.outlineOffset != b.outlineOffset ||
                 a.transform != b.transform || a.originX != b.originX || a.originY != b.originY ||
                 a.filter != b.filter || a.textDecoLine != b.textDecoLine || a.textDecoColor.c != b.textDecoColor.c ||
                 a.caretColor != b.caretColor || a.accentColor != b.accentColor || a.objectFit != b.objectFit ||
                 a.objPosX != b.objPosX || a.objPosY != b.objPosY || a.pixelated != b.pixelated ||
                 a.svgFill.c != b.svgFill.c || a.svgFill.current != b.svgFill.current || a.svgFillNone != b.svgFillNone ||
                 a.svgStroke.c != b.svgStroke.c || a.svgStroke.current != b.svgStroke.current ||
                 a.svgStrokeNone != b.svgStrokeNone || a.svgStrokeWidth != b.svgStrokeWidth || a.svgCap != b.svgCap ||
                 a.svgJoin != b.svgJoin;
    for (int i = 0; i < 4 && !paint; i++)
      paint = a.borderColor[i].c != b.borderColor[i].c || a.borderColor[i].current != b.borderColor[i].current ||
              a.borderStyle[i] != b.borderStyle[i] || a.radius[i] != b.radius[i];
    if (paint) d |= SD_PAINT;
  }
  return d;
}

// ============================================================ var() substitution

static const std::string *findCustom(const ComputedStyle &cs, Atom name) {
  if (!cs.customProps) return nullptr;
  for (auto &kv : *cs.customProps)
    if (kv.first == name) return &kv.second;
  return nullptr;
}

// Replace var(--x, fallback) occurrences in text using the element's custom properties.
static bool substituteVars(const std::string &in, const ComputedStyle &cs, std::string &out, int depth = 0) {
  if (depth > 8) return false;
  out.clear();
  size_t i = 0;
  while (i < in.size()) {
    size_t v = in.find("var(", i);
    size_t e = in.find("env(", i);
    if (e != std::string::npos && (v == std::string::npos || e < v)) v = e;
    if (v == std::string::npos) {
      out += in.substr(i);
      break;
    }
    out += in.substr(i, v - i);
    bool isEnv = in.compare(v, 4, "env(") == 0;
    // find matching paren
    size_t j = v + 4;
    int d = 1;
    while (j < in.size() && d > 0) {
      if (in[j] == '(') d++;
      else if (in[j] == ')') d--;
      if (d > 0) j++;
    }
    std::string args = in.substr(v + 4, j - v - 4);
    i = j + 1;
    size_t comma = args.find(',');
    std::string name = trim(comma == std::string::npos ? args : args.substr(0, comma));
    std::string fallback = comma == std::string::npos ? "" : trim(args.substr(comma + 1));
    bool hasFallback = comma != std::string::npos;
    std::string repl;
    if (isEnv) {
      repl = hasFallback ? fallback : "0px";
    } else {
      Atom a = findAtom(name.data(), name.size());
      const std::string *val = a ? findCustom(cs, a) : nullptr;
      if (val && !val->empty()) repl = *val;
      else if (hasFallback) repl = fallback;
      else return false;
    }
    std::string sub;
    if (repl.find("var(") != std::string::npos) {
      if (!substituteVars(repl, cs, sub, depth + 1)) return false;
      repl = sub;
    }
    out += repl;
  }
  return true;
}

// ============================================================ engine

void StyleEngine::init(const MediaEnv &env) {
  env_ = env;
  initAtoms();
  ua_ = StyleSheet();
  author_ = StyleSheet();
  order_ = 0;
  parseStyleSheet(kUserAgentCss, ua_, 0, order_, env_);
  indexDirty_ = true;
}

void StyleEngine::addUserAgentSheet(const std::string &css) {
  parseStyleSheet(css, ua_, 0, order_, env_);
  indexDirty_ = true;
  gen_++;
}

void StyleEngine::clearAuthor() {
  author_ = StyleSheet();
  indexDirty_ = true;
  gen_++;
}

void StyleEngine::addAuthorSheet(const std::string &css) {
  parseStyleSheet(css, author_, 1, order_, env_);
  indexDirty_ = true;
  gen_++;
}

const Keyframes *StyleEngine::findKeyframes(Atom name) const {
  for (auto it = author_.keyframes.rbegin(); it != author_.keyframes.rend(); ++it)
    if (it->name == name) return &*it;
  for (auto it = ua_.keyframes.rbegin(); it != ua_.keyframes.rend(); ++it)
    if (it->name == name) return &*it;
  return nullptr;
}

void StyleEngine::rebuildIndex() {
  idx_ = Bucketed();
  auto add = [&](const StyleRule &r) {
    const Compound &last = r.sel.parts.back();
    Atom id = 0, cls = 0, tag = 0;
    for (auto &s : last.simple) {
      if (s.m == SM_ID) id = s.a;
      else if (s.m == SM_CLASS && !cls) cls = s.a;
      else if (s.m == SM_TAG) tag = s.a;
    }
    if (id) idx_.byId[id].push_back(&r);
    else if (cls) idx_.byClass[cls].push_back(&r);
    else if (tag) idx_.byTag[tag].push_back(&r);
    else idx_.universal.push_back(&r);
  };
  pseudoMask_ = 0;
  bool sibling = false, stateDesc = false;
  auto scan = [&](const StyleRule &r) {
    add(r);
    pseudoMask_ |= 1u << r.sel.pseudo;
    const auto &parts = r.sel.parts;
    for (size_t i = 0; i < parts.size(); i++) {
      if (parts[i].comb == CB_ADJ || parts[i].comb == CB_SIB) sibling = true;
      for (auto &s : parts[i].simple) {
        if (s.m == SM_HAS) sibling = stateDesc = true;  // depends on anything around
        else if (i + 1 < parts.size() && (s.m == SM_PSEUDO || s.m == SM_NOT || s.m == SM_IS)) stateDesc = true;
      }
    }
  };
  for (auto &r : ua_.rules) scan(r);
  for (auto &r : author_.rules) scan(r);
  gSiblingSelectors = sibling;
  gStateDescendants = stateDesc;
  static const bool dbg = getenv("RT_STYLE_DEBUG") != nullptr;
  if (dbg) fprintf(stderr, "style index: %zu ua + %zu author rules, %zu universal, sibling combinators: %d, state on ancestors: %d\n",
                   ua_.rules.size(), author_.rules.size(), idx_.universal.size(), (int)sibling, (int)stateDesc);
  indexDirty_ = false;
}

void StyleEngine::collect(Element *e, PseudoElem pe, std::vector<const StyleRule *> &out) const {
  auto consider = [&](const std::vector<const StyleRule *> &v) {
    for (const StyleRule *r : v) {
      if (r->sel.pseudo != pe) continue;
      if (matchSelector(r->sel, e)) out.push_back(r);
    }
  };
  if (e->idAtom) {
    auto it = idx_.byId.find(e->idAtom);
    if (it != idx_.byId.end()) consider(it->second);
  }
  for (Atom c : e->classes) {
    auto it = idx_.byClass.find(c);
    if (it != idx_.byClass.end()) consider(it->second);
  }
  auto it = idx_.byTag.find(e->tag);
  if (it != idx_.byTag.end()) consider(it->second);
  consider(idx_.universal);
  std::sort(out.begin(), out.end(), [](const StyleRule *a, const StyleRule *b) {
    if (a->layer != b->layer) return a->layer < b->layer;
    if (a->sel.specificity != b->sel.specificity) return a->sel.specificity < b->sel.specificity;
    return a->order < b->order;
  });
}

static void copyProp(ComputedStyle &d, const ComputedStyle &s, uint16_t p) {
#define C(f) d.f = s.f
  switch ((Prop)p) {
    case P_display: C(display); break;
    case P_position: C(position); break;
    case P_top: C(top); break;
    case P_right: C(right); break;
    case P_bottom: C(bottom); break;
    case P_left: C(left); break;
    case P_z_index: C(zIndex); C(zAuto); break;
    case P_width: C(width); break;
    case P_height: C(height); break;
    case P_min_width: C(minWidth); break;
    case P_min_height: C(minHeight); break;
    case P_max_width: C(maxWidth); break;
    case P_max_height: C(maxHeight); break;
    case P_margin_top: C(margin[0]); break;
    case P_margin_right: C(margin[1]); break;
    case P_margin_bottom: C(margin[2]); break;
    case P_margin_left: C(margin[3]); break;
    case P_padding_top: C(padding[0]); break;
    case P_padding_right: C(padding[1]); break;
    case P_padding_bottom: C(padding[2]); break;
    case P_padding_left: C(padding[3]); break;
    case P_border_top_width: C(borderW[0]); break;
    case P_border_right_width: C(borderW[1]); break;
    case P_border_bottom_width: C(borderW[2]); break;
    case P_border_left_width: C(borderW[3]); break;
    case P_border_top_style: C(borderStyle[0]); break;
    case P_border_right_style: C(borderStyle[1]); break;
    case P_border_bottom_style: C(borderStyle[2]); break;
    case P_border_left_style: C(borderStyle[3]); break;
    case P_border_top_color: C(borderColor[0]); break;
    case P_border_right_color: C(borderColor[1]); break;
    case P_border_bottom_color: C(borderColor[2]); break;
    case P_border_left_color: C(borderColor[3]); break;
    case P_border_top_left_radius: C(radius[0]); break;
    case P_border_top_right_radius: C(radius[1]); break;
    case P_border_bottom_right_radius: C(radius[2]); break;
    case P_border_bottom_left_radius: C(radius[3]); break;
    case P_box_sizing: C(boxSizing); break;
    case P_overflow_x: C(overflowX); break;
    case P_overflow_y: C(overflowY); break;
    case P_visibility: C(visibility); break;
    case P_opacity: C(opacity); break;
    case P_background_color: C(bgColor); break;
    case P_background_image: C(bgImages); break;
    case P_background_size: C(bgSizeKind); C(bgSizeX); C(bgSizeY); break;
    case P_background_position: C(bgPosX); C(bgPosY); break;
    case P_background_repeat: C(bgRepeat); break;
    case P_background_clip: C(bgClip); break;
    case P_color: C(color); break;
    case P_font_family: C(fontFamily); break;
    case P_font_size: C(fontSize); break;
    case P_font_weight: C(fontWeight); break;
    case P_font_style: C(italic); break;
    case P_line_height: C(lineHeight); C(lineHeightNum); C(lineHeightIsNum); break;
    case P_letter_spacing: C(letterSpacing); break;
    case P_word_spacing: C(wordSpacing); break;
    case P_text_align: C(textAlign); break;
    case P_text_decoration_line: C(textDecoLine); break;
    case P_text_decoration_color: C(textDecoColor); break;
    case P_text_transform: C(textTransform); break;
    case P_text_overflow: C(textOverflow); break;
    case P_white_space: C(whiteSpace); break;
    case P_word_break: C(wordBreak); break;
    case P_overflow_wrap: C(overflowWrap); break;
    case P_vertical_align: C(verticalAlign); C(verticalAlignLen); break;
    case P_text_indent: C(textIndent); break;
    case P_text_shadow: C(textShadow); break;
    case P_line_clamp: C(lineClamp); break;
    case P_box_shadow: C(boxShadow); break;
    case P_outline_width: C(outlineWidth); break;
    case P_outline_style: C(outlineStyle); break;
    case P_outline_color: C(outlineColor); break;
    case P_outline_offset: C(outlineOffset); break;
    case P_transform: C(transform); break;
    case P_transform_origin: C(originX); C(originY); break;
    case P_filter: C(filter); break;
    case P_flex_direction: C(flexDirection); break;
    case P_flex_wrap: C(flexWrap); break;
    case P_justify_content: C(justifyContent); break;
    case P_align_items: C(alignItems); break;
    case P_align_self: C(alignSelf); break;
    case P_align_content: C(alignContent); break;
    case P_justify_items: C(justifyItems); break;
    case P_justify_self: C(justifySelf); break;
    case P_flex_grow: C(flexGrow); break;
    case P_flex_shrink: C(flexShrink); break;
    case P_flex_basis: C(flexBasis); break;
    case P_order: C(order); break;
    case P_row_gap: C(rowGap); break;
    case P_column_gap: C(columnGap); break;
    case P_grid_template_columns: C(gridCols); break;
    case P_grid_template_rows: C(gridRows); break;
    case P_grid_auto_rows: C(gridAutoRows); break;
    case P_grid_auto_columns: C(gridAutoCols); break;
    case P_grid_auto_flow: C(gridFlow); break;
    case P_grid_column_start: C(gridColStart); break;
    case P_grid_column_end: C(gridColEnd); break;
    case P_grid_row_start: C(gridRowStart); break;
    case P_grid_row_end: C(gridRowEnd); break;
    case P_transition_property: case P_transition_duration: case P_transition_timing_function:
    case P_transition_delay: case P_animation_name: case P_animation_duration:
    case P_animation_timing_function: case P_animation_delay: case P_animation_iteration_count:
    case P_animation_direction: case P_animation_fill_mode: case P_animation_play_state:
      C(animLists[p - P_transition_property]);
      break;
    case P_content: C(content); C(contentNone); break;
    case P_list_style_type: C(listStyleType); break;
    case P_list_style_position: C(listStyleInside); break;
    case P_border_collapse: C(borderCollapse); break;
    case P_border_spacing: C(borderSpacingX); C(borderSpacingY); break;
    case P_table_layout: C(tableLayout); break;
    case P_pointer_events: C(pointerEvents); break;
    case P_accent_color: C(accentColor); C(accentAuto); break;
    case P_caret_color: C(caretColor); C(caretAuto); break;
    case P_appearance: C(appearance); break;
    case P_object_fit: C(objectFit); break;
    case P_object_position: C(objPosX); C(objPosY); break;
    case P_aspect_ratio: C(aspectRatio); break;
    case P_image_rendering: C(pixelated); break;
    case P_scroll_snap_type: C(scrollSnapX); C(scrollSnapY); break;
    case P_scroll_snap_align: C(scrollSnapAlign); break;
    case P_font_variant_numeric: C(tabularNums); break;
    case P_fill: C(svgFill); C(svgFillNone); d.svgFillSet = true; break;
    case P_stroke: C(svgStroke); C(svgStrokeNone); d.svgStrokeSet = true; break;
    case P_stroke_width: C(svgStrokeWidth); d.svgStrokeWidthSet = true; break;
    case P_stroke_linecap: C(svgCap); d.svgCapSet = true; break;
    case P_stroke_linejoin: C(svgJoin); d.svgJoinSet = true; break;
    default: break;
  }
#undef C
}

void StyleEngine::applyDecl(ComputedStyle &cs, const Decl &d, const ComputedStyle *parent, const ResolveCtx &rc) {
  const Value &v = d.v;
  Prop p = (Prop)d.prop;
  if (d.prop >= P__COUNT) return;
  if (v.type == VT_INHERIT) {
    if (parent) copyProp(cs, *parent, p);
    else copyProp(cs, initial_, p);
    return;
  }
  if (v.type == VT_INITIAL) {
    copyProp(cs, initial_, p);
    return;
  }
  if (v.type == VT_UNSET) {
    if (propInherited(p) && parent) copyProp(cs, *parent, p);
    else copyProp(cs, initial_, p);
    return;
  }
  auto L = [&](const LenSpec &l) { return computeLen(l, rc); };
  auto px = [&](const LenSpec &l) { return computeLen(l, rc).px; };
  switch (p) {
    case P_display: cs.display = (Display)v.kw; break;
    case P_position: cs.position = (Position)v.kw; break;
    case P_top: cs.top = L(v.len); break;
    case P_right: cs.right = L(v.len); break;
    case P_bottom: cs.bottom = L(v.len); break;
    case P_left: cs.left = L(v.len); break;
    case P_z_index:
      if (v.type == VT_KW) cs.zAuto = true;
      else {
        cs.zAuto = false;
        cs.zIndex = (int)v.num;
      }
      break;
    case P_width: cs.width = L(v.len); break;
    case P_height: cs.height = L(v.len); break;
    case P_min_width: cs.minWidth = L(v.len); break;
    case P_min_height: cs.minHeight = L(v.len); break;
    case P_max_width: cs.maxWidth = L(v.len); break;
    case P_max_height: cs.maxHeight = L(v.len); break;
    case P_margin_top: cs.margin[0] = L(v.len); break;
    case P_margin_right: cs.margin[1] = L(v.len); break;
    case P_margin_bottom: cs.margin[2] = L(v.len); break;
    case P_margin_left: cs.margin[3] = L(v.len); break;
    case P_padding_top: cs.padding[0] = L(v.len); break;
    case P_padding_right: cs.padding[1] = L(v.len); break;
    case P_padding_bottom: cs.padding[2] = L(v.len); break;
    case P_padding_left: cs.padding[3] = L(v.len); break;
    case P_border_top_width: cs.borderW[0] = px(v.len); break;
    case P_border_right_width: cs.borderW[1] = px(v.len); break;
    case P_border_bottom_width: cs.borderW[2] = px(v.len); break;
    case P_border_left_width: cs.borderW[3] = px(v.len); break;
    case P_border_top_style: cs.borderStyle[0] = (BorderStyle)v.kw; break;
    case P_border_right_style: cs.borderStyle[1] = (BorderStyle)v.kw; break;
    case P_border_bottom_style: cs.borderStyle[2] = (BorderStyle)v.kw; break;
    case P_border_left_style: cs.borderStyle[3] = (BorderStyle)v.kw; break;
    case P_border_top_color: cs.borderColor[0] = v.color; break;
    case P_border_right_color: cs.borderColor[1] = v.color; break;
    case P_border_bottom_color: cs.borderColor[2] = v.color; break;
    case P_border_left_color: cs.borderColor[3] = v.color; break;
    case P_border_top_left_radius: cs.radius[0] = L(v.len); break;
    case P_border_top_right_radius: cs.radius[1] = L(v.len); break;
    case P_border_bottom_right_radius: cs.radius[2] = L(v.len); break;
    case P_border_bottom_left_radius: cs.radius[3] = L(v.len); break;
    case P_box_sizing: cs.boxSizing = (BoxSizing)v.kw; break;
    case P_overflow_x: cs.overflowX = (Overflow)v.kw; break;
    case P_overflow_y: cs.overflowY = (Overflow)v.kw; break;
    case P_visibility: cs.visibility = (Visibility)v.kw; break;
    case P_opacity: cs.opacity = clampf(v.num, 0, 1); break;
    case P_background_color: cs.bgColor = v.color.current ? cs.color : v.color.c; break;
    case P_background_image:
      if (v.type == VT_KW) cs.bgImages.reset();
      else cs.bgImages = std::static_pointer_cast<std::vector<BgLayer>>(v.obj());
      break;
    case P_background_size:
      if (v.type == VT_KW) cs.bgSizeKind = (BgSizeKind)v.kw;
      else {
        cs.bgSizeKind = BGS_LEN;
        cs.bgSizeX = L(v.len);
        cs.bgSizeY = L(v.len2());
        if (cs.bgSizeX.isAuto() && cs.bgSizeY.isAuto()) cs.bgSizeKind = BGS_AUTO;
      }
      break;
    case P_background_position: cs.bgPosX = L(v.len); cs.bgPosY = L(v.len2()); break;
    case P_background_repeat: cs.bgRepeat = (BgRepeat)v.kw; break;
    case P_background_clip: cs.bgClip = (uint8_t)v.kw; break;
    case P_color: cs.color = v.color.current ? (parent ? parent->color : kBlack) : v.color.c; break;
    case P_font_family: cs.fontFamily = std::static_pointer_cast<std::vector<std::string>>(v.obj()); break;
    case P_font_size: {
      ResolveCtx r2 = rc;
      r2.em = parent ? parent->fontSize : 16;
      cs.fontSize = std::max(0.f, computeLen(v.len, r2).px);
      break;
    }
    case P_font_weight:
      if (v.type == VT_KW) {
        int pw = parent ? parent->fontWeight : 400;
        if (v.kw == 1) cs.fontWeight = pw < 350 ? 400 : pw < 550 ? 700 : 900;
        else cs.fontWeight = pw < 550 ? 100 : pw < 750 ? 400 : 700;
      } else {
        cs.fontWeight = clampi((int)v.num, 1, 1000);
      }
      break;
    case P_font_style: cs.italic = v.kw != 0; break;
    case P_line_height:
      if (v.type == VT_KW) {
        cs.lineHeight = -1;
        cs.lineHeightIsNum = false;
      } else if (v.type == VT_NUM) {
        cs.lineHeightIsNum = true;
        cs.lineHeightNum = v.num;
        cs.lineHeight = -1;
      } else {
        cs.lineHeightIsNum = false;
        cs.lineHeight = px(v.len);
      }
      break;
    case P_letter_spacing: cs.letterSpacing = px(v.len); break;
    case P_word_spacing: cs.wordSpacing = px(v.len); break;
    case P_text_align: cs.textAlign = (TextAlign)v.kw; break;
    case P_text_decoration_line: cs.textDecoLine = (uint8_t)v.kw; break;
    case P_text_decoration_color: cs.textDecoColor = v.color; break;
    case P_text_transform: cs.textTransform = (TextTransform)v.kw; break;
    case P_text_overflow: cs.textOverflow = (TextOverflow)v.kw; break;
    case P_white_space: cs.whiteSpace = (WhiteSpace)v.kw; break;
    case P_word_break: cs.wordBreak = (WordBreak)v.kw; break;
    case P_overflow_wrap: cs.overflowWrap = (OverflowWrap)v.kw; break;
    case P_vertical_align:
      if (v.type == VT_KW) cs.verticalAlign = (VAlign)v.kw;
      else {
        cs.verticalAlign = VA_LENGTH;
        cs.verticalAlignLen = computeLen(v.len, rc).resolve(cs.usedLineHeight(cs.fontSize * 1.2f));
      }
      break;
    case P_text_indent: cs.textIndent = px(v.len); break;
    case P_text_shadow: cs.textShadow = std::static_pointer_cast<std::vector<Shadow>>(v.obj()); break;
    case P_line_clamp: cs.lineClamp = (int)v.num; break;
    case P_box_shadow: cs.boxShadow = std::static_pointer_cast<std::vector<Shadow>>(v.obj()); break;
    case P_outline_width: cs.outlineWidth = px(v.len); break;
    case P_outline_style: cs.outlineStyle = (BorderStyle)v.kw; break;
    case P_outline_color: cs.outlineColor = v.color; break;
    case P_outline_offset: cs.outlineOffset = px(v.len); break;
    case P_transform: {
      auto t = std::static_pointer_cast<std::vector<TransformFn>>(v.obj());
      cs.transform = (t && !t->empty()) ? t : nullptr;
      break;
    }
    case P_transform_origin: cs.originX = L(v.len); cs.originY = L(v.len2()); break;
    case P_filter: {
      auto f = std::static_pointer_cast<std::vector<FilterFn>>(v.obj());
      cs.filter = (f && !f->empty()) ? f : nullptr;
      break;
    }
    case P_flex_direction: cs.flexDirection = (FlexDir)v.kw; break;
    case P_flex_wrap: cs.flexWrap = (FlexWrap)v.kw; break;
    case P_justify_content: cs.justifyContent = (Align)v.kw; break;
    case P_align_items: cs.alignItems = (Align)v.kw; break;
    case P_align_self: cs.alignSelf = (Align)v.kw; break;
    case P_align_content: cs.alignContent = (Align)v.kw; break;
    case P_justify_items: cs.justifyItems = (Align)v.kw; break;
    case P_justify_self: cs.justifySelf = (Align)v.kw; break;
    case P_flex_grow: cs.flexGrow = std::max(0.f, v.num); break;
    case P_flex_shrink: cs.flexShrink = std::max(0.f, v.num); break;
    case P_flex_basis: cs.flexBasis = L(v.len); break;
    case P_order: cs.order = (int)v.num; break;
    case P_row_gap: cs.rowGap = L(v.len); break;
    case P_column_gap: cs.columnGap = L(v.len); break;
    case P_grid_template_columns: cs.gridCols = std::static_pointer_cast<TrackList>(v.obj()); break;
    case P_grid_template_rows: cs.gridRows = std::static_pointer_cast<TrackList>(v.obj()); break;
    case P_grid_auto_rows: cs.gridAutoRows = std::static_pointer_cast<TrackList>(v.obj()); break;
    case P_grid_auto_columns: cs.gridAutoCols = std::static_pointer_cast<TrackList>(v.obj()); break;
    case P_grid_auto_flow: cs.gridFlow = (uint8_t)v.kw; break;
    case P_grid_column_start: cs.gridColStart = {v.kw, (int)v.num}; break;
    case P_grid_column_end: cs.gridColEnd = {v.kw, (int)v.num}; break;
    case P_grid_row_start: cs.gridRowStart = {v.kw, (int)v.num}; break;
    case P_grid_row_end: cs.gridRowEnd = {v.kw, (int)v.num}; break;
    case P_transition_property: case P_transition_duration: case P_transition_timing_function:
    case P_transition_delay: case P_animation_name: case P_animation_duration:
    case P_animation_timing_function: case P_animation_delay: case P_animation_iteration_count:
    case P_animation_direction: case P_animation_fill_mode: case P_animation_play_state:
      cs.animLists[p - P_transition_property] = v.obj();
      break;
    case P_content:
      if (v.type == VT_KW) {
        cs.contentNone = true;
        cs.content.reset();
      } else {
        cs.contentNone = false;
        cs.content = std::static_pointer_cast<std::vector<ContentPart>>(v.obj());
      }
      break;
    case P_list_style_type: cs.listStyleType = (ListStyle)v.kw; break;
    case P_list_style_position: cs.listStyleInside = v.kw != 0; break;
    case P_border_collapse: cs.borderCollapse = v.kw != 0; break;
    case P_border_spacing: cs.borderSpacingX = px(v.len); cs.borderSpacingY = px(v.len2()); break;
    case P_table_layout: cs.tableLayout = (TableLayout)v.kw; break;
    case P_pointer_events: cs.pointerEvents = (PointerEvents)v.kw; break;
    case P_accent_color:
      if (v.type == VT_KW) cs.accentAuto = true;
      else {
        cs.accentAuto = false;
        cs.accentColor = v.color.current ? cs.color : v.color.c;
      }
      break;
    case P_caret_color:
      if (v.type == VT_KW) cs.caretAuto = true;
      else {
        cs.caretAuto = false;
        cs.caretColor = v.color.current ? cs.color : v.color.c;
      }
      break;
    case P_appearance: cs.appearance = (Appearance)v.kw; break;
    case P_object_fit: cs.objectFit = (ObjectFit)v.kw; break;
    case P_object_position: cs.objPosX = L(v.len); cs.objPosY = L(v.len2()); break;
    case P_aspect_ratio: cs.aspectRatio = v.num; break;
    case P_image_rendering: cs.pixelated = v.kw != 0; break;
    case P_scroll_snap_type: cs.scrollSnapX = v.kw & 1; cs.scrollSnapY = (v.kw & 2) != 0; break;
    case P_scroll_snap_align: cs.scrollSnapAlign = (uint8_t)v.kw; break;
    case P_font_variant_numeric: cs.tabularNums = v.kw != 0; break;
    case P_fill:
      cs.svgFillSet = true;
      cs.svgFillNone = v.type == VT_KW;
      if (v.type == VT_COLOR) cs.svgFill = v.color;
      break;
    case P_stroke:
      cs.svgStrokeSet = true;
      cs.svgStrokeNone = v.type == VT_KW;
      if (v.type == VT_COLOR) cs.svgStroke = v.color;
      break;
    case P_stroke_width:
      cs.svgStrokeWidthSet = true;
      cs.svgStrokeWidth = std::max(0.f, computeLen(v.len, rc).px);
      break;
    case P_stroke_linecap:
      cs.svgCapSet = true;
      cs.svgCap = (uint8_t)v.kw;
      break;
    case P_stroke_linejoin:
      cs.svgJoinSet = true;
      cs.svgJoin = (uint8_t)v.kw;
      break;
    default: break;
  }
}

template <class T>
static std::shared_ptr<std::vector<T>> listOf(const std::shared_ptr<void> &p) {
  return std::static_pointer_cast<std::vector<T>>(p);
}

static void buildAnimationLists(ComputedStyle &cs) {
  // transitions
  auto props = listOf<Atom>(cs.animLists[0]);
  auto durs = listOf<float>(cs.animLists[1]);
  auto tims = listOf<TimingFn>(cs.animLists[2]);
  auto dels = listOf<float>(cs.animLists[3]);
  cs.transitions.reset();
  if (durs && !durs->empty()) {
    size_t n = props && !props->empty() ? props->size() : 1;
    auto out = std::make_shared<std::vector<TransitionSpec>>();
    for (size_t i = 0; i < n; i++) {
      TransitionSpec t;
      t.prop = props && !props->empty() ? (*props)[i] : 0;
      if (t.prop == (Atom)0xFFFFFFFF) continue;
      t.duration = (*durs)[i % durs->size()];
      t.delay = dels && !dels->empty() ? (*dels)[i % dels->size()] : 0;
      if (tims && !tims->empty()) t.timing = (*tims)[i % tims->size()];
      if (t.duration > 0 || t.delay > 0) out->push_back(t);
    }
    if (!out->empty()) cs.transitions = out;
  }
  // animations
  auto names = listOf<Atom>(cs.animLists[4]);
  cs.animations.reset();
  if (names && !names->empty()) {
    auto ad = listOf<float>(cs.animLists[5]);
    auto at = listOf<TimingFn>(cs.animLists[6]);
    auto adl = listOf<float>(cs.animLists[7]);
    auto ai = listOf<float>(cs.animLists[8]);
    auto adir = listOf<float>(cs.animLists[9]);
    auto af = listOf<float>(cs.animLists[10]);
    auto ap = listOf<float>(cs.animLists[11]);
    auto out = std::make_shared<std::vector<AnimationSpec>>();
    for (size_t i = 0; i < names->size(); i++) {
      if (!(*names)[i]) continue;
      AnimationSpec a;
      a.name = (*names)[i];
      auto pick = [i](const std::shared_ptr<std::vector<float>> &v, float d) { return v && !v->empty() ? (*v)[i % v->size()] : d; };
      a.duration = pick(ad, 0);
      a.delay = pick(adl, 0);
      a.iterations = pick(ai, 1);
      a.direction = (uint8_t)pick(adir, 0);
      a.fill = (uint8_t)pick(af, 0);
      a.paused = pick(ap, 0) != 0;
      if (at && !at->empty()) a.timing = (*at)[i % at->size()];
      out->push_back(a);
    }
    if (!out->empty()) cs.animations = out;
  }
}

void StyleEngine::cascade(Element *e, const ComputedStyle *parent, ComputedStyle &out, PseudoElem pe) {
  if (indexDirty_) rebuildIndex();
  std::vector<const StyleRule *> rules;
  rules.reserve(32);
  collect(e, pe, rules);

  // start from initial + inherited
  out = initial_;
  if (parent) copyInherited(out, *parent);
  for (auto &l : out.animLists) l.reset();

  const DeclBlock *inl = (pe == PE_NONE && !(e->flags & NF_PSEUDO)) ? e->inlineStyle : nullptr;

  // ---- tiers: 0 UA normal, 1 author normal, 2 inline normal, 3 author !important, 4 inline !important, 5 UA !important
  auto tierOf = [](uint8_t layer, bool important, bool isInline) -> int {
    if (!important) return isInline ? 2 : (layer == 0 ? 0 : 1);
    return isInline ? 4 : (layer == 0 ? 5 : 3);
  };

  // pass 1: custom properties
  struct CW {
    Atom name;
    const std::string *val;
    int tier;
  };
  std::vector<CW> customs;
  auto considerCustom = [&](const Decl &d, int tier) {
    if (d.prop != P_CUSTOM) return;
    for (auto &c : customs) {
      if (c.name == d.customName) {
        if (tier >= c.tier) {
          c.val = &d.v.str();
          c.tier = tier;
        }
        return;
      }
    }
    customs.push_back({d.customName, &d.v.str(), tier});
  };
  for (const StyleRule *r : rules)
    for (auto &d : r->decls->decls) considerCustom(d, tierOf(r->layer, d.important, false));
  if (inl)
    for (auto &d : inl->decls) considerCustom(d, tierOf(1, d.important, true));
  if (!customs.empty()) {
    auto map = std::make_shared<std::vector<std::pair<Atom, std::string>>>();
    if (out.customProps) *map = *out.customProps;
    for (auto &c : customs) {
      bool found = false;
      for (auto &kv : *map) {
        if (kv.first == c.name) {
          kv.second = *c.val;
          found = true;
        }
      }
      if (!found) map->push_back({c.name, *c.val});
    }
    out.customProps = map;
    // resolve var() references between custom properties
    ComputedStyle tmp;
    tmp.customProps = map;
    for (auto &kv : *map) {
      if (kv.second.find("var(") != std::string::npos) {
        std::string r;
        if (substituteVars(kv.second, tmp, r)) kv.second = r;
        else kv.second.clear();
      }
    }
  }

  // pass 2: winners per property
  std::vector<Decl> expanded;  // var()-substituted declarations (owned here)
  expanded.reserve(16);
  struct W {
    const Decl *d = nullptr;
    int tier = -1;
  };
  W win[P__COUNT];
  std::vector<std::pair<size_t, int>> pendingExpanded;  // index into expanded, tier
  auto consider = [&](const Decl &d, int tier) {
    if (d.prop == P_CUSTOM) return;
    if (d.v.type == VT_RAW) {
      std::string sub;
      if (!substituteVars(d.v.str(), out, sub)) return;  // invalid at computed time -> unset
      std::vector<Tok> toks;
      cssTokenize(sub.data(), sub.size(), toks);
      size_t before = expanded.size();
      if (d.prop == P_SHORTHAND_RAW) {
        expandShorthandPublic(atomStr(d.customName), toks, expanded, d.important);
      } else {
        Value v;
        if (!parseLonghand((Prop)d.prop, toks, v)) return;
        Decl nd;
        nd.prop = d.prop;
        nd.important = d.important;
        nd.v = std::move(v);
        expanded.push_back(std::move(nd));
      }
      for (size_t k = before; k < expanded.size(); k++) pendingExpanded.push_back({k, tier});
      // apply in order immediately (indices stay valid because we reserve below)
      return;
    }
    if (d.prop >= P__COUNT) return;
    if (tier >= win[d.prop].tier) win[d.prop] = {&d, tier};
  };
  // To keep cascade order with expanded declarations, we process in two loops: first
  // collect (expanding var()s), then resolve winners in source order.
  struct Ent {
    const Decl *d;
    int tier;
    int exIdx;  // -1 = d valid; else index into expanded
  };
  std::vector<Ent> ents;
  ents.reserve(64);
  auto add = [&](const Decl &d, int tier) {
    if (d.v.type == VT_RAW && d.prop != P_CUSTOM) {
      size_t before = pendingExpanded.size();
      consider(d, tier);
      for (size_t k = before; k < pendingExpanded.size(); k++)
        ents.push_back({nullptr, pendingExpanded[k].second, (int)pendingExpanded[k].first});
    } else if (d.prop < P__COUNT) {
      ents.push_back({&d, tier, -1});
    }
  };
  for (const StyleRule *r : rules)
    for (auto &d : r->decls->decls) add(d, tierOf(r->layer, d.important, false));
  if (inl)
    for (auto &d : inl->decls) add(d, tierOf(1, d.important, true));
  for (auto &en : ents) {
    const Decl *d = en.exIdx >= 0 ? &expanded[en.exIdx] : en.d;
    if (d->prop >= P__COUNT) continue;
    if (en.tier >= win[d->prop].tier) win[d->prop] = {d, en.tier};
  }

  // pass 3: apply - font size first (em depends on it), then colour, then the rest
  ResolveCtx rc;
  rc.vw = env_.width;
  rc.vh = env_.height;
  rc.rem = rootFontSize();
  rc.em = parent ? parent->fontSize : 16;
  if (win[P_font_size].d) applyDecl(out, *win[P_font_size].d, parent, rc);
  if (e->tag == TAG_html && pe == PE_NONE) rc.rem = out.fontSize;
  rc.em = out.fontSize;
  rc.ch = out.fontSize * 0.55f;
  if (win[P_color].d) applyDecl(out, *win[P_color].d, parent, rc);
  if (win[P_line_height].d) applyDecl(out, *win[P_line_height].d, parent, rc);
  for (int p = 0; p < P__COUNT; p++) {
    if (!win[p].d || p == P_font_size || p == P_color || p == P_line_height) continue;
    applyDecl(out, *win[p].d, parent, rc);
  }
  (void)pendingExpanded;

  // ---- fix-ups
  Display pd = parent ? parent->display : D_BLOCK;
  bool parentFlexGrid = parent && (parent->isFlex() || parent->isGrid()) && !(e->flags & NF_INTERNAL);
  if (out.position == POS_ABSOLUTE || out.position == POS_FIXED || parentFlexGrid || e->tag == TAG_html) {
    out.display = out.display == D_NONE ? D_NONE : (out.display == D_CONTENTS ? D_CONTENTS : blockify(out.display));
    if (out.display == D_LIST_ITEM && parentFlexGrid) out.display = D_LIST_ITEM;
  }
  (void)pd;
  if (out.overflowX != OV_VISIBLE && out.overflowY == OV_VISIBLE) out.overflowY = OV_AUTO;
  if (out.overflowY != OV_VISIBLE && out.overflowX == OV_VISIBLE) out.overflowX = OV_AUTO;
  if (out.overflowX == OV_CLIP) out.overflowX = OV_HIDDEN;
  if (out.overflowY == OV_CLIP) out.overflowY = OV_HIDDEN;
  for (int i = 0; i < 4; i++)
    if (out.borderStyle[i] == BS_NONE || out.borderStyle[i] == BS_HIDDEN) out.borderW[i] = 0;
  if (out.outlineStyle == BS_NONE) out.outlineWidth = 0;
  if (out.accentAuto) out.accentColor = {37, 99, 235, 255};
  buildAnimationLists(out);
  if (overrideHook && pe == PE_NONE) overrideHook(e, out);
}

float StyleEngine::rootFontSize() const { return rootFontSize_; }

void StyleEngine::computeFor(Element *e, const ComputedStyle *parent, ComputedStyle &out, PseudoElem pe) {
  cascade(e, parent, out, pe);
}

void StyleEngine::updatePseudo(Element *e, PseudoElem pe, const std::function<void(Element *, uint8_t)> &onChange) {
  Element *&slot = pe == PE_BEFORE ? e->before : e->after;
  // do any rules target this pseudo element?
  std::vector<const StyleRule *> rules;
  if (pseudoMask_ & (1u << pe)) collect(e, pe, rules);
  if (rules.empty() || !e->cs || e->cs->display == D_NONE) {
    if (slot) {
      onChange(e, SD_LAYOUT | SD_PAINT);
      slot->unref();
      slot = nullptr;
    }
    return;
  }
  ComputedStyle *cs = new ComputedStyle();
  cascade(e, e->cs, *cs, pe);
  if (cs->contentNone || cs->display == D_NONE) {
    delete cs;
    if (slot) {
      onChange(e, SD_LAYOUT | SD_PAINT);
      slot->unref();
      slot = nullptr;
    }
    return;
  }
  // build the text
  std::string text;
  if (cs->content)
    for (auto &part : *cs->content) {
      if (part.kind == ContentPart::ATTR) text += e->attrOr(internLower(part.s), "");
      else text += part.s;
    }
  if (!slot) {
    slot = new Element(pe == PE_BEFORE ? (Atom)TAG_PSEUDO_BEFORE : (Atom)TAG_PSEUDO_AFTER);
    slot->doc = e->doc;
    slot->flags |= NF_PSEUDO | NF_CONNECTED;
    slot->parent = e;  // not linked into the child list
    slot->ref();
    slot->cs = cs;
    if (!text.empty()) {
      Text *t = new Text(text);
      t->doc = e->doc;
      slot->first = slot->last = t;
      t->parent = slot;
      t->flags |= NF_CONNECTED;
      t->ref();
    }
    onChange(e, SD_LAYOUT | SD_PAINT);
    return;
  }
  uint8_t d = diffStyles(*slot->cs, *cs);
  delete slot->cs;
  slot->cs = cs;
  Text *t = slot->first ? slot->first->asText() : nullptr;
  std::string old = t ? t->data : "";
  if (old != text) {
    if (t) {
      t->data = text;
      t->flags |= NF_TEXT_DIRTY;
    } else if (!text.empty()) {
      t = new Text(text);
      t->doc = e->doc;
      t->parent = slot;
      t->flags |= NF_CONNECTED;
      t->ref();
      slot->first = slot->last = t;
    }
    d |= SD_LAYOUT | SD_PAINT;
  }
  if (d) onChange(slot, d);
}

void StyleEngine::recalcElement(Element *e, const ComputedStyle *parent, bool force,
                                const std::function<void(Element *, uint8_t)> &onChange) {
  bool self = force || (e->flags & NF_STYLE_DIRTY) || !e->cs;
  bool forceKids = force || (e->flags & NF_SUBTREE_STYLE);
  if (self) {
    gRestyleCount++;
    static const bool dbg = getenv("RT_STYLE_DEBUG") != nullptr;
    if (dbg) {
      const std::string *c = e->getAttr(intern("class"));
      fprintf(stderr, "  restyle <%s%s%s>%s%s\n", e->tagName().c_str(), c ? " ." : "", c ? c->c_str() : "",
              (e->flags & NF_STYLE_DIRTY) ? " dirty" : "", force ? " forced" : "");
    }
    ComputedStyle *ns = new ComputedStyle();
    cascade(e, parent, *ns, PE_NONE);
    if (e->tag == TAG_html) {
      if (ns->fontSize != rootFontSize_) {
        rootFontSize_ = ns->fontSize;
        forceKids = true;
      }
    }
    if (!e->cs) {
      e->cs = ns;
      onChange(e, SD_LAYOUT | SD_PAINT | SD_STACKING);
      forceKids = true;
    } else {
      uint8_t d = diffStyles(*e->cs, *ns);
      bool inh = inheritedDiffers(*e->cs, *ns);
      delete e->cs;
      e->cs = ns;
      if (d) onChange(e, d);
      if (inh) forceKids = true;
    }
    // form control pseudo styles (::placeholder, slider parts)
    Tag t = e->tagId();
    if (t == TAG_input || t == TAG_textarea || t == TAG_dialog || t == TAG_li) {
      ElementExtra &x = e->extra();
      PseudoElem pes[] = {PE_PLACEHOLDER, PE_THUMB, PE_TRACK, PE_BACKDROP, PE_MARKER};
      for (PseudoElem pe : pes) {
        std::vector<const StyleRule *> r;
        if (pseudoMask_ & (1u << pe)) collect(e, pe, r);
        if (r.empty()) {
          x.pseudoStyles[pe].reset();
          continue;
        }
        auto ps = std::make_shared<ComputedStyle>();
        cascade(e, e->cs, *ps, pe);
        x.pseudoStyles[pe] = ps;
      }
    }
    updatePseudo(e, PE_BEFORE, onChange);
    updatePseudo(e, PE_AFTER, onChange);
  }
  e->flags &= ~(NF_STYLE_DIRTY | NF_SUBTREE_STYLE);
  bool walkKids = forceKids || (e->flags & NF_DESC_STYLE_DIRTY);
  e->flags &= ~NF_DESC_STYLE_DIRTY;
  if (!walkKids) return;
  for (Node *c = e->first; c; c = c->next)
    if (c->isElement()) recalcElement((Element *)c, e->cs, forceKids, onChange);
}

void StyleEngine::recalc(Element *root, bool force, const std::function<void(Element *, uint8_t)> &onChange) {
  if (indexDirty_) {
    rebuildIndex();
    force = true;
  }
  Element *p = root->parentElement();
  recalcElement(root, p ? p->cs : nullptr, force, onChange);
}

}  // namespace rt
