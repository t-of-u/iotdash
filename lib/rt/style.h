#pragma once
// Computed style and the cascade.

#include "css.h"

namespace rt {

enum Display : uint8_t {
  D_NONE, D_BLOCK, D_INLINE, D_INLINE_BLOCK, D_FLEX, D_INLINE_FLEX, D_GRID, D_INLINE_GRID, D_TABLE,
  D_INLINE_TABLE, D_TABLE_ROW, D_TABLE_CELL, D_TABLE_ROW_GROUP, D_TABLE_HEADER_GROUP,
  D_TABLE_FOOTER_GROUP, D_TABLE_CAPTION, D_TABLE_COLUMN, D_TABLE_COLUMN_GROUP, D_LIST_ITEM, D_CONTENTS,
  D_FLOW_ROOT
};
enum Position : uint8_t { POS_STATIC, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED, POS_STICKY };
enum BorderStyle : uint8_t { BS_NONE, BS_HIDDEN, BS_SOLID, BS_DASHED, BS_DOTTED, BS_DOUBLE, BS_GROOVE, BS_RIDGE, BS_INSET, BS_OUTSET };
enum Overflow : uint8_t { OV_VISIBLE, OV_HIDDEN, OV_SCROLL, OV_AUTO, OV_CLIP };
enum BoxSizing : uint8_t { BOX_CONTENT, BOX_BORDER };
enum TextAlign : uint8_t { TA_START, TA_LEFT, TA_RIGHT, TA_CENTER, TA_JUSTIFY, TA_END };
enum WhiteSpace : uint8_t { WS_NORMAL, WS_NOWRAP, WS_PRE, WS_PRE_WRAP, WS_PRE_LINE, WS_BREAK_SPACES };
enum TextTransform : uint8_t { TT_NONE, TT_UPPER, TT_LOWER, TT_CAPITALIZE };
enum VAlign : uint8_t { VA_BASELINE, VA_MIDDLE, VA_TOP, VA_BOTTOM, VA_TEXT_TOP, VA_TEXT_BOTTOM, VA_SUB, VA_SUPER, VA_LENGTH };
enum FlexDir : uint8_t { FD_ROW, FD_ROW_REVERSE, FD_COLUMN, FD_COLUMN_REVERSE };
enum FlexWrap : uint8_t { FW_NOWRAP, FW_WRAP, FW_WRAP_REVERSE };
enum Align : uint8_t {
  AL_AUTO, AL_NORMAL, AL_STRETCH, AL_START, AL_END, AL_CENTER, AL_BASELINE, AL_SPACE_BETWEEN,
  AL_SPACE_AROUND, AL_SPACE_EVENLY, AL_LEFT, AL_RIGHT
};
enum Visibility : uint8_t { VIS_VISIBLE, VIS_HIDDEN, VIS_COLLAPSE };
enum PointerEvents : uint8_t { PE_AUTO_, PE_NONE_ };
enum ListStyle : uint8_t { LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_NONE, LS_LOWER_ALPHA, LS_UPPER_ALPHA, LS_LOWER_ROMAN, LS_UPPER_ROMAN, LS_THAI };
enum ObjectFit : uint8_t { OF_FILL, OF_CONTAIN, OF_COVER, OF_NONE, OF_SCALE_DOWN };
enum BgRepeat : uint8_t { BR_REPEAT, BR_NO_REPEAT, BR_REPEAT_X, BR_REPEAT_Y };
enum BgSizeKind : uint8_t { BGS_AUTO, BGS_COVER, BGS_CONTAIN, BGS_LEN };
enum TextOverflow : uint8_t { TO_CLIP, TO_ELLIPSIS };
enum WordBreak : uint8_t { WB_NORMAL, WB_BREAK_ALL, WB_KEEP_ALL, WB_BREAK_WORD };
enum OverflowWrap : uint8_t { OW_NORMAL, OW_ANYWHERE, OW_BREAK_WORD };
enum TextDecoLine : uint8_t { TD_UNDERLINE = 1, TD_OVERLINE = 2, TD_LINE_THROUGH = 4 };
enum GridFlow : uint8_t { GF_ROW = 0, GF_COLUMN = 1, GF_DENSE = 2 };
enum Appearance : uint8_t { AP_AUTO, AP_NONE };
enum TableLayout : uint8_t { TL_AUTO, TL_FIXED };

struct FilterFn {
  enum Kind : uint8_t { GRAYSCALE, BRIGHTNESS, CONTRAST, INVERT, OPACITY, SEPIA, SATURATE, BLUR, DROP_SHADOW, HUE_ROTATE } kind;
  float v;
};

struct ComputedStyle {
  // --- box ---
  Display display = D_INLINE;
  Position position = POS_STATIC;
  Len top = Len::Kind(LK_AUTO), right = Len::Kind(LK_AUTO), bottom = Len::Kind(LK_AUTO), left = Len::Kind(LK_AUTO);
  int zIndex = 0;
  bool zAuto = true;
  Len width = Len::Kind(LK_AUTO), height = Len::Kind(LK_AUTO);
  Len minWidth = Len::Kind(LK_AUTO), minHeight = Len::Kind(LK_AUTO);
  Len maxWidth = Len::Kind(LK_NONE), maxHeight = Len::Kind(LK_NONE);
  Len margin[4];   // top right bottom left
  Len padding[4];
  float borderW[4] = {0, 0, 0, 0};
  BorderStyle borderStyle[4] = {BS_NONE, BS_NONE, BS_NONE, BS_NONE};
  ColorSpec borderColor[4] = {{{0, 0, 0, 255}, true}, {{0, 0, 0, 255}, true}, {{0, 0, 0, 255}, true}, {{0, 0, 0, 255}, true}};
  Len radius[4];  // tl tr br bl
  BoxSizing boxSizing = BOX_CONTENT;
  Overflow overflowX = OV_VISIBLE, overflowY = OV_VISIBLE;
  Visibility visibility = VIS_VISIBLE;
  float opacity = 1;
  float aspectRatio = 0;  // 0 = auto

