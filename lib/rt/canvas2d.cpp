#include "canvas2d.h"

#include "engine.h"
#include "extra.h"
#include "js.h"
#include "layer.h"
#include "svg.h"

namespace rt {

// ============================================================ Canvas2D

Canvas2D::Canvas2D(Engine &eng, Element *el) : eng_(eng), el_(el) {
  w_ = std::max(1, (int)el->attrNum(A.width, 300));
  h_ = std::max(1, (int)el->attrNum(A.height, 150));
  setFont(st.font);
}

void Canvas2D::resize(int w, int h) {
  w_ = std::max(1, w);
  h_ = std::max(1, h);
  cmds_.clear();
  flat_.reset();
  pathBytes_ = 0;
  st = State();
  stack.clear();
  path.clear();
  setFont(st.font);
  invalidate();
}

void Canvas2D::invalidate() {
  version_++;
  eng_.invalidateElement(el_);
  eng_.requestFrame();
}

void Canvas2D::setFont(const std::string &css) {
  std::vector<Decl> decls;
  if (!parsePropertyValue("font", css, decls)) return;
  st.font = css;
  ComputedStyle tmp;
  tmp.fontSize = 10;
  ResolveCtx rc;
  rc.em = el_->cs ? el_->cs->fontSize : 16;
  if (el_->cs) tmp.fontFamily = el_->cs->fontFamily;
  for (auto &d : decls) eng_.style().applyDecl(tmp, d, el_->cs, rc);
  st.fontRef = eng_.layout().fontFor(tmp);
}

void Canvas2D::push(Cmd &&c) {
  c.clip = st.clip;
  pathBytes_ += c.path.pts.size() * sizeof(Path::Pt) + c.glyphs.size() * sizeof(Cmd::G) + 64;
  cmds_.push_back(std::move(c));
  // very long command lists (incremental drawing) are flattened into a bitmap
  if (cmds_.size() > 3000 || pathBytes_ > 400 * 1024) flatten();
  invalidate();
}

Color Canvas2D::effectiveBackground() const {
  for (Element *p = el_; p; p = p->parent) {
    if (!p->cs) continue;
    if (p->cs->bgColor.a == 255) return p->cs->bgColor;
  }
  Document *d = el_->doc;
  if (d && d->body && d->body->cs && d->body->cs->bgColor.a) return d->body->cs->bgColor;
  return kWhite;
}

bool Canvas2D::paintFromStyle(Cmd &c, const CanvasPaint &p) {
  c.paint = p;
  c.alpha = st.alpha;
  if (p.kind == CanvasPaint::COLOR && p.color.a == 0) return false;
  return st.alpha > 0;
}

void Canvas2D::addShadow(const Cmd &c) {
  if (st.shadowColor.a == 0 || (st.shadowX == 0 && st.shadowY == 0 && st.shadowBlur == 0)) return;
  Cmd s = c;
  s.paint = CanvasPaint();
  s.paint.color = st.shadowColor;
  if (st.shadowBlur > 2) s.paint.color = st.shadowColor.scaleAlpha(140);
  Affine t = Affine::translate(st.shadowX, st.shadowY);
  s.path.transform(t);
  s.xf = t * s.xf;
  s.dst = s.dst.translated(st.shadowX, st.shadowY);
  if (s.kind == Cmd::IMAGE) {
    s.kind = Cmd::FILL;
    s.path.clear();
    s.path.rect(s.dst.x, s.dst.y, s.dst.w, s.dst.h);
    s.path.transform(s.xf);
  }
  cmds_.push_back(std::move(s));
  version_++;
}

void Canvas2D::fillRect(float x, float y, float w, float h) {
  Cmd c;
  if (!paintFromStyle(c, st.fill)) return;
  c.path.rect(x, y, w, h);
  c.path.transform(st.ctm);
  addShadow(c);
  push(std::move(c));
}

void Canvas2D::strokeRect(float x, float y, float w, float h) {
  Path p;
  p.rect(x, y, w, h);
  p.transform(st.ctm);
  stroke(p);
}

void Canvas2D::clearRect(float x, float y, float w, float h) {
  // a clear covering the whole canvas resets the command list
  RectF r = st.ctm.mapRect({x, y, w, h});
  if (st.ctm.isTranslate() && r.x <= 0 && r.y <= 0 && r.right() >= w_ && r.bottom() >= h_) {
    cmds_.clear();
    flat_.reset();
    pathBytes_ = 0;
    invalidate();
    return;
  }
  Cmd c;
  c.kind = Cmd::BG;
  c.paint.color = effectiveBackground();
  c.path.rect(x, y, w, h);
  c.path.transform(st.ctm);
  push(std::move(c));
}

void Canvas2D::fill(const Path &p, FillRule rule) {
  Cmd c;
  if (!paintFromStyle(c, st.fill) || p.empty()) return;
  c.path = p;
  c.rule = rule;
  addShadow(c);
  push(std::move(c));
}

void Canvas2D::stroke(const Path &p) {
  Cmd c;
  if (!paintFromStyle(c, st.stroke) || p.empty()) return;
  // stroke in user space so the line width scales with the transform
  Affine inv;
  StrokeStyle ss;
  ss.width = st.lineWidth;
  ss.cap = st.cap;
  ss.join = st.join;
  ss.miter = st.miter;
  ss.dash = st.dash;
  ss.dashOffset = st.dashOffset;
  if (st.ctm.isTranslate() || !st.ctm.invert(inv)) {
    strokePath(p, ss, c.path);
  } else {
    // uniform-ish scale: scale the width instead of untransforming (keeps it cheap)
    float sx = std::sqrt(st.ctm.a * st.ctm.a + st.ctm.b * st.ctm.b);
    float sy = std::sqrt(st.ctm.c * st.ctm.c + st.ctm.d * st.ctm.d);
    ss.width *= (sx + sy) / 2;
    for (auto &d : ss.dash) d *= (sx + sy) / 2;
    strokePath(p, ss, c.path);
  }
  c.rule = FR_NONZERO;
  addShadow(c);
  push(std::move(c));
}

void Canvas2D::clip(const Path &p) {
  // axis-aligned bounding rect of the clip path (exact for rectangular clips)
  RectF b = p.bounds();
  st.clip = st.clip.intersect(b);
}

float Canvas2D::measureText(const std::string &text, float &ascent, float &descent) {
  ascent = st.fontRef.ascent();
  descent = st.fontRef.descent();
  return rt::measureText(st.fontRef, text);
}

void Canvas2D::fillText(const std::string &text, float x, float y, float maxWidth, bool strokeIt) {
  if (!st.fontRef.face) return;
  Cmd c;
  if (!paintFromStyle(c, strokeIt ? st.stroke : st.fill)) return;
  std::vector<ShapedGlyph> gl;
  float w = shapeText(st.fontRef, text.data(), text.size(), ShapeOpts(), gl);
  float sx = 1;
  if (maxWidth > 0 && w > maxWidth) {
    sx = maxWidth / w;
    w = maxWidth;
  }
  float x0 = x;
  switch (st.textAlign) {
    case 2: x0 = x - w / 2; break;
    case 3: case 4: x0 = x - w; break;
    default: break;
  }
  float A = st.fontRef.ascent(), D = st.fontRef.descent();
  float y0 = y;
  switch (st.textBaseline) {
    case 1: case 4: y0 = y + A; break;
    case 2: y0 = y + (A - D) / 2; break;
    case 3: case 5: y0 = y - D; break;
    default: break;
  }
  c.kind = Cmd::TEXT;
  c.font = st.fontRef;
  c.xf = st.ctm * Affine::translate(x0, y0) * Affine::scale(sx, 1);
  for (auto &g : gl)
    if (g.g && g.g->w) c.glyphs.push_back({g.g, g.x, -g.dy});
  RectF b{0, -A, w / sx, A + D};
  c.path.rect(b.x, b.y, b.w, b.h);  // bounds only (for culling)
  c.path.transform(c.xf);
  addShadow(c);
  push(std::move(c));
}

void Canvas2D::drawImage(std::shared_ptr<Image> img, float sx, float sy, float sw, float sh, float dx, float dy, float dw,
                         float dh) {
  if (!img || !img->w || !img->h || st.alpha <= 0) return;
  Cmd c;
  c.kind = Cmd::IMAGE;
  c.img = img;
  c.src = {sx, sy, sw, sh};
  c.dst = {dx, dy, dw, dh};
  c.xf = st.ctm;
  c.alpha = st.alpha;
  c.smooth = st.smoothing;
  c.path.rect(dx, dy, dw, dh);
  c.path.transform(st.ctm);
  addShadow(c);
  push(std::move(c));
}

void Canvas2D::putImageData(const uint8_t *rgba, int w, int h, int dx, int dy) {
  auto img = std::make_shared<Image>();
  img->w = w;
  img->h = h;
  img->owned.resize((size_t)w * h * 3);
  uint16_t *px = (uint16_t *)img->owned.data();
  uint8_t *al = img->owned.data() + (size_t)w * h * 2;
  for (int i = 0; i < w * h; i++) {
    px[i] = Color{rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], 255}.to565();
    al[i] = rgba[i * 4 + 3];
  }
  img->px = px;
  img->alpha = al;
  Affine save = st.ctm;
  float a = st.alpha;
  st.ctm = Affine();
  st.alpha = 1;
  drawImage(img, 0, 0, (float)w, (float)h, (float)dx, (float)dy, (float)w, (float)h);
  st.ctm = save;
  st.alpha = a;
}

