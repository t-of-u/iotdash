#include "render.h"

#include "extra.h"
#include "prof.h"

namespace rt {

// ============================================================ geometry helpers

void borderRadii(Element *e, const RectF &br, float out[4]) {
  const ComputedStyle &s = *e->cs;
  float ref = std::min(br.w, br.h);
  for (int i = 0; i < 4; i++) {
    const Len &l = s.radius[i];
    float v = l.kind == LK_LEN ? (l.pct ? l.resolve(i == 0 || i == 2 ? br.w : br.w) : l.px) : 0;
    if (l.pct && !l.calc) v = l.px + l.pct * 0.01f * ref;  // % of the box (circle for 50%)
    out[i] = std::max(0.f, v);
  }
}

void insetRadii(const float in[4], float l, float t, float r, float b, float out[4]) {
  out[0] = std::max(0.f, in[0] - std::max(l, t));
  out[1] = std::max(0.f, in[1] - std::max(r, t));
  out[2] = std::max(0.f, in[2] - std::max(r, b));
  out[3] = std::max(0.f, in[3] - std::max(l, b));
}

Affine elementTransform(Element *e) {
  const ComputedStyle &s = *e->cs;
  Box &b = *e->box;
  if (!s.transform) return Affine();
  float ox = b.absX + s.originX.resolve(b.w), oy = b.absY + s.originY.resolve(b.h);
  Affine m = Affine::translate(ox, oy);
  ResolveCtx rc;
  rc.em = s.fontSize;
  for (auto &f : *s.transform) {
    switch (f.kind) {
      case TransformFn::TRANSLATE: {
        Len lx = computeLen(f.a, rc), ly = computeLen(f.b, rc);
        m = m * Affine::translate(lx.resolve(b.w), ly.resolve(b.h));
        break;
      }
      case TransformFn::SCALE: m = m * Affine::scale(f.v[0], f.v[1]); break;
      case TransformFn::ROTATE: m = m * Affine::rotate(f.v[0]); break;
      case TransformFn::SKEW: m = m * Affine{1, std::tan(f.v[1]), std::tan(f.v[0]), 1, 0, 0}; break;
      case TransformFn::MATRIX: m = m * Affine{f.v[0], f.v[1], f.v[2], f.v[3], f.v[4], f.v[5]}; break;
    }
  }
  return m * Affine::translate(-ox, -oy);
}

static bool isInlineBox(Element *e) {
  return e->cs && e->cs->display == D_INLINE && !LayoutEngine::isAtomicInline(e);
}

// ============================================================ paint list

Renderer::St Renderer::selfState(Element *e, const St &st) {
  St s = st;
  const ComputedStyle &cs = *e->cs;
  Box &b = *e->box;
  if (b.fixed && cs.position == POS_FIXED) {
    s.clip = {0, 0, width, height};
    s.m = Affine();
    s.hasXf = false;
  }
  s.alpha = st.alpha * cs.opacity;
  if (cs.hasTransform()) {
    Affine t = elementTransform(e);
    s.m = s.m * t;
    s.hasXf = !s.m.isTranslate();
  }
  if (s.hasXf) {
    xforms_.push_back(s.m);
    s.xf = (int16_t)(xforms_.size() - 1);
  } else {
    s.xf = -1;
  }
  s.dx = s.m.e;
  s.dy = s.m.f;
  return s;
}

Renderer::St Renderer::childState(Element *e, const St &self) const {
  St s = self;
  const ComputedStyle &cs = *e->cs;
  Box &b = *e->box;
  if (cs.clips() || e->tag == TAG_html) {
    RectF pr = b.paddingRect();
    if (e->tag == TAG_html) pr = RectF{0, 0, (float)width, (float)height};
    RectF scr = e->tag == TAG_html ? pr : self.m.mapRect(pr);
    if (e->tag != TAG_html) s.clip = s.clip.intersect(scr.outer());
    // scroll offset
    if (e->x && (e->x->scrollX != 0 || e->x->scrollY != 0)) {
      s.m = s.m * Affine::translate(-e->x->scrollX, -e->x->scrollY);
      s.dx = s.m.e;
      s.dy = s.m.f;
    }
  }
  return s;
}

RectI Renderer::inkRect(Element *e, const St &st) const {
  const ComputedStyle &cs = *e->cs;
  Box &b = *e->box;
  RectF r = b.borderRect();
  if (isInlineBox(e)) {
    RectF u;
    Box &o = *b.origin->box;
    for (auto &ir : b.inlineRects) u = u.unite(ir.translated(o.absX, o.absY));
    r = u;
  }
  // text that overflows the box
  for (auto &il : b.inlines) {
    for (auto &ln : il->lines) {
      if (ln.f1 == ln.f0) continue;
      float x0 = il->frags[ln.f0].x, x1 = il->frags[ln.f1 - 1].x + il->frags[ln.f1 - 1].w;
      RectF lr{b.absX + x0 - 2, b.absY + ln.y - 2, x1 - x0 + 4, ln.h + 4};
      r = r.unite(lr);
    }
  }
  if (cs.boxShadow) {
    for (auto &sh : *cs.boxShadow) {
      if (sh.inset) continue;
      ResolveCtx rc;
      rc.em = cs.fontSize;
      float bl = computeLen(sh.blur, rc).px, sp = computeLen(sh.spread, rc).px;
      float ox = computeLen(sh.x, rc).px, oy = computeLen(sh.y, rc).px;
      float ext = bl * 1.5f + sp + 1;
      RectF bb = b.borderRect();
      r = r.unite({bb.x + ox - ext, bb.y + oy - ext, bb.w + 2 * ext, bb.h + 2 * ext});
    }
  }
  if (cs.outlineStyle != BS_NONE && cs.outlineWidth > 0) {
    float o = cs.outlineWidth + cs.outlineOffset;
    RectF bb = b.borderRect();
    r = r.unite({bb.x - o, bb.y - o, bb.w + 2 * o, bb.h + 2 * o});
  }
  if (cs.textShadow) r = RectF{r.x - 8, r.y - 8, r.w + 16, r.h + 16};
  // painted controls draw a little outside their box (range thumb shadow, focus rings)
  if (e->tag == TAG_input || e->tag == TAG_select || e->tag == TAG_progress || e->tag == TAG_meter)
    r = RectF{r.x - 8, r.y - 8, r.w + 16, r.h + 16};
  if (cs.display == D_LIST_ITEM) r = r.unite({b.absX - 40, b.absY, 40, std::min(b.h, cs.fontSize * 2)});
  RectF sr = st.m.mapRect(r);
  RectI ir = sr.outer();
  ir = {ir.x - 1, ir.y - 1, ir.w + 2, ir.h + 2};
  return ir.intersect(st.clip);
}

void Renderer::addItem(Element *e, const St &st) {
  PaintItem it;
  it.el = e;
  it.clip = st.clip;
  it.dx = st.dx;
  it.dy = st.dy;
  it.alpha = (uint8_t)clampi((int)std::lround(st.alpha * 255), 0, 255);
  it.xf = st.xf;
  it.ink = inkRect(e, st);
  index_[e] = items_.size();
  items_.push_back(it);
}

static bool createsSC(Element *e) {
  const ComputedStyle &s = *e->cs;
  return s.makesStackingContext() || s.position != POS_STATIC;
}

void Renderer::walkFlow(Element *e, const St &st, std::vector<std::pair<Element *, St>> &positioned,
                        std::vector<std::pair<Element *, St>> &negZ, std::vector<std::pair<Element *, St>> &posZ) {
  St cst = childState(e, st);
  LayoutEngine::forEachLayoutChild(e, [&](Node *n) {
    Element *c = n->asElement();
    if (!c || !c->cs || !c->box || c->cs->display == D_NONE) return;
    if (c->cs->display == D_CONTENTS) return;
    St base = cst;
    if (c->cs->position == POS_FIXED) {
      base.clip = {0, 0, width, height};
      base.m = Affine();
      base.hasXf = false;
      base.dx = base.dy = 0;
    }
    if (createsSC(c)) {
      if (!c->cs->zAuto && c->cs->zIndex < 0) negZ.push_back({c, base});
      else if (!c->cs->zAuto && c->cs->zIndex > 0) posZ.push_back({c, base});
      else positioned.push_back({c, base});
      return;
    }
    St ss = selfState(c, base);
    addItem(c, ss);
    walkFlow(c, ss, positioned, negZ, posZ);
  });
}

void Renderer::buildSC(Element *root, const St &st, bool) {
  St self = selfState(root, st);
  addItem(root, self);
  std::vector<std::pair<Element *, St>> positioned, negZ, posZ;
  // negative z children must paint before the root's flow content but after its
  // background; we approximate by painting them right after the root item.
  size_t rootEnd = items_.size();
  std::vector<PaintItem> flow;
  walkFlow(root, self, positioned, negZ, posZ);
  // move flow items aside, emit negZ, then put them back
  std::vector<PaintItem> flowItems(items_.begin() + rootEnd, items_.end());
  items_.resize(rootEnd);
  std::stable_sort(negZ.begin(), negZ.end(), [](auto &a, auto &b) { return a.first->cs->zIndex < b.first->cs->zIndex; });
  for (auto &p : negZ) buildSC(p.first, p.second, true);
  for (auto &it : flowItems) {
    index_[it.el] = items_.size();
    items_.push_back(it);
  }
  for (auto &p : positioned) buildSC(p.first, p.second, true);
  std::stable_sort(posZ.begin(), posZ.end(), [](auto &a, auto &b) { return a.first->cs->zIndex < b.first->cs->zIndex; });
  for (auto &p : posZ) buildSC(p.first, p.second, true);
}

void Renderer::build(Document *doc) {
  // what was painted before: an element that drops out of the list (display: none, now
  // outside every stacking context) leaves its last ink behind unless it is damaged here
  std::vector<Element *> before;
  before.reserve(index_.size());
  for (auto &kv : index_) before.push_back(kv.first);
  items_.clear();
  xforms_.clear();
  index_.clear();
  struct Gone {
    Renderer *r;
    std::vector<Element *> &before;
    ~Gone() {
      for (Element *e : before)
        if (!r->index_.count(e) && e->box && e->box->painted) {
          if (!e->box->lastInk.empty()) r->gone_.push_back(e->box->lastInk);
          e->box->painted = false;
        }
    }
  } gone{this, before};
  Element *html = doc->html;
  if (!html || !html->cs || !html->box || html->cs->display == D_NONE) return;
  St st;
  st.clip = {0, 0, width, height};
  st.m = Affine::translate(0, -std::round(pageShiftY));
  st.dx = 0;
  st.dy = st.m.f;
  st.alpha = 1;
  st.xf = -1;
  st.hasXf = false;
  buildSC(html, st, true);
}

const PaintItem *Renderer::itemFor(Element *e) const {
  auto it = index_.find(e);
  return it == index_.end() ? nullptr : &items_[it->second];
}

void Renderer::forget(Element *e) { index_.erase(e); }

void Renderer::reset() {
  items_.clear();
  xforms_.clear();
  index_.clear();
  gone_.clear();
}

void Renderer::collectDamage(std::vector<RectI> &out, const std::function<bool(Element *)> &paintDirty) {
  out.insert(out.end(), gone_.begin(), gone_.end());
  gone_.clear();
  for (auto &it : items_) {
    Box &b = *it.el->box;
    bool dirty = paintDirty(it.el);
    if (!b.painted || b.lastInk != it.ink || dirty) {
      if (b.painted && !b.lastInk.empty()) out.push_back(b.lastInk);
      if (!it.ink.empty()) out.push_back(it.ink);
      b.lastInk = it.ink;
      b.painted = true;
    }
  }
}

RectF Renderer::screenRect(Element *e) const {
  if (!e->box) return {};
  const PaintItem *it = itemFor(e);
  RectF r = e->box->borderRect();
  if (!it) return r;
  if (it->xf >= 0) return xforms_[it->xf].mapRect(r);
  return r.translated(it->dx, it->dy);
}

Element *Renderer::hitTest(float x, float y, bool forPointer) const {
  for (size_t i = items_.size(); i-- > 0;) {
    const PaintItem &it = items_[i];
    Element *e = it.el;
    const ComputedStyle &s = *e->cs;
    if (forPointer && s.pointerEvents == PE_NONE_) continue;
    if (s.visibility != VIS_VISIBLE) continue;
    if (!it.clip.contains((int)x, (int)y)) continue;
    float dx = x, dy = y;
    if (it.xf >= 0) {
      Affine inv;
      if (!xforms_[it.xf].invert(inv)) continue;
      inv.apply(x, y, dx, dy);
    } else {
      dx -= it.dx;
      dy -= it.dy;
    }
    Box &b = *e->box;
    if (isInlineBox(e)) {
      Box &o = *b.origin->box;
      for (auto &r : b.inlineRects)
        if (r.translated(o.absX, o.absY).contains(dx, dy)) return e;
      continue;
    }
    if (b.borderRect().contains(dx, dy)) return e;
  }
  return nullptr;
}

// ============================================================ painting

void Renderer::paint(const Band &band, const RectI &area) {
  RT_PROF(PF_PAINT);
  RectI a = area.intersect(band.rect());
  if (a.empty()) return;
  // canvas background: html background, else body background, else white
  Color bg = kWhite;
  if (!items_.empty()) {
    Element *html = items_[0].el;
    if (html->cs->bgColor.a > 0) bg = html->cs->bgColor;
    else if (html->doc && html->doc->body && html->doc->body->cs && html->doc->body->cs->bgColor.a > 0)
      bg = html->doc->body->cs->bgColor;
    if (bg.a < 255) bg = lerpColor(kWhite, bg.withAlpha(255), bg.a / 255.f);
  }
  RasterTarget t{&band, a};
  {
    RT_PROF(PF_BAND_BG);
    fillRectSolid(t, a, bg);
  }
  for (auto &it : items_) {
    if (!it.ink.intersects(a)) continue;
    paintItem(it, band, a);
  }
}

static Color withAlpha(Color c, uint8_t alpha) {
  if (alpha == 255) return c;
  return c.scaleAlpha(alpha);
}

// Fill a (document-space) rounded rect, honouring an optional transform.
static void fillShape(const PaintCtx &pc, const RectF &doc, const float radii[4], Paint p) {
  RasterTarget t = pc.target();
  if (!pc.xf) {
    fillRRect(t, pc.map(doc), radii, p);
    return;
  }
  Path path;
  path.roundRect(doc.x, doc.y, doc.w, doc.h, radii);
  path.transform(*pc.xf);
  fillPath(t, path, FR_NONZERO, p);
}

static void setupGradient(GradientPaint &g, const Gradient &spec, const RectF &box, const ComputedStyle &cs, uint8_t alpha) {
  g.kind = spec.kind == Gradient::LINEAR ? 0 : spec.kind == Gradient::RADIAL ? 1 : 2;
  g.repeating = spec.repeating;
  float cx = box.x + box.w / 2, cy = box.y + box.h / 2;
  float len = 1;
  if (g.kind == 0) {
    float dxv, dyv;
    if (spec.toCorner) {
      float vx = spec.cornerX * box.h, vy = spec.cornerY * box.w;
      float l = std::sqrt(vx * vx + vy * vy);
      dxv = vx / l;
      dyv = vy / l;
    } else {
      float a = spec.angleDeg * 0.0174532925f;
      dxv = std::sin(a);
      dyv = -std::cos(a);
    }
    len = std::fabs(box.w * dxv) + std::fabs(box.h * dyv);
    g.ax = cx - dxv * len / 2;
    g.ay = cy - dyv * len / 2;
    g.bx = cx + dxv * len / 2;
    g.by = cy + dyv * len / 2;
  } else {
    ResolveCtx rc;
    rc.em = cs.fontSize;
    g.cx = box.x + computeLen(spec.cx, rc).resolve(box.w);
    g.cy = box.y + computeLen(spec.cy, rc).resolve(box.h);
    float fx = std::max(g.cx - box.x, box.right() - g.cx), fy = std::max(g.cy - box.y, box.bottom() - g.cy);
    if (g.kind == 1) {
      if (spec.circle) g.rx = g.ry = std::sqrt(fx * fx + fy * fy);
      else {
        g.rx = fx * 1.41421356f;
        g.ry = fy * 1.41421356f;
      }
      g.rx = std::max(g.rx, 0.5f);
      g.ry = std::max(g.ry, 0.5f);
      len = g.rx;
    } else {
      g.startAngle = spec.angleDeg * 0.0174532925f;
      len = 1;
    }
  }
  // stops: resolve positions
  size_t n = spec.stops.size();
  std::vector<float> pos(n, -1);
  for (size_t i = 0; i < n; i++) {
    const GradStop &s = spec.stops[i];
    if (s.pos >= 0 || (s.isPx)) pos[i] = s.isPx ? s.pos / std::max(len, 1e-3f) : s.pos;
  }
  if (pos[0] < 0) pos[0] = 0;
  if (pos[n - 1] < 0) pos[n - 1] = 1;
  for (size_t i = 1; i < n; i++)
    if (pos[i] >= 0 && pos[i] < pos[i - 1]) pos[i] = pos[i - 1];
  for (size_t i = 1; i < n; i++) {
    if (pos[i] >= 0) continue;
    size_t j = i;
    while (j < n && pos[j] < 0) j++;
    float a = pos[i - 1], b = pos[j];
    for (size_t k = i; k < j; k++) pos[k] = a + (b - a) * (k - i + 1) / (j - i + 1);
    i = j;
  }
  g.stops.clear();
  float first = pos[0], last = pos[n - 1];
  bool rep = spec.repeating && last > first;
  for (size_t i = 0; i < n; i++) {
    Color c = cs.resolve(spec.stops[i].color);
    c = withAlpha(c, alpha);
    float t = rep ? (pos[i] - first) / (last - first) : pos[i];
    g.stops.push_back({t, c});
  }
  if (rep) {
    // map t over the repeat period: adjust endpoints so tAt() spans one period
    if (g.kind == 0) {
      float ax = g.ax + (g.bx - g.ax) * first, ay = g.ay + (g.by - g.ay) * first;
      float bx = g.ax + (g.bx - g.ax) * last, by = g.ay + (g.by - g.ay) * last;
      g.ax = ax; g.ay = ay; g.bx = bx; g.by = by;
    } else if (g.kind == 1) {
      g.rx *= last;
      g.ry *= last;
    }
  } else {
    // clamp: values beyond [0,1] use end colours (the LUT covers 0..1)
    for (auto &s : g.stops) s.t = clampf(s.t, 0, 1);
  }
  g.build();
}

void Renderer::paintBackground(Element *e, const PaintCtx &pc, const RectF &border, const float radii[4]) {
  const ComputedStyle &s = *e->cs;
  Box &b = *e->box;
  RectF area = border;
  float rad[4];
  memcpy(rad, radii, sizeof rad);
  if (s.bgClip == 1 || s.bgClip == 2) {
    float l = b.b[3], t = b.b[0], r = b.b[1], bt = b.b[2];
    if (s.bgClip == 2) {
      l += b.p[3];
      t += b.p[0];
      r += b.p[1];
      bt += b.p[2];
    }
    area = border.inset(l, t, r, bt);
    insetRadii(radii, l, t, r, bt, rad);
  }
  if (s.bgColor.a > 0) {
    RT_PROF(PF_BGFILL);
    Paint p;
    p.color = withAlpha(s.bgColor, pc.alpha);
    fillShape(pc, area, rad, p);
  }
  if (!s.bgImages) return;
  RectF posArea = border.inset(b.b[3], b.b[0], b.b[1], b.b[2]);  // padding box
  for (size_t li = s.bgImages->size(); li-- > 0;) {
    const BgLayer &L = (*s.bgImages)[li];
    if (L.grad) {
      GradientPaint g;
      RectF gbox = posArea;
      if (s.bgSizeKind == BGS_LEN) {
        gbox.w = s.bgSizeX.isAuto() ? posArea.w : s.bgSizeX.resolve(posArea.w);
        gbox.h = s.bgSizeY.isAuto() ? posArea.h : s.bgSizeY.resolve(posArea.h);
      }
      RectF scr = pc.xf ? gbox : pc.map(gbox);
      setupGradient(g, *L.grad, scr, s, pc.alpha);
      Paint p;
      p.kind = Paint::GRADIENT;
      p.grad = &g;
      if (pc.xf) {
        // transform the gradient geometry into screen space
        pc.xf->apply(g.ax, g.ay, g.ax, g.ay);
        pc.xf->apply(g.bx, g.by, g.bx, g.by);
        pc.xf->apply(g.cx, g.cy, g.cx, g.cy);
      }
      fillShape(pc, area, rad, p);
    } else if (!L.url.empty() && host) {
      std::shared_ptr<Image> img = host->bgImage(L.url);
      if (!img || !img->w || !img->h) continue;
      float iw = (float)img->w, ih = (float)img->h;
      float tw = iw, th = ih;
      if (s.bgSizeKind == BGS_COVER || s.bgSizeKind == BGS_CONTAIN) {
        float sc = s.bgSizeKind == BGS_COVER ? std::max(posArea.w / iw, posArea.h / ih) : std::min(posArea.w / iw, posArea.h / ih);
        tw = iw * sc;
        th = ih * sc;
      } else if (s.bgSizeKind == BGS_LEN) {
        bool ax = s.bgSizeX.isAuto(), ay = s.bgSizeY.isAuto();
        if (!ax) tw = s.bgSizeX.resolve(posArea.w);
        if (!ay) th = s.bgSizeY.resolve(posArea.h);
        if (ax && !ay) tw = th * iw / ih;
        if (ay && !ax) th = tw * ih / iw;
      }
      float px = posArea.x + s.bgPosX.resolve(posArea.w - tw);
      float py = posArea.y + s.bgPosY.resolve(posArea.h - th);
      Paint p;
      p.kind = Paint::IMAGE;
      p.img = img->view();
      p.alpha = pc.alpha;
      p.imgRepeatX = s.bgRepeat == BR_REPEAT || s.bgRepeat == BR_REPEAT_X;
      p.imgRepeatY = s.bgRepeat == BR_REPEAT || s.bgRepeat == BR_REPEAT_Y;
      p.smooth = !s.pixelated;
      Affine docToImg = Affine::scale(iw / tw, ih / th) * Affine::translate(-px, -py);
      Affine screenToDoc;
      if (pc.xf) pc.xf->invert(screenToDoc);
      else screenToDoc = Affine::translate(-pc.dx, -pc.dy);
      p.imgInv = docToImg * screenToDoc;
      RectF fillArea = area;
      if (!p.imgRepeatX) {
        float x0 = std::max(area.x, px), x1 = std::min(area.right(), px + tw);
        fillArea.x = x0;
        fillArea.w = std::max(0.f, x1 - x0);
      }
      if (!p.imgRepeatY) {
        float y0 = std::max(area.y, py), y1 = std::min(area.bottom(), py + th);
        fillArea.y = y0;
        fillArea.h = std::max(0.f, y1 - y0);
      }
      if (fillArea.empty()) continue;
      if (img->mask) {
        p.kind = Paint::SOLID;
        p.color = withAlpha(s.color, pc.alpha);
      }
      fillShape(pc, fillArea, rad, p);
    }
  }
}

namespace {
// Paint that picks one of four border colours by which side a pixel belongs to.
struct SideShader {};
}  // namespace

void Renderer::paintBorder(Element *e, const PaintCtx &pc, const RectF &border, const float radii[4]) {
  const ComputedStyle &s = *e->cs;
  Box &b = *e->box;
  float bw[4] = {b.b[0], b.b[1], b.b[2], b.b[3]};
  if (bw[0] <= 0 && bw[1] <= 0 && bw[2] <= 0 && bw[3] <= 0) return;
  Color col[4];
  for (int i = 0; i < 4; i++) col[i] = withAlpha(s.resolve(s.borderColor[i]), pc.alpha);
  // inset/outset/groove/ridge: shade sides
  for (int i = 0; i < 4; i++) {
    BorderStyle st = s.borderStyle[i];
    bool dark = (st == BS_INSET && (i == 0 || i == 3)) || (st == BS_OUTSET && (i == 1 || i == 2)) ||
                (st == BS_GROOVE && (i == 0 || i == 3)) || (st == BS_RIDGE && (i == 1 || i == 2));
    if (dark) col[i] = lerpColor(col[i], kBlack.withAlpha(col[i].a), 0.35f);
  }
  RectF inner = border.inset(bw[3], bw[0], bw[1], bw[2]);
  float ri[4];
  insetRadii(radii, bw[3], bw[0], bw[1], bw[2], ri);
  bool uniform = true;
  for (int i = 1; i < 4; i++)
    if ((bw[i] > 0 && bw[0] > 0 && col[i] != col[0]) || (bw[i] > 0) != (bw[0] > 0)) uniform = false;
  bool dashed = false;
  for (int i = 0; i < 4; i++)
    if (bw[i] > 0 && (s.borderStyle[i] == BS_DASHED || s.borderStyle[i] == BS_DOTTED)) dashed = true;
  RasterTarget t = pc.target();
  if (dashed && !pc.xf) {
    // stroke each side's centre line with a dash pattern
    for (int i = 0; i < 4; i++) {
      if (bw[i] <= 0) continue;
      StrokeStyle st;
      st.width = bw[i];
      bool dotted = s.borderStyle[i] == BS_DOTTED;
      if (dotted) {
        st.cap = CAP_ROUND;
        st.dash = {0.001f, bw[i] * 2};
      } else {
        st.dash = {bw[i] * 3, bw[i] * 3};
      }
      RectF r = pc.map(border);
      Path line;
      float h = bw[i] / 2;
      if (i == 0) { line.moveTo(r.x, r.y + h); line.lineTo(r.right(), r.y + h); }
      if (i == 1) { line.moveTo(r.right() - h, r.y); line.lineTo(r.right() - h, r.bottom()); }
      if (i == 2) { line.moveTo(r.x, r.bottom() - h); line.lineTo(r.right(), r.bottom() - h); }
      if (i == 3) { line.moveTo(r.x + h, r.y); line.lineTo(r.x + h, r.bottom()); }
      Path out;
      strokePath(line, st, out);
      Paint p;
      p.color = col[i];
      fillPath(t, out, FR_NONZERO, p);
    }
    return;
  }
  if (uniform) {
    Paint p;
    for (int i = 0; i < 4; i++)
      if (bw[i] > 0) {
        p.color = col[i];
        break;
      }
    if (!pc.xf) {
      fillRRectRing(t, pc.map(border), radii, pc.map(inner), ri, p);
    } else {
      Path path;
      path.roundRect(border.x, border.y, border.w, border.h, radii);
      // inner contour reversed (evenodd handles the hole)
      Path in;
      in.roundRect(inner.x, inner.y, inner.w, inner.h, ri);
      for (size_t k = 0; k < in.pts.size(); k++) {}
      size_t base = path.pts.size();
      path.pts.insert(path.pts.end(), in.pts.begin(), in.pts.end());
      for (auto end : in.contourEnds) path.contourEnds.push_back((uint32_t)(end + base));
      path.transform(*pc.xf);
      fillPath(t, path, FR_EVENODD, p);
    }
    return;
  }
  // per-side colours (e.g. spinners: border-top-color differs): fill the rounded ring with a
  // paint that picks the side colour per pixel
  Paint p;
  p.kind = Paint::SIDES;
  p.boxW = border.w;
  p.boxH = border.h;
  for (int i = 0; i < 4; i++) {
    p.bw[i] = bw[i];
    p.sideColor[i] = col[i];
  }
  Affine boxToScreen = (pc.xf ? *pc.xf : Affine::translate(pc.dx, pc.dy)) * Affine::translate(border.x, border.y);
  boxToScreen.invert(p.sidesInv);
  if (!pc.xf) {
    fillRRectRing(t, pc.map(border), radii, pc.map(inner), ri, p);
  } else {
    Path path;
    path.roundRect(border.x, border.y, border.w, border.h, radii);
    Path in;
    in.roundRect(inner.x, inner.y, inner.w, inner.h, ri);
    size_t base = path.pts.size();
    path.pts.insert(path.pts.end(), in.pts.begin(), in.pts.end());
    for (auto end : in.contourEnds) path.contourEnds.push_back((uint32_t)(end + base));
    path.transform(*pc.xf);
    fillPath(t, path, FR_EVENODD, p);
  }
}

void Renderer::paintElementBox(Element *e, const PaintCtx &pc) {
  RT_PROF(PF_ITEM_BOX);
  const ComputedStyle &s = *e->cs;
  Box &b = *e->box;
  RectF border = b.borderRect();
  float radii[4];
  borderRadii(e, border, radii);
  // outer box shadows; under a transform that only moves and scales (a press effect, a zoom)
  // the shadow scales with the box, under a rotation or skew it is left out
  const bool scaledXf = pc.xf && std::fabs(pc.xf->b) < 1e-4f && std::fabs(pc.xf->c) < 1e-4f && pc.xf->a > 0 && pc.xf->d > 0;
  if (s.boxShadow && (!pc.xf || scaledXf)) {
    RT_PROF(PF_SHADOW);
    ResolveCtx rc;
    rc.em = s.fontSize;
    float sx = pc.xf ? pc.xf->a : 1, sy = pc.xf ? pc.xf->d : 1, sm = (sx + sy) / 2;
    float rr[4];
    for (int k = 0; k < 4; k++) rr[k] = radii[k] * sm;
    for (size_t i = s.boxShadow->size(); i-- > 0;) {
      const Shadow &sh = (*s.boxShadow)[i];
      if (sh.inset) continue;
      float ox = computeLen(sh.x, rc).px * sx, oy = computeLen(sh.y, rc).px * sy;
      float bl = computeLen(sh.blur, rc).px * sm, sp = computeLen(sh.spread, rc).px * sm;
      RectF sr = pc.xf ? pc.xf->mapRect(border) : pc.map(border);
      RectF shapeR{sr.x + ox - sp, sr.y + oy - sp, sr.w + 2 * sp, sr.h + 2 * sp};
      float sradii[4];
      for (int k = 0; k < 4; k++) sradii[k] = rr[k] > 0 ? std::max(0.f, rr[k] + sp) : 0;
      Color c = withAlpha(s.resolve(sh.color), pc.alpha);
      RectF hole = sr;
      fillShadow(pc.target(), shapeR, sradii, bl, c, &hole, rr);
    }
  }
  {
    RT_PROF(PF_BACKGROUND);
    // <body>'s background moves to the page canvas when <html> has none (CSS root
    // background propagation): it is already the band background
    bool propagated = e->tag == TAG_body && e->parent && e->parent->cs && e->parent->cs->bgColor.a == 0 &&
                      !e->parent->cs->bgImages && !s.bgImages && s.bgColor.a == 255;
    if (!propagated) paintBackground(e, pc, border, radii);
  }
  {
    RT_PROF(PF_BORDER);
    paintBorder(e, pc, border, radii);
  }
  // inset shadows (approximation: blurred ring inside the padding box)
  if (s.boxShadow && !pc.xf) {
    ResolveCtx rc;
    rc.em = s.fontSize;
    for (auto &sh : *s.boxShadow) {
      if (!sh.inset) continue;
      float ox = computeLen(sh.x, rc).px, oy = computeLen(sh.y, rc).px;
      float bl = computeLen(sh.blur, rc).px, sp = computeLen(sh.spread, rc).px;
      RectF pr = pc.map(border.inset(b.b[3], b.b[0], b.b[1], b.b[2]));
      float pr4[4];
      insetRadii(radii, b.b[3], b.b[0], b.b[1], b.b[2], pr4);
      // draw the shadow of the "outside" by filling a ring between a big rect and the shifted inner shape
      RectF innerShape{pr.x + ox + sp, pr.y + oy + sp, pr.w - 2 * sp, pr.h - 2 * sp};
      Color c = withAlpha(s.resolve(sh.color), pc.alpha);
      // clip to the padding box
      PaintCtx sub = pc;
      sub.clip = pc.clip.intersect(pr.outer());
      RasterTarget t = sub.target();
      float sigma = std::max(0.5f, bl / 2);
      RectI bb = pr.outer().intersect(sub.clip);
      for (int y = bb.y; y < bb.bottom(); y++) {
        for (int x = bb.x; x < bb.right(); x++) {
          float cx = x + 0.5f, cy = y + 0.5f;
          float dx = std::max(innerShape.x - cx, cx - innerShape.right());
          float dy = std::max(innerShape.y - cy, cy - innerShape.bottom());
          float d = std::max(dx, dy);
          float a = clampf(0.5f + d / (sigma * 2.5f), 0, 1);
          uint32_t a255 = (uint32_t)(a * c.a);
          if (a255) {
            uint16_t *px = t.band->at(x, y);
            *px = blend565(*px, c.to565(), (a255 + 4) >> 3);
          }
        }
      }
    }
  }
}

void Renderer::paintInlineDecorations(Element *container, InlineLayout &il, const PaintCtx &pc) {
  Box &cb = *container->box;
  for (auto &it : il.items) {
    if (it.kind != InlineItem::OPEN) continue;
    Element *el = it.el;
    const ComputedStyle &s = *el->cs;
    if (s.visibility != VIS_VISIBLE) continue;
    bool deco = s.bgColor.a > 0 || s.bgImages || el->box->b[0] > 0 || el->box->b[1] > 0 || el->box->b[2] > 0 ||
                el->box->b[3] > 0 || s.boxShadow;
    if (!deco) continue;
    for (auto &r : el->box->inlineRects) {
      RectF br = r.translated(cb.absX, cb.absY);
      float radii[4];
      borderRadii(el, br, radii);
      // the fragment is passed as the border box (the element's box is shared by all
      // fragments and may be read by the other painting core)
      paintBackground(el, pc, br, radii);
      paintBorder(el, pc, br, radii);
    }
  }
}

RT_HOT static void drawGlyph(const PaintCtx &pc, const FontRef &f, const FntGlyph *g, float penX, float baseline, Color c) {
  if (!g || !g->w || !g->h) return;
  const uint8_t *bm = f.face->bitmap(g);
  if (!bm) return;  // not read (memory short)
  RasterTarget t = pc.target();
  if (!pc.xf && f.scale == 1) {
    int y = ifloor(baseline + pc.dy + 0.5f) - g->by;
    if (y >= t.clip.bottom() || y + g->h <= t.clip.y) return;
    int x = ifloor(penX + pc.dx + 0.5f) + g->bx;
    blitMask(t, bm, g->w, g->h, x, y, c);
    return;
  }
  // scaled and/or transformed: glyph pixel (u,v) -> doc (penX + (bx+u)*s, baseline + (v-by)*s)
  float sc = f.scale;
  Affine glyphToDoc{sc, 0, 0, sc, penX + g->bx * sc, baseline - g->by * sc};
  Affine toScreen = pc.xf ? (*pc.xf * glyphToDoc) : (Affine::translate(pc.dx, pc.dy) * glyphToDoc);
  if (!pc.xf) {
    // keep scaled glyphs on whole pixels vertically for crispness
    toScreen.e = std::round(toScreen.e * 2) / 2;
    toScreen.f = std::round(toScreen.f);
  }
  Affine inv;
  if (!toScreen.invert(inv)) return;
  RectI bb = toScreen.mapRect({0, 0, (float)g->w, (float)g->h}).outer();
  bb = {bb.x - 1, bb.y - 1, bb.w + 2, bb.h + 2};
  blitMaskTransformed(t, bm, g->w, g->h, inv, bb, c);
}

RT_HOT void Renderer::paintText(Element *container, InlineLayout &il, const PaintCtx &pc) {
  RT_PROF(PF_TEXT);
  Box &cb = *container->box;
  float ox = cb.absX, oy = cb.absY;
  for (auto &ln : il.lines) {
    // vertical cull
    float top = oy + ln.y + pc.dy, bot = top + ln.h;
    if (!pc.xf && (bot < pc.clip.y - 40 || top > pc.clip.bottom() + 40)) continue;
    for (uint32_t fi = ln.f0; fi < ln.f1; fi++) {
      const LineFrag &f = il.frags[fi];
      if (f.kind == LineFrag::ATOMIC) continue;
      if (f.kind == LineFrag::ELLIPSIS) {
        const ComputedStyle &s = *container->cs;
        FontRef font = layout->fontFor(s);
        if (!font.face) continue;
        std::vector<ShapedGlyph> gl;
        shapeText(font, "\xE2\x80\xA6", 3, ShapeOpts(), gl);
        Color c = withAlpha(s.color, pc.alpha);
        for (auto &g : gl) drawGlyph(pc, font, g.g, ox + f.x + g.x, oy + f.baseline, c);
        continue;
      }
      InlineItem &it = il.items[f.item];
      const ComputedStyle &s = *it.el->cs;
      if (s.visibility != VIS_VISIBLE) continue;
      if (!it.font.face) continue;
      Color c = withAlpha(s.color, pc.alpha);
      if (c.a == 0) continue;
      float base = oy + f.baseline;
      float fx = ox + f.x - f.shift;
      // horizontal cull
      if (!pc.xf && (fx + pc.dx + it.glyphs[f.g1 > f.g0 ? f.g1 - 1 : f.g0].x + 64 < pc.clip.x ||
                     ox + f.x + pc.dx > pc.clip.right() + 8))
        continue;
      // vertical cull by the font's reach around the baseline (marks above/below included)
      if (!pc.xf && !s.textShadow) {
        float em = it.font.size(), sb = base + pc.dy;
        if (sb + em * 0.7f + 2 < pc.clip.y || sb - em * 1.5f - 2 > pc.clip.bottom()) continue;
      }
      if (s.textShadow) {
        ResolveCtx rc;
        rc.em = s.fontSize;
        for (size_t k = s.textShadow->size(); k-- > 0;) {
          const Shadow &sh = (*s.textShadow)[k];
          float sx = computeLen(sh.x, rc).px, sy = computeLen(sh.y, rc).px;
          float blur = computeLen(sh.blur, rc).px;
          Color sc = withAlpha(s.resolve(sh.color), pc.alpha);
          if (blur > 1) sc = sc.scaleAlpha(160);
          for (uint32_t gi = f.g0; gi < f.g1; gi++) {
            const ShapedGlyph &g = it.glyphs[gi];
            drawGlyph(pc, it.font, g.g, fx + g.x + sx, base - g.dy + sy, sc);
          }
        }
      }
      for (uint32_t gi = f.g0; gi < f.g1; gi++) {
        const ShapedGlyph &g = it.glyphs[gi];
        if (!g.g) continue;
        drawGlyph(pc, it.font, g.g, fx + g.x, base - g.dy, c);
      }
      // decorations (inherited from the owner chain)
      uint8_t deco = 0;
      Color dc = c;
      for (Element *p = it.el; p; p = p->parent) {
        if (!p->cs) break;
        if (p->cs->textDecoLine) {
          deco |= p->cs->textDecoLine;
          dc = withAlpha(p->cs->resolve(p->cs->textDecoColor), pc.alpha);
          if (p->cs->textDecoColor.current) dc = c;
        }
        if (p == container) break;
      }
      if (deco && f.w > 0) {
        float th = std::max(1.f, std::round(it.font.size() / 14));
        RasterTarget t = pc.target();
        float decoW = f.w;
        auto line = [&](float y) {
          RectF r{ox + f.x, y, decoW, th};
          if (!pc.xf) fillRectSolid(t, pc.map(r).outer(), dc);
          else {
            Paint p;
            p.color = dc;
            float rz[4] = {0, 0, 0, 0};
            fillShape(pc, r, rz, p);
          }
        };
        // trailing spaces are not decorated (the fragment is shared: paint may run on two cores)
        float trail = 0;
        for (int gi = (int)f.g1 - 1; gi >= (int)f.g0 && (it.glyphs[gi].flags & SG_SPACE); gi--) trail += it.glyphs[gi].adv;
        decoW = f.w - trail;
        if (deco & TD_UNDERLINE) line(std::round(base + std::max(1.f, it.font.size() * 0.1f)));
        if (deco & TD_LINE_THROUGH) line(std::round(base - it.font.xHeight() * 0.5f - th / 2));
        if (deco & TD_OVERLINE) line(std::round(base - it.font.ascent()));
      }
    }
  }
}

static std::string markerText(Element *e) {
  const ComputedStyle &s = *e->cs;
  int ord = 1;
  Element *list = e->parentElement();
  int start = list && list->tagId() == TAG_ol ? (int)list->attrNum(A.lang /*unused*/, 1) : 1;
  start = 1;
  static const char kStart[] = "start";
  Atom aStart = findAtom(kStart, 5);  // never intern while painting (two cores)
  if (list && aStart && list->hasAttr(aStart)) start = (int)list->attrNum(aStart, 1);
  ord = start;
  for (Element *p = e->prevElementSibling(); p; p = p->prevElementSibling())
    if (p->cs && p->cs->display == D_LIST_ITEM) ord++;
  if (e->hasAttr(A.value)) ord = (int)e->attrNum(A.value, (float)ord);
  char buf[32];
  switch (s.listStyleType) {
    case LS_DISC: return "\xE2\x80\xA2";
    case LS_CIRCLE: return "\xE2\x97\xA6";
    case LS_SQUARE: return "\xE2\x96\xAA";
    case LS_DECIMAL: snprintf(buf, sizeof buf, "%d.", ord); return buf;
    case LS_LOWER_ALPHA: snprintf(buf, sizeof buf, "%c.", 'a' + (ord - 1) % 26); return buf;
    case LS_UPPER_ALPHA: snprintf(buf, sizeof buf, "%c.", 'A' + (ord - 1) % 26); return buf;
    case LS_THAI: {
      std::string r;
      std::string digits = std::to_string(ord);
      for (char c : digits) utf8Append(r, 0x0E50 + (c - '0'));
      return r + ".";
    }
    case LS_LOWER_ROMAN: case LS_UPPER_ROMAN: {
      static const char *rn[] = {"", "i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix", "x", "xi", "xii"};
      std::string r = ord < 13 ? rn[ord] : std::to_string(ord);
      if (s.listStyleType == LS_UPPER_ROMAN)
        for (auto &c : r) c = (char)toupper(c);
      return r + ".";
    }
    default: return "";
  }
}

void Renderer::paintMarker(Element *e, const PaintCtx &pc) {
  const ComputedStyle &s = *e->cs;
  if (s.listStyleType == LS_NONE) return;
  std::string text = markerText(e);
  if (text.empty()) return;
  FontRef f = layout->fontFor(s);
  if (!f.face) return;
  std::vector<ShapedGlyph> gl;
  float w = shapeText(f, text.data(), text.size(), ShapeOpts(), gl);
  Box &b = *e->box;
  float baseline = b.baseline >= 0 ? b.absY + b.baseline : b.absY + std::round(f.ascent());
  float x = b.absX + b.b[3] + b.p[3] - w - (s.listStyleType <= LS_SQUARE ? f.size() * 0.5f : f.size() * 0.3f);
  Color c = withAlpha(s.color, pc.alpha);
  for (auto &g : gl) drawGlyph(pc, f, g.g, x + g.x, baseline, c);
}

void Renderer::paintScrollbar(Element *e, const PaintCtx &pc) {
  if (!e->x || e->x->scrollbarAlpha <= 0.01f) return;
  Box &b = *e->box;
  RectF pr = e->tag == TAG_html ? RectF{0, 0, (float)width, (float)height} : pc.map(b.paddingRect());
  float viewH = pr.h, contentH = b.scrollH;
  if (e->tag == TAG_html) contentH = std::max(contentH, b.h + b.m[0] + b.m[2]);
  if (contentH > viewH + 1) {
    float th = std::max(24.f, viewH * viewH / contentH);
    float maxScroll = contentH - viewH;
    float ty = pr.y + (viewH - th) * clampf(e->x->scrollY / maxScroll, 0, 1);
    RectF bar{pr.right() - 6, ty + 2, 4, th - 4};
    float r[4] = {2, 2, 2, 2};
    Paint p;
    p.color = Color{0, 0, 0, (uint8_t)(110 * e->x->scrollbarAlpha)};
    PaintCtx sub = pc;
    sub.clip = pc.band->rect().intersect({0, 0, width, height});
    fillRRect(sub.target(), bar, r, p);
  }
  float contentW = b.scrollW;
  float viewW = pr.w;
  if (contentW > viewW + 1 && e->tag != TAG_html) {
    float tw = std::max(24.f, viewW * viewW / contentW);
    float maxScroll = contentW - viewW;
    float tx = pr.x + (viewW - tw) * clampf(e->x->scrollX / maxScroll, 0, 1);
    RectF bar{tx + 2, pr.bottom() - 6, tw - 4, 4};
    float r[4] = {2, 2, 2, 2};
    Paint p;
    p.color = Color{0, 0, 0, (uint8_t)(110 * e->x->scrollbarAlpha)};
    fillRRect(pc.target(), bar, r, p);
  }
}

void Renderer::paintItem(const PaintItem &it, const Band &band, const RectI &area) {
  Element *e = it.el;
  const ComputedStyle &s = *e->cs;
  PaintCtx pc;
  pc.band = &band;
  pc.clip = it.clip.intersect(area);
  if (pc.clip.empty()) return;
  pc.dx = it.dx;
  pc.dy = it.dy;
  pc.alpha = it.alpha;
  pc.xf = it.xf >= 0 ? &xforms_[it.xf] : nullptr;
  pc.fonts = fonts;
  pc.layout = layout;
  if (pc.alpha == 0) return;
  bool visible = s.visibility == VIS_VISIBLE;
  if (isInlineBox(e)) return;  // painted by its block container
  if (visible) {
    if (e->tag != TAG_html) paintElementBox(e, pc);
    else {
      // root: background is the canvas; paint borders only
      RectF border = e->box->borderRect();
      float radii[4];
      borderRadii(e, border, radii);
      paintBorder(e, pc, border, radii);
    }
  }
  // content is clipped to the padding box for overflow containers
  PaintCtx content = pc;
  if (s.clips() && e->tag != TAG_html) {
    RectF pr = e->box->paddingRect();
    RectF scr = pc.xf ? pc.xf->mapRect(pr) : pc.map(pr);
    content.clip = content.clip.intersect(scr.outer());
    if (e->x && (e->x->scrollX || e->x->scrollY)) {
      content.dx -= e->x->scrollX;
      content.dy -= e->x->scrollY;
    }
  } else if (e->tag == TAG_html && e->x) {
    content.dx -= e->x->scrollX;
    content.dy -= e->x->scrollY;
  }
  bool handled = false;
  if (host && visible) {
    RT_PROF(PF_REPLACED);
    handled = host->paintReplaced(e, content);
  }
  if (!handled) {
    for (auto &il : e->box->inlines) {
      paintInlineDecorations(e, *il, content);
      paintText(e, *il, content);
    }
  }
  if (visible && s.display == D_LIST_ITEM && !s.listStyleInside) paintMarker(e, pc);
  // outline
  if (visible && s.outlineStyle != BS_NONE && s.outlineWidth > 0) {
    RectF border = e->box->borderRect();
    float o = s.outlineOffset;
    RectF in = {border.x - o, border.y - o, border.w + 2 * o, border.h + 2 * o};
    RectF out = {in.x - s.outlineWidth, in.y - s.outlineWidth, in.w + 2 * s.outlineWidth, in.h + 2 * s.outlineWidth};
    float radii[4], ro[4], rin[4];
    borderRadii(e, border, radii);
    for (int i = 0; i < 4; i++) {
      rin[i] = radii[i] > 0 ? std::max(0.f, radii[i] + o) : 0;
      ro[i] = radii[i] > 0 ? rin[i] + s.outlineWidth : 0;
    }
    Paint p;
    p.color = withAlpha(s.resolve(s.outlineColor), pc.alpha);
    if (!pc.xf) fillRRectRing(pc.target(), pc.map(out), ro, pc.map(in), rin, p);
  }
  if (s.clips() || e->tag == TAG_html) paintScrollbar(e, pc);
}

}  // namespace rt