  // --- background ---
  Color bgColor = kTransparent;
  std::shared_ptr<std::vector<BgLayer>> bgImages;
  BgSizeKind bgSizeKind = BGS_AUTO;
  Len bgSizeX = Len::Kind(LK_AUTO), bgSizeY = Len::Kind(LK_AUTO);
  Len bgPosX = Len(), bgPosY = Len();
  BgRepeat bgRepeat = BR_REPEAT;
  uint8_t bgClip = 0;  // 0 border-box 1 padding-box 2 content-box 3 text

  // --- text (inherited) ---
  Color color = kBlack;
  std::shared_ptr<std::vector<std::string>> fontFamily;
  float fontSize = 16;
  int fontWeight = 400;
  bool italic = false;
  float lineHeight = -1;  // <0: normal; >0 px when lineHeightIsNum == false
  float lineHeightNum = 0;  // multiplier when set
  bool lineHeightIsNum = false;
  float letterSpacing = 0;
  float wordSpacing = 0;
  TextAlign textAlign = TA_START;
  TextTransform textTransform = TT_NONE;
  WhiteSpace whiteSpace = WS_NORMAL;
  WordBreak wordBreak = WB_NORMAL;
  OverflowWrap overflowWrap = OW_NORMAL;
  float textIndent = 0;
  std::shared_ptr<std::vector<Shadow>> textShadow;
  Visibility inheritedVisibility = VIS_VISIBLE;
  ListStyle listStyleType = LS_DISC;
  bool listStyleInside = false;
  bool borderCollapse = false;
  float borderSpacingX = 0, borderSpacingY = 0;
  Color caretColor = kTransparent;  // transparent = auto (currentColor)
  bool caretAuto = true;
  Color accentColor = {37, 99, 235, 255};
  bool accentAuto = true;
  PointerEvents pointerEvents = PE_AUTO_;
  bool tabularNums = false;
  std::shared_ptr<std::vector<std::pair<Atom, std::string>>> customProps;  // --vars (inherited)
  // SVG paint (inherited values; *Set flags mark a value set by CSS on this element)
  ColorSpec svgFill{{0, 0, 0, 255}, false}, svgStroke{{0, 0, 0, 255}, false};
  bool svgFillNone = false, svgStrokeNone = true;
  float svgStrokeWidth = 1;
  bool svgFillSet = false, svgStrokeSet = false, svgStrokeWidthSet = false;
  uint8_t svgCap = 0, svgJoin = 0;  // LineCap / LineJoin (stroke-linecap / stroke-linejoin)
  bool svgCapSet = false, svgJoinSet = false;