// Render the canvas (bg + commands) into an RGB565 buffer of canvas pixels.
static void renderToBuffer(Canvas2D &cv, std::vector<uint16_t> &buf, int w, int h, Color bg,
                           const std::function<void(const PaintCtx &)> &paintFn) {
  buf.assign((size_t)w * h, bg.to565());
  Band b{buf.data(), w, 0, 0, w, h};
  PaintCtx pc;
  pc.band = &b;
  pc.clip = {0, 0, w, h};
  pc.dx = pc.dy = 0;
  pc.alpha = 255;
  pc.xf = nullptr;
  (void)cv;
  paintFn(pc);
}

void Canvas2D::flatten() {
  std::vector<uint16_t> buf;
  Color bg = effectiveBackground();
  renderToBuffer(*this, buf, w_, h_, bg, [&](const PaintCtx &pc) { paint(pc, RectF{0, 0, (float)w_, (float)h_}); });
  auto img = std::make_shared<Image>();
  img->w = w_;
  img->h = h_;
  img->owned.resize(buf.size() * 2);
  memcpy(img->owned.data(), buf.data(), buf.size() * 2);
  img->px = (const uint16_t *)img->owned.data();
  flat_ = img;
  cmds_.clear();
  pathBytes_ = 0;
}

std::vector<uint8_t> Canvas2D::getImageData(int x, int y, int w, int h) {
  std::vector<uint16_t> buf;
  Color bg = effectiveBackground();
  renderToBuffer(*this, buf, w_, h_, bg, [&](const PaintCtx &pc) { paint(pc, RectF{0, 0, (float)w_, (float)h_}); });
  std::vector<uint8_t> out((size_t)w * h * 4, 0);
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) {
      int sx = x + i, sy = y + j;
      if (sx < 0 || sy < 0 || sx >= w_ || sy >= h_) continue;
      uint16_t c = buf[(size_t)sy * w_ + sx];
      uint8_t *o = &out[((size_t)j * w + i) * 4];
      o[0] = (uint8_t)(((c >> 11) * 255 + 15) / 31);
      o[1] = (uint8_t)((((c >> 5) & 63) * 255 + 31) / 63);
      o[2] = (uint8_t)(((c & 31) * 255 + 15) / 31);
      o[3] = 255;
    }
  return out;
}

void Canvas2D::paint(const PaintCtx &pc, const RectF &box) {
  float sx = box.w / w_, sy = box.h / h_;
  // 1:1 on a plain background: render once per drawing change, copy per band
  if (std::fabs(sx - 1) < 1e-4f && std::fabs(sy - 1) < 1e-4f &&
      paintLayered(layer_, el_, pc, box, version_, [&](const PaintCtx &lp) { paintCommands(lp, box); }))
    return;
  paintCommands(pc, box);
}

void Canvas2D::paintCommands(const PaintCtx &pc, const RectF &box) {
  float sx = box.w / w_, sy = box.h / h_;
  Affine docToScreen = pc.xf ? *pc.xf : Affine::translate(pc.dx, pc.dy);
  Affine M = docToScreen * Affine::translate(box.x, box.y) * Affine::scale(sx, sy);
  RectI clipAll = pc.clip.intersect(M.mapRect({0, 0, (float)w_, (float)h_}).outer());
  if (clipAll.empty()) return;
  RasterTarget base{pc.band, clipAll};
  if (flat_) {
    ImageView v = flat_->view();
    if (M.isTranslate() || (M.b == 0 && M.c == 0)) {
      RectF dst = M.mapRect({0, 0, (float)w_, (float)h_});
      rt::drawImage(base, v, dst, {0, 0, (float)w_, (float)h_}, pc.alpha, true);
    }
  }
  for (auto &c : cmds_) {
    // clip rect (canvas px) -> screen
    RectI clip = clipAll;
    if (c.clip.w < 1e8f) clip = clip.intersect(M.mapRect(c.clip).outer());
    if (clip.empty()) continue;
    // double-checked: bands may be painted on two cores
    if (!__atomic_load_n(&c.haveScreen, __ATOMIC_ACQUIRE) || memcmp(&c.screenKey, &M, sizeof(Affine)) != 0) {
      PaintLock lock;
      if (!c.haveScreen || memcmp(&c.screenKey, &M, sizeof(Affine)) != 0) {
        __atomic_store_n(&c.haveScreen, false, __ATOMIC_RELEASE);
        c.screen = c.path;
        c.screen.transform(M);
        c.screenGrad.reset();
        if (c.kind == Cmd::FILL && c.paint.kind == CanvasPaint::GRADIENT && c.paint.grad) {
          auto g = std::make_shared<GradientPaint>(*c.paint.grad);
          M.apply(g->ax, g->ay, g->ax, g->ay);
          M.apply(g->bx, g->by, g->bx, g->by);
          float cx = g->cx, cy = g->cy;
          M.apply(cx, cy, g->cx, g->cy);
          float sc = std::sqrt(std::fabs(M.a * M.d - M.b * M.c));
          g->rx *= sc;
          g->ry *= sc;
          g->build();
          c.screenGrad = g;
        }
        c.screenKey = M;
        __atomic_store_n(&c.haveScreen, true, __ATOMIC_RELEASE);
      }
    }
    RectI bounds = c.screen.bounds().outer();
    bounds = {bounds.x - 1, bounds.y - 1, bounds.w + 2, bounds.h + 2};
    if (!bounds.intersects(clip)) continue;
    RasterTarget t{pc.band, clip};
    uint8_t alpha = (uint8_t)clampi((int)std::lround(c.alpha * pc.alpha), 0, 255);
    switch (c.kind) {
      case Cmd::BG:
      case Cmd::FILL: {
        Paint p;
        if (c.paint.kind == CanvasPaint::COLOR || c.kind == Cmd::BG) {
          p.color = c.paint.color.scaleAlpha(c.kind == Cmd::BG ? pc.alpha : alpha);
        } else if (c.paint.kind == CanvasPaint::GRADIENT && c.screenGrad) {
          p.kind = Paint::GRADIENT;
          p.grad = c.screenGrad.get();
          p.alpha = alpha;
        } else if (c.paint.kind == CanvasPaint::PATTERN && c.paint.pattern) {
          p.kind = Paint::IMAGE;
          p.img = c.paint.pattern->view();
          M.invert(p.imgInv);
          p.imgRepeatX = c.paint.repeatX;
          p.imgRepeatY = c.paint.repeatY;
          p.alpha = alpha;
        }
        fillPath(t, c.screen, c.rule, p);
        break;
      }
      case Cmd::TEXT: {
        Color col = c.paint.color.scaleAlpha(alpha);
        Affine G = M * c.xf;
        bool simple = G.b == 0 && G.c == 0 && std::fabs(G.a - 1) < 1e-3f && std::fabs(G.d - 1) < 1e-3f && c.font.scale == 1;
        for (auto &gl : c.glyphs) {
          const FntGlyph *g = gl.g;
          if (!g) continue;
          const uint8_t *bm = c.font.face->bitmap(g);
          if (!bm) continue;
          if (simple) {
            float px, py;
            G.apply(gl.x, gl.y, px, py);
            blitMask(t, bm, g->w, g->h, (int)std::lround(px) + g->bx, (int)std::lround(py) - g->by, col);
          } else {
            float fs = c.font.scale;
            Affine toScreen = G * Affine{fs, 0, 0, fs, gl.x + g->bx * fs, gl.y - g->by * fs};
            Affine inv;
            if (!toScreen.invert(inv)) continue;
            RectI bb = toScreen.mapRect({0, 0, (float)g->w, (float)g->h}).outer();
            blitMaskTransformed(t, bm, g->w, g->h, inv, {bb.x - 1, bb.y - 1, bb.w + 2, bb.h + 2}, col);
          }
        }
        break;
      }
      case Cmd::IMAGE: {
        Affine G = M * c.xf;
        if (G.b == 0 && G.c == 0) {
          RectF dst = G.mapRect(c.dst);
          rt::drawImage(t, c.img->view(), dst, c.src, alpha, c.smooth);
        } else {
          Paint p;
          p.kind = Paint::IMAGE;
          p.img = c.img->view();
          p.alpha = alpha;
          p.smooth = c.smooth;
          // screen -> image: inverse(G * dstMap) where dstMap maps image px -> dst rect
          Affine dstMap{c.dst.w / c.src.w, 0, 0, c.dst.h / c.src.h, c.dst.x - c.src.x * c.dst.w / c.src.w,
                        c.dst.y - c.src.y * c.dst.h / c.src.h};
          Affine all = G * dstMap;
          all.invert(p.imgInv);
          fillPath(t, c.screen, FR_NONZERO, p);
        }
        break;
      }
    }
  }
}

