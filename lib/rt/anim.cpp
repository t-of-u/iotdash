#include "anim.h"

namespace rt {

// ============================================================ property tables

static const struct {
  const char *name;
  AnimProp p;
} kAnimNames[] = {
    {"opacity", AP_OPACITY}, {"transform", AP_TRANSFORM}, {"color", AP_COLOR},
    {"background-color", AP_BG_COLOR}, {"border-top-color", AP_BORDER_TOP_COLOR},
    {"border-right-color", AP_BORDER_RIGHT_COLOR}, {"border-bottom-color", AP_BORDER_BOTTOM_COLOR},
    {"border-left-color", AP_BORDER_LEFT_COLOR}, {"outline-color", AP_OUTLINE_COLOR}, {"width", AP_WIDTH},
    {"height", AP_HEIGHT}, {"min-width", AP_MIN_WIDTH}, {"min-height", AP_MIN_HEIGHT}, {"max-width", AP_MAX_WIDTH},
    {"max-height", AP_MAX_HEIGHT}, {"top", AP_TOP}, {"right", AP_RIGHT}, {"bottom", AP_BOTTOM}, {"left", AP_LEFT},
    {"margin-top", AP_MARGIN_TOP}, {"margin-right", AP_MARGIN_RIGHT}, {"margin-bottom", AP_MARGIN_BOTTOM},
    {"margin-left", AP_MARGIN_LEFT}, {"padding-top", AP_PADDING_TOP}, {"padding-right", AP_PADDING_RIGHT},
    {"padding-bottom", AP_PADDING_BOTTOM}, {"padding-left", AP_PADDING_LEFT},
    {"border-top-left-radius", AP_RADIUS_TL}, {"border-top-right-radius", AP_RADIUS_TR},
    {"border-bottom-right-radius", AP_RADIUS_BR}, {"border-bottom-left-radius", AP_RADIUS_BL},
    {"font-size", AP_FONT_SIZE}, {"letter-spacing", AP_LETTER_SPACING}, {"box-shadow", AP_BOX_SHADOW},
    {"row-gap", AP_ROW_GAP}, {"column-gap", AP_COLUMN_GAP}, {"flex-grow", AP_FLEX_GROW},
    {"flex-basis", AP_FLEX_BASIS}, {"visibility", AP_VISIBILITY}, {"border-top-width", AP_BORDER_TOP_WIDTH},
    {"border-right-width", AP_BORDER_RIGHT_WIDTH}, {"border-bottom-width", AP_BORDER_BOTTOM_WIDTH},
    {"border-left-width", AP_BORDER_LEFT_WIDTH}, {"outline-width", AP_OUTLINE_WIDTH},
    {"outline-offset", AP_OUTLINE_OFFSET}, {"line-height", AP_LINE_HEIGHT}, {"text-shadow", AP_TEXT_SHADOW},
    {"background-position-x", AP_BG_POSITION_X}, {"background-position-y", AP_BG_POSITION_Y},
    {"filter", AP_FILTER}, {"accent-color", AP_ACCENT_COLOR}, {"text-decoration-color", AP_TEXT_DECO_COLOR},
    {"z-index", AP_Z_INDEX}, {"display", AP_DISPLAY},
};

int animPropByName(const std::string &n) {
  for (auto &e : kAnimNames)
    if (n == e.name) return e.p;
  return -1;
}

// Shorthands covered by a transition-property name.
static void animPropsFor(const std::string &n, std::vector<AnimProp> &out) {
  int p = animPropByName(n);
  if (p >= 0) {
    out.push_back((AnimProp)p);
    return;
  }
  if (n == "all") {
    for (int i = 0; i < AP__COUNT; i++)
      if (i != AP_DISPLAY && i != AP_Z_INDEX) out.push_back((AnimProp)i);
  } else if (n == "border-color") {
    out.insert(out.end(), {AP_BORDER_TOP_COLOR, AP_BORDER_RIGHT_COLOR, AP_BORDER_BOTTOM_COLOR, AP_BORDER_LEFT_COLOR});
  } else if (n == "border" || n == "border-width") {
    out.insert(out.end(), {AP_BORDER_TOP_WIDTH, AP_BORDER_RIGHT_WIDTH, AP_BORDER_BOTTOM_WIDTH, AP_BORDER_LEFT_WIDTH});
    if (n == "border")
      out.insert(out.end(), {AP_BORDER_TOP_COLOR, AP_BORDER_RIGHT_COLOR, AP_BORDER_BOTTOM_COLOR, AP_BORDER_LEFT_COLOR});
  } else if (n == "margin") {
    out.insert(out.end(), {AP_MARGIN_TOP, AP_MARGIN_RIGHT, AP_MARGIN_BOTTOM, AP_MARGIN_LEFT});
  } else if (n == "padding") {
    out.insert(out.end(), {AP_PADDING_TOP, AP_PADDING_RIGHT, AP_PADDING_BOTTOM, AP_PADDING_LEFT});
  } else if (n == "border-radius") {
    out.insert(out.end(), {AP_RADIUS_TL, AP_RADIUS_TR, AP_RADIUS_BR, AP_RADIUS_BL});
  } else if (n == "inset") {
    out.insert(out.end(), {AP_TOP, AP_RIGHT, AP_BOTTOM, AP_LEFT});
  } else if (n == "gap") {
    out.insert(out.end(), {AP_ROW_GAP, AP_COLUMN_GAP});
  } else if (n == "background") {
    out.insert(out.end(), {AP_BG_COLOR, AP_BG_POSITION_X, AP_BG_POSITION_Y});
  } else if (n == "background-position") {
    out.insert(out.end(), {AP_BG_POSITION_X, AP_BG_POSITION_Y});
  } else if (n == "outline") {
    out.insert(out.end(), {AP_OUTLINE_COLOR, AP_OUTLINE_WIDTH, AP_OUTLINE_OFFSET});
  } else if (n == "flex") {
    out.insert(out.end(), {AP_FLEX_GROW, AP_FLEX_BASIS});
  } else if (n == "font") {
    out.insert(out.end(), {AP_FONT_SIZE, AP_LINE_HEIGHT, AP_LETTER_SPACING});
  } else if (n == "translate" || n == "scale" || n == "rotate") {
    out.push_back(AP_TRANSFORM);
  }
}

static int animPropForCss(uint16_t p) {
  switch ((Prop)p) {
    case P_opacity: return AP_OPACITY;
    case P_transform: return AP_TRANSFORM;
    case P_color: return AP_COLOR;
    case P_background_color: return AP_BG_COLOR;
    case P_border_top_color: return AP_BORDER_TOP_COLOR;
    case P_border_right_color: return AP_BORDER_RIGHT_COLOR;
    case P_border_bottom_color: return AP_BORDER_BOTTOM_COLOR;
    case P_border_left_color: return AP_BORDER_LEFT_COLOR;
    case P_outline_color: return AP_OUTLINE_COLOR;
    case P_width: return AP_WIDTH;
    case P_height: return AP_HEIGHT;
    case P_min_width: return AP_MIN_WIDTH;
    case P_min_height: return AP_MIN_HEIGHT;
    case P_max_width: return AP_MAX_WIDTH;
    case P_max_height: return AP_MAX_HEIGHT;
    case P_top: return AP_TOP;
    case P_right: return AP_RIGHT;
    case P_bottom: return AP_BOTTOM;
    case P_left: return AP_LEFT;
    case P_margin_top: return AP_MARGIN_TOP;
    case P_margin_right: return AP_MARGIN_RIGHT;
    case P_margin_bottom: return AP_MARGIN_BOTTOM;
    case P_margin_left: return AP_MARGIN_LEFT;
    case P_padding_top: return AP_PADDING_TOP;
    case P_padding_right: return AP_PADDING_RIGHT;
    case P_padding_bottom: return AP_PADDING_BOTTOM;
    case P_padding_left: return AP_PADDING_LEFT;
    case P_border_top_left_radius: return AP_RADIUS_TL;
    case P_border_top_right_radius: return AP_RADIUS_TR;
    case P_border_bottom_right_radius: return AP_RADIUS_BR;
    case P_border_bottom_left_radius: return AP_RADIUS_BL;
    case P_font_size: return AP_FONT_SIZE;
    case P_letter_spacing: return AP_LETTER_SPACING;
    case P_box_shadow: return AP_BOX_SHADOW;
    case P_row_gap: return AP_ROW_GAP;
    case P_column_gap: return AP_COLUMN_GAP;
    case P_flex_grow: return AP_FLEX_GROW;
    case P_flex_basis: return AP_FLEX_BASIS;
    case P_visibility: return AP_VISIBILITY;
    case P_border_top_width: return AP_BORDER_TOP_WIDTH;
    case P_border_right_width: return AP_BORDER_RIGHT_WIDTH;
    case P_border_bottom_width: return AP_BORDER_BOTTOM_WIDTH;
    case P_border_left_width: return AP_BORDER_LEFT_WIDTH;
    case P_outline_width: return AP_OUTLINE_WIDTH;
    case P_outline_offset: return AP_OUTLINE_OFFSET;
    case P_line_height: return AP_LINE_HEIGHT;
    case P_text_shadow: return AP_TEXT_SHADOW;
    case P_background_position: return AP_BG_POSITION_X;
    case P_filter: return AP_FILTER;
    case P_accent_color: return AP_ACCENT_COLOR;
    case P_text_decoration_color: return AP_TEXT_DECO_COLOR;
    case P_z_index: return AP_Z_INDEX;
    case P_display: return AP_DISPLAY;
    default: return -1;
  }
}

bool animPropAffectsLayout(AnimProp p) {
  switch (p) {
    case AP_OPACITY: case AP_TRANSFORM: case AP_COLOR: case AP_BG_COLOR: case AP_BORDER_TOP_COLOR:
    case AP_BORDER_RIGHT_COLOR: case AP_BORDER_BOTTOM_COLOR: case AP_BORDER_LEFT_COLOR: case AP_OUTLINE_COLOR:
    case AP_RADIUS_TL: case AP_RADIUS_TR: case AP_RADIUS_BR: case AP_RADIUS_BL: case AP_BOX_SHADOW:
    case AP_VISIBILITY: case AP_OUTLINE_WIDTH: case AP_OUTLINE_OFFSET: case AP_TEXT_SHADOW:
    case AP_BG_POSITION_X: case AP_BG_POSITION_Y: case AP_FILTER: case AP_ACCENT_COLOR:
    case AP_TEXT_DECO_COLOR: case AP_Z_INDEX:
      return false;
    default:
      return true;
  }
}

// ============================================================ get / set / lerp

static AnimValue F(float f) {
  AnimValue v;
  v.f = f;
  return v;
}
static AnimValue Lv(const Len &l) {
  AnimValue v;
  v.len = l;
  return v;
}
static AnimValue Cv(Color c) {
  AnimValue v;
  v.color = c;
  return v;
}

AnimValue animGet(const ComputedStyle &s, AnimProp p) {
  switch (p) {
    case AP_OPACITY: return F(s.opacity);
    case AP_TRANSFORM: {
      AnimValue v;
      v.tf = s.transform;
      return v;
    }
    case AP_COLOR: return Cv(s.color);
    case AP_BG_COLOR: return Cv(s.bgColor);
    case AP_BORDER_TOP_COLOR: return Cv(s.resolve(s.borderColor[0]));
    case AP_BORDER_RIGHT_COLOR: return Cv(s.resolve(s.borderColor[1]));
    case AP_BORDER_BOTTOM_COLOR: return Cv(s.resolve(s.borderColor[2]));
    case AP_BORDER_LEFT_COLOR: return Cv(s.resolve(s.borderColor[3]));
    case AP_OUTLINE_COLOR: return Cv(s.resolve(s.outlineColor));
    case AP_TEXT_DECO_COLOR: return Cv(s.resolve(s.textDecoColor));
    case AP_ACCENT_COLOR: return Cv(s.accentColor);
    case AP_WIDTH: return Lv(s.width);
    case AP_HEIGHT: return Lv(s.height);
    case AP_MIN_WIDTH: return Lv(s.minWidth);
    case AP_MIN_HEIGHT: return Lv(s.minHeight);
    case AP_MAX_WIDTH: return Lv(s.maxWidth);
    case AP_MAX_HEIGHT: return Lv(s.maxHeight);
    case AP_TOP: return Lv(s.top);
    case AP_RIGHT: return Lv(s.right);
    case AP_BOTTOM: return Lv(s.bottom);
    case AP_LEFT: return Lv(s.left);
    case AP_MARGIN_TOP: return Lv(s.margin[0]);
    case AP_MARGIN_RIGHT: return Lv(s.margin[1]);
    case AP_MARGIN_BOTTOM: return Lv(s.margin[2]);
    case AP_MARGIN_LEFT: return Lv(s.margin[3]);
    case AP_PADDING_TOP: return Lv(s.padding[0]);
    case AP_PADDING_RIGHT: return Lv(s.padding[1]);
    case AP_PADDING_BOTTOM: return Lv(s.padding[2]);
    case AP_PADDING_LEFT: return Lv(s.padding[3]);
    case AP_RADIUS_TL: return Lv(s.radius[0]);
    case AP_RADIUS_TR: return Lv(s.radius[1]);
    case AP_RADIUS_BR: return Lv(s.radius[2]);
    case AP_RADIUS_BL: return Lv(s.radius[3]);
    case AP_FONT_SIZE: return F(s.fontSize);
    case AP_LETTER_SPACING: return F(s.letterSpacing);
    case AP_BOX_SHADOW: {
      AnimValue v;
      v.sh = s.boxShadow;
      return v;
    }
    case AP_TEXT_SHADOW: {
      AnimValue v;
      v.sh = s.textShadow;
      return v;
    }
    case AP_ROW_GAP: return Lv(s.rowGap);
    case AP_COLUMN_GAP: return Lv(s.columnGap);
    case AP_FLEX_GROW: return F(s.flexGrow);
    case AP_FLEX_BASIS: return Lv(s.flexBasis);
    case AP_VISIBILITY: {
      AnimValue v;
      v.i = s.visibility;
      return v;
    }
    case AP_BORDER_TOP_WIDTH: return F(s.borderW[0]);
    case AP_BORDER_RIGHT_WIDTH: return F(s.borderW[1]);
    case AP_BORDER_BOTTOM_WIDTH: return F(s.borderW[2]);
    case AP_BORDER_LEFT_WIDTH: return F(s.borderW[3]);
    case AP_OUTLINE_WIDTH: return F(s.outlineWidth);
    case AP_OUTLINE_OFFSET: return F(s.outlineOffset);
    case AP_LINE_HEIGHT: return F(s.lineHeightIsNum ? -s.lineHeightNum : s.lineHeight);
    case AP_BG_POSITION_X: return Lv(s.bgPosX);
    case AP_BG_POSITION_Y: return Lv(s.bgPosY);
    case AP_FILTER: {
      AnimValue v;
      v.filter = s.filter;
      return v;
    }
    case AP_Z_INDEX: {
      AnimValue v;
      v.i = s.zAuto ? INT32_MIN : s.zIndex;
      return v;
    }
    case AP_DISPLAY: {
      AnimValue v;
      v.i = s.display;
      return v;
    }
    default: return AnimValue();
  }
}

void animSet(ComputedStyle &s, AnimProp p, const AnimValue &v) {
  switch (p) {
    case AP_OPACITY: s.opacity = clampf(v.f, 0, 1); break;
    case AP_TRANSFORM: s.transform = v.tf; break;
    case AP_COLOR: s.color = v.color; break;
    case AP_BG_COLOR: s.bgColor = v.color; break;
    case AP_BORDER_TOP_COLOR: s.borderColor[0] = {v.color, false}; break;
    case AP_BORDER_RIGHT_COLOR: s.borderColor[1] = {v.color, false}; break;
    case AP_BORDER_BOTTOM_COLOR: s.borderColor[2] = {v.color, false}; break;
    case AP_BORDER_LEFT_COLOR: s.borderColor[3] = {v.color, false}; break;
    case AP_OUTLINE_COLOR: s.outlineColor = {v.color, false}; break;
    case AP_TEXT_DECO_COLOR: s.textDecoColor = {v.color, false}; break;
    case AP_ACCENT_COLOR: s.accentColor = v.color; break;
    case AP_WIDTH: s.width = v.len; break;
    case AP_HEIGHT: s.height = v.len; break;
    case AP_MIN_WIDTH: s.minWidth = v.len; break;
    case AP_MIN_HEIGHT: s.minHeight = v.len; break;
    case AP_MAX_WIDTH: s.maxWidth = v.len; break;
    case AP_MAX_HEIGHT: s.maxHeight = v.len; break;
    case AP_TOP: s.top = v.len; break;
    case AP_RIGHT: s.right = v.len; break;
    case AP_BOTTOM: s.bottom = v.len; break;
    case AP_LEFT: s.left = v.len; break;
    case AP_MARGIN_TOP: s.margin[0] = v.len; break;
    case AP_MARGIN_RIGHT: s.margin[1] = v.len; break;
    case AP_MARGIN_BOTTOM: s.margin[2] = v.len; break;
    case AP_MARGIN_LEFT: s.margin[3] = v.len; break;
    case AP_PADDING_TOP: s.padding[0] = v.len; break;
    case AP_PADDING_RIGHT: s.padding[1] = v.len; break;
    case AP_PADDING_BOTTOM: s.padding[2] = v.len; break;
    case AP_PADDING_LEFT: s.padding[3] = v.len; break;
    case AP_RADIUS_TL: s.radius[0] = v.len; break;
    case AP_RADIUS_TR: s.radius[1] = v.len; break;
    case AP_RADIUS_BR: s.radius[2] = v.len; break;
    case AP_RADIUS_BL: s.radius[3] = v.len; break;
    case AP_FONT_SIZE: s.fontSize = std::max(0.f, v.f); break;
    case AP_LETTER_SPACING: s.letterSpacing = v.f; break;
    case AP_BOX_SHADOW: s.boxShadow = v.sh; break;
    case AP_TEXT_SHADOW: s.textShadow = v.sh; break;
    case AP_ROW_GAP: s.rowGap = v.len; break;
    case AP_COLUMN_GAP: s.columnGap = v.len; break;
    case AP_FLEX_GROW: s.flexGrow = std::max(0.f, v.f); break;
    case AP_FLEX_BASIS: s.flexBasis = v.len; break;
    case AP_VISIBILITY: s.visibility = (Visibility)v.i; break;
    case AP_BORDER_TOP_WIDTH: s.borderW[0] = std::max(0.f, v.f); break;
    case AP_BORDER_RIGHT_WIDTH: s.borderW[1] = std::max(0.f, v.f); break;
    case AP_BORDER_BOTTOM_WIDTH: s.borderW[2] = std::max(0.f, v.f); break;
    case AP_BORDER_LEFT_WIDTH: s.borderW[3] = std::max(0.f, v.f); break;
    case AP_OUTLINE_WIDTH: s.outlineWidth = std::max(0.f, v.f); break;
    case AP_OUTLINE_OFFSET: s.outlineOffset = v.f; break;
    case AP_LINE_HEIGHT:
      if (v.f < 0) {
        s.lineHeightIsNum = true;
        s.lineHeightNum = -v.f;
      } else {
        s.lineHeightIsNum = false;
        s.lineHeight = v.f;
      }
      break;
    case AP_BG_POSITION_X: s.bgPosX = v.len; break;
    case AP_BG_POSITION_Y: s.bgPosY = v.len; break;
    case AP_FILTER: s.filter = v.filter; break;
    case AP_Z_INDEX:
      if (v.i == INT32_MIN) s.zAuto = true;
      else {
        s.zAuto = false;
        s.zIndex = v.i;
      }
      break;
    case AP_DISPLAY: s.display = (Display)v.i; break;
    default: break;
  }
}

static bool lenEq(const Len &a, const Len &b) { return a == b; }

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static Len lerpLen(const Len &a, const Len &b, float t) {
  if (a.kind != LK_LEN || b.kind != LK_LEN || a.calc || b.calc) return t < 0.5f ? a : b;
  Len r;
  r.px = lerpf(a.px, b.px, t);
  r.pct = lerpf(a.pct, b.pct, t);
  return r;
}

static LenSpec lerpSpec(const LenSpec &a, const LenSpec &b, float t) {
  LenSpec r;
  r.px = lerpf(a.px, b.px, t);
  r.pct = lerpf(a.pct, b.pct, t);
  r.em = lerpf(a.em, b.em, t);
  r.rem = lerpf(a.rem, b.rem, t);
  r.vw = lerpf(a.vw, b.vw, t);
  r.vh = lerpf(a.vh, b.vh, t);
  r.ch = lerpf(a.ch, b.ch, t);
  return r;
}

static TransformFn identityLike(const TransformFn &f) {
  TransformFn r;
  r.kind = f.kind;
  if (f.kind == TransformFn::SCALE) r.v[0] = r.v[1] = 1;
  if (f.kind == TransformFn::MATRIX) {
    r.v[0] = r.v[3] = 1;
  }
  return r;
}

static std::shared_ptr<std::vector<TransformFn>> lerpTransform(const std::shared_ptr<std::vector<TransformFn>> &a,
                                                               const std::shared_ptr<std::vector<TransformFn>> &b, float t) {
  std::vector<TransformFn> A = a ? *a : std::vector<TransformFn>(), B = b ? *b : std::vector<TransformFn>();
  // pad the shorter list with identity functions of the other's kinds
  if (A.size() < B.size())
    for (size_t i = A.size(); i < B.size(); i++) A.push_back(identityLike(B[i]));
  if (B.size() < A.size())
    for (size_t i = B.size(); i < A.size(); i++) B.push_back(identityLike(A[i]));
  for (size_t i = 0; i < A.size(); i++)
    if (A[i].kind != B[i].kind) return t < 0.5f ? a : b;
  auto out = std::make_shared<std::vector<TransformFn>>();
  for (size_t i = 0; i < A.size(); i++) {
    TransformFn f;
    f.kind = A[i].kind;
    f.a = lerpSpec(A[i].a, B[i].a, t);
    f.b = lerpSpec(A[i].b, B[i].b, t);
    for (int k = 0; k < 6; k++) f.v[k] = lerpf(A[i].v[k], B[i].v[k], t);
    out->push_back(f);
  }
  return out;
}

static std::shared_ptr<std::vector<Shadow>> lerpShadows(const std::shared_ptr<std::vector<Shadow>> &a,
                                                        const std::shared_ptr<std::vector<Shadow>> &b, float t) {
  std::vector<Shadow> A = a ? *a : std::vector<Shadow>(), B = b ? *b : std::vector<Shadow>();
  auto transparentOf = [](const Shadow &s) {
    Shadow r = s;
    r.x = r.y = r.blur = r.spread = LenSpec();
    r.color = {s.color.c.withAlpha(0), false};
    return r;
  };
  while (A.size() < B.size()) A.push_back(transparentOf(B[A.size()]));
  while (B.size() < A.size()) B.push_back(transparentOf(A[B.size()]));
  auto out = std::make_shared<std::vector<Shadow>>();
  for (size_t i = 0; i < A.size(); i++) {
    if (A[i].inset != B[i].inset) return t < 0.5f ? a : b;
    Shadow s = A[i];
    s.x = lerpSpec(A[i].x, B[i].x, t);
    s.y = lerpSpec(A[i].y, B[i].y, t);
    s.blur = lerpSpec(A[i].blur, B[i].blur, t);
    s.spread = lerpSpec(A[i].spread, B[i].spread, t);
    s.color = {lerpColor(A[i].color.c, B[i].color.c, t), false};
    out->push_back(s);
  }
  if (out->empty()) return nullptr;
  return out;
}

AnimValue animLerp(AnimProp p, const AnimValue &a, const AnimValue &b, float t) {
  switch (p) {
    case AP_OPACITY: case AP_FONT_SIZE: case AP_LETTER_SPACING: case AP_FLEX_GROW: case AP_BORDER_TOP_WIDTH:
    case AP_BORDER_RIGHT_WIDTH: case AP_BORDER_BOTTOM_WIDTH: case AP_BORDER_LEFT_WIDTH: case AP_OUTLINE_WIDTH:
    case AP_OUTLINE_OFFSET:
      return F(lerpf(a.f, b.f, t));
    case AP_LINE_HEIGHT:
      if ((a.f < 0) != (b.f < 0)) return t < 0.5f ? a : b;
      return F(lerpf(a.f, b.f, t));
    case AP_COLOR: case AP_BG_COLOR: case AP_BORDER_TOP_COLOR: case AP_BORDER_RIGHT_COLOR:
    case AP_BORDER_BOTTOM_COLOR: case AP_BORDER_LEFT_COLOR: case AP_OUTLINE_COLOR: case AP_TEXT_DECO_COLOR:
    case AP_ACCENT_COLOR:
      return Cv(lerpColor(a.color, b.color, t));
    case AP_TRANSFORM: {
      AnimValue v;
      v.tf = lerpTransform(a.tf, b.tf, t);
      return v;
    }
    case AP_BOX_SHADOW: case AP_TEXT_SHADOW: {
      AnimValue v;
      v.sh = lerpShadows(a.sh, b.sh, t);
      return v;
    }
    case AP_VISIBILITY: {
      AnimValue v;
      // visible for the whole transition if either end is visible
      if (a.i == VIS_VISIBLE || b.i == VIS_VISIBLE) v.i = (t <= 0 ? a.i : (t >= 1 ? b.i : VIS_VISIBLE));
      else v.i = t < 0.5f ? a.i : b.i;
      return v;
    }
    case AP_Z_INDEX: {
      AnimValue v;
      if (a.i == INT32_MIN || b.i == INT32_MIN) v.i = t < 0.5f ? a.i : b.i;
      else v.i = (int)std::lround(lerpf((float)a.i, (float)b.i, t));
      return v;
    }
    case AP_DISPLAY: {
      AnimValue v;
      v.i = (a.i == D_NONE || b.i == D_NONE) ? (t <= 0 ? a.i : (t >= 1 ? b.i : (a.i == D_NONE ? b.i : a.i))) : (t < 0.5f ? a.i : b.i);
      return v;
    }
    case AP_FILTER: {
      AnimValue v;
      if (a.filter && b.filter && a.filter->size() == b.filter->size()) {
        auto out = std::make_shared<std::vector<FilterFn>>(*a.filter);
        for (size_t i = 0; i < out->size(); i++) (*out)[i].v = lerpf((*a.filter)[i].v, (*b.filter)[i].v, t);
        v.filter = out;
      } else {
        v.filter = t < 0.5f ? a.filter : b.filter;
      }
      return v;
    }
    default:
      return Lv(lerpLen(a.len, b.len, t));
  }
}

bool animEqual(AnimProp p, const AnimValue &a, const AnimValue &b) {
  switch (p) {
    case AP_OPACITY: case AP_FONT_SIZE: case AP_LETTER_SPACING: case AP_FLEX_GROW: case AP_BORDER_TOP_WIDTH:
    case AP_BORDER_RIGHT_WIDTH: case AP_BORDER_BOTTOM_WIDTH: case AP_BORDER_LEFT_WIDTH: case AP_OUTLINE_WIDTH:
    case AP_OUTLINE_OFFSET: case AP_LINE_HEIGHT:
      return std::fabs(a.f - b.f) < 1e-4f;
    case AP_COLOR: case AP_BG_COLOR: case AP_BORDER_TOP_COLOR: case AP_BORDER_RIGHT_COLOR:
    case AP_BORDER_BOTTOM_COLOR: case AP_BORDER_LEFT_COLOR: case AP_OUTLINE_COLOR: case AP_TEXT_DECO_COLOR:
    case AP_ACCENT_COLOR:
      return a.color == b.color;
    case AP_TRANSFORM: {
      if (a.tf == b.tf) return true;
      size_t na = a.tf ? a.tf->size() : 0, nb = b.tf ? b.tf->size() : 0;
      if (na != nb) return false;
      for (size_t i = 0; i < na; i++) {
        const TransformFn &x = (*a.tf)[i], &y = (*b.tf)[i];
        if (x.kind != y.kind) return false;
        if (x.a.px != y.a.px || x.a.pct != y.a.pct || x.a.em != y.a.em || x.b.px != y.b.px || x.b.pct != y.b.pct ||
            x.b.em != y.b.em)
          return false;
        for (int k = 0; k < 6; k++)
          if (x.v[k] != y.v[k]) return false;
      }
      return true;
    }
    case AP_BOX_SHADOW: case AP_TEXT_SHADOW: {
      if (a.sh == b.sh) return true;
      size_t na = a.sh ? a.sh->size() : 0, nb = b.sh ? b.sh->size() : 0;
      if (na != nb) return false;
      for (size_t i = 0; i < na; i++) {
        const Shadow &x = (*a.sh)[i], &y = (*b.sh)[i];
        if (x.x.px != y.x.px || x.y.px != y.y.px || x.blur.px != y.blur.px || x.spread.px != y.spread.px ||
            x.color.c != y.color.c || x.inset != y.inset)
          return false;
      }
      return true;
    }
    case AP_VISIBILITY: case AP_Z_INDEX: case AP_DISPLAY: return a.i == b.i;
    case AP_FILTER: {
      if (a.filter == b.filter) return true;
      if (!a.filter || !b.filter || a.filter->size() != b.filter->size()) return false;
      for (size_t i = 0; i < a.filter->size(); i++)
        if ((*a.filter)[i].kind != (*b.filter)[i].kind || (*a.filter)[i].v != (*b.filter)[i].v) return false;
      return true;
    }
    default: return lenEq(a.len, b.len);
  }
}

// ============================================================ animator

bool Animator::anyRunning() const {
  for (auto &r : runs_)
    if (!r.held && !r.paused && r.duration >= 0) return true;
  return false;
}

static const TransitionSpec *transitionFor(const ComputedStyle &cs, AnimProp p) {
  if (!cs.transitions) return nullptr;
  const TransitionSpec *found = nullptr;
  for (auto &t : *cs.transitions) {
    if (t.prop == 0) {
      if (p != AP_DISPLAY && p != AP_Z_INDEX) found = &t;
      continue;
    }
    std::vector<AnimProp> ps;
    animPropsFor(atomStr(t.prop), ps);
    for (AnimProp q : ps)
      if (q == p) found = &t;
  }
  return found;
}

void Animator::startKeyframes(Element *e, const AnimationSpec &spec, const ComputedStyle &base, const StyleEngine &se) {
  const Keyframes *kf = se.findKeyframes(spec.name);
  if (!kf || kf->frames.empty()) return;
  int id = nextId_++;
  // tracks per property
  std::vector<int> props;
  for (auto &fr : kf->frames)
    for (auto &d : fr.decls->decls) {
      int ap = animPropForCss(d.prop);
      if (ap >= 0 && std::find(props.begin(), props.end(), ap) == props.end()) props.push_back(ap);
      if (d.prop == P_border_top_color) {
        for (int q : {AP_BORDER_RIGHT_COLOR, AP_BORDER_BOTTOM_COLOR, AP_BORDER_LEFT_COLOR}) (void)q;
      }
    }
  ResolveCtx rc;
  rc.em = base.fontSize;
  for (int ap : props) {
    Running r;
    r.el = e;
    r.prop = (AnimProp)ap;
    r.keyframes = true;
    r.id = id;
    r.name = spec.name;
    r.start = now_;
    r.duration = spec.duration * 1000;
    r.delay = spec.delay * 1000;
    r.timing = spec.timing;
    r.iterations = spec.iterations;
    r.direction = spec.direction;
    r.fill = spec.fill;
    r.paused = spec.paused;
    r.pausedAt = now_;
    for (auto &fr : kf->frames) {
      bool has = false;
      ComputedStyle tmp = base;
      for (auto &d : fr.decls->decls) {
        if (animPropForCss(d.prop) != ap) continue;
        const_cast<StyleEngine &>(se).applyDecl(tmp, d, &base, rc);
        has = true;
      }
      if (!has) continue;
      Running::Key k;
      k.offset = fr.offset;
      k.v = animGet(tmp, (AnimProp)ap);
      if (fr.timing) {
        k.hasTiming = true;
        k.timing = *fr.timing;
      }
      r.keys.push_back(k);
    }
    AnimValue bv = animGet(base, (AnimProp)ap);
    if (r.keys.empty() || r.keys.front().offset > 0) r.keys.insert(r.keys.begin(), Running::Key{0, bv});
    if (r.keys.back().offset < 1) r.keys.push_back(Running::Key{1, bv});
    r.current = r.keys.front().v;
    runs_.push_back(std::move(r));
  }
  if (onEvent) onEvent(e, "animationstart", atomStr(spec.name));
}

void Animator::styleHook(Element *e, const ComputedStyle *old, ComputedStyle &nw, const StyleEngine &se) {
  // ---- keyframe animations: start new names, stop removed ones
  std::vector<Atom> names;
  if (nw.animations)
    for (auto &a : *nw.animations) names.push_back(a.name);
  for (size_t i = 0; i < runs_.size();) {
    Running &r = runs_[i];
    if (r.el == e && r.keyframes && !r.waapi && std::find(names.begin(), names.end(), r.name) == names.end()) {
      runs_.erase(runs_.begin() + i);
      continue;
    }
    i++;
  }
  if (nw.animations && nw.display != D_NONE) {
    for (auto &a : *nw.animations) {
      bool have = false;
      for (auto &r : runs_)
        if (r.el == e && r.keyframes && !r.waapi && r.name == a.name) {
          have = true;
          // play-state changes
          if (r.paused != a.paused) {
            if (a.paused) r.pausedAt = now_;
            else r.start += now_ - r.pausedAt;
            r.paused = a.paused;
          }
        }
      if (!have && a.duration > 0) startKeyframes(e, a, nw, se);
    }
  }
  if (nw.display == D_NONE) {
    // animations stop when the element stops being rendered
    for (size_t i = 0; i < runs_.size();) {
      if (runs_[i].el == e && runs_[i].keyframes && !runs_[i].waapi) runs_.erase(runs_.begin() + i);
      else i++;
    }
  }
  // ---- transitions
  if (old && nw.display != D_NONE && old->display != D_NONE) {
    for (int pi = 0; pi < AP__COUNT; pi++) {
      AnimProp p = (AnimProp)pi;
      Running *run = nullptr;
      bool keyframed = false;
      for (auto &r : runs_)
        if (r.el == e && r.prop == p) {
          if (r.keyframes) keyframed = true;
          else run = &r;
        }
      if (keyframed) continue;
      AnimValue newBase = animGet(nw, p);
      AnimValue oldBase = run ? run->to : animGet(*old, p);
      if (animEqual(p, oldBase, newBase)) continue;
      const TransitionSpec *ts = transitionFor(nw, p);
      AnimValue cur = run ? run->current : animGet(*old, p);
      if (run) {
        size_t idx = run - &runs_[0];
        runs_.erase(runs_.begin() + idx);
      }
      if (!ts || (ts->duration <= 0)) continue;
      Running r;
      r.el = e;
      r.prop = p;
      r.from = cur;
      r.to = newBase;
      r.current = cur;
      r.start = now_;
      r.duration = ts->duration * 1000;
      r.delay = ts->delay * 1000;
      r.timing = ts->timing;
      r.id = nextId_++;
      runs_.push_back(std::move(r));
    }
  }
  // ---- apply current animated values
  for (auto &r : runs_)
    if (r.el == e && (r.started || !r.keyframes || r.fill >= 2 || r.held)) animSet(nw, r.prop, r.current);
}

bool Animator::computeValue(Running &r, double now, bool &finished) {
  finished = false;
  double t = r.paused ? r.pausedAt : now;
  double el = (t - r.start - r.delay) * r.playbackRate;
  if (!r.keyframes) {
    if (el < 0) {
      r.current = r.from;
      return true;
    }
    double p = r.duration > 0 ? el / r.duration : 1;
    if (p >= 1) {
      finished = true;
      r.current = r.to;
      return true;
    }
    r.current = animLerp(r.prop, r.from, r.to, r.timing.eval((float)p));
    return true;
  }
  // keyframes
  if (el < 0) {
    if (r.fill >= 2) {  // backwards / both
      r.current = r.keys.front().v;
      r.started = true;
      return true;
    }
    return false;
  }
  r.started = true;
  double dur = std::max(r.duration, 1e-3);
  double total = el / dur;
  float iters = r.iterations;
  double iteration;
  double p;
  if (!std::isinf(iters) && total >= iters) {
    finished = true;
    iteration = std::ceil(iters) - 1;
    p = iters - iteration;
    if (p <= 0) p = 1;
  } else {
    iteration = std::floor(total);
    p = total - iteration;
  }
  bool rev = r.direction == 1 || (r.direction == 2 && ((int)iteration & 1)) || (r.direction == 3 && !((int)iteration & 1));
  if (rev) p = 1 - p;
  float pf = (float)clampf((float)p, 0, 1);
  // find the keyframe segment
  size_t k = 0;
  while (k + 1 < r.keys.size() - 1 && r.keys[k + 1].offset <= pf) k++;
  const Running::Key &a = r.keys[k], &b = r.keys[std::min(k + 1, r.keys.size() - 1)];
  float span = b.offset - a.offset;
  float lt = span > 0 ? (pf - a.offset) / span : 1;
  const TimingFn &tf = a.hasTiming ? a.timing : r.timing;
  r.current = animLerp(r.prop, a.v, b.v, tf.eval(lt));
  return true;
}

static const char *animatableName(AnimProp p) {
  for (auto &e : kAnimNames)
    if (e.p == p) return e.name;
  return "";
}

uint8_t Animator::tick(double nowMs, const std::function<void(Element *, uint8_t)> &dirty) {
  now_ = nowMs;
  uint8_t all = 0;
  std::vector<std::pair<Element *, std::string>> ends;
  std::vector<int> waapiDone;
  for (size_t i = 0; i < runs_.size();) {
    Running &r = runs_[i];
    if (r.held) {
      i++;
      continue;
    }
    bool finished;
    bool apply = computeValue(r, nowMs, finished);
    uint8_t flags = animPropAffectsLayout(r.prop) ? (SD_LAYOUT | SD_PAINT) : SD_PAINT;
    if (r.prop == AP_OPACITY || r.prop == AP_TRANSFORM || r.prop == AP_Z_INDEX) flags |= SD_STACKING;
    if (apply && r.el->cs) {
      animSet(*r.el->cs, r.prop, r.current);
      dirty(r.el, flags);
      all |= flags;
    }
    if (finished) {
      if (!r.keyframes) {
        ends.push_back({r.el, std::string(animatableName(r.prop))});
        runs_.erase(runs_.begin() + i);
        continue;
      }
      // keyframes: fire end once per animation id (first track)
      bool firstTrack = true;
      for (size_t j = 0; j < i; j++)
        if (runs_[j].id == r.id) firstTrack = false;
      if (firstTrack) {
        if (r.waapi) waapiDone.push_back(r.id);
        else ends.push_back({r.el, "@" + atomStr(r.name)});
      }
      if (r.fill == 1 || r.fill == 3) {
        r.held = true;
        i++;
      } else {
        // keep a marker so the CSS animation does not restart; restore the base value
        r.held = true;
        r.started = false;
        r.fill = 0;
        dirty(r.el, flags | 0x80);  // 0x80: element needs restyle
        all |= flags;
        if (r.waapi) {
          runs_.erase(runs_.begin() + i);
          continue;
        }
        i++;
      }
      continue;
    }
    i++;
  }
  if (onEvent)
    for (auto &e : ends) {
      if (e.second[0] == '@') onEvent(e.first, "animationend", e.second.substr(1));
      else onEvent(e.first, "transitionend", e.second);
    }
  if (onWaapiFinish)
    for (int id : waapiDone) onWaapiFinish(id);
  return all;
}

void Animator::cancel(Element *e) {
  for (size_t i = 0; i < runs_.size();) {
    if (runs_[i].el == e) runs_.erase(runs_.begin() + i);
    else i++;
  }
}

int Animator::animate(Element *e, const std::vector<std::pair<float, std::vector<std::pair<std::string, std::string>>>> &frames,
                      double duration, double delay, const TimingFn &easing, float iterations, uint8_t direction,
                      uint8_t fill, const StyleEngine &se) {
  if (!e->cs || frames.empty()) return 0;
  int id = nextId_++;
  const ComputedStyle &base = *e->cs;
  ResolveCtx rc;
  rc.em = base.fontSize;
  // collect properties
  std::vector<int> props;
  std::vector<std::vector<std::pair<float, AnimValue>>> tracks;
  for (auto &fr : frames) {
    for (auto &kv : fr.second) {
      std::vector<Decl> decls;
      parsePropertyValue(kv.first, kv.second, decls);
      for (auto &d : decls) {
        int ap = animPropForCss(d.prop);
        if (ap < 0) continue;
        ComputedStyle tmp = base;
        const_cast<StyleEngine &>(se).applyDecl(tmp, d, &base, rc);
        size_t idx = std::find(props.begin(), props.end(), ap) - props.begin();
        if (idx == props.size()) {
          props.push_back(ap);
          tracks.emplace_back();
        }
        tracks[idx].push_back({fr.first, animGet(tmp, (AnimProp)ap)});
      }
    }
  }
  for (size_t i = 0; i < props.size(); i++) {
    Running r;
    r.el = e;
    r.prop = (AnimProp)props[i];
    r.keyframes = true;
    r.waapi = true;
    r.id = id;
    r.start = now_;
    r.duration = duration;
    r.delay = delay;
    r.timing = easing;
    r.iterations = iterations;
    r.direction = direction;
    r.fill = fill;
    AnimValue bv = animGet(base, r.prop);
    for (auto &kv : tracks[i]) r.keys.push_back(Running::Key{kv.first, kv.second});
    std::stable_sort(r.keys.begin(), r.keys.end(), [](const Running::Key &a, const Running::Key &b) { return a.offset < b.offset; });
    if (r.keys.empty() || r.keys.front().offset > 0) r.keys.insert(r.keys.begin(), Running::Key{0, bv});
    if (r.keys.back().offset < 1) r.keys.push_back(Running::Key{1, bv});
    r.current = r.keys.front().v;
    runs_.push_back(std::move(r));
  }
  return id;
}

void Animator::cancelAnimation(int id) {
  for (size_t i = 0; i < runs_.size();) {
    if (runs_[i].id == id) {
      if (runs_[i].el) runs_[i].el->flags |= NF_STYLE_DIRTY;
      runs_.erase(runs_.begin() + i);
    } else {
      i++;
    }
  }
}

void Animator::finishAnimation(int id) {
  for (auto &r : runs_)
    if (r.id == id) r.start = now_ - r.delay - r.duration * (std::isinf(r.iterations) ? 1 : r.iterations) - 1;
}

void Animator::pauseAnimation(int id, bool pause) {
  for (auto &r : runs_)
    if (r.id == id && r.paused != pause) {
      if (pause) r.pausedAt = now_;
      else r.start += now_ - r.pausedAt;
      r.paused = pause;
    }
}

bool Animator::animationFinished(int id) const {
  for (auto &r : runs_)
    if (r.id == id && !r.held) return false;
  return true;
}

}  // namespace rt