  // --- text (not inherited) ---
  uint8_t textDecoLine = 0;
  ColorSpec textDecoColor{{0, 0, 0, 255}, true};
  TextOverflow textOverflow = TO_CLIP;
  VAlign verticalAlign = VA_BASELINE;
  float verticalAlignLen = 0;
  int lineClamp = 0;

  // --- effects ---
  std::shared_ptr<std::vector<Shadow>> boxShadow;
  float outlineWidth = 3;
  BorderStyle outlineStyle = BS_NONE;
  ColorSpec outlineColor{{0, 0, 0, 255}, true};
  float outlineOffset = 0;
  std::shared_ptr<std::vector<TransformFn>> transform;
  Len originX = Len{0, 50, LK_LEN}, originY = Len{0, 50, LK_LEN};
  std::shared_ptr<std::vector<FilterFn>> filter;

  // --- flex / grid ---
  FlexDir flexDirection = FD_ROW;
  FlexWrap flexWrap = FW_NOWRAP;
  Align justifyContent = AL_NORMAL, alignItems = AL_NORMAL, alignSelf = AL_AUTO, alignContent = AL_NORMAL;
  Align justifyItems = AL_NORMAL, justifySelf = AL_AUTO;
  float flexGrow = 0, flexShrink = 1;
  Len flexBasis = Len::Kind(LK_AUTO);
  int order = 0;
  Len rowGap = Len::Kind(LK_NORMAL), columnGap = Len::Kind(LK_NORMAL);
  std::shared_ptr<TrackList> gridCols, gridRows;
  std::shared_ptr<TrackList> gridAutoRows, gridAutoCols;
  uint8_t gridFlow = GF_ROW;
  GridLine gridColStart, gridColEnd, gridRowStart, gridRowEnd;

  // --- animation ---
  std::shared_ptr<std::vector<TransitionSpec>> transitions;  // built from the longhand lists
  std::shared_ptr<std::vector<AnimationSpec>> animations;
  // Longhand lists as specified (index = P_transition_property .. P_animation_play_state).
  std::shared_ptr<void> animLists[12];

  // --- misc ---
  std::shared_ptr<std::vector<ContentPart>> content;  // ::before/::after
  bool contentNone = true;
  TableLayout tableLayout = TL_AUTO;
  Appearance appearance = AP_AUTO;
  ObjectFit objectFit = OF_FILL;
  Len objPosX = Len{0, 50, LK_LEN}, objPosY = Len{0, 50, LK_LEN};
  bool pixelated = false;
  bool scrollSnapX = false, scrollSnapY = false;
  uint8_t scrollSnapAlign = 0;  // 0 none 1 start 2 center 3 end

