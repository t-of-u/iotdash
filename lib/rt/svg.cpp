#include "svg.h"

#include "extra.h"
#include "style.h"

namespace rt {

// ============================================================ path data

namespace {
struct PD {
  const char *s;
  size_t n, i = 0;
  void ws() {
    while (i < n && (isSpace(s[i]) || s[i] == ',')) i++;
  }
  bool number(float &out) {
    ws();
    if (i >= n) return false;
    size_t st = i;
    if (s[i] == '+' || s[i] == '-') i++;
    bool digits = false, dot = false;
    while (i < n) {
      char c = s[i];
      if (isdigit((unsigned char)c)) {
        digits = true;
        i++;
      } else if (c == '.' && !dot) {
        dot = true;
        i++;
      } else {
        break;
      }
    }
    if (digits && i < n && (s[i] == 'e' || s[i] == 'E')) {
      size_t save = i;
      i++;
      if (i < n && (s[i] == '+' || s[i] == '-')) i++;
      if (i < n && isdigit((unsigned char)s[i])) {
        while (i < n && isdigit((unsigned char)s[i])) i++;
      } else {
        i = save;
      }
    }
    if (!digits) {
      i = st;
      return false;
    }
    out = strtof(std::string(s + st, i - st).c_str(), nullptr);
    return true;
  }
  bool flag(bool &out) {
    ws();
    if (i < n && (s[i] == '0' || s[i] == '1')) {
      out = s[i] == '1';
      i++;
      return true;
    }
    return false;
  }
};

void arcTo(Path &p, float x0, float y0, float rx, float ry, float phiDeg, bool large, bool sweep, float x, float y) {
  if (rx == 0 || ry == 0) {
    p.lineTo(x, y);
    return;
  }
  rx = std::fabs(rx);
  ry = std::fabs(ry);
  float phi = phiDeg * 0.0174532925f, c = std::cos(phi), s = std::sin(phi);
  float dx = (x0 - x) / 2, dy = (y0 - y) / 2;
  float x1 = c * dx + s * dy, y1 = -s * dx + c * dy;
  float lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
  if (lam > 1) {
    float sl = std::sqrt(lam);
    rx *= sl;
    ry *= sl;
  }
  float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
  float den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
  float k = den > 0 ? std::sqrt(std::max(0.f, num / den)) : 0;
  if (large == sweep) k = -k;
  float cx1 = k * rx * y1 / ry, cy1 = -k * ry * x1 / rx;
  float cx = c * cx1 - s * cy1 + (x0 + x) / 2, cy = s * cx1 + c * cy1 + (y0 + y) / 2;
  auto ang = [](float ux, float uy, float vx, float vy) {
    float a = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    return a;
  };
  float t1 = ang(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
  float dt = ang((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
  if (!sweep && dt > 0) dt -= 6.2831853f;
  if (sweep && dt < 0) dt += 6.2831853f;
  int segs = std::max(2, (int)std::ceil(std::fabs(dt) * std::sqrt(std::max(rx, ry)) * 1.2f));
  for (int k2 = 1; k2 <= segs; k2++) {
    float t = t1 + dt * k2 / segs;
    float ex = rx * std::cos(t), ey = ry * std::sin(t);
    p.lineTo(c * ex - s * ey + cx, s * ex + c * ey + cy);
  }
}
}  // namespace

bool parseSvgPath(const std::string &d, Path &p) {
  PD pd{d.data(), d.size()};
  char cmd = 0;
  float cx = 0, cy = 0, sx = 0, sy = 0;   // current point, subpath start
  float lcx = 0, lcy = 0;                 // last control point
  char lastCmd = 0;
  while (true) {
    pd.ws();
    if (pd.i >= pd.n) break;
    char ch = pd.s[pd.i];
    if (isalpha((unsigned char)ch)) {
      cmd = ch;
      pd.i++;
    } else if (!cmd) {
      return false;
    }
    bool rel = islower((unsigned char)cmd);
    char C = (char)toupper((unsigned char)cmd);
    float a[7];
    auto nums = [&](int k) {
      for (int j = 0; j < k; j++)
        if (!pd.number(a[j])) return false;
      return true;
    };
    switch (C) {
      case 'Z':
        p.close();  // the next drawing command starts a new contour at (sx, sy)
        cx = sx;
        cy = sy;
        lastCmd = 'Z';
        cmd = 0;
        continue;
      case 'M':
        if (!nums(2)) return true;
        cx = rel ? cx + a[0] : a[0];
        cy = rel ? cy + a[1] : a[1];
        p.moveTo(cx, cy);
        sx = cx;
        sy = cy;
        cmd = rel ? 'l' : 'L';  // implicit lineto
        break;
      case 'L':
        if (!nums(2)) return true;
        cx = rel ? cx + a[0] : a[0];
        cy = rel ? cy + a[1] : a[1];
        if (!p.open_) p.moveTo(sx, sy);
        p.lineTo(cx, cy);
        break;
      case 'H':
        if (!nums(1)) return true;
        cx = rel ? cx + a[0] : a[0];
        if (!p.open_) p.moveTo(sx, sy);
        p.lineTo(cx, cy);
        break;
      case 'V':
        if (!nums(1)) return true;
        cy = rel ? cy + a[0] : a[0];
        if (!p.open_) p.moveTo(sx, sy);
        p.lineTo(cx, cy);
        break;
      case 'C': {
        if (!nums(6)) return true;
        float x1 = a[0], y1 = a[1], x2 = a[2], y2 = a[3], x = a[4], y = a[5];
        if (rel) { x1 += cx; y1 += cy; x2 += cx; y2 += cy; x += cx; y += cy; }
        if (!p.open_) p.moveTo(cx, cy);
        p.cubicTo(x1, y1, x2, y2, x, y);
        lcx = x2; lcy = y2; cx = x; cy = y;
        lastCmd = 'C';
        continue;
      }
      case 'S': {
        if (!nums(4)) return true;
        float x2 = a[0], y2 = a[1], x = a[2], y = a[3];
        if (rel) { x2 += cx; y2 += cy; x += cx; y += cy; }
        float x1 = (lastCmd == 'C' || lastCmd == 'S') ? 2 * cx - lcx : cx;
        float y1 = (lastCmd == 'C' || lastCmd == 'S') ? 2 * cy - lcy : cy;
        if (!p.open_) p.moveTo(cx, cy);
        p.cubicTo(x1, y1, x2, y2, x, y);
        lcx = x2; lcy = y2; cx = x; cy = y;
        lastCmd = 'S';
        continue;
      }
      case 'Q': {
        if (!nums(4)) return true;
        float x1 = a[0], y1 = a[1], x = a[2], y = a[3];
        if (rel) { x1 += cx; y1 += cy; x += cx; y += cy; }
        if (!p.open_) p.moveTo(cx, cy);
        p.quadTo(x1, y1, x, y);
        lcx = x1; lcy = y1; cx = x; cy = y;
        lastCmd = 'Q';
        continue;
      }
      case 'T': {
        if (!nums(2)) return true;
        float x = a[0], y = a[1];
        if (rel) { x += cx; y += cy; }
        float x1 = (lastCmd == 'Q' || lastCmd == 'T') ? 2 * cx - lcx : cx;
        float y1 = (lastCmd == 'Q' || lastCmd == 'T') ? 2 * cy - lcy : cy;
        if (!p.open_) p.moveTo(cx, cy);
        p.quadTo(x1, y1, x, y);
        lcx = x1; lcy = y1; cx = x; cy = y;
        lastCmd = 'T';
        continue;
      }
      case 'A': {
        bool large, sweep;
        if (!pd.number(a[0]) || !pd.number(a[1]) || !pd.number(a[2]) || !pd.flag(large) || !pd.flag(sweep) ||
            !pd.number(a[5]) || !pd.number(a[6]))
          return true;
        float x = rel ? cx + a[5] : a[5], y = rel ? cy + a[6] : a[6];
        if (!p.open_) p.moveTo(cx, cy);
        arcTo(p, cx, cy, a[0], a[1], a[2], large, sweep, x, y);
        cx = x;
        cy = y;
        break;
      }
      default:
        return false;
    }
    lastCmd = C;
  }
  return true;
}

bool parseSvgTransform(const std::string &s, Affine &out) {
  out = Affine();
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (isSpace(s[i]) || s[i] == ',')) i++;
    size_t st = i;
    while (i < s.size() && isalpha((unsigned char)s[i])) i++;
    std::string fn = toLower(s.substr(st, i - st));
    size_t open = s.find('(', i), close = s.find(')', i);
    if (fn.empty() || open == std::string::npos || close == std::string::npos) break;
    std::string args = s.substr(open + 1, close - open - 1);
    i = close + 1;
    std::vector<float> v;
    PD pd{args.data(), args.size()};
    float f;
    while (pd.number(f)) v.push_back(f);
    Affine m;
    if (fn == "matrix" && v.size() >= 6) m = Affine{v[0], v[1], v[2], v[3], v[4], v[5]};
    else if (fn == "translate" && !v.empty()) m = Affine::translate(v[0], v.size() > 1 ? v[1] : 0);
    else if (fn == "scale" && !v.empty()) m = Affine::scale(v[0], v.size() > 1 ? v[1] : v[0]);
    else if (fn == "rotate" && !v.empty()) {
      float r = v[0] * 0.0174532925f;
      if (v.size() >= 3) m = Affine::translate(v[1], v[2]) * Affine::rotate(r) * Affine::translate(-v[1], -v[2]);
      else m = Affine::rotate(r);
    } else if (fn == "skewx" && !v.empty()) m = Affine{1, 0, std::tan(v[0] * 0.0174532925f), 1, 0, 0};
    else if (fn == "skewy" && !v.empty()) m = Affine{1, std::tan(v[0] * 0.0174532925f), 0, 1, 0, 0};
    out = out * m;
  }
  return true;
}

// ============================================================ building

namespace {
struct SvgState {
  bool fill = true, stroke = false;
  Color fillColor = kBlack, strokeColor = kBlack;
  bool fillCurrent = false, strokeCurrent = false;
  float strokeWidth = 1, opacity = 1, fillOpacity = 1, strokeOpacity = 1;
  FillRule rule = FR_NONZERO;
  LineCap cap = CAP_BUTT;
  LineJoin join = JOIN_MITER;
  std::vector<float> dash;
  Affine m;
};

void applyPaint(const std::string &v, bool &on, Color &c, bool &current) {
  std::string s = toLower(trim(v));
  if (s.empty()) return;
  if (s == "none" || s == "transparent") {
    on = false;
    return;
  }
  if (s == "currentcolor") {
    on = true;
    current = true;
    return;
  }
  Color col;
  if (startsWith(s, "url(")) {  // gradients/patterns unsupported: grey fallback
    on = true;
    current = false;
    c = Color{128, 128, 128, 255};
    return;
  }
  if (parseColorStr(s, col)) {
    on = true;
    current = false;
    c = col;
  }
}

void applyProp(SvgState &st, const std::string &name, const std::string &value) {
  if (name == "fill") applyPaint(value, st.fill, st.fillColor, st.fillCurrent);
  else if (name == "stroke") applyPaint(value, st.stroke, st.strokeColor, st.strokeCurrent);
  else if (name == "stroke-width") st.strokeWidth = std::max(0.f, strtof(value.c_str(), nullptr));
  else if (name == "opacity") st.opacity *= clampf(strtof(value.c_str(), nullptr), 0, 1);
  else if (name == "fill-opacity") st.fillOpacity = clampf(strtof(value.c_str(), nullptr), 0, 1);
  else if (name == "stroke-opacity") st.strokeOpacity = clampf(strtof(value.c_str(), nullptr), 0, 1);
  else if (name == "fill-rule") st.rule = trim(value) == "evenodd" ? FR_EVENODD : FR_NONZERO;
  else if (name == "stroke-linecap") {
    std::string v = trim(value);
    st.cap = v == "round" ? CAP_ROUND : v == "square" ? CAP_SQUARE : CAP_BUTT;
  } else if (name == "stroke-linejoin") {
    std::string v = trim(value);
    st.join = v == "round" ? JOIN_ROUND : v == "bevel" ? JOIN_BEVEL : JOIN_MITER;
  } else if (name == "stroke-dasharray") {
    st.dash.clear();
    PD pd{value.data(), value.size()};
    float f;
    while (pd.number(f)) st.dash.push_back(f);
    if (st.dash.size() % 2) {
      auto d = st.dash;
      st.dash.insert(st.dash.end(), d.begin(), d.end());
    }
  }
}

void applyElementStyle(Element *e, SvgState &st) {
  static const char *names[] = {"fill", "stroke", "stroke-width", "opacity", "fill-opacity", "stroke-opacity",
                                "fill-rule", "stroke-linecap", "stroke-linejoin", "stroke-dasharray"};
  for (auto n : names) {
    // read-only lookup: this runs while painting (possibly on both cores), and an
    // attribute name that was never interned is not on any element
    Atom a = findAtom(n, strlen(n));
    const std::string *v = a ? e->getAttr(a) : nullptr;
    if (v) applyProp(st, n, *v);
  }
  // inline style="fill: ..." (presentation via CSS declarations)
  if (const std::string *style = e->getAttr(A.style)) {
    for (auto &decl : split(*style, ';')) {
      size_t c = decl.find(':');
      if (c == std::string::npos) continue;
      applyProp(st, trim(toLower(decl.substr(0, c))), trim(decl.substr(c + 1)));
    }
  }
  // author CSS for fill/stroke (e.g. ".icon path { stroke: currentColor }")
  if (e->cs) {
    const ComputedStyle &cs = *e->cs;
    if (cs.svgFillSet) {
      st.fill = !cs.svgFillNone;
      st.fillCurrent = cs.svgFill.current;
      st.fillColor = cs.svgFill.c;
    }
    if (cs.svgStrokeSet) {
      st.stroke = !cs.svgStrokeNone;
      st.strokeCurrent = cs.svgStroke.current;
      st.strokeColor = cs.svgStroke.c;
    }
    if (cs.svgStrokeWidthSet) st.strokeWidth = cs.svgStrokeWidth;
    if (cs.svgCapSet) st.cap = (LineCap)cs.svgCap;
    if (cs.svgJoinSet) st.join = (LineJoin)cs.svgJoin;
  }
}

float A_(Element *e, Atom a, float d = 0) { return e->attrNum(a, d); }

void walk(Element *e, SvgState st, SvgCache &out) {
  if (e->cs && e->cs->display == D_NONE) return;
  if (const std::string *t = e->getAttr(A.transform)) {
    Affine m;
    parseSvgTransform(*t, m);
    st.m = st.m * m;
  }
  applyElementStyle(e, st);
  Tag tag = e->tagId();
  Path p;
  bool shape = true;
  switch (tag) {
    case TAG_path: parseSvgPath(e->attrOr(A.d, ""), p); break;
    case TAG_rect: {
      float x = A_(e, A.x), y = A_(e, A.y), w = A_(e, A.width), h = A_(e, A.height);
      float rx = A_(e, A.rx, -1), ry = A_(e, A.ry, -1);
      if (rx < 0) rx = ry;
      if (rx < 0) rx = 0;
      if (w <= 0 || h <= 0) return;
      if (rx > 0) {
        float r[4] = {rx, rx, rx, rx};
        p.roundRect(x, y, w, h, r);
      } else {
        p.rect(x, y, w, h);
      }
      break;
    }
    case TAG_circle: {
      float r = A_(e, A.r);
      if (r <= 0) return;
      p.arc(A_(e, A.cx), A_(e, A.cy), r, 0, 6.2831853f, false);
      p.close();
      break;
    }
    case TAG_ellipse: {
      float rx = A_(e, A.rx), ry = A_(e, A.ry);
      if (rx <= 0 || ry <= 0) return;
      p.ellipse(A_(e, A.cx), A_(e, A.cy), rx, ry, 0, 0, 6.2831853f, false);
      p.close();
      break;
    }
    case TAG_line:
      p.moveTo(A_(e, A.x1), A_(e, A.y1));
      p.lineTo(A_(e, A.x2), A_(e, A.y2));
      st.fill = false;
      break;
    case TAG_polyline: case TAG_polygon: {
      std::string pts = e->attrOr(A.points, "");
      PD pd{pts.data(), pts.size()};
      float x, y;
      bool first = true;
      while (pd.number(x) && pd.number(y)) {
        if (first) p.moveTo(x, y);
        else p.lineTo(x, y);
        first = false;
      }
      if (tag == TAG_polygon) p.close();
      break;
    }
    default:
      shape = false;
  }
  if (shape && !p.empty()) {
    p.transform(st.m);
    SvgShape s;
    s.path = std::move(p);
    s.rule = st.rule;
    s.fill = st.fill;
    s.stroke = st.stroke && st.strokeWidth > 0;
    s.fillColor = st.fillColor.scaleAlpha((int)(st.fillOpacity * 255));
    s.strokeColor = st.strokeColor.scaleAlpha((int)(st.strokeOpacity * 255));
    s.fillCurrent = st.fillCurrent;
    s.strokeCurrent = st.strokeCurrent;
    // stroke width scales with the element transform
    float sc = std::sqrt(std::fabs(st.m.a * st.m.d - st.m.b * st.m.c));
    s.strokeWidth = st.strokeWidth * (sc > 0 ? sc : 1);
    s.cap = st.cap;
    s.join = st.join;
    s.dash = st.dash;
    for (auto &d : s.dash) d *= sc > 0 ? sc : 1;
    s.opacity = st.opacity;
    if (st.fillCurrent) s.fillColor = Color{0, 0, 0, (uint8_t)(st.fillOpacity * 255)};
    if (st.strokeCurrent) s.strokeColor = Color{0, 0, 0, (uint8_t)(st.strokeOpacity * 255)};
    out.shapes.push_back(std::move(s));
  }
  if (tag == TAG_defs || tag == TAG_text) return;  // text/defs not rendered
  for (Node *c = e->first; c; c = c->next)
    if (c->isElement()) walk((Element *)c, st, out);
}
}  // namespace

std::shared_ptr<SvgCache> buildSvg(Element *svg) {
  auto c = std::make_shared<SvgCache>();
  std::string vb = svg->attrOr(A.viewBox, "");
  if (!vb.empty()) {
    PD pd{vb.data(), vb.size()};
    float v[4];
    if (pd.number(v[0]) && pd.number(v[1]) && pd.number(v[2]) && pd.number(v[3]) && v[2] > 0 && v[3] > 0) {
      c->vbX = v[0];
      c->vbY = v[1];
      c->vbW = v[2];
      c->vbH = v[3];
      c->hasViewBox = true;
    }
  }
  std::string par = toLower(svg->attrOr(A.preserveAspectRatio, "xmidymid meet"));
  if (startsWith(par, "none")) c->align = 0;
  else {
    int ax = par.find("xmin") != std::string::npos ? 0 : par.find("xmax") != std::string::npos ? 2 : 1;
    int ay = par.find("ymin") != std::string::npos ? 0 : par.find("ymax") != std::string::npos ? 2 : 1;
    c->align = 1 + ay * 3 + ax;
    c->slice = par.find("slice") != std::string::npos;
  }
  SvgState st;
  walk(svg, st, *c);
  return c;
}

static void rebuildSvgScreen(SvgCache &c, const Affine &M) {
  __atomic_store_n(&c.ready, false, __ATOMIC_RELEASE);
  c.fills.assign(c.shapes.size(), Path());
  c.strokes.assign(c.shapes.size(), Path());
  float ms = std::sqrt(std::fabs(M.a * M.d - M.b * M.c));
  for (size_t i = 0; i < c.shapes.size(); i++) {
    const SvgShape &s = c.shapes[i];
    if (s.fill) {
      c.fills[i] = s.path;
      c.fills[i].transform(M);
    }
    if (s.stroke) {
      Path screen = s.path;
      screen.transform(M);
      StrokeStyle ss;
      ss.width = s.strokeWidth * ms;
      ss.cap = s.cap;
      ss.join = s.join;
      ss.miter = 4;  // SVG's default stroke-miterlimit
      ss.dash = s.dash;
      for (auto &d : ss.dash) d *= ms;
      strokePath(screen, ss, c.strokes[i]);
    }
  }
  c.key = M;
  __atomic_store_n(&c.ready, true, __ATOMIC_RELEASE);
}

void paintSvg(Element *svg, SvgCache &c, const PaintCtx &pc, const RectF &box, Color current) {
  (void)svg;
  float vbW = c.hasViewBox ? c.vbW : box.w, vbH = c.hasViewBox ? c.vbH : box.h;
  float sx = box.w / vbW, sy = box.h / vbH;
  float tx = box.x, ty = box.y;
  if (c.align != 0) {
    float s = c.slice ? std::max(sx, sy) : std::min(sx, sy);
    int ax = (c.align - 1) % 3, ay = (c.align - 1) / 3;
    tx += (box.w - vbW * s) * ax / 2;
    ty += (box.h - vbH * s) * ay / 2;
    sx = sy = s;
  }
  Affine docToScreen = pc.xf ? *pc.xf : Affine::translate(pc.dx, pc.dy);
  Affine M = docToScreen * Affine::translate(tx, ty) * Affine::scale(sx, sy) * Affine::translate(-c.vbX, -c.vbY);
  // screen paths are rebuilt when the transform changes; bands may be painted on two
  // cores, so the rebuild is locked and the key is published last
  auto stale = [&] {
    return !__atomic_load_n(&c.ready, __ATOMIC_ACQUIRE) || memcmp(&M, &c.key, sizeof(Affine)) != 0 ||
           c.fills.size() != c.shapes.size();
  };
  if (stale()) {
    PaintLock lock;
    if (stale()) rebuildSvgScreen(c, M);
  }
  RectI clip = pc.clip.intersect(docToScreen.mapRect(box).outer());
  if (clip.empty()) return;
  RasterTarget t{pc.band, clip};
  for (size_t i = 0; i < c.shapes.size(); i++) {
    const SvgShape &s = c.shapes[i];
    int a = (int)(s.opacity * pc.alpha);
    if (s.fill && !c.fills[i].empty()) {
      Paint p;
      Color col = s.fillCurrent ? current.withAlpha((uint8_t)(current.a * s.fillColor.a / 255)) : s.fillColor;
      p.color = col.scaleAlpha(a);
      fillPath(t, c.fills[i], s.rule, p);
    }
    if (s.stroke && !c.strokes[i].empty()) {
      Paint p;
      Color col = s.strokeCurrent ? current.withAlpha((uint8_t)(current.a * s.strokeColor.a / 255)) : s.strokeColor;
      p.color = col.scaleAlpha(a);
      fillPath(t, c.strokes[i], FR_NONZERO, p);
    }
  }
}

}  // namespace rt