// ============================================================ JS bindings

namespace {

JSClassID gCtxClass = 0, gGradClass = 0, gPathClass = 0;

struct CtxHandle {
  std::shared_ptr<Canvas2D> cv;
  Element *el;
};

void ctxFinalizer(JSRuntime *, JSValueConst v) {
  CtxHandle *h = (CtxHandle *)JS_GetOpaque(v, gCtxClass);
  if (!h) return;
  if (h->el) h->el->unref();
  delete h;
}
void gradFinalizer(JSRuntime *, JSValueConst v) { delete (std::shared_ptr<GradientPaint> *)JS_GetOpaque(v, gGradClass); }
void pathFinalizer(JSRuntime *, JSValueConst v) { delete (Path *)JS_GetOpaque(v, gPathClass); }

Canvas2D *C(JSContext *ctx, JSValueConst v) {
  CtxHandle *h = (CtxHandle *)JS_GetOpaque(v, gCtxClass);
  return h ? h->cv.get() : nullptr;
}

double F(JSContext *ctx, JSValueConst v, double d = 0) {
  double r;
  if (JS_IsUndefined(v) || JS_ToFloat64(ctx, &r, v)) return d;
  return r;
}

std::string S(JSContext *ctx, JSValueConst v) {
  size_t n;
  const char *s = JS_ToCStringLen(ctx, &n, v);
  std::string r = s ? std::string(s, n) : "";
  if (s) JS_FreeCString(ctx, s);
  return r;
}

#define CTX_FN(name) JSValue name(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
#define GET_CV                         \
  Canvas2D *cv = C(ctx, self);         \
  if (!cv) return JS_ThrowTypeError(ctx, "not a CanvasRenderingContext2D");

bool styleFrom(JSContext *ctx, JSValueConst v, CanvasPaint &out) {
  if (JS_IsString(v)) {
    Color c;
    if (!parseColorStr(S(ctx, v), c)) return false;
    out = CanvasPaint();
    out.color = c;
    return true;
  }
  if (auto *g = (std::shared_ptr<GradientPaint> *)JS_GetOpaque(v, gGradClass)) {
    out = CanvasPaint();
    out.kind = CanvasPaint::GRADIENT;
    out.grad = *g;
    return true;
  }
  // pattern: {__pattern: image element, repeat}
  JSValue pe = JS_GetPropertyStr(ctx, v, "__patternEl");
  if (!JS_IsUndefined(pe)) {
    JsHost *h = (JsHost *)JS_GetContextOpaque(ctx);
    Node *n = h->unwrap(pe);
    Element *e = n ? n->asElement() : nullptr;
    JS_FreeValue(ctx, pe);
    if (e && e->x && e->x->image) {
      out = CanvasPaint();
      out.kind = CanvasPaint::PATTERN;
      out.pattern = e->x->image;
      JSValue rep = JS_GetPropertyStr(ctx, v, "__repeat");
      std::string r = S(ctx, rep);
      JS_FreeValue(ctx, rep);
      out.repeatX = r == "repeat" || r == "repeat-x" || r.empty();
      out.repeatY = r == "repeat" || r == "repeat-y" || r.empty();
      return true;
    }
    return false;
  }
  JS_FreeValue(ctx, pe);
  return false;
}

std::string colorStr(Color c) {
  char b[64];
  if (c.a == 255) snprintf(b, sizeof b, "#%02x%02x%02x", c.r, c.g, c.b);
  else snprintf(b, sizeof b, "rgba(%d, %d, %d, %g)", c.r, c.g, c.b, c.a / 255.0);
  return b;
}

enum PropId {
  PR_FILL, PR_STROKE, PR_LINEWIDTH, PR_CAP, PR_JOIN, PR_MITER, PR_ALPHA, PR_COMPOSITE, PR_FONT, PR_ALIGN, PR_BASELINE,
  PR_SHADOWCOLOR, PR_SHADOWBLUR, PR_SHADOWX, PR_SHADOWY, PR_DASHOFFSET, PR_SMOOTH, PR_CANVAS, PR_DIRECTION, PR_FILTER,
  PR_LETTERSPACING, PR_KERNING, PR_SMOOTHQ
};

JSValue ctxGet(JSContext *ctx, JSValueConst self, int magic) {
  Canvas2D *cv = C(ctx, self);
  if (!cv) return JS_UNDEFINED;
  auto &st = cv->st;
  switch (magic) {
    case PR_FILL: case PR_STROKE: {
      JSValue v = JS_GetPropertyStr(ctx, self, magic == PR_FILL ? "__fill" : "__stroke");
      if (!JS_IsUndefined(v)) return v;
      return JS_NewString(ctx, colorStr(magic == PR_FILL ? st.fill.color : st.stroke.color).c_str());
    }
    case PR_LINEWIDTH: return JS_NewFloat64(ctx, st.lineWidth);
    case PR_CAP: return JS_NewString(ctx, st.cap == CAP_ROUND ? "round" : st.cap == CAP_SQUARE ? "square" : "butt");
    case PR_JOIN: return JS_NewString(ctx, st.join == JOIN_ROUND ? "round" : st.join == JOIN_BEVEL ? "bevel" : "miter");
    case PR_MITER: return JS_NewFloat64(ctx, st.miter);
    case PR_ALPHA: return JS_NewFloat64(ctx, st.alpha);
    case PR_COMPOSITE: return JS_NewString(ctx, "source-over");
    case PR_FONT: return JS_NewString(ctx, st.font.c_str());
    case PR_ALIGN: {
      static const char *n[] = {"start", "left", "center", "right", "end"};
      return JS_NewString(ctx, n[st.textAlign]);
    }
    case PR_BASELINE: {
      static const char *n[] = {"alphabetic", "top", "middle", "bottom", "hanging", "ideographic"};
      return JS_NewString(ctx, n[st.textBaseline]);
    }
    case PR_SHADOWCOLOR: return JS_NewString(ctx, colorStr(st.shadowColor).c_str());
    case PR_SHADOWBLUR: return JS_NewFloat64(ctx, st.shadowBlur);
    case PR_SHADOWX: return JS_NewFloat64(ctx, st.shadowX);
    case PR_SHADOWY: return JS_NewFloat64(ctx, st.shadowY);
    case PR_DASHOFFSET: return JS_NewFloat64(ctx, st.dashOffset);
    case PR_SMOOTH: return JS_NewBool(ctx, st.smoothing);
    case PR_CANVAS: {
      CtxHandle *h = (CtxHandle *)JS_GetOpaque(self, gCtxClass);
      return ((JsHost *)JS_GetContextOpaque(ctx))->wrap(h->el);
    }
    case PR_DIRECTION: return JS_NewString(ctx, "ltr");
    case PR_FILTER: return JS_NewString(ctx, "none");
    case PR_LETTERSPACING: return JS_NewString(ctx, "0px");
    case PR_KERNING: return JS_NewString(ctx, "auto");
    case PR_SMOOTHQ: return JS_NewString(ctx, "low");
  }
  return JS_UNDEFINED;
}

JSValue ctxSet(JSContext *ctx, JSValueConst self, JSValueConst v, int magic) {
  Canvas2D *cv = C(ctx, self);
  if (!cv) return JS_UNDEFINED;
  auto &st = cv->st;
  switch (magic) {
    case PR_FILL: case PR_STROKE: {
      CanvasPaint p;
      if (styleFrom(ctx, v, p)) {
        (magic == PR_FILL ? st.fill : st.stroke) = p;
        JS_SetPropertyStr(ctx, self, magic == PR_FILL ? "__fill" : "__stroke", JS_DupValue(ctx, v));
      }
      break;
    }
    case PR_LINEWIDTH: {
      double w = F(ctx, v, st.lineWidth);
      if (w > 0 && std::isfinite(w)) st.lineWidth = (float)w;
      break;
    }
    case PR_CAP: {
      std::string s = S(ctx, v);
      st.cap = s == "round" ? CAP_ROUND : s == "square" ? CAP_SQUARE : CAP_BUTT;
      break;
    }
    case PR_JOIN: {
      std::string s = S(ctx, v);
      st.join = s == "round" ? JOIN_ROUND : s == "bevel" ? JOIN_BEVEL : JOIN_MITER;
      break;
    }
    case PR_MITER: st.miter = (float)F(ctx, v, st.miter); break;
    case PR_ALPHA: {
      double a = F(ctx, v, st.alpha);
      if (a >= 0 && a <= 1) st.alpha = (float)a;
      break;
    }
    case PR_FONT: cv->setFont(S(ctx, v)); break;
    case PR_ALIGN: {
      std::string s = S(ctx, v);
      st.textAlign = s == "left" ? 1 : s == "center" ? 2 : s == "right" ? 3 : s == "end" ? 4 : 0;
      break;
    }
    case PR_BASELINE: {
      std::string s = S(ctx, v);
      st.textBaseline = s == "top" ? 1 : s == "middle" ? 2 : s == "bottom" ? 3 : s == "hanging" ? 4 : s == "ideographic" ? 5 : 0;
      break;
    }
    case PR_SHADOWCOLOR: {
      Color c;
      if (parseColorStr(S(ctx, v), c)) st.shadowColor = c;
      break;
    }
    case PR_SHADOWBLUR: st.shadowBlur = (float)F(ctx, v); break;
    case PR_SHADOWX: st.shadowX = (float)F(ctx, v); break;
    case PR_SHADOWY: st.shadowY = (float)F(ctx, v); break;
    case PR_DASHOFFSET: st.dashOffset = (float)F(ctx, v); break;
    case PR_SMOOTH: st.smoothing = JS_ToBool(ctx, v); break;
    default: break;
  }
  return JS_UNDEFINED;
}

Path *pathArg(JSContext *ctx, int argc, JSValueConst *argv, int &idx) {
  if (argc > 0) {
    Path *p = (Path *)JS_GetOpaque(argv[0], gPathClass);
    if (p) {
      idx = 1;
      return p;
    }
  }
  idx = 0;
  return nullptr;
}

CTX_FN(c_save) { GET_CV; cv->save(); return JS_UNDEFINED; }
CTX_FN(c_restore) { GET_CV; cv->restore(); return JS_UNDEFINED; }
CTX_FN(c_scale) { GET_CV; cv->st.ctm = cv->st.ctm * Affine::scale((float)F(ctx, argv[0], 1), (float)F(ctx, argv[1], 1)); return JS_UNDEFINED; }
CTX_FN(c_rotate) { GET_CV; cv->st.ctm = cv->st.ctm * Affine::rotate((float)F(ctx, argv[0])); return JS_UNDEFINED; }
CTX_FN(c_translate) { GET_CV; cv->st.ctm = cv->st.ctm * Affine::translate((float)F(ctx, argv[0]), (float)F(ctx, argv[1])); return JS_UNDEFINED; }
CTX_FN(c_transform) {
  GET_CV;
  Affine m{(float)F(ctx, argv[0], 1), (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), (float)F(ctx, argv[3], 1), (float)F(ctx, argv[4]), (float)F(ctx, argv[5])};
  cv->st.ctm = cv->st.ctm * m;
  return JS_UNDEFINED;
}
CTX_FN(c_setTransform) {
  GET_CV;
  if (argc == 1 && JS_IsObject(argv[0])) {
    auto g = [&](const char *k, double d) {
      JSValue v = JS_GetPropertyStr(ctx, argv[0], k);
      double r = F(ctx, v, d);
      JS_FreeValue(ctx, v);
      return (float)r;
    };
    cv->st.ctm = Affine{g("a", 1), g("b", 0), g("c", 0), g("d", 1), g("e", 0), g("f", 0)};
  } else if (argc == 0) {
    cv->st.ctm = Affine();
  } else {
    cv->st.ctm = Affine{(float)F(ctx, argv[0], 1), (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), (float)F(ctx, argv[3], 1), (float)F(ctx, argv[4]), (float)F(ctx, argv[5])};
  }
  return JS_UNDEFINED;
}
CTX_FN(c_resetTransform) { GET_CV; cv->st.ctm = Affine(); return JS_UNDEFINED; }
CTX_FN(c_getTransform) {
  GET_CV;
  JSValue o = JS_NewObject(ctx);
  const Affine &m = cv->st.ctm;
  const char *k[] = {"a", "b", "c", "d", "e", "f"};
  float v[] = {m.a, m.b, m.c, m.d, m.e, m.f};
  for (int i = 0; i < 6; i++) JS_SetPropertyStr(ctx, o, k[i], JS_NewFloat64(ctx, v[i]));
  return o;
}
CTX_FN(c_fillRect) { GET_CV; cv->fillRect((float)F(ctx, argv[0]), (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), (float)F(ctx, argv[3])); return JS_UNDEFINED; }
CTX_FN(c_strokeRect) { GET_CV; cv->strokeRect((float)F(ctx, argv[0]), (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), (float)F(ctx, argv[3])); return JS_UNDEFINED; }
CTX_FN(c_clearRect) { GET_CV; cv->clearRect((float)F(ctx, argv[0]), (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), (float)F(ctx, argv[3])); return JS_UNDEFINED; }
CTX_FN(c_beginPath) { GET_CV; cv->path.clear(); return JS_UNDEFINED; }
CTX_FN(c_closePath) {
  GET_CV;
  Path &p = cv->path;
  if (p.open_ && p.pts.size() > p.start_) {
    Path::Pt first = p.pts[p.start_];
    p.close();
    p.moveTo(first.x, first.y);  // subsequent lineTo continues from the start point
  }
  return JS_UNDEFINED;
}
CTX_FN(c_moveTo) {
  GET_CV;
  float x, y;
  cv->st.ctm.apply((float)F(ctx, argv[0]), (float)F(ctx, argv[1]), x, y);
  cv->path.moveTo(x, y);
  return JS_UNDEFINED;
}
CTX_FN(c_lineTo) {
  GET_CV;
  float x, y;
  cv->st.ctm.apply((float)F(ctx, argv[0]), (float)F(ctx, argv[1]), x, y);
  cv->path.lineTo(x, y);
  return JS_UNDEFINED;
}
CTX_FN(c_quadTo) {
  GET_CV;
  float cx, cy, x, y;
  cv->st.ctm.apply((float)F(ctx, argv[0]), (float)F(ctx, argv[1]), cx, cy);
  cv->st.ctm.apply((float)F(ctx, argv[2]), (float)F(ctx, argv[3]), x, y);
  cv->path.quadTo(cx, cy, x, y);
  return JS_UNDEFINED;
}
CTX_FN(c_bezierTo) {
  GET_CV;
  float a, b, c, d, x, y;
  cv->st.ctm.apply((float)F(ctx, argv[0]), (float)F(ctx, argv[1]), a, b);
  cv->st.ctm.apply((float)F(ctx, argv[2]), (float)F(ctx, argv[3]), c, d);
  cv->st.ctm.apply((float)F(ctx, argv[4]), (float)F(ctx, argv[5]), x, y);
  cv->path.cubicTo(a, b, c, d, x, y);
  return JS_UNDEFINED;
}
// arcs are generated in user space and transformed point by point
static void arcUser(Canvas2D *cv, float cx, float cy, float rx, float ry, float rot, float a0, float a1, bool ccw) {
  Path tmp;
  if (cv->path.open_ && !cv->path.pts.empty()) {
    Affine inv;
    cv->st.ctm.invert(inv);
    float ux, uy;
    inv.apply(cv->path.pts.back().x, cv->path.pts.back().y, ux, uy);
    tmp.moveTo(ux, uy);
  }
  bool started = tmp.open_;
  tmp.ellipse(cx, cy, rx, ry, rot, a0, a1, ccw);
  tmp.transform(cv->st.ctm);
  size_t i = 0;
  if (started) i = 1;  // skip the copied current point
  for (; i < tmp.pts.size(); i++) {
    if (!cv->path.open_) cv->path.moveTo(tmp.pts[i].x, tmp.pts[i].y);
    else cv->path.lineTo(tmp.pts[i].x, tmp.pts[i].y);
  }
}
CTX_FN(c_arc) {
  GET_CV;
  float r = (float)F(ctx, argv[2]);
  if (r < 0) return JS_ThrowRangeError(ctx, "IndexSizeError: negative radius");
  arcUser(cv, (float)F(ctx, argv[0]), (float)F(ctx, argv[1]), r, r, 0, (float)F(ctx, argv[3]), (float)F(ctx, argv[4]),
          argc > 5 && JS_ToBool(ctx, argv[5]));
  return JS_UNDEFINED;
}
CTX_FN(c_ellipse) {
  GET_CV;
  arcUser(cv, (float)F(ctx, argv[0]), (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), (float)F(ctx, argv[3]),
          (float)F(ctx, argv[4]), (float)F(ctx, argv[5]), (float)F(ctx, argv[6]), argc > 7 && JS_ToBool(ctx, argv[7]));
  return JS_UNDEFINED;
}
CTX_FN(c_arcTo) {
  GET_CV;
  float x1 = (float)F(ctx, argv[0]), y1 = (float)F(ctx, argv[1]), x2 = (float)F(ctx, argv[2]), y2 = (float)F(ctx, argv[3]);
  float r = (float)F(ctx, argv[4]);
  Affine inv;
  cv->st.ctm.invert(inv);
  float x0, y0;
  if (cv->path.pts.empty() || !cv->path.open_) {
    float sx, sy;
    cv->st.ctm.apply(x1, y1, sx, sy);
    cv->path.moveTo(sx, sy);
    return JS_UNDEFINED;
  }
  inv.apply(cv->path.pts.back().x, cv->path.pts.back().y, x0, y0);
  float d0x = x0 - x1, d0y = y0 - y1, d1x = x2 - x1, d1y = y2 - y1;
  float l0 = std::sqrt(d0x * d0x + d0y * d0y), l1 = std::sqrt(d1x * d1x + d1y * d1y);
  if (l0 < 1e-6f || l1 < 1e-6f || r == 0) {
    float sx, sy;
    cv->st.ctm.apply(x1, y1, sx, sy);
    cv->path.lineTo(sx, sy);
    return JS_UNDEFINED;
  }
  d0x /= l0; d0y /= l0; d1x /= l1; d1y /= l1;
  float cosA = d0x * d1x + d0y * d1y;
  float angle = std::acos(clampf(cosA, -1, 1));
  float t = r / std::tan(angle / 2);
  float tx0 = x1 + d0x * t, ty0 = y1 + d0y * t, tx1 = x1 + d1x * t, ty1 = y1 + d1y * t;
  float bx = d0x + d1x, by = d0y + d1y, bl = std::sqrt(bx * bx + by * by);
  float dc = r / std::sin(angle / 2);
  float cx = x1 + bx / bl * dc, cy = y1 + by / bl * dc;
  float a0 = std::atan2(ty0 - cy, tx0 - cx), a1 = std::atan2(ty1 - cy, tx1 - cx);
  float cross = d0x * d1y - d0y * d1x;
  float sx, sy;
  cv->st.ctm.apply(tx0, ty0, sx, sy);
  cv->path.lineTo(sx, sy);
  arcUser(cv, cx, cy, r, r, 0, a0, a1, cross > 0);
  return JS_UNDEFINED;
}
CTX_FN(c_rect) {
  GET_CV;
  float x = (float)F(ctx, argv[0]), y = (float)F(ctx, argv[1]), w = (float)F(ctx, argv[2]), h = (float)F(ctx, argv[3]);
  Path tmp;
  tmp.rect(x, y, w, h);
  tmp.transform(cv->st.ctm);
  size_t base = cv->path.pts.size();
  cv->path.finishContour();
  cv->path.pts.insert(cv->path.pts.end(), tmp.pts.begin(), tmp.pts.end());
  cv->path.contourEnds.push_back((uint32_t)cv->path.pts.size());
  float sx, sy;
  cv->st.ctm.apply(x, y, sx, sy);
  cv->path.moveTo(sx, sy);
  (void)base;
  return JS_UNDEFINED;
}
CTX_FN(c_roundRect) {
  GET_CV;
  float x = (float)F(ctx, argv[0]), y = (float)F(ctx, argv[1]), w = (float)F(ctx, argv[2]), h = (float)F(ctx, argv[3]);
  float r[4] = {0, 0, 0, 0};
  if (argc > 4) {
    if (JS_IsArray(argv[4])) {
      int64_t n = 0;
      JS_GetLength(ctx, argv[4], &n);
      float v[4] = {0, 0, 0, 0};
      for (int64_t i = 0; i < n && i < 4; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[4], (uint32_t)i);
        v[i] = (float)F(ctx, e);
        JS_FreeValue(ctx, e);
      }
      if (n == 1) r[0] = r[1] = r[2] = r[3] = v[0];
      else if (n == 2) { r[0] = r[2] = v[0]; r[1] = r[3] = v[1]; }
      else if (n == 3) { r[0] = v[0]; r[1] = r[3] = v[1]; r[2] = v[2]; }
      else { r[0] = v[0]; r[1] = v[1]; r[2] = v[2]; r[3] = v[3]; }
    } else {
      r[0] = r[1] = r[2] = r[3] = (float)F(ctx, argv[4]);
    }
  }
  Path tmp;
  tmp.roundRect(x, y, w, h, r);
  tmp.transform(cv->st.ctm);
  cv->path.finishContour();
  cv->path.pts.insert(cv->path.pts.end(), tmp.pts.begin(), tmp.pts.end());
  cv->path.contourEnds.push_back((uint32_t)cv->path.pts.size());
  return JS_UNDEFINED;
}
static FillRule ruleArg(JSContext *ctx, int argc, JSValueConst *argv, int idx) {
  return argc > idx && S(ctx, argv[idx]) == "evenodd" ? FR_EVENODD : FR_NONZERO;
}
CTX_FN(c_fill) {
  GET_CV;
  int idx;
  Path *p2 = pathArg(ctx, argc, argv, idx);
  if (p2) {
    Path p = *p2;
    p.transform(cv->st.ctm);
    cv->fill(p, ruleArg(ctx, argc, argv, idx));
  } else {
    cv->fill(cv->path, ruleArg(ctx, argc, argv, idx));
  }
  return JS_UNDEFINED;
}
CTX_FN(c_stroke) {
  GET_CV;
  int idx;
  Path *p2 = pathArg(ctx, argc, argv, idx);
  if (p2) {
    Path p = *p2;
    p.transform(cv->st.ctm);
    cv->stroke(p);
  } else {
    cv->stroke(cv->path);
  }
  return JS_UNDEFINED;
}
CTX_FN(c_clip) {
  GET_CV;
  int idx;
  Path *p2 = pathArg(ctx, argc, argv, idx);
  if (p2) {
    Path p = *p2;
    p.transform(cv->st.ctm);
    cv->clip(p);
  } else {
    cv->clip(cv->path);
  }
  return JS_UNDEFINED;
}
CTX_FN(c_isPointInPath) {
  GET_CV;
  int idx;
  Path *p2 = pathArg(ctx, argc, argv, idx);
  Path p = p2 ? *p2 : cv->path;
  if (p2) p.transform(cv->st.ctm);
  float x = (float)F(ctx, argv[idx]), y = (float)F(ctx, argv[idx + 1]);
  // even-odd ray cast over the flattened contours
  bool inside = false;
  uint32_t start = 0;
  std::vector<uint32_t> ends = p.contourEnds;
  if (p.open_) ends.push_back((uint32_t)p.pts.size());
  for (uint32_t end : ends) {
    for (uint32_t i = start, j = end - 1; i < end; j = i++) {
      const auto &a = p.pts[i], &b = p.pts[j];
      if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    start = end;
  }
  return JS_NewBool(ctx, inside);
}
CTX_FN(c_fillText) {
  GET_CV;
  cv->fillText(S(ctx, argv[0]), (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), argc > 3 ? (float)F(ctx, argv[3], -1) : -1, false);
  return JS_UNDEFINED;
}
CTX_FN(c_strokeText) {
  GET_CV;
  cv->fillText(S(ctx, argv[0]), (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), argc > 3 ? (float)F(ctx, argv[3], -1) : -1, true);
  return JS_UNDEFINED;
}
CTX_FN(c_measureText) {
  GET_CV;
  float a, d;
  float w = cv->measureText(S(ctx, argv[0]), a, d);
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, w));
  JS_SetPropertyStr(ctx, o, "actualBoundingBoxAscent", JS_NewFloat64(ctx, a * 0.75));
  JS_SetPropertyStr(ctx, o, "actualBoundingBoxDescent", JS_NewFloat64(ctx, d * 0.5));
  JS_SetPropertyStr(ctx, o, "fontBoundingBoxAscent", JS_NewFloat64(ctx, a));
  JS_SetPropertyStr(ctx, o, "fontBoundingBoxDescent", JS_NewFloat64(ctx, d));
  JS_SetPropertyStr(ctx, o, "actualBoundingBoxLeft", JS_NewFloat64(ctx, 0));
  JS_SetPropertyStr(ctx, o, "actualBoundingBoxRight", JS_NewFloat64(ctx, w));
  return o;
}
CTX_FN(c_drawImage) {
  GET_CV;
  JsHost *h = (JsHost *)JS_GetContextOpaque(ctx);
  Node *n = h->unwrap(argv[0]);
  Element *e = n ? n->asElement() : nullptr;
  std::shared_ptr<Image> img;
  if (e && e->x && e->x->image) img = e->x->image;
  if (e && e->tagId() == TAG_canvas && e->x && e->x->canvas) {
    // canvas source: snapshot it
    Canvas2D *src = e->x->canvas.get();
    auto data = src->getImageData(0, 0, src->width(), src->height());
    img = std::make_shared<Image>();
    img->w = src->width();
    img->h = src->height();
    img->owned.resize((size_t)img->w * img->h * 2);
    uint16_t *px = (uint16_t *)img->owned.data();
    for (int i = 0; i < img->w * img->h; i++) px[i] = Color{data[i * 4], data[i * 4 + 1], data[i * 4 + 2], 255}.to565();
    img->px = px;
  }
  if (!img) return JS_UNDEFINED;
  float iw = (float)img->w, ih = (float)img->h;
  if (argc >= 9) {
    cv->drawImage(img, (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), (float)F(ctx, argv[3]), (float)F(ctx, argv[4]),
                  (float)F(ctx, argv[5]), (float)F(ctx, argv[6]), (float)F(ctx, argv[7]), (float)F(ctx, argv[8]));
  } else if (argc >= 5) {
    cv->drawImage(img, 0, 0, iw, ih, (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), (float)F(ctx, argv[3]), (float)F(ctx, argv[4]));
  } else {
    cv->drawImage(img, 0, 0, iw, ih, (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), iw, ih);
  }
  return JS_UNDEFINED;
}
JSValue makeGradient(JSContext *ctx, std::shared_ptr<GradientPaint> g) {
  JsHost *h = (JsHost *)JS_GetContextOpaque(ctx);
  (void)h;
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue proto = JS_GetPropertyStr(ctx, global, "__gradProto");
  JS_FreeValue(ctx, global);
  JSValue o = JS_NewObjectProtoClass(ctx, proto, gGradClass);
  JS_FreeValue(ctx, proto);
  JS_SetOpaque(o, new std::shared_ptr<GradientPaint>(g));
  return o;
}
CTX_FN(c_linearGradient) {
  GET_CV;
  auto g = std::make_shared<GradientPaint>();
  g->kind = 0;
  float x0 = (float)F(ctx, argv[0]), y0 = (float)F(ctx, argv[1]), x1 = (float)F(ctx, argv[2]), y1 = (float)F(ctx, argv[3]);
  cv->st.ctm.apply(x0, y0, g->ax, g->ay);
  cv->st.ctm.apply(x1, y1, g->bx, g->by);
  return makeGradient(ctx, g);
}
CTX_FN(c_radialGradient) {
  GET_CV;
  auto g = std::make_shared<GradientPaint>();
  g->kind = 1;
  // approximation: concentric circles centred on the end circle
  float x1 = (float)F(ctx, argv[3]), y1 = (float)F(ctx, argv[4]), r1 = (float)F(ctx, argv[5]);
  float r0 = (float)F(ctx, argv[2]);
  cv->st.ctm.apply(x1, y1, g->cx, g->cy);
  float s = std::sqrt(std::fabs(cv->st.ctm.a * cv->st.ctm.d - cv->st.ctm.b * cv->st.ctm.c));
  g->rx = g->ry = std::max(0.5f, r1 * s);
  (void)r0;
  return makeGradient(ctx, g);
}
CTX_FN(c_conicGradient) {
  GET_CV;
  auto g = std::make_shared<GradientPaint>();
  g->kind = 2;
  g->startAngle = (float)F(ctx, argv[0]) + 1.5707963f;
  cv->st.ctm.apply((float)F(ctx, argv[1]), (float)F(ctx, argv[2]), g->cx, g->cy);
  return makeGradient(ctx, g);
}
CTX_FN(c_pattern) {
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "__patternEl", JS_DupValue(ctx, argv[0]));
  JS_SetPropertyStr(ctx, o, "__repeat", argc > 1 ? JS_DupValue(ctx, argv[1]) : JS_NewString(ctx, "repeat"));
  JS_SetPropertyStr(ctx, o, "setTransform", JS_UNDEFINED);
  return o;
}
CTX_FN(c_setLineDash) {
  GET_CV;
  cv->st.dash.clear();
  int64_t n = 0;
  JS_GetLength(ctx, argv[0], &n);
  for (int64_t i = 0; i < n; i++) {
    JSValue e = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
    cv->st.dash.push_back((float)F(ctx, e));
    JS_FreeValue(ctx, e);
  }
  if (cv->st.dash.size() % 2) {
    auto d = cv->st.dash;
    cv->st.dash.insert(cv->st.dash.end(), d.begin(), d.end());
  }
  bool allZero = true;
  for (float d : cv->st.dash)
    if (d > 0) allZero = false;
  if (allZero) cv->st.dash.clear();
  return JS_UNDEFINED;
}
CTX_FN(c_getLineDash) {
  GET_CV;
  JSValue a = JS_NewArray(ctx);
  for (uint32_t i = 0; i < cv->st.dash.size(); i++) JS_SetPropertyUint32(ctx, a, i, JS_NewFloat64(ctx, cv->st.dash[i]));
  return a;
}
static JSValue newImageData(JSContext *ctx, const uint8_t *data, int w, int h) {
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue ctor = JS_GetPropertyStr(ctx, global, "ImageData");
  JS_FreeValue(ctx, global);
  JSValue buf = JS_NewArrayBufferCopy(ctx, data, (size_t)w * h * 4);
  JSValue args[3] = {buf, JS_NewInt32(ctx, w), JS_NewInt32(ctx, h)};
  JSValue r = JS_CallConstructor(ctx, ctor, 3, args);
  JS_FreeValue(ctx, ctor);
  JS_FreeValue(ctx, buf);
  return r;
}
CTX_FN(c_getImageData) {
  GET_CV;
  int x = (int)F(ctx, argv[0]), y = (int)F(ctx, argv[1]), w = std::max(1, (int)F(ctx, argv[2])), h = std::max(1, (int)F(ctx, argv[3]));
  auto d = cv->getImageData(x, y, w, h);
  return newImageData(ctx, d.data(), w, h);
}
CTX_FN(c_createImageData) {
  int w = std::max(1, (int)F(ctx, argv[0])), h = std::max(1, (int)F(ctx, argv[1]));
  if (JS_IsObject(argv[0])) {
    JSValue vw = JS_GetPropertyStr(ctx, argv[0], "width"), vh = JS_GetPropertyStr(ctx, argv[0], "height");
    w = (int)F(ctx, vw, 1);
    h = (int)F(ctx, vh, 1);
    JS_FreeValue(ctx, vw);
    JS_FreeValue(ctx, vh);
  }
  std::vector<uint8_t> z((size_t)w * h * 4, 0);
  return newImageData(ctx, z.data(), w, h);
}
CTX_FN(c_putImageData) {
  GET_CV;
  JSValue data = JS_GetPropertyStr(ctx, argv[0], "data");
  JSValue vw = JS_GetPropertyStr(ctx, argv[0], "width"), vh = JS_GetPropertyStr(ctx, argv[0], "height");
  int w = (int)F(ctx, vw), h = (int)F(ctx, vh);
  JSValue ab = JS_GetPropertyStr(ctx, data, "buffer");
  size_t size = 0;
  uint8_t *p = JS_GetArrayBuffer(ctx, &size, ab);
  if (p && size >= (size_t)w * h * 4) cv->putImageData(p, w, h, (int)F(ctx, argv[1]), (int)F(ctx, argv[2]));
  JS_FreeValue(ctx, ab);
  JS_FreeValue(ctx, data);
  JS_FreeValue(ctx, vw);
  JS_FreeValue(ctx, vh);
  return JS_UNDEFINED;
}
CTX_FN(c_reset) {
  GET_CV;
  cv->resize(cv->width(), cv->height());
  return JS_UNDEFINED;
}
CTX_FN(c_noop) { return JS_UNDEFINED; }