  // derived helpers
  bool isFlex() const { return display == D_FLEX || display == D_INLINE_FLEX; }
  bool isGrid() const { return display == D_GRID || display == D_INLINE_GRID; }
  bool isBlockLevel() const;
  bool isInlineLevel() const { return !isBlockLevel(); }
  bool isPositioned() const { return position != POS_STATIC; }
  bool isOutOfFlow() const { return position == POS_ABSOLUTE || position == POS_FIXED; }
  bool clips() const { return overflowX != OV_VISIBLE || overflowY != OV_VISIBLE; }
  bool scrollableX() const { return overflowX == OV_AUTO || overflowX == OV_SCROLL; }
  bool scrollableY() const { return overflowY == OV_AUTO || overflowY == OV_SCROLL; }
  bool hasTransform() const { return transform && !transform->empty(); }
  bool makesStackingContext() const {
    return (position != POS_STATIC && !zAuto) || opacity < 1 || hasTransform() || (filter && !filter->empty()) || position == POS_FIXED;
  }
  float borderTop() const { return borderStyle[0] == BS_NONE || borderStyle[0] == BS_HIDDEN ? 0 : borderW[0]; }
  float borderRight() const { return borderStyle[1] == BS_NONE || borderStyle[1] == BS_HIDDEN ? 0 : borderW[1]; }
  float borderBottom() const { return borderStyle[2] == BS_NONE || borderStyle[2] == BS_HIDDEN ? 0 : borderW[2]; }
  float borderLeft() const { return borderStyle[3] == BS_NONE || borderStyle[3] == BS_HIDDEN ? 0 : borderW[3]; }
  float usedLineHeight(float normal) const {
    if (lineHeightIsNum) return lineHeightNum * fontSize;
    if (lineHeight > 0) return lineHeight;
    return normal;
  }
  Color resolve(const ColorSpec &c) const { return c.current ? color : c.c; }
};

// Bits describing what a style change affects.
enum StyleDiff : uint8_t { SD_NONE = 0, SD_PAINT = 1, SD_LAYOUT = 2, SD_STACKING = 4, SD_TEXT = 8 };
uint8_t diffStyles(const ComputedStyle &a, const ComputedStyle &b);

class StyleEngine {
 public:
  void init(const MediaEnv &env);
  void clearAuthor();
  void addAuthorSheet(const std::string &css);
  void addUserAgentSheet(const std::string &css);
  const Keyframes *findKeyframes(Atom name) const;

  // Recompute styles for dirty elements under root (or everything when force).
  // Calls onChange(e, diff) for elements whose computed style changed.
  void recalc(Element *root, bool force, const std::function<void(Element *, uint8_t)> &onChange);
  // Compute the style an element would get (no caching); used for pseudo elements.
  void computeFor(Element *e, const ComputedStyle *parent, ComputedStyle &out, PseudoElem pe);
  // Apply one declaration onto a computed style (used by animations / inline overrides).
  void applyDecl(ComputedStyle &cs, const Decl &d, const ComputedStyle *parent, const ResolveCtx &rc);
  const MediaEnv &env() const { return env_; }
  uint32_t generation() const { return gen_; }
  void bump() { gen_++; }

  // Animated values: (element, property) overrides applied after the cascade.
  std::function<void(Element *, ComputedStyle &)> overrideHook;

 private:
  struct Bucketed {
    std::unordered_map<Atom, std::vector<const StyleRule *>> byId, byClass, byTag;
    std::vector<const StyleRule *> universal;
  };
  void rebuildIndex();
  void collect(Element *e, PseudoElem pe, std::vector<const StyleRule *> &out) const;
  void cascade(Element *e, const ComputedStyle *parent, ComputedStyle &out, PseudoElem pe);
  void recalcElement(Element *e, const ComputedStyle *parent, bool force,
                     const std::function<void(Element *, uint8_t)> &onChange);
  void updatePseudo(Element *e, PseudoElem pe, const std::function<void(Element *, uint8_t)> &onChange);

  float rootFontSize() const;
  MediaEnv env_;
  StyleSheet ua_, author_;
  float rootFontSize_ = 16;
  uint32_t pseudoMask_ = 0;  // which pseudo-elements any rule targets
  uint32_t order_ = 0;
  Bucketed idx_;
  bool indexDirty_ = true;
  uint32_t gen_ = 1;
  ComputedStyle initial_;
};

// Built-in user-agent stylesheet (HTML defaults + form controls).
extern const char *kUserAgentCss;

}  // namespace rt
