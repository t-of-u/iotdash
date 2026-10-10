#include "widgets.h"
#include "prof.h"

#include "engine.h"
#include "extra.h"
#include "input.h"
#include "canvas2d.h"
#include "svg.h"
#include "layer.h"

namespace rt {

// ============================================================ internal stylesheet

const char *Widgets::internalCss() {
  return R"CSS(
.__ov { position: fixed !important; left: 0 !important; top: 0 !important; right: 0 !important; bottom: 0 !important;
  z-index: 2147483000 !important; background: rgba(15, 23, 42, 0.38) !important; display: flex !important;
  align-items: center !important; justify-content: center !important; opacity: 1; transition: opacity .18s ease-out;
  font-family: sans-serif !important; margin: 0 !important; padding: 0 !important; border: none !important; }
.__sheet { background: #ffffff !important; border-radius: 16px !important; padding: 8px !important; min-width: 280px !important;
  max-width: 560px !important; max-height: 420px !important; overflow-y: auto !important; display: block !important;
  box-shadow: 0 12px 40px rgba(0, 0, 0, 0.28) !important; box-sizing: border-box !important; color: #111827 !important;
  margin: 0 !important; transform: scale(1); transition: transform .18s ease-out; }
.__opt { display: block !important; padding: 12px 16px !important; border-radius: 10px !important; font-size: 18px !important;
  color: #111827 !important; background: transparent; margin: 0 !important; line-height: 1.25 !important; }
.__opt:active { background: #e5e7eb !important; }
.__opt.__sel { background: #dbeafe !important; color: #1d4ed8 !important; font-weight: 600 !important; }
.__opt.__dis { color: #9ca3af !important; }
.__grp { display: block !important; padding: 10px 16px 4px !important; font-size: 13px !important; color: #6b7280 !important; font-weight: 600 !important; }
.__kb { position: fixed !important; left: 0 !important; right: 0 !important; bottom: 0 !important; z-index: 2147483600 !important;
  background: #d1d5db !important; padding: 6px 4px 8px !important; display: block !important; box-sizing: border-box !important;
  font-family: sans-serif !important; transform: translateY(100%); transition: transform .22s ease-out; margin: 0 !important;
  border: none !important; }
.__kb.__open { transform: none; }
.__kbrow { display: flex !important; justify-content: center !important; gap: 5px !important; margin: 0 0 5px 0 !important; padding: 0 !important; }
.__key { flex: 1 1 0 !important; max-width: 64px !important; height: 42px !important; background: #ffffff !important; border-radius: 8px !important;
  display: flex !important; align-items: center !important; justify-content: center !important; font-size: 21px !important;
  color: #111827 !important; box-shadow: 0 1px 0 rgba(0, 0, 0, 0.35) !important; margin: 0 !important; padding: 0 !important;
  border: none !important; line-height: 1 !important; }
.__key:active { background: #9ca3af !important; }
.__key.__fn { background: #aeb4bf !important; font-size: 15px !important; max-width: 96px !important; flex-grow: 1.5 !important; }
.__key.__fn:active { background: #ffffff !important; }
.__key.__on { background: #ffffff !important; color: #2563eb !important; }
.__key.__space { flex-grow: 5 !important; max-width: 340px !important; font-size: 15px !important; color: #6b7280 !important; }
.__key.__enter { background: #2563eb !important; color: #fff !important; flex-grow: 1.8 !important; max-width: 110px !important; font-size: 16px !important; }
.__kb.__num .__key { max-width: 180px !important; font-size: 24px !important; }
.__cal { width: 340px !important; padding: 12px !important; }
.__calhead { display: flex !important; align-items: center !important; justify-content: space-between !important; margin: 0 0 8px 0 !important; padding: 0 !important; }
.__title { font-size: 18px !important; font-weight: 600 !important; color: #111827 !important; display: block !important; margin: 0 !important; }
.__nav { width: 44px !important; height: 40px !important; border-radius: 10px !important; display: flex !important;
  align-items: center !important; justify-content: center !important; font-size: 24px !important; color: #374151 !important; margin: 0 !important; }
.__nav:active { background: #e5e7eb !important; }
.__grid { display: grid !important; grid-template-columns: repeat(7, 1fr) !important; gap: 2px !important; margin: 0 !important; padding: 0 !important; }
.__dow { text-align: center !important; font-size: 13px !important; color: #9ca3af !important; padding: 4px 0 !important; display: block !important; margin: 0 !important; }
.__day { text-align: center !important; font-size: 16px !important; height: 38px !important; line-height: 38px !important;
  border-radius: 19px !important; color: #111827 !important; display: block !important; margin: 0 !important; padding: 0 !important; }
.__day:active { background: #e5e7eb !important; }
.__day.__other { color: #d1d5db !important; }
.__day.__today { color: #2563eb !important; font-weight: 700 !important; }
.__day.__sel { background: #2563eb !important; color: #ffffff !important; }
.__bar { display: flex !important; gap: 8px !important; justify-content: flex-end !important; margin: 10px 0 0 0 !important; padding: 0 !important; }
.__btn { padding: 10px 18px !important; border-radius: 10px !important; font-size: 16px !important; color: #2563eb !important;
  font-weight: 600 !important; display: block !important; margin: 0 !important; background: transparent; }
.__btn:active { background: #dbeafe !important; }
.__btn.__primary { background: #2563eb !important; color: #ffffff !important; }
.__time { display: flex !important; align-items: center !important; justify-content: center !important; gap: 18px !important; padding: 8px 4px !important; margin: 0 !important; }
.__col { display: flex !important; flex-direction: column !important; align-items: center !important; gap: 4px !important; margin: 0 !important; padding: 0 !important; }
.__big { font-size: 44px !important; font-weight: 600 !important; color: #111827 !important; width: 90px !important; text-align: center !important; display: block !important; margin: 0 !important; font-variant-numeric: tabular-nums; }
.__colon { font-size: 40px !important; color: #6b7280 !important; display: block !important; margin: 0 !important; }
.__swatches { display: grid !important; grid-template-columns: repeat(8, 40px) !important; gap: 8px !important; padding: 6px !important; margin: 0 !important; }
.__sw { width: 40px !important; height: 40px !important; border-radius: 20px !important; display: block !important; margin: 0 !important; padding: 0 !important;
  box-shadow: inset 0 0 0 1px rgba(0, 0, 0, 0.12) !important; }
.__sw.__sel { box-shadow: 0 0 0 3px #ffffff, 0 0 0 5px #2563eb !important; }
)CSS";
}

// ============================================================ helpers

std::string Widgets::inputType(Element *e) {
  if (e->tagId() != TAG_input) return "";
  std::string t = toLower(e->attrOr(A.type, "text"));
  static const char *known[] = {"text", "password", "email", "number", "tel", "url", "search", "checkbox", "radio",
                                "range", "color", "date", "time", "datetime-local", "month", "week", "button",
                                "submit", "reset", "hidden", "file", "image"};
  for (auto k : known)
    if (t == k) return t;
  return "text";
}

bool Widgets::isTextInput(Element *e) {
  Tag tg = e->tagId();
  if (tg == TAG_textarea) return true;
  if (tg != TAG_input) return false;
  std::string t = inputType(e);
  return t == "text" || t == "password" || t == "email" || t == "number" || t == "tel" || t == "url" || t == "search";
}

bool Widgets::isInternal(Element *e) const {
  for (Element *p = e; p; p = p->parent)
    if (p->flags & NF_INTERNAL) return true;
  return false;
}

static Color withA(Color c, uint8_t a) { return a == 255 ? c : c.scaleAlpha(a); }

// ============================================================ lifecycle

void Widgets::init(Element *e) {
  Tag t = e->tagId();
  if (t == TAG_input) {
    std::string type = inputType(e);
    if ((type == "checkbox" || type == "radio") && !(e->x && e->x->checkedDirty))
      e->state = e->hasAttr(A.checked) ? (e->state | ST_CHECKED) : (e->state & ~ST_CHECKED);
    if (e->hasAttr(A.placeholder) && eng_.currentValue(e).empty()) e->state |= ST_PLACEHOLDER;
    if (e->hasAttr(A.required) && eng_.currentValue(e).empty() && type != "checkbox" && type != "radio") e->state |= ST_INVALID;
  } else if (t == TAG_textarea) {
    if (e->hasAttr(A.placeholder) && eng_.currentValue(e).empty()) e->state |= ST_PLACEHOLDER;
  } else if (t == TAG_option) {
    if (e->hasAttr(A.selected)) e->state |= ST_CHECKED;
  } else if (t == TAG_details || t == TAG_dialog) {
    if (e->hasAttr(A.open)) e->state |= ST_OPEN;
  } else if (t == TAG_img) {
    std::string src = e->attrOr(A.src, "");
    if (!src.empty()) {
      e->extra().image = eng_.loadImage(src);
      e->extra().imageSrc = src;
    }
  }
}

void Widgets::attrChanged(Element *e, Atom name) {
  Tag t = e->tagId();
  if (t == TAG_img && name == A.src) {
    std::string src = e->attrOr(A.src, "");
    e->extra().image = src.empty() ? nullptr : eng_.loadImage(src);
    e->extra().imageSrc = src;
    eng_.markLayoutDirty(e);
  }
  if ((t == TAG_input || t == TAG_textarea) && (name == A.value || name == A.placeholder || name == A.type)) {
    eng_.refreshFormState(e);
    eng_.invalidateElement(e);
    eng_.markLayoutDirty(e);
  }
  if (t == TAG_input && name == A.checked && !(e->x && e->x->checkedDirty))
    e->setState(ST_CHECKED, e->hasAttr(A.checked));
  if ((t == TAG_details || t == TAG_dialog) && name == A.open) {
    e->setState(ST_OPEN, e->hasAttr(A.open));
    eng_.markLayoutDirty(e);
  }
  if (t == TAG_option && name == A.selected) {
    Element *sel = e->parent;
    while (sel && sel->tagId() != TAG_select) sel = sel->parent;
    if (sel) eng_.invalidateElement(sel);
  }
  if ((t == TAG_progress || t == TAG_meter || t == TAG_input) && (name == A.value || name == A.max || name == A.min))
    eng_.invalidateElement(e);
  if (t == TAG_canvas && (name == A.width || name == A.height)) {
    eng_.markLayoutDirty(e);
    if (e->x && e->x->canvas) e->x->canvas->resize((int)e->attrNum(A.width, 300), (int)e->attrNum(A.height, 150));
  }
}

void Widgets::forget(Element *e) {
  if (drag_ == e) drag_ = nullptr;
  if (kbFor_ == e) {
    kbFor_ = nullptr;
    closeKeyboard();
  }
  if (popupFor_ == e) {
    popupFor_ = nullptr;
    closePopup();
  }
  if (kb_ == e) kb_ = nullptr;
  if (popup_ == e) popup_ = nullptr;
}

void Widgets::closeAll() {
  drag_ = nullptr;
  kb_ = nullptr;
  popup_ = nullptr;
  kbFor_ = popupFor_ = nullptr;
  kbOpen_ = false;
  eng_.renderer().layout = &eng_.layout();
}

// ============================================================ intrinsic sizes

bool Widgets::intrinsic(Element *e, float &w, float &h) {
  w = h = 0;
  const ComputedStyle *cs = e->cs;
  if (!cs) return false;
  FontRef f = eng_.layout().fontFor(*cs);
  float lh = cs->usedLineHeight(LayoutEngine::normalLineHeight(f));
  switch (e->tagId()) {
    case TAG_img: {
      auto &img = e->extra().image;
      float iw = img ? (float)img->w : 0, ih = img ? (float)img->h : 0;
      float aw = e->attrNum(A.width, -1), ah = e->attrNum(A.height, -1);
      if (aw > 0 && ah > 0) {
        w = aw;
        h = ah;
      } else if (aw > 0) {
        w = aw;
        h = iw > 0 ? aw * ih / iw : 0;
      } else if (ah > 0) {
        h = ah;
        w = ih > 0 ? ah * iw / ih : 0;
      } else {
        w = iw;
        h = ih;
      }
      return true;
    }
    case TAG_canvas:
      w = e->attrNum(A.width, 300);
      h = e->attrNum(A.height, 150);
      return true;
    case TAG_svg: {
      float aw = e->attrNum(A.width, -1), ah = e->attrNum(A.height, -1);
      float vw = 0, vh = 0;
      std::string vb = e->attrOr(A.viewBox, "");
      if (!vb.empty()) {
        for (auto &c : vb)
          if (c == ',') c = ' ';
        float a, b;
        if (sscanf(vb.c_str(), "%f %f %f %f", &a, &b, &vw, &vh) != 4) vw = vh = 0;
      }
      if (aw > 0 && ah > 0) {
        w = aw;
        h = ah;
      } else if (aw > 0) {
        w = aw;
        h = vw > 0 ? aw * vh / vw : 150;
      } else if (ah > 0) {
        h = ah;
        w = vh > 0 ? ah * vw / vh : 300;
      } else if (vw > 0) {
        w = 300;
        h = 300 * vh / vw;
        if (cs->width.isLen() && !cs->width.hasPct()) h = cs->width.px * vh / vw;
      } else {
        w = 300;
        h = 150;
      }
      return true;
    }
    case TAG_textarea: {
      float rows = e->attrNum(A.rows, 2), cols = e->attrNum(A.cols, 20);
      w = cols * cs->fontSize * 0.55f;
      h = rows * lh;
      return true;
    }
    case TAG_select: {
      float mx = 0;
      for (Element *o : options(e)) mx = std::max(mx, measureText(f, trim(o->textContent())));
      w = mx + 4;
      h = lh;
      return true;
    }
    case TAG_progress: w = 160; h = 16; return true;
    case TAG_meter: w = 80; h = 16; return true;
    case TAG_input: {
      RT_PROF(PF_WIDGET);
      std::string t = inputType(e);
      if (t == "checkbox" || t == "radio") { w = h = 13; return true; }
      if (t == "range") { w = 129; h = 16; return true; }
      if (t == "color") { w = 44; h = 23; return true; }
      if (t == "button" || t == "submit" || t == "reset") {
        std::string label = e->attrOr(A.value, t == "submit" ? "Submit" : t == "reset" ? "Reset" : "");
        w = measureText(f, label);
        h = lh;
        return true;
      }
      w = cs->fontSize * 0.55f * e->attrNum(A.size, 20);
      h = lh;
      return true;
    }
    default:
      return false;
  }
}

// ============================================================ painting

static RectF contentRectOf(Element *e) { return e->box->contentRect(); }

static void fillRR(const PaintCtx &pc, const RectF &doc, float r, Color c) {
  float rr[4] = {r, r, r, r};
  Paint p;
  p.color = c;
  fillRRect(pc.target(), pc.map(doc), rr, p);
}

std::string Widgets::displayText(Element *e, bool &placeholder) {
  std::string v = eng_.currentValue(e);
  placeholder = false;
  if (v.empty()) {
    if (e->hasAttr(A.placeholder)) {
      placeholder = true;
      return e->attrOr(A.placeholder, "");
    }
    std::string t = inputType(e);
    if (t == "date") { placeholder = true; return "dd/mm/yyyy"; }
    if (t == "time") { placeholder = true; return "--:--"; }
    if (t == "datetime-local") { placeholder = true; return "dd/mm/yyyy --:--"; }
    if (t == "month") { placeholder = true; return "-------- ----"; }
    return "";
  }
  if (inputType(e) == "password") {
    std::string dots;
    size_t i = 0;
    while (i < v.size()) {
      utf8Next(v.data(), v.size(), i);
      dots += "\xE2\x80\xA2";
    }
    return dots;
  }
  if (inputType(e) == "date" && v.size() == 10) return v.substr(8, 2) + "/" + v.substr(5, 2) + "/" + v.substr(0, 4);
  return v;
}

void Widgets::paintCenteredText(Element *e, const PaintCtx &pc, const std::string &text, Color c, float left) {
  const ComputedStyle &s = *e->cs;
  FontRef f = eng_.layout().fontFor(s);
  if (!f.face) return;
  RectF cr = contentRectOf(e);
  std::vector<ShapedGlyph> gl;
  ShapeOpts o;
  o.letterSpacing = s.letterSpacing;
  float w = shapeText(f, text.data(), text.size(), o, gl);
  float A = std::round(f.ascent()), D = std::round(f.descent());
  float base = cr.y + std::round((cr.h - (A + D)) / 2) + A;
  float x = cr.x + left;
  if (s.textAlign == TA_CENTER) x = cr.x + (cr.w - w) / 2;
  else if (s.textAlign == TA_RIGHT || s.textAlign == TA_END) x = cr.right() - w;
  PaintCtx sub = pc;
  sub.clip = pc.clip.intersect(pc.map(cr).outer());
  for (auto &g : gl) {
    if (!g.g || !g.g->w) continue;
    int gx = (int)std::lround(x + g.x + sub.dx) + g.g->bx;
    int gy = (int)std::lround(base - g.dy + sub.dy) - g.g->by;
    if (f.scale == 1) blitMask(sub.target(), f.face->bitmap(g.g), g.g->w, g.g->h, gx, gy, c);
    else {
      float sc = f.scale;
      Affine m{sc, 0, 0, sc, x + g.x + sub.dx + g.g->bx * sc, std::round(base - g.dy + sub.dy) - g.g->by * sc};
      Affine inv;
      if (m.invert(inv)) blitMaskTransformed(sub.target(), f.face->bitmap(g.g), g.g->w, g.g->h, inv, m.mapRect({0, 0, (float)g.g->w, (float)g.g->h}).outer(), c);
    }
  }
}

void Widgets::paintTextControl(Element *e, const PaintCtx &pc) {
  const ComputedStyle &s = *e->cs;
  FontRef f = eng_.layout().fontFor(s);
  if (!f.face) return;
  ElementExtra &x = e->extra();
  bool ph;
  std::string text = displayText(e, ph);
  Color c = withA(s.color, pc.alpha);
  if (ph) {
    auto &pst = x.pseudoStyles[PE_PLACEHOLDER];
    c = withA(pst ? pst->color : Color{156, 163, 175, 255}, pc.alpha);
  }
  RectF cr = contentRectOf(e);
  std::vector<ShapedGlyph> gl;
  ShapeOpts o;
  o.letterSpacing = s.letterSpacing;
  float w = shapeText(f, text.data(), text.size(), o, gl);
  float A = std::round(f.ascent()), D = std::round(f.descent());
  float base = cr.y + std::round((cr.h - (A + D)) / 2) + A;
  bool focused = e == eng_.focused();
  // caret x (in text coordinates)
  float caretX = 0;
  if (!ph) {
    std::string val = eng_.currentValue(e);
    int caret = std::min<int>(x.caret, (int)val.size());
    if (inputType(e) == "password") {
      // bullets: count code points before the caret
      int cps = 0;
      size_t i = 0;
      while (i < (size_t)caret) {
        utf8Next(val.data(), val.size(), i);
        cps++;
      }
      caretX = cps > 0 && cps <= (int)gl.size() ? gl[cps - 1].x + gl[cps - 1].adv : 0;
    } else {
      caretX = w;
      for (auto &g : gl)
        if ((int)g.src >= caret) {
          caretX = g.x;
          break;
        }
    }
  }
  // keep the caret visible
  // both painting cores compute and store the same values: relaxed atomics keep it defined
  float scroll;
  __atomic_load(&x.textScroll, &scroll, __ATOMIC_RELAXED);
  if (focused) {
    if (caretX - scroll > cr.w - 2) scroll = caretX - cr.w + 2;
    if (caretX - scroll < 0) scroll = caretX;
    if (w - scroll < cr.w && scroll > 0) scroll = std::max(0.f, w - cr.w + 2);
  } else {
    scroll = 0;
  }
  __atomic_store(&x.textScroll, &scroll, __ATOMIC_RELAXED);
  float x0 = cr.x - scroll;
  if (s.textAlign == TA_CENTER && w < cr.w) x0 = cr.x + (cr.w - w) / 2;
  else if ((s.textAlign == TA_RIGHT || s.textAlign == TA_END) && w < cr.w) x0 = cr.right() - w;
  PaintCtx sub = pc;
  sub.clip = pc.clip.intersect(pc.map(cr).outer());
  RasterTarget t = sub.target();
  for (auto &g : gl) {
    if (!g.g || !g.g->w) continue;
    float gxF = x0 + g.x;
    if (gxF + sub.dx > sub.clip.right() || gxF + sub.dx + 64 < sub.clip.x) continue;
    if (f.scale == 1) {
      int gx = (int)std::lround(gxF + sub.dx) + g.g->bx;
      int gy = (int)std::lround(base - g.dy + sub.dy) - g.g->by;
      blitMask(t, f.face->bitmap(g.g), g.g->w, g.g->h, gx, gy, c);
    } else {
      float sc = f.scale;
      Affine m{sc, 0, 0, sc, gxF + sub.dx + g.g->bx * sc, std::round(base - g.dy + sub.dy) - g.g->by * sc};
      Affine inv;
      if (m.invert(inv)) blitMaskTransformed(t, f.face->bitmap(g.g), g.g->w, g.g->h, inv, m.mapRect({0, 0, (float)g.g->w, (float)g.g->h}).outer(), c);
    }
  }
  if (focused && caretOn_) {
    Color cc = s.caretAuto ? s.accentColor : s.caretColor;
    if (s.caretAuto) cc = Color{37, 99, 235, 255};
    float cx = std::round(x0 + caretX + sub.dx);
    RectI cr2{(int)cx, (int)std::round(base - A + sub.dy), 2, (int)(A + D)};
    fillRectSolid(t, cr2, withA(cc, pc.alpha));
    float cy = base - A + sub.dy, ch = A + D;
    __atomic_store(&x.caretX, &cx, __ATOMIC_RELAXED);
    __atomic_store(&x.caretY, &cy, __ATOMIC_RELAXED);
    __atomic_store(&x.caretH, &ch, __ATOMIC_RELAXED);
  }
}

void Widgets::paintTextarea(Element *e, const PaintCtx &pc) {
  const ComputedStyle &s = *e->cs;
  FontRef f = eng_.layout().fontFor(s);
  if (!f.face) return;
  ElementExtra &x = e->extra();
  bool ph;
  std::string text = displayText(e, ph);
  Color c = withA(s.color, pc.alpha);
  if (ph) {
    auto &pst = x.pseudoStyles[PE_PLACEHOLDER];
    c = withA(pst ? pst->color : Color{156, 163, 175, 255}, pc.alpha);
  }
  RectF cr = contentRectOf(e);
  float lh = s.usedLineHeight(LayoutEngine::normalLineHeight(f));
  float A = std::round(f.ascent()), D = std::round(f.descent());
  float half = (lh - (A + D)) / 2;
  std::vector<ShapedGlyph> gl;
  ShapeOpts o;
  o.letterSpacing = s.letterSpacing;
  shapeText(f, text.data(), text.size(), o, gl);
  // greedy wrap
  struct Ln {
    size_t g0, g1;
    float shift;
  };
  std::vector<Ln> lines;
  size_t start = 0, lastBreak = SIZE_MAX;
  float lineStartX = 0;
  for (size_t i = 0; i < gl.size(); i++) {
    const ShapedGlyph &g = gl[i];
    if (g.flags & SG_HARD_BREAK) {
      lines.push_back({start, i, lineStartX});
      start = i + 1;
      lineStartX = i + 1 < gl.size() ? gl[i + 1].x : 0;
      lastBreak = SIZE_MAX;
      continue;
    }
    float right = g.x + g.adv - lineStartX;
    if (right > cr.w && i > start && !(g.flags & SG_SPACE)) {
      size_t br = lastBreak != SIZE_MAX ? lastBreak + 1 : i;
      if (br <= start) br = i;
      lines.push_back({start, br, lineStartX});
      start = br;
      lineStartX = gl[br].x;
      lastBreak = SIZE_MAX;
    }
    if (g.flags & SG_BREAK_AFTER) lastBreak = i;
  }
  lines.push_back({start, gl.size(), lineStartX});
  // caret line
  bool focused = e == eng_.focused();
  int caret = x.caret;
  float scrollY = x.scrollY;
  float caretLX = 0;
  size_t caretLine = lines.size() - 1;
  if (!ph) {
    for (size_t li = 0; li < lines.size(); li++) {
      bool found = false;
      for (size_t gi = lines[li].g0; gi < lines[li].g1; gi++)
        if ((int)gl[gi].src >= caret) {
          caretLine = li;
          caretLX = gl[gi].x - lines[li].shift;
          found = true;
          break;
        }
      if (found) break;
      if (li + 1 == lines.size()) {
        caretLine = li;
        caretLX = lines[li].g1 > lines[li].g0 ? gl[lines[li].g1 - 1].x + gl[lines[li].g1 - 1].adv - lines[li].shift : 0;
        // after a trailing newline the caret sits on a fresh line
        if (!text.empty() && text.back() == '\n' && caret >= (int)text.size()) caretLX = 0;
      }
    }
  }
  if (focused) {
    float cy = caretLine * lh;
    if (cy - scrollY + lh > cr.h) scrollY = cy + lh - cr.h;
    if (cy < scrollY) scrollY = cy;
    x.scrollY = scrollY;
  }
  PaintCtx sub = pc;
  sub.clip = pc.clip.intersect(pc.map(cr).outer());
  RasterTarget t = sub.target();
  for (size_t li = 0; li < lines.size(); li++) {
    float base = cr.y + li * lh - scrollY + half + A;
    if (base - A + sub.dy > sub.clip.bottom() || base + D + sub.dy < sub.clip.y) continue;
    for (size_t gi = lines[li].g0; gi < lines[li].g1; gi++) {
      const ShapedGlyph &g = gl[gi];
      if (!g.g || !g.g->w) continue;
      int gx = (int)std::lround(cr.x + g.x - lines[li].shift + sub.dx) + g.g->bx;
      int gy = (int)std::lround(base - g.dy + sub.dy) - g.g->by;
      blitMask(t, f.face->bitmap(g.g), g.g->w, g.g->h, gx, gy, c);
    }
  }
  if (focused && caretOn_) {
    float base = cr.y + caretLine * lh - scrollY + half + A;
    RectI cr2{(int)std::round(cr.x + caretLX + sub.dx), (int)std::round(base - A + sub.dy), 2, (int)(A + D)};
    fillRectSolid(t, cr2, withA(Color{37, 99, 235, 255}, pc.alpha));
  }
}

void Widgets::paintCheck(Element *e, const PaintCtx &pc, bool radio) {
  const ComputedStyle &s = *e->cs;
  if (s.appearance == AP_NONE) return;
  RectF br = e->box->borderRect();
  bool on = e->hasState(ST_CHECKED);
  bool ind = e->hasState(ST_INDETERMINATE) && !radio;
  Color accent = s.accentColor;
  float r = radio ? br.w / 2 : std::min(br.w, br.h) * 0.25f;
  if (on || ind) {
    fillRR(pc, br, r, withA(accent, pc.alpha));
    if (radio) {
      float d = br.w * 0.36f;
      RectF dot{br.x + (br.w - d) / 2, br.y + (br.h - d) / 2, d, d};
      fillRR(pc, dot, d / 2, withA(kWhite, pc.alpha));
    } else if (ind) {
      RectF bar{br.x + br.w * 0.22f, br.y + br.h * 0.44f, br.w * 0.56f, std::max(2.f, br.h * 0.12f)};
      fillRR(pc, bar, 1, withA(kWhite, pc.alpha));
    } else {
      // check mark
      Path p;
      RectF sr = pc.map(br);
      p.moveTo(sr.x + sr.w * 0.24f, sr.y + sr.h * 0.52f);
      p.lineTo(sr.x + sr.w * 0.43f, sr.y + sr.h * 0.70f);
      p.lineTo(sr.x + sr.w * 0.77f, sr.y + sr.h * 0.32f);
      StrokeStyle st;
      st.width = std::max(2.f, sr.w * 0.13f);
      st.cap = CAP_ROUND;
      st.join = JOIN_ROUND;
      Path out;
      strokePath(p, st, out);
      Paint paint;
      paint.color = withA(kWhite, pc.alpha);
      fillPath(pc.target(), out, FR_NONZERO, paint);
    }
  }
}

float Widgets::rangeValue(Element *e, float &mn, float &mx, float &step) {
  mn = e->attrNum(A.min, 0);
  mx = e->attrNum(A.max, 100);
  if (mx < mn) mx = mn;
  std::string st = e->attrOr(A.step, "1");
  step = st == "any" ? 0 : (float)atof(st.c_str());
  float v = (float)atof(eng_.currentValue(e).c_str());
  return clampf(v, mn, mx);
}

void Widgets::paintRange(Element *e, const PaintCtx &pc) {
  const ComputedStyle &s = *e->cs;
  if (s.appearance == AP_NONE && !e->x) return;
  RectF cr = contentRectOf(e);
  float mn, mx, step;
  float v = rangeValue(e, mn, mx, step);
  float t = mx > mn ? (v - mn) / (mx - mn) : 0;
  float thumb = std::min(cr.h, 22.f);
  float trackH = 4;
  float x0 = cr.x + thumb / 2, x1 = cr.right() - thumb / 2;
  float cy = cr.y + cr.h / 2;
  float tx = x0 + (x1 - x0) * t;
  auto &trackStyle = e->extra().pseudoStyles[PE_TRACK];
  Color trackC = trackStyle && trackStyle->bgColor.a ? trackStyle->bgColor : Color{209, 213, 219, 255};
  fillRR(pc, {x0, cy - trackH / 2, x1 - x0, trackH}, trackH / 2, withA(trackC, pc.alpha));
  fillRR(pc, {x0, cy - trackH / 2, tx - x0, trackH}, trackH / 2, withA(s.accentColor, pc.alpha));
  // thumb with a soft shadow
  RectF th{tx - thumb / 2, cy - thumb / 2, thumb, thumb};
  RectF sth = pc.map(th);
  float rr[4] = {thumb / 2, thumb / 2, thumb / 2, thumb / 2};
  RectF sh{sth.x, sth.y + 1, sth.w, sth.h};
  fillShadow(pc.target(), sh, rr, 4, Color{0, 0, 0, (uint8_t)(70 * pc.alpha / 255)}, nullptr, nullptr);
  auto &thumbStyle = e->extra().pseudoStyles[PE_THUMB];
  Color thumbC = thumbStyle && thumbStyle->bgColor.a ? thumbStyle->bgColor : kWhite;
  fillRR(pc, th, thumb / 2, withA(thumbC, pc.alpha));
  float ri[4] = {thumb / 2 - 1, thumb / 2 - 1, thumb / 2 - 1, thumb / 2 - 1};
  Paint p;
  p.color = withA(Color{0, 0, 0, 30}, pc.alpha);
  fillRRectRing(pc.target(), sth, rr, {sth.x + 1, sth.y + 1, sth.w - 2, sth.h - 2}, ri, p);
}

void Widgets::paintProgress(Element *e, const PaintCtx &pc, bool meter) {
  const ComputedStyle &s = *e->cs;
  RectF br = e->box->borderRect();
  RectF cr = br.inset(e->box->b[3], e->box->b[0], e->box->b[1], e->box->b[2]);
  float mx = e->attrNum(A.max, 1), mn = meter ? e->attrNum(A.min, 0) : 0;
  bool indeterminate = !meter && !e->hasAttr(A.value);
  float v = e->attrNum(A.value, 0);
  float t = mx > mn ? clampf((v - mn) / (mx - mn), 0, 1) : 0;
  float radii[4];
  borderRadii(e, br, radii);
  float r = radii[0];
  Color c = s.accentColor;
  if (meter) {
    float low = e->attrNum(A.low, mn), high = e->attrNum(A.high, mx), opt = e->attrNum(A.optimum, (mn + mx) / 2);
    bool optLow = opt < low, optHigh = opt > high;
    bool vLow = v < low, vHigh = v > high;
    c = {22, 163, 74, 255};
    if ((vLow && !optLow) || (vHigh && !optHigh)) c = {234, 179, 8, 255};
    if ((vLow && optHigh) || (vHigh && optLow)) c = {220, 38, 38, 255};
  }
  if (indeterminate) {
    double ph = std::fmod(eng_.nowMs() / 1200.0, 1.0);
    float w = cr.w * 0.35f;
    float x = cr.x - w + (float)ph * (cr.w + w);
    PaintCtx sub = pc;
    sub.clip = pc.clip.intersect(pc.map(cr).outer());
    fillRR(sub, {x, cr.y, w, cr.h}, r, withA(c, pc.alpha));
    eng_.invalidateElement(e);
    return;
  }
  if (t > 0) fillRR(pc, {cr.x, cr.y, std::max(cr.h, cr.w * t), cr.h}, r, withA(c, pc.alpha));
}

void Widgets::paintSelect(Element *e, const PaintCtx &pc) {
  const ComputedStyle &s = *e->cs;
  int idx = selectedIndex(e);
  auto opts = options(e);
  std::string label = idx >= 0 && idx < (int)opts.size() ? trim(opts[idx]->textContent()) : "";
  paintCenteredText(e, pc, label, withA(s.color, pc.alpha));
  if (s.appearance == AP_NONE) return;
  // chevron in the right padding
  RectF br = e->box->borderRect();
  float cx = br.right() - e->box->b[1] - 18, cy = br.y + br.h / 2;
  RectF sr = pc.map({cx - 5, cy - 3, 10, 6});
  Path p;
  p.moveTo(sr.x, sr.y);
  p.lineTo(sr.x + 5, sr.y + 5);
  p.lineTo(sr.x + 10, sr.y);
  StrokeStyle st;
  st.width = 2;
  st.cap = CAP_ROUND;
  st.join = JOIN_ROUND;
  Path out;
  strokePath(p, st, out);
  Paint paint;
  paint.color = withA(Color{107, 114, 128, 255}, pc.alpha);
  fillPath(pc.target(), out, FR_NONZERO, paint);
}

void Widgets::paintColor(Element *e, const PaintCtx &pc) {
  Color c;
  if (!parseColorStr(eng_.currentValue(e), c)) c = kBlack;
  RectF cr = contentRectOf(e);
  fillRR(pc, cr, 4, withA(c, pc.alpha));
}

void Widgets::paintImage(Element *e, const PaintCtx &pc) {
  auto img = e->x ? e->x->image : nullptr;
  if (!img || !img->w || !img->h) return;
  const ComputedStyle &s = *e->cs;
  RectF cr = contentRectOf(e);
  float iw = (float)img->w, ih = (float)img->h;
  float dw = cr.w, dh = cr.h;
  switch (s.objectFit) {
    case OF_CONTAIN: {
      float sc = std::min(cr.w / iw, cr.h / ih);
      dw = iw * sc;
      dh = ih * sc;
      break;
    }
    case OF_COVER: {
      float sc = std::max(cr.w / iw, cr.h / ih);
      dw = iw * sc;
      dh = ih * sc;
      break;
    }
    case OF_NONE: dw = iw; dh = ih; break;
    case OF_SCALE_DOWN: {
      float sc = std::min(1.f, std::min(cr.w / iw, cr.h / ih));
      dw = iw * sc;
      dh = ih * sc;
      break;
    }
    default: break;
  }
  float dx = cr.x + s.objPosX.resolve(cr.w - dw), dy = cr.y + s.objPosY.resolve(cr.h - dh);
  float radii[4];
  RectF br = e->box->borderRect();
  borderRadii(e, br, radii);
  float cr4[4];
  insetRadii(radii, e->box->b[3] + e->box->p[3], e->box->b[0] + e->box->p[0], e->box->b[1] + e->box->p[1],
             e->box->b[2] + e->box->p[2], cr4);
  bool rounded = cr4[0] > 0 || cr4[1] > 0 || cr4[2] > 0 || cr4[3] > 0;
  if (pc.xf) {
    // transformed (e.g. "transform: scale(.9)" while pressed): sampled through the whole matrix,
    // inside the content box (rounded) or the picture, whichever is smaller
    Affine toScreen = *pc.xf * Affine::translate(dx, dy) * Affine::scale(dw / iw, dh / ih);
    Affine inv;
    if (!toScreen.invert(inv)) return;
    RectF area = (dw <= cr.w && dh <= cr.h) ? RectF{dx, dy, dw, dh} : cr;
    PaintCtx sub = pc;
    sub.clip = pc.clip.intersect(pc.xf->mapRect(area).outer());
    if (sub.clip.empty()) return;
    if (img->mask) {
      blitMaskTransformed(sub.target(), img->alpha, img->w, img->h, inv, pc.xf->mapRect({dx, dy, dw, dh}).outer(),
                          withA(s.color, pc.alpha));
      return;
    }
    Path shape;
    if (rounded && area.w == cr.w && area.h == cr.h) shape.roundRect(cr.x, cr.y, cr.w, cr.h, cr4);
    else shape.rect(area.x, area.y, area.w, area.h);
    shape.transform(*pc.xf);
    Paint p;
    p.kind = Paint::IMAGE;
    p.img = img->view();
    p.imgInv = inv;
    p.smooth = !s.pixelated;
    p.alpha = pc.alpha;
    fillPath(sub.target(), shape, FR_NONZERO, p);
    return;
  }
  RectF dst = pc.map({dx, dy, dw, dh});
  PaintCtx sub = pc;
  sub.clip = pc.clip.intersect(pc.map(cr).outer());
  RectF clipR = pc.map(cr);
  if (img->mask) {
    // monochrome icon tinted with the CSS colour
    Paint p;
    p.kind = Paint::IMAGE;
    ImageView v = img->view();
    p.img = v;
    p.alpha = pc.alpha;
    // draw via mask blit: scale to the destination
    Affine m{dw / iw, 0, 0, dh / ih, dst.x, dst.y};
    Affine inv;
    if (m.invert(inv)) blitMaskTransformed(sub.target(), img->alpha, img->w, img->h, inv, dst.outer(), withA(s.color, pc.alpha));
    return;
  }
  drawImage(sub.target(), img->view(), dst, {0, 0, iw, ih}, pc.alpha, !s.pixelated, rounded ? &clipR : nullptr,
            rounded ? cr4 : nullptr);
}

void Widgets::paintButtonLabel(Element *e, const PaintCtx &pc, const std::string &label) {
  const ComputedStyle &s = *e->cs;
  const_cast<ComputedStyle &>(s).textAlign = TA_CENTER;
  paintCenteredText(e, pc, label, withA(s.color, pc.alpha));
}

bool Widgets::paint(Element *e, const PaintCtx &pc) {
  if (pc.xf) {
    // transformed controls: images, svg and canvas handle arbitrary transforms
    Tag t = e->tagId();
    if (t != TAG_img && t != TAG_svg && t != TAG_canvas) return false;
  }
  switch (e->tagId()) {
    case TAG_img: {
      RT_PROF(PF_IMAGE);
      paintImage(e, pc);
      return true;
    }
    case TAG_svg: {
      RT_PROF(PF_SVG);
      ElementExtra &x = e->extra();
      std::shared_ptr<SvgCache> svg;
      std::shared_ptr<LayerCache> layer;
      {
        PaintLock lock;
        if (!x.svg) x.svg = buildSvg(e);
        if (!x.layer) x.layer = std::make_shared<LayerCache>();
        svg = x.svg;
        layer = x.layer;
      }
      RectF box = contentRectOf(e);
      Color cur = e->cs->color;
      // the tree is rebuilt on any change (svgDirty), so its address plus the colour and
      // size identify the picture
      uint64_t key = (uint64_t)(uintptr_t)svg.get() ^ ((uint64_t)cur.to565() << 48) ^ ((uint64_t)cur.a << 40) ^
                     ((uint64_t)(uint32_t)(box.w * 16) << 20) ^ (uint64_t)(uint32_t)(box.h * 16);
      if (!paintLayered(*layer, e, pc, box, key, [&](const PaintCtx &lp) { paintSvg(e, *svg, lp, box, cur); }))
        paintSvg(e, *svg, pc, box, cur);
      return true;
    }
    case TAG_canvas: {
      RT_PROF(PF_CANVAS);
      if (e->x && e->x->canvas) e->x->canvas->paint(pc, contentRectOf(e));
      return true;
    }
    case TAG_textarea: paintTextarea(e, pc); return true;
    case TAG_select: paintSelect(e, pc); return true;
    case TAG_progress: paintProgress(e, pc, false); return true;
    case TAG_meter: paintProgress(e, pc, true); return true;
    case TAG_input: {
      RT_PROF(PF_WIDGET);
      std::string t = inputType(e);
      if (t == "checkbox") paintCheck(e, pc, false);
      else if (t == "radio") paintCheck(e, pc, true);
      else if (t == "range") paintRange(e, pc);
      else if (t == "color") paintColor(e, pc);
      else if (t == "button" || t == "submit" || t == "reset")
        paintButtonLabel(e, pc, e->attrOr(A.value, t == "submit" ? "Submit" : t == "reset" ? "Reset" : ""));
      else if (t == "file") paintButtonLabel(e, pc, "Choose file");
      else if (t == "hidden") {}
      else paintTextControl(e, pc);
      return true;
    }
    default:
      return false;
  }
}

void Widgets::afterLayout() {}

// ============================================================ timing

void Widgets::tick(double now) {
  Element *f = eng_.focused();
  if (f && isTextInput(f)) {
    if (now >= nextBlink_) {
      double phase = std::fmod(now - caretT0_, 1060.0);
      bool on = phase < 530;
      if (on != caretOn_) {
        caretOn_ = on;
        eng_.invalidateElement(f);
      }
      nextBlink_ = now + (on ? 530 - phase : 1060 - phase);
    }
  }
  if (kb_ || popup_) {
    // keep the focused field visible above the keyboard
    shiftForKeyboard();
  }
}

uint64_t Widgets::nextWakeMs() const {
  Element *f = eng_.focused();
  if (f && isTextInput(f)) return (uint64_t)nextBlink_;
  return UINT64_MAX;
}

void Widgets::fireInput(Element *e) {
  if (eng_.script()) eng_.script()->dispatch(e, "input", "", true, false);
}

void Widgets::fireChange(Element *e) {
  if (eng_.script()) eng_.script()->dispatch(e, "change", "", true, false);
}

// ============================================================ text editing

void Widgets::focusChanged(Element *old, Element *nw) {
  caretT0_ = eng_.nowMs();
  caretOn_ = true;
  nextBlink_ = 0;
  if (old) eng_.invalidateElement(old);
  if (nw && isTextInput(nw)) {
    ElementExtra &x = nw->extra();
    x.caret = (int)eng_.currentValue(nw).size();
    if (!nw->hasAttr(A.readonly) && !(nw->state & ST_DISABLED)) openKeyboard(nw);
  } else if (kb_) {
    closeKeyboard();
  }
  if (old && isTextInput(old) && old->x && old->x->valueDirty) {
    // change fires on blur when the value changed since focus
    fireChange(old);
  }
}

void Widgets::insertText(Element *e, const std::string &s) {
  if (!e || !isTextInput(e) || e->hasAttr(A.readonly) || (e->state & ST_DISABLED)) return;
  std::string v = eng_.currentValue(e);
  ElementExtra &x = e->extra();
  int caret = clampi(x.caret, 0, (int)v.size());
  std::string ins = s;
  std::string t = inputType(e);
  if (e->tagId() == TAG_input) {
    // single line: no newlines
    ins.erase(std::remove(ins.begin(), ins.end(), '\n'), ins.end());
    if (t == "number") {
      std::string f;
      for (char c : ins)
        if (isdigit((unsigned char)c) || c == '.' || c == '-' || c == 'e') f += c;
      ins = f;
    }
  }
  int maxLen = (int)e->attrNum(A.maxlength, -1);
  if (maxLen >= 0) {
    // count code points
    auto cps = [](const std::string &z) {
      size_t i = 0;
      int n = 0;
      while (i < z.size()) {
        utf8Next(z.data(), z.size(), i);
        n++;
      }
      return n;
    };
    if (cps(v) + cps(ins) > maxLen) return;
  }
  if (ins.empty()) return;
  v.insert(caret, ins);
  x.caret = caret + (int)ins.size();
  eng_.setValue(e, v, true);
  x.caret = caret + (int)ins.size();
  caretT0_ = eng_.nowMs();
  caretOn_ = true;
  fireInput(e);
}

void Widgets::backspace(Element *e) {
  if (!e || !isTextInput(e) || e->hasAttr(A.readonly)) return;
  std::string v = eng_.currentValue(e);
  ElementExtra &x = e->extra();
  int caret = clampi(x.caret, 0, (int)v.size());
  if (caret == 0) return;
  size_t prev = utf8PrevBoundary(v, caret);
  v.erase(prev, caret - prev);
  eng_.setValue(e, v, true);
  x.caret = (int)prev;
  caretT0_ = eng_.nowMs();
  caretOn_ = true;
  fireInput(e);
}

void Widgets::moveCaret(Element *e, int dir) {
  std::string v = eng_.currentValue(e);
  ElementExtra &x = e->extra();
  int c = clampi(x.caret, 0, (int)v.size());
  if (dir < 0 && c > 0) c = (int)utf8PrevBoundary(v, c);
  if (dir > 0 && c < (int)v.size()) {
    size_t i = c;
    utf8Next(v.data(), v.size(), i);
    c = (int)i;
  }
  if (dir == -100) c = 0;
  if (dir == 100) c = (int)v.size();
  x.caret = c;
  caretT0_ = eng_.nowMs();
  caretOn_ = true;
  eng_.invalidateElement(e);
}

void Widgets::placeCaret(Element *e, float sx, float sy) {
  (void)sy;
  if (e->tagId() != TAG_input) {
    e->extra().caret = (int)eng_.currentValue(e).size();
    return;
  }
  const ComputedStyle &s = *e->cs;
  FontRef f = eng_.layout().fontFor(s);
  std::string v = eng_.currentValue(e);
  std::vector<ShapedGlyph> gl;
  ShapeOpts o;
  o.letterSpacing = s.letterSpacing;
  shapeText(f, v.data(), v.size(), o, gl);
  RectF cr = eng_.renderer().screenRect(e);
  float lx = sx - (cr.x + e->box->contentX()) + e->extra().textScroll;
  int caret = (int)v.size();
  for (auto &g : gl) {
    if (lx < g.x + g.adv / 2 && !(g.flags & SG_THAI && !(g.flags & SG_CLUSTER_START))) {
      caret = (int)g.src;
      break;
    }
  }
  e->extra().caret = caret;
  caretT0_ = eng_.nowMs();
  caretOn_ = true;
  eng_.invalidateElement(e);
}

void Widgets::typeText(const std::string &utf8) {
  Element *f = eng_.focused();
  if (!f) return;
  insertText(f, utf8);
}

void Widgets::key(const std::string &k) {
  Element *f = eng_.focused();
  // a handler may cancel the key, or move the focus on (Enter as "next field")
  if (eng_.script() && f && !eng_.script()->dispatch(f, "keydown", "{\"key\":\"" + k + "\"}", true, true)) return;
  if (!f || eng_.focused() != f) return;
  if (k == "Backspace") backspace(f);
  else if (k == "ArrowLeft") moveCaret(f, -1);
  else if (k == "ArrowRight") moveCaret(f, 1);
  else if (k == "Home") moveCaret(f, -100);
  else if (k == "End") moveCaret(f, 100);
  else if (k == "Enter") {
    if (f->tagId() == TAG_textarea) insertText(f, "\n");
    else {
      fireChange(f);
      if (f->x) f->x->valueDirty = f->x->valueDirty;
      // submit the enclosing form
      for (Element *p = f->parent; p; p = p->parent)
        if (p->tagId() == TAG_form) {
          if (eng_.script()) eng_.script()->dispatch(p, "submit", "", true, true);
          break;
        }
      eng_.focus(nullptr);
    }
  } else if (k == "Escape" || k == "Tab") {
    eng_.focus(nullptr);
  }
}

// ============================================================ pointer

bool Widgets::pointerDown(Element *target, float x, float y) {
  (void)y;
  if (target && inputType(target) == "range" && !(target->state & ST_DISABLED)) {
    drag_ = target;
    float mn, mx, st;
    dragStartVal_ = rangeValue(target, mn, mx, st);
    setRangeFromX(target, x);
    return true;
  }
  return false;
}

void Widgets::setRangeFromX(Element *e, float sx) {
  RectF r = eng_.renderer().screenRect(e);
  Box &b = *e->box;
  float thumb = std::min(b.contentH(), 22.f);
  float x0 = r.x + b.contentX() + thumb / 2, x1 = r.x + b.contentX() + b.contentW() - thumb / 2;
  float t = x1 > x0 ? clampf((sx - x0) / (x1 - x0), 0, 1) : 0;
  float mn, mx, step;
  rangeValue(e, mn, mx, step);
  float v = mn + (mx - mn) * t;
  if (step > 0) v = mn + std::round((v - mn) / step) * step;
  v = clampf(v, mn, mx);
  char buf[32];
  snprintf(buf, sizeof buf, "%g", v);
  if (eng_.currentValue(e) != buf) {
    eng_.setValue(e, buf, true);
    fireInput(e);
  }
}

void Widgets::pointerMove(float x, float y) {
  (void)y;
  if (drag_) setRangeFromX(drag_, x);
}

void Widgets::pointerUp(float x, float y, bool cancelled) {
  (void)y;
  if (!drag_) return;
  Element *e = drag_;
  drag_ = nullptr;
  if (!cancelled) setRangeFromX(e, x);
  float mn, mx, st;
  if (rangeValue(e, mn, mx, st) != dragStartVal_) fireChange(e);
}

bool Widgets::activate(Element *t, float x, float y) {
  if (!t) return false;
  // label -> its control
  for (Element *p = t; p; p = p->parent) {
    if (p->tagId() == TAG_label) {
      Element *ctl = nullptr;
      std::string forId = p->attrOr(A.for_, "");
      if (!forId.empty()) ctl = eng_.document()->getElementById(forId);
      if (!ctl)
        p->forEachElement([&](Element *d) {
          if (!ctl && (d->tagId() == TAG_input || d->tagId() == TAG_select || d->tagId() == TAG_textarea)) ctl = d;
        });
      if (ctl && ctl != t && !ctl->isInclusiveAncestorOf(t)) {
        if (eng_.script()) eng_.script()->dispatch(ctl, "click", "", true, true);
        return activate(ctl, x, y);
      }
      break;
    }
  }
  if (t->state & ST_DISABLED) return true;
  Tag tg = t->tagId();
  if (tg == TAG_input) {
    std::string type = inputType(t);
    if (type == "checkbox") {
      eng_.setChecked(t, !t->hasState(ST_CHECKED), true);
      t->setState(ST_INDETERMINATE, false);
      fireInput(t);
      fireChange(t);
      return true;
    }
    if (type == "radio") {
      if (!t->hasState(ST_CHECKED)) {
        eng_.setChecked(t, true, true);
        fireInput(t);
        fireChange(t);
      }
      return true;
    }
    if (type == "date" || type == "datetime-local" || type == "month") {
      eng_.focus(t);
      openDatePicker(t);
      return true;
    }
    if (type == "time") {
      eng_.focus(t);
      openTimePicker(t);
      return true;
    }
    if (type == "color") {
      eng_.focus(t);
      openColorPicker(t);
      return true;
    }
    if (type == "submit" || type == "reset" || type == "button" || type == "image") {
      for (Element *p = t->parent; p; p = p->parent)
        if (p->tagId() == TAG_form) {
          if (type == "submit" && eng_.script()) eng_.script()->dispatch(p, "submit", "", true, true);
          if (type == "reset") {
            p->forEachElement([&](Element *c) {
              if (c->x) {
                c->x->valueDirty = false;
                c->x->checkedDirty = false;
                c->x->selectedIndex = -2;
              }
              if (c->tagId() == TAG_input) c->setState(ST_CHECKED, c->hasAttr(A.checked));
              eng_.refreshFormState(c);
              eng_.invalidateElement(c);
            });
          }
          break;
        }
      return true;
    }
    if (isTextInput(t)) {
      eng_.focus(t);
      placeCaret(t, x, y);
      return true;
    }
    return true;
  }
  if (tg == TAG_textarea) {
    eng_.focus(t);
    placeCaret(t, x, y);
    return true;
  }
  if (tg == TAG_select) {
    eng_.focus(t);
    openSelect(t);
    return true;
  }
  if (tg == TAG_button) {
    std::string type = toLower(t->attrOr(A.type, "submit"));
    for (Element *p = t->parent; p; p = p->parent)
      if (p->tagId() == TAG_form) {
        if (type == "submit" && eng_.script()) eng_.script()->dispatch(p, "submit", "", true, true);
        break;
      }
    eng_.focus(nullptr);
    return true;
  }
  if (tg == TAG_summary) {
    Element *d = t->parent;
    if (d && d->tagId() == TAG_details) {
      if (d->hasAttr(A.open)) d->removeAttr(A.open);
      else d->setAttr(A.open, "");
      if (eng_.script()) eng_.script()->dispatch(d, "toggle", "", false, false);
    }
    return true;
  }
  return false;
}

// ============================================================ <select>

std::vector<Element *> Widgets::options(Element *sel) const {
  std::vector<Element *> v;
  sel->forEachElement([&](Element *e) {
    if (e->tagId() == TAG_option) v.push_back(e);
  });
  return v;
}

int Widgets::selectedIndex(Element *sel) const {
  auto opts = options(sel);
  if (sel->x && sel->x->selectedIndex >= -1) return sel->x->selectedIndex < (int)opts.size() ? sel->x->selectedIndex : -1;
  for (size_t i = 0; i < opts.size(); i++)
    if (opts[i]->hasAttr(A.selected)) return (int)i;
  return opts.empty() ? -1 : 0;
}

void Widgets::setSelectedIndex(Element *sel, int i, bool fromUser) {
  int old = selectedIndex(sel);
  sel->extra().selectedIndex = i;
  auto opts = options(sel);
  for (size_t k = 0; k < opts.size(); k++) opts[k]->setState(ST_CHECKED, (int)k == i);
  eng_.invalidateElement(sel);
  if (fromUser && old != i) {
    fireInput(sel);
    fireChange(sel);
  }
}

std::string Widgets::selectValue(Element *sel) const {
  int i = selectedIndex(sel);
  auto opts = options(sel);
  if (i < 0 || i >= (int)opts.size()) return "";
  Element *o = opts[i];
  return o->hasAttr(A.value) ? o->attrOr(A.value, "") : trim(o->textContent());
}

void Widgets::selectSetValue(Element *sel, const std::string &v) {
  auto opts = options(sel);
  for (size_t i = 0; i < opts.size(); i++) {
    std::string ov = opts[i]->hasAttr(A.value) ? opts[i]->attrOr(A.value, "") : trim(opts[i]->textContent());
    if (ov == v) {
      setSelectedIndex(sel, (int)i, false);
      return;
    }
  }
  setSelectedIndex(sel, -1, false);
}

// ============================================================ overlays

Element *Widgets::makeOverlay(const std::string &html) {
  Document *doc = eng_.document();
  if (!doc || !doc->html) return nullptr;
  Element *frag = parseFragment(doc, html, TAG_div);
  frag->ref();
  Element *root = frag->firstElementChild();
  if (root) {
    root->forEachElement([](Element *e) { e->flags |= NF_INTERNAL; });
    doc->html->appendChild(root);
  }
  frag->unref();
  return root;
}

static std::string esc(const std::string &s) { return escapeText(s, true); }

void Widgets::buildKeyboard() {
  static const char *th[2][4] = {
      {"\xE0\xB9\x85 / - \xE0\xB8\xA0 \xE0\xB8\x96 \xE0\xB8\xB8 \xE0\xB8\xB6 \xE0\xB8\x84 \xE0\xB8\x95 \xE0\xB8\x88 \xE0\xB8\x82 \xE0\xB8\x8A",
       "\xE0\xB9\x86 \xE0\xB9\x84 \xE0\xB8\xB3 \xE0\xB8\x9E \xE0\xB8\xB0 \xE0\xB8\xB1 \xE0\xB8\xB5 \xE0\xB8\xA3 \xE0\xB8\x99 \xE0\xB8\xA2 \xE0\xB8\x9A \xE0\xB8\xA5 \xE0\xB8\x83",
       "\xE0\xB8\x9F \xE0\xB8\xAB \xE0\xB8\x81 \xE0\xB8\x94 \xE0\xB9\x80 \xE0\xB9\x89 \xE0\xB9\x88 \xE0\xB8\xB2 \xE0\xB8\xAA \xE0\xB8\xA7 \xE0\xB8\x87",
       "\xE0\xB8\x9C \xE0\xB8\x9B \xE0\xB9\x81 \xE0\xB8\xAD \xE0\xB8\xB4 \xE0\xB8\xB7 \xE0\xB8\x97 \xE0\xB8\xA1 \xE0\xB9\x83 \xE0\xB8\x9D"},
      {"+ \xE0\xB9\x91 \xE0\xB9\x92 \xE0\xB9\x93 \xE0\xB9\x94 \xE0\xB8\xB9 \xE0\xB8\xBF \xE0\xB9\x95 \xE0\xB9\x96 \xE0\xB9\x97 \xE0\xB9\x98 \xE0\xB9\x99",
       "\xE0\xB9\x90 \" \xE0\xB8\x8E \xE0\xB8\x91 \xE0\xB8\x98 \xE0\xB9\x8D \xE0\xB9\x8A \xE0\xB8\x93 \xE0\xB8\xAF \xE0\xB8\x8D \xE0\xB8\x90 , \xE0\xB8\x85",
       "\xE0\xB8\xA4 \xE0\xB8\x86 \xE0\xB8\x8F \xE0\xB9\x82 \xE0\xB8\x8C \xE0\xB9\x87 \xE0\xB9\x8B \xE0\xB8\xA9 \xE0\xB8\xA8 \xE0\xB8\x8B .",
       "( ) \xE0\xB8\x89 \xE0\xB8\xAE \xE0\xB8\xBA \xE0\xB9\x8C ? \xE0\xB8\x92 \xE0\xB8\xAC \xE0\xB8\xA6"}};
  static const char *en[2][4] = {{"1 2 3 4 5 6 7 8 9 0", "q w e r t y u i o p", "a s d f g h j k l", "z x c v b n m"},
                                 {"1 2 3 4 5 6 7 8 9 0", "Q W E R T Y U I O P", "A S D F G H J K L", "Z X C V B N M"}};
  static const char *sym[4] = {"1 2 3 4 5 6 7 8 9 0", "- / : ; ( ) \xE0\xB8\xBF & @ \"", ". , ? ! ' # % * + =",
                               "_ \\ | ~ < > [ ] { }"};
  std::string h = "<div class=\"__kb" + std::string(kbLayout_ == 3 ? " __num" : "") + (kbOpen_ ? " __open" : "") + "\">";
  auto key = [&](const std::string &k, const std::string &label, const char *cls) {
    h += "<div class=\"__key";
    if (cls) {
      h += " ";
      h += cls;
    }
    h += "\" data-k=\"" + esc(k) + "\">" + escapeText(label, false) + "</div>";
  };
  auto row = [&](const char *line, bool withShift, bool withBack) {
    h += "<div class=\"__kbrow\">";
    if (withShift) key("#shift", "\xE2\x87\xA7", kbShift_ ? "__fn __on" : "__fn");
    for (auto &k : split(line, ' ')) {
      if (k.empty()) continue;
      size_t i = 0;
      uint32_t cp = utf8Next(k.data(), k.size(), i);
      std::string label = isThaiMark(cp) ? "\xE2\x97\x8C" + k : k;  // show marks on a dotted circle
      key(k, label, nullptr);
    }
    if (withBack) key("#back", "\xE2\x8C\xAB", "__fn");
    h += "</div>";
  };
  if (kbLayout_ == 3) {
    std::string t = kbFor_ ? inputType(kbFor_) : "";
    const char *rows[] = {"1 2 3", "4 5 6", "7 8 9"};
    for (auto r : rows) row(r, false, false);
    h += "<div class=\"__kbrow\">";
    key(t == "tel" ? "+" : "-", t == "tel" ? "+" : "\xE2\x88\x92", "__fn");
    key("0", "0", nullptr);
    key(".", ".", "__fn");
    key("#back", "\xE2\x8C\xAB", "__fn");
    key("#enter", "\xE0\xB9\x80\xE0\xB8\xAA\xE0\xB8\xA3\xE0\xB9\x87\xE0\xB8\x88", "__enter");
    h += "</div>";
  } else {
    const char *const *rows = kbLayout_ == 0 ? th[kbShift_] : kbLayout_ == 1 ? en[kbShift_] : sym;
    row(rows[0], false, false);
    row(rows[1], false, false);
    row(rows[2], false, false);
    row(rows[3], kbLayout_ != 2, true);
    h += "<div class=\"__kbrow\">";
    key("#sym", kbLayout_ == 2 ? "ABC" : "?123", "__fn");
    key("#lang", kbLayout_ == 0 ? "EN" : "\xE0\xB9\x84\xE0\xB8\x97\xE0\xB8\xA2", "__fn");
    key(" ", kbLayout_ == 0 ? "\xE0\xB9\x80\xE0\xB8\xA7\xE0\xB9\x89\xE0\xB8\x99\xE0\xB8\xA7\xE0\xB8\xA3\xE0\xB8\xA3\xE0\xB8\x84" : "space", "__space");
    key(".", ".", "__fn");
    key("#hide", "\xE2\x8C\x84", "__fn");
    key("#enter", "\xE2\x86\xB5", "__enter");
    h += "</div>";
  }
  h += "</div>";
  Element *old = kb_;
  kb_ = makeOverlay(h);
  if (old) old->remove();
}

void Widgets::openKeyboard(Element *forInput) {
  kbFor_ = forInput;
  std::string t = inputType(forInput);
  std::string mode = toLower(forInput->attrOr(A.inputmode, ""));
  int want = (t == "number" || t == "tel" || mode == "numeric" || mode == "decimal" || mode == "tel") ? 3
             : (t == "email" || t == "url" || t == "password") ? 1
                                                                : (kbLayout_ == 3 ? 0 : kbLayout_);
  bool rebuild = !kb_ || want != kbLayout_;
  kbLayout_ = want;
  kbShift_ = false;
  if (rebuild) buildKeyboard();
  if (!kbOpen_ && kb_) {
    kbOpen_ = true;
    // add the open class next frame so the slide-in transitions
    kb_->setClassList({intern("__kb"), intern(kbLayout_ == 3 ? "__num" : "__kbx"), intern("__open")});
  }
  eng_.requestFrame();
}

void Widgets::closeKeyboard() {
  if (!kb_) return;
  kbOpen_ = false;
  kb_->setClassList({intern("__kb"), intern(kbLayout_ == 3 ? "__num" : "__kbx")});
  eng_.requestFrame();
}

float Widgets::keyboardHeight() const {
  if (!kb_ || !kbOpen_ || !kb_->box) return 0;
  return kb_->box->h;
}

void Widgets::shiftForKeyboard() {
  // the renderer shifts the page up when the focused control would be covered
  float shift = 0;
  Element *f = eng_.focused();
  if (kbOpen_ && f && f->box && kb_ && kb_->box) {
    float kbTop = eng_.display().height() - kb_->box->h;
    RectF r = f->box->borderRect();
    float scroll = eng_.document()->html && eng_.document()->html->x ? eng_.document()->html->x->scrollY : 0;
    float bottom = r.bottom() - scroll + 12;
    if (bottom > kbTop) shift = bottom - kbTop;
  }
  InputController &in = eng_.input();
  in.setPageShift(shift);
}

void Widgets::openSelect(Element *sel) {
  closePopup();
  popupFor_ = sel;
  popupKind_ = 1;
  std::string h = "<div class=\"__ov\" data-a=\"close\"><div class=\"__sheet\">";
  int cur = selectedIndex(sel);
  int i = 0;
  std::function<void(Element *)> walk = [&](Element *p) {
    for (Node *c = p->first; c; c = c->next) {
      Element *e = c->asElement();
      if (!e) continue;
      if (e->tagId() == TAG_optgroup) {
        h += "<div class=\"__grp\">" + escapeText(e->attrOr(A.label, ""), false) + "</div>";
        walk(e);
      } else if (e->tagId() == TAG_option) {
        std::string cls = "__opt";
        if (i == cur) cls += " __sel";
        bool dis = e->hasAttr(A.disabled);
        if (dis) cls += " __dis";
        h += "<div class=\"" + cls + "\" data-i=\"" + (dis ? std::string("-1") : std::to_string(i)) + "\">" +
             escapeText(trim(e->textContent()), false) + "</div>";
        i++;
      }
    }
  };
  walk(sel);
  h += "</div></div>";
  popup_ = makeOverlay(h);
  eng_.requestFrame();
}

static const char *kMonthsTh[] = {"\xE0\xB8\xA1\xE0\xB8\x81\xE0\xB8\xA3\xE0\xB8\xB2\xE0\xB8\x84\xE0\xB8\xA1", "\xE0\xB8\x81\xE0\xB8\xB8\xE0\xB8\xA1\xE0\xB8\xA0\xE0\xB8\xB2\xE0\xB8\x9E\xE0\xB8\xB1\xE0\xB8\x99\xE0\xB8\x98\xE0\xB9\x8C",
                                  "\xE0\xB8\xA1\xE0\xB8\xB5\xE0\xB8\x99\xE0\xB8\xB2\xE0\xB8\x84\xE0\xB8\xA1", "\xE0\xB9\x80\xE0\xB8\xA1\xE0\xB8\xA9\xE0\xB8\xB2\xE0\xB8\xA2\xE0\xB8\x99",
                                  "\xE0\xB8\x9E\xE0\xB8\xA4\xE0\xB8\xA9\xE0\xB8\xA0\xE0\xB8\xB2\xE0\xB8\x84\xE0\xB8\xA1", "\xE0\xB8\xA1\xE0\xB8\xB4\xE0\xB8\x96\xE0\xB8\xB8\xE0\xB8\x99\xE0\xB8\xB2\xE0\xB8\xA2\xE0\xB8\x99",
                                  "\xE0\xB8\x81\xE0\xB8\xA3\xE0\xB8\x81\xE0\xB8\x8E\xE0\xB8\xB2\xE0\xB8\x84\xE0\xB8\xA1", "\xE0\xB8\xAA\xE0\xB8\xB4\xE0\xB8\x87\xE0\xB8\xAB\xE0\xB8\xB2\xE0\xB8\x84\xE0\xB8\xA1",
                                  "\xE0\xB8\x81\xE0\xB8\xB1\xE0\xB8\x99\xE0\xB8\xA2\xE0\xB8\xB2\xE0\xB8\xA2\xE0\xB8\x99", "\xE0\xB8\x95\xE0\xB8\xB8\xE0\xB8\xA5\xE0\xB8\xB2\xE0\xB8\x84\xE0\xB8\xA1",
                                  "\xE0\xB8\x9E\xE0\xB8\xA4\xE0\xB8\xA8\xE0\xB8\x88\xE0\xB8\xB4\xE0\xB8\x81\xE0\xB8\xB2\xE0\xB8\xA2\xE0\xB8\x99", "\xE0\xB8\x98\xE0\xB8\xB1\xE0\xB8\x99\xE0\xB8\xA7\xE0\xB8\xB2\xE0\xB8\x84\xE0\xB8\xA1"};

static int daysIn(int y, int m) {
  static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
  return d[m - 1];
}

static int weekday(int y, int m, int d) {  // 0 = Sunday (Zeller)
  if (m < 3) {
    m += 12;
    y--;
  }
  int k = y % 100, j = y / 100;
  int h = (d + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
  return (h + 6) % 7;
}

void Widgets::openDatePicker(Element *input) {
  std::string v = eng_.currentValue(input);
  int y = 0, m = 0, d = 0;
  if (sscanf(v.c_str(), "%d-%d-%d", &y, &m, &d) < 2 || y < 1900) {
    if (popupFor_ != input || popupKind_ != 2) {
      time_t now = (time_t)(eng_.platform().micros() / 1000000);
      (void)now;
      y = pickYear_;
      m = pickMonth_;
    } else {
      y = pickYear_;
      m = pickMonth_;
    }
  } else if (popupFor_ == input && popupKind_ == 2) {
    y = pickYear_;
    m = pickMonth_;
  }
  pickYear_ = y;
  pickMonth_ = m < 1 ? 1 : m;
  closePopup();
  popupFor_ = input;
  popupKind_ = 2;
  char title[64];
  snprintf(title, sizeof title, "%s %d", kMonthsTh[pickMonth_ - 1], pickYear_ + 543);
  std::string h = "<div class=\"__ov\" data-a=\"close\"><div class=\"__sheet __cal\"><div class=\"__calhead\">"
                  "<div class=\"__nav\" data-a=\"prev\">\xE2\x80\xB9</div><div class=\"__title\">";
  h += title;
  h += "</div><div class=\"__nav\" data-a=\"next\">\xE2\x80\xBA</div></div><div class=\"__grid\">";
  static const char *dow[] = {"\xE0\xB8\xAD\xE0\xB8\xB2", "\xE0\xB8\x88", "\xE0\xB8\xAD", "\xE0\xB8\x9E", "\xE0\xB8\x9E\xE0\xB8\xA4", "\xE0\xB8\xA8", "\xE0\xB8\xAA"};
  for (auto w : dow) h += std::string("<div class=\"__dow\">") + w + "</div>";
  int first = weekday(pickYear_, pickMonth_, 1);
  int nd = daysIn(pickYear_, pickMonth_);
  for (int i = 0; i < first; i++) h += "<div class=\"__day __other\"></div>";
  for (int day = 1; day <= nd; day++) {
    char iso[16];
    snprintf(iso, sizeof iso, "%04d-%02d-%02d", pickYear_, pickMonth_, day);
    std::string cls = "__day";
    if (v == iso) cls += " __sel";
    h += "<div class=\"" + cls + "\" data-d=\"" + iso + "\">" + std::to_string(day) + "</div>";
  }
  h += "</div><div class=\"__bar\"><div class=\"__btn\" data-a=\"clear\">\xE0\xB8\xA5\xE0\xB9\x89\xE0\xB8\xB2\xE0\xB8\x87</div>"
       "<div class=\"__btn __primary\" data-a=\"close\">\xE0\xB9\x80\xE0\xB8\xAA\xE0\xB8\xA3\xE0\xB9\x87\xE0\xB8\x88</div></div></div></div>";
  popup_ = makeOverlay(h);
  eng_.requestFrame();
}

void Widgets::openTimePicker(Element *input) {
  std::string v = eng_.currentValue(input);
  int hh = 12, mm = 0;
  if (sscanf(v.c_str(), "%d:%d", &hh, &mm) < 2) {
    hh = pickHour_;
    mm = pickMin_;
  }
  if (popupFor_ == input && popupKind_ == 3) {
    hh = pickHour_;
    mm = pickMin_;
  }
  pickHour_ = hh;
  pickMin_ = mm;
  closePopup();
  popupFor_ = input;
  popupKind_ = 3;
  char hs[8], ms[8];
  snprintf(hs, sizeof hs, "%02d", hh);
  snprintf(ms, sizeof ms, "%02d", mm);
  std::string h = "<div class=\"__ov\" data-a=\"close\"><div class=\"__sheet\"><div class=\"__time\">"
                  "<div class=\"__col\"><div class=\"__nav\" data-a=\"h+\">\xE2\x96\xB2</div><div class=\"__big\">";
  h += hs;
  h += "</div><div class=\"__nav\" data-a=\"h-\">\xE2\x96\xBC</div></div><div class=\"__colon\">:</div>"
       "<div class=\"__col\"><div class=\"__nav\" data-a=\"m+\">\xE2\x96\xB2</div><div class=\"__big\">";
  h += ms;
  h += "</div><div class=\"__nav\" data-a=\"m-\">\xE2\x96\xBC</div></div></div>"
       "<div class=\"__bar\"><div class=\"__btn __primary\" data-a=\"settime\">\xE0\xB9\x80\xE0\xB8\xAA\xE0\xB8\xA3\xE0\xB9\x87\xE0\xB8\x88</div></div></div></div>";
  popup_ = makeOverlay(h);
  eng_.requestFrame();
}

void Widgets::openColorPicker(Element *input) {
  closePopup();
  popupFor_ = input;
  popupKind_ = 4;
  static const char *pal[] = {"#ef4444", "#f97316", "#f59e0b", "#eab308", "#84cc16", "#22c55e", "#10b981", "#14b8a6",
                              "#06b6d4", "#0ea5e9", "#3b82f6", "#6366f1", "#8b5cf6", "#a855f7", "#d946ef", "#ec4899",
                              "#f43f5e", "#78716c", "#64748b", "#000000", "#374151", "#9ca3af", "#e5e7eb", "#ffffff",
                              "#7f1d1d", "#7c2d12", "#713f12", "#14532d", "#134e4a", "#1e3a8a", "#4c1d95", "#831843"};
  std::string cur = toLower(eng_.currentValue(input));
  std::string h = "<div class=\"__ov\" data-a=\"close\"><div class=\"__sheet\"><div class=\"__swatches\">";
  for (auto c : pal) {
    std::string cls = std::string("__sw") + (cur == c ? " __sel" : "");
    h += "<div class=\"" + cls + "\" data-c=\"" + c + "\" style=\"background:" + c + "\"></div>";
  }
  h += "</div></div></div>";
  popup_ = makeOverlay(h);
  eng_.requestFrame();
}

void Widgets::closePopup() {
  if (popup_) {
    Element *p = popup_;
    popup_ = nullptr;
    p->remove();
  }
  popupKind_ = 0;
  eng_.requestFrame();
}

bool Widgets::internalTap(Element *t) {
  // find the nearest element carrying an action
  Element *a = t;
  while (a && !(a->hasAttr(intern("data-k")) || a->hasAttr(intern("data-a")) || a->hasAttr(intern("data-i")) ||
                a->hasAttr(intern("data-d")) || a->hasAttr(intern("data-c"))))
    a = a->parent;
  if (!a) return true;
  // keyboard
  if (a->hasAttr(intern("data-k"))) {
    std::string k = a->attrOr(intern("data-k"), "");
    Element *f = kbFor_ ? kbFor_ : eng_.focused();
    if (k == "#shift") {
      kbShift_ = !kbShift_;
      buildKeyboard();
    } else if (k == "#back") {
      backspace(f);
    } else if (k == "#enter") {
      if (f && f->tagId() == TAG_textarea) insertText(f, "\n");
      else key("Enter");
    } else if (k == "#lang") {
      kbLayout_ = kbLayout_ == 0 ? 1 : 0;
      kbShift_ = false;
      buildKeyboard();
    } else if (k == "#sym") {
      kbLayout_ = kbLayout_ == 2 ? 0 : 2;
      kbShift_ = false;
      buildKeyboard();
    } else if (k == "#hide") {
      eng_.focus(nullptr);
    } else {
      insertText(f, k);
      if (kbShift_ && kbLayout_ != 2) {
        kbShift_ = false;
        buildKeyboard();
      }
    }
    return true;
  }
  Element *forEl = popupFor_;
  if (a->hasAttr(intern("data-i")) && forEl) {
    int i = atoi(a->attrOr(intern("data-i"), "-1").c_str());
    if (i >= 0) {
      setSelectedIndex(forEl, i, true);
      closePopup();
      eng_.focus(nullptr);
    }
    return true;
  }
  if (a->hasAttr(intern("data-d")) && forEl) {
    eng_.setValue(forEl, a->attrOr(intern("data-d"), ""), true);
    fireInput(forEl);
    fireChange(forEl);
    closePopup();
    eng_.focus(nullptr);
    return true;
  }
  if (a->hasAttr(intern("data-c")) && forEl) {
    eng_.setValue(forEl, a->attrOr(intern("data-c"), ""), true);
    fireInput(forEl);
    fireChange(forEl);
    closePopup();
    eng_.focus(nullptr);
    return true;
  }
  std::string act = a->attrOr(intern("data-a"), "");
  if (act == "close") {
    // only when tapping the backdrop itself or a close button
    if (a == t || a->hasClass(intern("__btn"))) {
      closePopup();
      eng_.focus(nullptr);
    }
  } else if (act == "prev" || act == "next") {
    pickMonth_ += act == "next" ? 1 : -1;
    if (pickMonth_ < 1) {
      pickMonth_ = 12;
      pickYear_--;
    }
    if (pickMonth_ > 12) {
      pickMonth_ = 1;
      pickYear_++;
    }
    Element *in = forEl;
    openDatePicker(in);
    popupFor_ = in;
  } else if (act == "clear" && forEl) {
    eng_.setValue(forEl, "", true);
    fireInput(forEl);
    fireChange(forEl);
    closePopup();
    eng_.focus(nullptr);
  } else if (act == "h+" || act == "h-" || act == "m+" || act == "m-") {
    if (act == "h+") pickHour_ = (pickHour_ + 1) % 24;
    if (act == "h-") pickHour_ = (pickHour_ + 23) % 24;
    if (act == "m+") pickMin_ = (pickMin_ + 5) % 60;
    if (act == "m-") pickMin_ = (pickMin_ + 55) % 60;
    Element *in = forEl;
    popupFor_ = in;
    popupKind_ = 3;
    openTimePicker(in);
  } else if (act == "settime" && forEl) {
    char buf[8];
    snprintf(buf, sizeof buf, "%02d:%02d", pickHour_, pickMin_);
    eng_.setValue(forEl, buf, true);
    fireInput(forEl);
    fireChange(forEl);
    closePopup();
    eng_.focus(nullptr);
  }
  return true;
}

}  // namespace rt