// gradients
JSValue g_addColorStop(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
  auto *g = (std::shared_ptr<GradientPaint> *)JS_GetOpaque(self, gGradClass);
  if (!g) return JS_UNDEFINED;
  Color c;
  if (!parseColorStr(S(ctx, argv[1]), c)) return JS_ThrowSyntaxError(ctx, "invalid colour");
  float off = (float)clampf((float)F(ctx, argv[0]), 0, 1);
  auto &stops = (*g)->stops;
  stops.push_back({off, c});
  std::stable_sort(stops.begin(), stops.end(), [](const GradientPaint::Stop &a, const GradientPaint::Stop &b) { return a.t < b.t; });
  (*g)->build();
  return JS_UNDEFINED;
}

// Path2D
JSValue p_ctor(JSContext *ctx, JSValueConst newTarget, int argc, JSValueConst *argv) {
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue proto = JS_GetPropertyStr(ctx, global, "__pathProto");
  JS_FreeValue(ctx, global);
  JSValue o = JS_NewObjectProtoClass(ctx, proto, gPathClass);
  JS_FreeValue(ctx, proto);
  Path *p = new Path();
  if (argc > 0) {
    if (Path *src = (Path *)JS_GetOpaque(argv[0], gPathClass)) *p = *src;
    else if (JS_IsString(argv[0])) parseSvgPath(S(ctx, argv[0]), *p);
  }
  JS_SetOpaque(o, p);
  return o;
}
Path *P(JSValueConst self) { return (Path *)JS_GetOpaque(self, gPathClass); }
#define PATH_FN(name, ...)                                                        \
  JSValue name(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) { \
    Path *p = P(self);                                                            \
    if (!p) return JS_UNDEFINED;                                                  \
    __VA_ARGS__;                                                                  \
    return JS_UNDEFINED;                                                          \
  }
PATH_FN(p_moveTo, p->moveTo((float)F(ctx, argv[0]), (float)F(ctx, argv[1])))
PATH_FN(p_lineTo, p->lineTo((float)F(ctx, argv[0]), (float)F(ctx, argv[1])))
PATH_FN(p_closePath, {
  if (p->open_ && p->pts.size() > p->start_) {
    Path::Pt f = p->pts[p->start_];
    p->close();
    p->moveTo(f.x, f.y);
  }
})
PATH_FN(p_quadTo, p->quadTo((float)F(ctx, argv[0]), (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), (float)F(ctx, argv[3])))
PATH_FN(p_bezierTo, p->cubicTo((float)F(ctx, argv[0]), (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), (float)F(ctx, argv[3]),
                               (float)F(ctx, argv[4]), (float)F(ctx, argv[5])))
PATH_FN(p_arc, p->arc((float)F(ctx, argv[0]), (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), (float)F(ctx, argv[3]),
                      (float)F(ctx, argv[4]), argc > 5 && JS_ToBool(ctx, argv[5])))
PATH_FN(p_ellipse, p->ellipse((float)F(ctx, argv[0]), (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), (float)F(ctx, argv[3]),
                              (float)F(ctx, argv[4]), (float)F(ctx, argv[5]), (float)F(ctx, argv[6]), argc > 7 && JS_ToBool(ctx, argv[7])))
PATH_FN(p_rect, p->rect((float)F(ctx, argv[0]), (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), (float)F(ctx, argv[3])))
PATH_FN(p_roundRect, {
  float r = argc > 4 ? (float)F(ctx, argv[4]) : 0;
  float rr[4] = {r, r, r, r};
  p->roundRect((float)F(ctx, argv[0]), (float)F(ctx, argv[1]), (float)F(ctx, argv[2]), (float)F(ctx, argv[3]), rr);
})
PATH_FN(p_addPath, {
  if (Path *o = P(argv[0])) {
    p->finishContour();
    size_t base = p->pts.size();
    p->pts.insert(p->pts.end(), o->pts.begin(), o->pts.end());
    for (auto e : o->contourEnds) p->contourEnds.push_back((uint32_t)(e + base));
    for (auto e : o->openEnds) p->openEnds.push_back((uint32_t)(e + base));
  }
})

}  // namespace

JSClassID registerCanvasClass(JSRuntime *rt) {
  JSClassDef cd = {};
  cd.class_name = "CanvasRenderingContext2D";
  cd.finalizer = ctxFinalizer;
  gCtxClass = 0;
  JS_NewClassID(rt, &gCtxClass);
  JS_NewClass(rt, gCtxClass, &cd);
  JSClassDef gd = {};
  gd.class_name = "CanvasGradient";
  gd.finalizer = gradFinalizer;
  gGradClass = 0;
  JS_NewClassID(rt, &gGradClass);
  JS_NewClass(rt, gGradClass, &gd);
  JSClassDef pd = {};
  pd.class_name = "Path2D";
  pd.finalizer = pathFinalizer;
  gPathClass = 0;
  JS_NewClassID(rt, &gPathClass);
  JS_NewClass(rt, gPathClass, &pd);
  return gCtxClass;
}

static void defineCanvasProtos(JSContext *ctx) {
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue has = JS_GetPropertyStr(ctx, global, "__ctxProto");
  if (!JS_IsUndefined(has)) {
    JS_FreeValue(ctx, has);
    JS_FreeValue(ctx, global);
    return;
  }
  // context prototype
  JSValue proto = JS_NewObject(ctx);
  struct M {
    const char *n;
    JSCFunction *f;
    int len;
  };
  static const M methods[] = {
      {"save", c_save, 0}, {"restore", c_restore, 0}, {"scale", c_scale, 2}, {"rotate", c_rotate, 1},
      {"translate", c_translate, 2}, {"transform", c_transform, 6}, {"setTransform", c_setTransform, 6},
      {"resetTransform", c_resetTransform, 0}, {"getTransform", c_getTransform, 0}, {"fillRect", c_fillRect, 4},
      {"strokeRect", c_strokeRect, 4}, {"clearRect", c_clearRect, 4}, {"beginPath", c_beginPath, 0},
      {"closePath", c_closePath, 0}, {"moveTo", c_moveTo, 2}, {"lineTo", c_lineTo, 2},
      {"quadraticCurveTo", c_quadTo, 4}, {"bezierCurveTo", c_bezierTo, 6}, {"arc", c_arc, 6}, {"arcTo", c_arcTo, 5},
      {"ellipse", c_ellipse, 8}, {"rect", c_rect, 4}, {"roundRect", c_roundRect, 5}, {"fill", c_fill, 2},
      {"stroke", c_stroke, 1}, {"clip", c_clip, 2}, {"isPointInPath", c_isPointInPath, 3},
      {"isPointInStroke", c_isPointInPath, 3}, {"fillText", c_fillText, 4}, {"strokeText", c_strokeText, 4},
      {"measureText", c_measureText, 1}, {"drawImage", c_drawImage, 9}, {"createLinearGradient", c_linearGradient, 4},
      {"createRadialGradient", c_radialGradient, 6}, {"createConicGradient", c_conicGradient, 3},
      {"createPattern", c_pattern, 2}, {"setLineDash", c_setLineDash, 1}, {"getLineDash", c_getLineDash, 0},
      {"getImageData", c_getImageData, 4}, {"createImageData", c_createImageData, 2},
      {"putImageData", c_putImageData, 3}, {"reset", c_reset, 0}, {"getContextAttributes", c_noop, 0},
      {"drawFocusIfNeeded", c_noop, 1}, {"scrollPathIntoView", c_noop, 0},
  };
  for (auto &m : methods) JS_SetPropertyStr(ctx, proto, m.n, JS_NewCFunction(ctx, m.f, m.n, m.len));
  static const char *props[] = {"fillStyle", "strokeStyle", "lineWidth", "lineCap", "lineJoin", "miterLimit", "globalAlpha",
                                "globalCompositeOperation", "font", "textAlign", "textBaseline", "shadowColor", "shadowBlur",
                                "shadowOffsetX", "shadowOffsetY", "lineDashOffset", "imageSmoothingEnabled", "canvas",
                                "direction", "filter", "letterSpacing", "fontKerning", "imageSmoothingQuality"};
  for (int i = 0; i < (int)(sizeof(props) / sizeof(props[0])); i++) {
    JSAtom atom = JS_NewAtom(ctx, props[i]);
    JSValue getter = JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)(void *)ctxGet, props[i], 0, JS_CFUNC_getter_magic, i);
    JSValue setter = JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)(void *)ctxSet, props[i], 1, JS_CFUNC_setter_magic, i);
    JS_DefinePropertyGetSet(ctx, proto, atom, getter, setter, JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, atom);
  }
  JS_SetPropertyStr(ctx, global, "__ctxProto", proto);
  // gradient prototype
  JSValue gp = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, gp, "addColorStop", JS_NewCFunction(ctx, g_addColorStop, "addColorStop", 2));
  JS_SetPropertyStr(ctx, global, "__gradProto", gp);
  // Path2D
  JSValue pp = JS_NewObject(ctx);
  static const M pm[] = {{"moveTo", p_moveTo, 2}, {"lineTo", p_lineTo, 2}, {"closePath", p_closePath, 0},
                         {"quadraticCurveTo", p_quadTo, 4}, {"bezierCurveTo", p_bezierTo, 6}, {"arc", p_arc, 6},
                         {"ellipse", p_ellipse, 8}, {"rect", p_rect, 4}, {"roundRect", p_roundRect, 5},
                         {"addPath", p_addPath, 1}};
  for (auto &m : pm) JS_SetPropertyStr(ctx, pp, m.n, JS_NewCFunction(ctx, m.f, m.n, m.len));
  JS_SetPropertyStr(ctx, global, "__pathProto", JS_DupValue(ctx, pp));
  JSValue ctor = JS_NewCFunction2(ctx, (JSCFunction *)p_ctor, "Path2D", 1, JS_CFUNC_constructor, 0);
  JS_SetConstructor(ctx, ctor, pp);
  JS_SetPropertyStr(ctx, global, "Path2D", ctor);
  JS_FreeValue(ctx, pp);
  JS_FreeValue(ctx, global);
}

JSValue canvasContext(JSContext *ctx, JsHost *host, Element *el) {
  (void)host;
  defineCanvasProtos(ctx);
  ElementExtra &x = el->extra();
  if (!x.canvas) x.canvas = std::make_shared<Canvas2D>(host->engine(), el);
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue proto = JS_GetPropertyStr(ctx, global, "__ctxProto");
  JS_FreeValue(ctx, global);
  JSValue o = JS_NewObjectProtoClass(ctx, proto, gCtxClass);
  JS_FreeValue(ctx, proto);
  CtxHandle *h = new CtxHandle{x.canvas, el};
  el->ref();
  JS_SetOpaque(o, h);
  return o;
}

}  // namespace rt
