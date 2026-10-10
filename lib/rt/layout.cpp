// Layout engine.
#include "layout.h"

#include "extra.h"
#include "thai.h"

namespace rt {

// ============================================================ helpers

static inline const ComputedStyle &S(Element *e) { return *e->cs; }

static bool isDisplayed(Node *n) {
  if (!n->isElement()) return true;
  Element *e = (Element *)n;
  return e->cs && e->cs->display != D_NONE;
}

static bool isOutOfFlow(Element *e) { return e->cs && e->cs->isOutOfFlow(); }

static float collapseMargins(float a, float b) {
  float pos = std::max(std::max(a, b), 0.f);
  float neg = std::min(std::min(a, b), 0.f);
  return pos + neg;
}

bool LayoutEngine::isReplaced(Element *e) {
  switch (e->tagId()) {
    case TAG_img: case TAG_canvas: case TAG_svg: case TAG_input: case TAG_select: case TAG_textarea:
    case TAG_progress: case TAG_meter: case TAG_video: case TAG_iframe:
      return true;
    case TAG_button:
      return false;
    default:
      return false;
  }
}

bool LayoutEngine::isAtomicInline(Element *e) {
  if (!e->cs) return false;
  Display d = e->cs->display;
  if (d == D_INLINE_BLOCK || d == D_INLINE_FLEX || d == D_INLINE_GRID || d == D_INLINE_TABLE) return true;
  if (d == D_INLINE && (isReplaced(e) || e->tagId() == TAG_button)) return true;
  return false;
}

static bool isBlockLevelNode(Node *n) {
  if (!n->isElement()) return false;
  Element *e = (Element *)n;
  if (!e->cs) return false;
  if (e->cs->isOutOfFlow()) return false;
  return e->cs->isBlockLevel();
}

float LayoutEngine::normalLineHeight(const FontRef &f) {
  if (!f.face) return 16 * 1.2f;
  // Chromium rounds ascent and descent separately.
  return std::round(f.ascent()) + std::round(f.descent()) + std::round(f.lineGap());
}

FontRef LayoutEngine::fontFor(const ComputedStyle &cs) const {
  if (!fonts) return FontRef();
  // Memo per thread (layout, and painting on two cores). Entries hold a reference to the
  // family list, so its address cannot be reused by another list while cached.
  struct Entry {
    std::shared_ptr<std::vector<std::string>> fam;
    const FontManager *mgr = nullptr;
    uint32_t gen = 0;
    int weight = 0;
    bool italic = false;
    float size = 0;
    FontRef ref;
  };
  static thread_local Entry *memo = nullptr;
  if (!memo) memo = new Entry[64];
  uintptr_t h = (uintptr_t)cs.fontFamily.get() >> 4;
  h ^= (uintptr_t)cs.fontWeight * 31u ^ (uintptr_t)(cs.fontSize * 8) * 131u ^ (cs.italic ? 7u : 0u);
  Entry &e = memo[(h ^ (h >> 6)) & 63];
  if (e.mgr == fonts && e.gen == fonts->generation() && e.fam == cs.fontFamily && e.weight == cs.fontWeight &&
      e.italic == cs.italic && e.size == cs.fontSize)
    return e.ref;
  FontRef r = fonts->get(cs.fontFamily.get(), cs.fontWeight, cs.italic, cs.fontSize);
  e.fam = cs.fontFamily;
  e.mgr = fonts;
  e.gen = fonts->generation();
  e.weight = cs.fontWeight;
  e.italic = cs.italic;
  e.size = cs.fontSize;
  e.ref = r;
  return r;
}

Element *containingBlockFor(Element *e) {
  if (!e->cs) return nullptr;
  if (e->cs->position == POS_FIXED) return nullptr;  // viewport
  for (Element *p = e->parentElement(); p; p = p->parentElement()) {
    if (!p->cs) continue;
    if (p->cs->position != POS_STATIC || p->cs->hasTransform()) return p;
    if (p->tag == TAG_html) return nullptr;
  }
  return nullptr;
}

static Box &ensureBox(Element *e) {
  if (!e->box) e->box = new Box();
  return *e->box;
}

static float borderPaddingH(const Box &b) { return b.b[1] + b.b[3] + b.p[1] + b.p[3]; }
static float borderPaddingV(const Box &b) { return b.b[0] + b.b[2] + b.p[0] + b.p[2]; }

static void resolveEdges(Element *e, float cbW) {
  Box &b = *e->box;
  const ComputedStyle &s = S(e);
  b.b[0] = s.borderTop();
  b.b[1] = s.borderRight();
  b.b[2] = s.borderBottom();
  b.b[3] = s.borderLeft();
  for (int i = 0; i < 4; i++) {
    b.p[i] = std::max(0.f, s.padding[i].resolve(cbW));
    b.m[i] = s.margin[i].isAuto() ? 0 : s.margin[i].resolve(cbW);
  }
}

// Is e a block formatting context root (margins don't collapse through it)?
static bool isBFC(Element *e) {
  const ComputedStyle &s = S(e);
  if (e->tag == TAG_html) return true;
  if (s.clips()) return true;
  if (s.display != D_BLOCK && s.display != D_LIST_ITEM) return true;
  if (s.isOutOfFlow()) return true;
  Element *p = e->parentElement();
  if (p && p->cs && (p->cs->isFlex() || p->cs->isGrid())) return true;
  return false;
}

// ============================================================ intrinsic sizes

float LayoutEngine::inlineItemsWidth(Element *container, bool minMode) {
  // Shape the inline content without layout to find min/max content widths.
  float lineW = 0, best = 0, word = 0;
  bool prevSpace = true;
  std::function<void(Node *)> walk = [&](Node *n) {
    if (Text *t = n->asText()) {
      Element *owner = t->parent ? t->parent : container;
      if (!owner->cs) return;
      InlineLayout tmp;
      bool ps = prevSpace;
      addTextItem(tmp, owner, t, ps);
      prevSpace = ps;
      if (tmp.items.empty()) return;
      const InlineItem &it = tmp.items[0];
      bool nowrap = owner->cs->whiteSpace == WS_NOWRAP || owner->cs->whiteSpace == WS_PRE;
      bool breakAll = owner->cs->wordBreak == WB_BREAK_ALL || owner->cs->overflowWrap == OW_ANYWHERE;
      for (auto &g : it.glyphs) {
        if (g.flags & SG_HARD_BREAK) {
          best = std::max(best, minMode ? word : lineW);
          lineW = 0;
          word = 0;
          continue;
        }
        lineW += g.adv;
        if (!(g.flags & SG_SPACE)) word += g.adv;
        bool brk = !nowrap && ((g.flags & SG_BREAK_AFTER) || breakAll);
        if (brk) {
          best = std::max(best, word);
          word = 0;
        }
      }
      if (!minMode) best = std::max(best, lineW);
      return;
    }
    Element *e = n->asElement();
    if (!e || !isDisplayed(e) || isOutOfFlow(e)) return;
    if (e->tagId() == TAG_br) {
      best = std::max(best, minMode ? word : lineW);
      lineW = 0;
      word = 0;
      prevSpace = true;
      return;
    }
    if (isAtomicInline(e) || e->cs->isBlockLevel()) {
      float w = minMode ? minContentWidth(e) : maxContentWidth(e);
      const ComputedStyle &s = S(e);
      float m = (s.margin[1].isAuto() ? 0 : s.margin[1].px) + (s.margin[3].isAuto() ? 0 : s.margin[3].px);
      if (e->cs->isBlockLevel()) {
        best = std::max(best, minMode ? std::max(word, lineW) : lineW);
        best = std::max(best, w + m);
        lineW = 0;
        word = 0;
        prevSpace = true;
        return;
      }
      if (minMode) {
        best = std::max(best, word);
        word = 0;
        best = std::max(best, w + m);
      }
      lineW += w + m;
      prevSpace = false;
      return;
    }
    // inline element: edges + children
    const ComputedStyle &s = S(e);
    float edgeL = s.borderLeft() + s.padding[3].px + (s.margin[3].isAuto() ? 0 : s.margin[3].px);
    float edgeR = s.borderRight() + s.padding[1].px + (s.margin[1].isAuto() ? 0 : s.margin[1].px);
    lineW += edgeL;
    word += edgeL;
    forEachLayoutChild(e, walk);
    lineW += edgeR;
    word += edgeR;
  };
  forEachLayoutChild(container, walk);
  best = std::max(best, minMode ? word : lineW);
  if (minMode) best = std::max(best, word);
  return best;
}

static float specifiedWidth(Element *e, float &out) {
  const ComputedStyle &s = S(e);
  if (s.width.isLen() && !s.width.hasPct()) {
    float bp = s.borderLeft() + s.borderRight() + s.padding[1].px + s.padding[3].px;
    // a border box is never narrower than its padding and border (layout makes it so too)
    out = s.boxSizing == BOX_CONTENT ? s.width.px + bp : std::max(s.width.px, bp);
    return true;
  }
  return false;
}

float LayoutEngine::minContentWidth(Element *e) {
  Box &b = ensureBox(e);
  if (b.layoutGen == gen_ && b.minW >= 0) return b.minW;
  float r = maxContentWidth(e);  // fills b.maxW (and sets layoutGen)
  (void)r;
  float w;
  const ComputedStyle &s = S(e);
  float bp = s.borderLeft() + s.borderRight() + s.padding[1].px + s.padding[3].px;
  if (specifiedWidth(e, w)) {
    b.minW = w;
  } else if (isReplaced(e)) {
    b.minW = b.maxW;
  } else if (s.isFlex() && (s.flexDirection == FD_ROW || s.flexDirection == FD_ROW_REVERSE)) {
    float sum = 0, mx = 0;
    int n = 0;
    forEachLayoutChild(e, [&](Node *c) {
      Element *ce = c->asElement();
      if (!ce || !isDisplayed(ce) || isOutOfFlow(ce)) return;
      float cw = minContentWidth(ce) + (ce->cs->margin[1].isAuto() ? 0 : ce->cs->margin[1].px) +
                 (ce->cs->margin[3].isAuto() ? 0 : ce->cs->margin[3].px);
      sum += cw;
      mx = std::max(mx, cw);
      n++;
    });
    float gap = s.columnGap.isLen() ? s.columnGap.px : 0;
    b.minW = (s.flexWrap == FW_NOWRAP ? sum + gap * std::max(0, n - 1) : mx) + bp;
  } else if (s.isFlex() || s.isGrid() || s.display == D_TABLE || s.display == D_INLINE_TABLE) {
    float mx = 0;
    forEachLayoutChild(e, [&](Node *c) {
      Element *ce = c->asElement();
      if (!ce || !isDisplayed(ce) || isOutOfFlow(ce)) return;
      mx = std::max(mx, minContentWidth(ce));
    });
    b.minW = mx + bp;
  } else {
    b.minW = inlineItemsWidth(e, true) + bp;
  }
  if (s.minWidth.isLen() && !s.minWidth.hasPct()) b.minW = std::max(b.minW, s.minWidth.px + (s.boxSizing == BOX_CONTENT ? bp : 0));
  if (s.maxWidth.isLen() && !s.maxWidth.hasPct()) b.minW = std::min(b.minW, s.maxWidth.px + (s.boxSizing == BOX_CONTENT ? bp : 0));
  return b.minW;
}

float LayoutEngine::maxContentWidth(Element *e) {
  Box &b = ensureBox(e);
  if (b.layoutGen == gen_ && b.maxW >= 0) return b.maxW;
  b.layoutGen = gen_;
  b.minW = -1;
  const ComputedStyle &s = S(e);
  float bp = s.borderLeft() + s.borderRight() + s.padding[1].px + s.padding[3].px;
  float w;
  if (specifiedWidth(e, w)) {
    b.maxW = w;
    return w;
  }
  if (isReplaced(e)) {
    float iw = 0, ih = 0;
    if (host) host->intrinsicSize(e, iw, ih);
    if (s.height.isLen() && !s.height.hasPct() && ih > 0 && iw > 0 && e->tagId() != TAG_input &&
        e->tagId() != TAG_select && e->tagId() != TAG_textarea) {
      float hh = s.height.px - (s.boxSizing == BOX_BORDER ? borderPaddingV(b) : 0);
      iw = hh * iw / ih;
    }
    b.maxW = iw + bp;
    return b.maxW;
  }
  float r = 0;
  if (s.isFlex() && (s.flexDirection == FD_ROW || s.flexDirection == FD_ROW_REVERSE)) {
    int n = 0;
    forEachLayoutChild(e, [&](Node *c) {
      Element *ce = c->asElement();
      if (!ce) {
        Text *t = c->asText();
        if (t) {
          InlineLayout tmp;
          bool ps = true;
          addTextItem(tmp, e, t, ps);
          if (!tmp.items.empty() && tmp.items[0].width > 0) {
            r += tmp.items[0].width;
            n++;
          }
        }
        return;
      }
      if (!isDisplayed(ce) || isOutOfFlow(ce)) return;
      r += maxContentWidth(ce) + (ce->cs->margin[1].isAuto() ? 0 : ce->cs->margin[1].px) +
           (ce->cs->margin[3].isAuto() ? 0 : ce->cs->margin[3].px);
      n++;
    });
    float gap = s.columnGap.isLen() ? s.columnGap.px : 0;
    r += gap * std::max(0, n - 1);
  } else if (s.isGrid() && s.gridCols && !s.gridCols->tracks.empty() && !s.gridCols->autoRepeat) {
    // sum of max-content per column (approximation: items in column order)
    size_t nc = s.gridCols->tracks.size();
    std::vector<float> col(nc, 0);
    size_t i = 0;
    forEachLayoutChild(e, [&](Node *c) {
      Element *ce = c->asElement();
      if (!ce || !isDisplayed(ce) || isOutOfFlow(ce)) return;
      const TrackSize &t = s.gridCols->tracks[i % nc];
      float cw = t.kind == TrackSize::FIXED && !t.len.pct ? t.len.px : maxContentWidth(ce);
      col[i % nc] = std::max(col[i % nc], cw);
      i++;
    });
    for (float c : col) r += c;
    float gap = s.columnGap.isLen() ? s.columnGap.px : 0;
    r += gap * (nc - 1);
  } else if (s.isFlex() || s.isGrid()) {
    forEachLayoutChild(e, [&](Node *c) {
      Element *ce = c->asElement();
      if (!ce || !isDisplayed(ce) || isOutOfFlow(ce)) return;
      r = std::max(r, maxContentWidth(ce) + (ce->cs->margin[1].isAuto() ? 0 : ce->cs->margin[1].px) +
                          (ce->cs->margin[3].isAuto() ? 0 : ce->cs->margin[3].px));
    });
  } else if (s.display == D_TABLE || s.display == D_INLINE_TABLE) {
    // sum of cell max widths per row, max over rows
    std::function<void(Element *)> rows = [&](Element *g) {
      forEachLayoutChild(g, [&](Node *c) {
        Element *ce = c->asElement();
        if (!ce || !isDisplayed(ce)) return;
        Display d = ce->cs->display;
        if (d == D_TABLE_ROW) {
          float sum = 0;
          int n = 0;
          forEachLayoutChild(ce, [&](Node *cc) {
            Element *cell = cc->asElement();
            if (!cell || !isDisplayed(cell)) return;
            sum += maxContentWidth(cell);
            n++;
          });
          sum += s.borderCollapse ? 0 : s.borderSpacingX * (n + 1);
          r = std::max(r, sum);
        } else if (d == D_TABLE_ROW_GROUP || d == D_TABLE_HEADER_GROUP || d == D_TABLE_FOOTER_GROUP) {
          rows(ce);
        }
      });
    };
    rows(e);
    b.maxW = r + (s.borderLeft() + s.borderRight());
    return b.maxW;
  } else {
    r = inlineItemsWidth(e, false);
  }
  b.maxW = r + bp;
  if (s.minWidth.isLen() && !s.minWidth.hasPct()) b.maxW = std::max(b.maxW, s.minWidth.px + (s.boxSizing == BOX_CONTENT ? bp : 0));
  if (s.maxWidth.isLen() && !s.maxWidth.hasPct()) b.maxW = std::min(b.maxW, s.maxWidth.px + (s.boxSizing == BOX_CONTENT ? bp : 0));
  return b.maxW;
}

// ============================================================ box sizing

void LayoutEngine::computeHorizontal(Element *e, const CB &cb, float availW, bool shrink, float forcedW) {
  Box &b = *e->box;
  const ComputedStyle &s = S(e);
  float bp = borderPaddingH(b);
  float w;
  bool autoW = false;
  if (forcedW >= 0) {
    w = forcedW;
  } else if (s.width.isLen() && (!s.width.hasPct() || cb.w >= 0)) {
    w = s.width.resolve(cb.w);
    if (s.boxSizing == BOX_CONTENT) w += bp;
  } else if (s.width.kind == LK_MIN_CONTENT) {
    w = minContentWidth(e);
  } else if (s.width.kind == LK_MAX_CONTENT) {
    w = maxContentWidth(e);
  } else {
    autoW = true;
    float avail = availW - b.m[1] - b.m[3];
    if (isReplaced(e)) {
      float iw = 0, ih = 0;
      if (host) host->intrinsicSize(e, iw, ih);
      if (s.height.isLen() && (!s.height.hasPct() || cb.h >= 0) && iw > 0 && ih > 0 && e->tagId() != TAG_input &&
          e->tagId() != TAG_select && e->tagId() != TAG_textarea) {
        float hh = s.height.resolve(cb.h);
        if (s.boxSizing == BOX_BORDER) hh -= borderPaddingV(b);
        iw = hh * iw / ih;
      } else if (s.aspectRatio > 0 && s.height.isLen() && (!s.height.hasPct() || cb.h >= 0)) {
        float hh = s.height.resolve(cb.h);
        if (s.boxSizing == BOX_BORDER) hh -= borderPaddingV(b);
        iw = hh * s.aspectRatio;
      }
      w = iw + bp;
    } else if (shrink || s.width.kind == LK_FIT_CONTENT) {
      float mn = minContentWidth(e), mx = maxContentWidth(e);
      w = std::min(std::max(mn, avail), mx);
    } else if (s.aspectRatio > 0 && s.height.isLen() && (!s.height.hasPct() || cb.h >= 0)) {
      float hh = s.height.resolve(cb.h);
      w = (s.boxSizing == BOX_BORDER ? hh : hh + borderPaddingV(b)) * s.aspectRatio;
      if (s.boxSizing == BOX_CONTENT) w = hh * s.aspectRatio + bp;
    } else {
      w = avail;
    }
  }
  // min/max
  float maxW = 1e9f, minW = 0;
  if (s.maxWidth.isLen() && (!s.maxWidth.hasPct() || cb.w >= 0)) maxW = s.maxWidth.resolve(cb.w) + (s.boxSizing == BOX_CONTENT ? bp : 0);
  if (s.minWidth.isLen() && (!s.minWidth.hasPct() || cb.w >= 0)) minW = s.minWidth.resolve(cb.w) + (s.boxSizing == BOX_CONTENT ? bp : 0);
  else if (s.minWidth.kind == LK_MIN_CONTENT) minW = minContentWidth(e);
  if (forcedW < 0) w = std::max(minW, std::min(w, maxW));
  w = std::max(w, bp);
  b.w = w;
  // auto margins (block-level, in flow)
  if (!shrink && forcedW < 0 && !s.isOutOfFlow()) {
    bool la = s.margin[3].isAuto(), ra = s.margin[1].isAuto();
    if ((la || ra) && !(autoW && !isReplaced(e) && w >= availW - b.m[1] - b.m[3] - 0.01f)) {
      float rem = availW - w - (la ? 0 : b.m[3]) - (ra ? 0 : b.m[1]);
      if (la && ra) {
        b.m[3] = b.m[1] = std::max(0.f, rem / 2);
      } else if (la) {
        b.m[3] = std::max(0.f, rem);
      } else {
        b.m[1] = rem;
      }
    }
    if (s.textAlign == TA_CENTER && false) {}
  }
}

void LayoutEngine::layoutReplaced(Element *e, float contentW, float contentH) {
  (void)e;
  (void)contentW;
  (void)contentH;
}

void LayoutEngine::layoutBox(Element *e, const CB &cb, float availW, bool shrink, float forcedW, float forcedH) {
  Box &b = ensureBox(e);
  const ComputedStyle &s = S(e);
  depth_++;
  b.inCbW = cb.w;
  b.inCbH = cb.h;
  b.inAvailW = availW;
  b.inShrink = shrink;
  b.inForcedW = forcedW;
  b.inForcedH = forcedH;
  b.inValid = true;
  b.inlines.clear();
  b.inlineRects.clear();
  b.baseline = b.lastBaseline = -1;
  b.relX = b.relY = 0;
  b.inFlowInline = false;
  b.fixed = false;
  b.originPadding = false;
  resolveEdges(e, cb.w < 0 ? 0 : cb.w);
  computeHorizontal(e, cb, availW, shrink, forcedW);
  float contentW = b.contentW();

  // definite content height?
  float contentH = -1;
  float bpv = borderPaddingV(b);
  if (forcedH >= 0) {
    contentH = std::max(0.f, forcedH - bpv);
  } else if (s.height.isLen() && (!s.height.hasPct() || cb.h >= 0)) {
    float hh = s.height.resolve(cb.h);
    contentH = s.boxSizing == BOX_BORDER ? std::max(0.f, hh - bpv) : hh;
  } else if (s.aspectRatio > 0 && !isReplaced(e) && s.height.isAuto()) {
    contentH = (s.boxSizing == BOX_BORDER ? b.w / s.aspectRatio - bpv : contentW / s.aspectRatio);
  }
  if (contentH >= 0) {
    float maxH = 1e9f, minH = 0;
    if (s.maxHeight.isLen() && (!s.maxHeight.hasPct() || cb.h >= 0)) maxH = s.maxHeight.resolve(cb.h) - (s.boxSizing == BOX_BORDER ? bpv : 0);
    if (s.minHeight.isLen() && (!s.minHeight.hasPct() || cb.h >= 0)) minH = s.minHeight.resolve(cb.h) - (s.boxSizing == BOX_BORDER ? bpv : 0);
    if (forcedH < 0) contentH = std::max(minH, std::min(contentH, maxH));
  }

  float usedH = 0;
  if (isReplaced(e)) {
    float iw = 0, ih = 0;
    if (host) host->intrinsicSize(e, iw, ih);
    if (contentH >= 0) usedH = contentH;
    else if (s.aspectRatio > 0) usedH = contentW / s.aspectRatio;
    else if (iw > 0 && ih > 0 && e->tagId() != TAG_input && e->tagId() != TAG_select && e->tagId() != TAG_textarea &&
             !(s.width.isAuto() && forcedW < 0))
      usedH = contentW * ih / iw;
    else usedH = ih;
    // baseline for text controls: the text line
    Tag t = e->tagId();
    if (t == TAG_input || t == TAG_select || t == TAG_button || t == TAG_textarea) {
      FontRef f = fontFor(s);
      float lh = s.usedLineHeight(normalLineHeight(f));
      float top = b.b[0] + b.p[0];
      if (t != TAG_textarea) top += std::max(0.f, (usedH - lh) / 2);
      float half = (lh - (std::round(f.ascent()) + std::round(f.descent()))) / 2;
      b.baseline = b.lastBaseline = top + half + std::round(f.ascent());
    }
  } else if (s.isFlex()) {
    // an auto height still has its min/max: a single-line row container sizes its line by them
    float minH = 0, maxH = 1e9f;
    if (contentH < 0) {
      if (s.maxHeight.isLen() && (!s.maxHeight.hasPct() || cb.h >= 0)) maxH = s.maxHeight.resolve(cb.h) - (s.boxSizing == BOX_BORDER ? bpv : 0);
      if (s.minHeight.isLen() && (!s.minHeight.hasPct() || cb.h >= 0)) minH = s.minHeight.resolve(cb.h) - (s.boxSizing == BOX_BORDER ? bpv : 0);
    }
    usedH = layoutFlex(e, contentW, contentH, minH, maxH);
  } else if (s.isGrid()) {
    usedH = layoutGrid(e, contentW, contentH);
  } else if (s.display == D_TABLE || s.display == D_INLINE_TABLE) {
    usedH = layoutTable(e, contentW, contentH);
  } else {
    MarginInfo mi;
    usedH = layoutBlockContents(e, contentW, contentH, mi);
    // margins escaping through our top/bottom are folded into ours
    if (mi.topThrough) b.m[0] = collapseMargins(b.m[0], mi.top);
    if (mi.bottomThrough && contentH < 0) b.m[2] = collapseMargins(b.m[2], mi.bottom);
  }
  if (contentH < 0) {
    contentH = usedH;
    float maxH = 1e9f, minH = 0;
    if (s.maxHeight.isLen() && (!s.maxHeight.hasPct() || cb.h >= 0)) maxH = s.maxHeight.resolve(cb.h) - (s.boxSizing == BOX_BORDER ? bpv : 0);
    if (s.minHeight.isLen() && (!s.minHeight.hasPct() || cb.h >= 0)) minH = s.minHeight.resolve(cb.h) - (s.boxSizing == BOX_BORDER ? bpv : 0);
    contentH = std::max(minH, std::min(contentH, maxH));
  }
  b.h = contentH + bpv;
  // scrollable overflow default (content may extend it)
  b.scrollW = std::max(b.scrollW, b.w - b.b[1] - b.b[3]);
  b.scrollH = std::max(b.scrollH, b.h - b.b[0] - b.b[2]);
  // relative positioning
  if (s.position == POS_RELATIVE || s.position == POS_STICKY) {
    if (!s.left.isAuto()) b.relX = s.left.resolve(cb.w);
    else if (!s.right.isAuto()) b.relX = -s.right.resolve(cb.w);
    if (!s.top.isAuto()) b.relY = s.top.resolve(cb.h >= 0 ? cb.h : 0);
    else if (!s.bottom.isAuto()) b.relY = -s.bottom.resolve(cb.h >= 0 ? cb.h : 0);
    if (s.position == POS_STICKY) b.relX = b.relY = 0;
  }
  // out-of-flow descendants whose containing block is this element
  if (s.position != POS_STATIC || s.hasTransform()) finishOutOfFlow(e);
  depth_--;
}

// ============================================================ block flow

float LayoutEngine::layoutBlockContents(Element *e, float contentW, float contentH, MarginInfo &mi) {
  Box &b = *e->box;
  const ComputedStyle &s = S(e);
  bool bfc = isBFC(e);
  bool canCollapseTop = !bfc && b.b[0] == 0 && b.p[0] == 0;
  bool canCollapseBottom = !bfc && b.b[2] == 0 && b.p[2] == 0 && s.height.isAuto() && contentH < 0;
  float x0 = b.contentX(), y0 = b.contentY();
  float y = 0;
  float pending = 0;  // margin waiting to collapse
  bool first = true;
  bool hasContent = false;
  float scrollW = 0;
  std::vector<Node *> run;
  CB childCb{contentW, contentH};

  auto flushRun = [&]() {
    if (run.empty()) return;
    auto il = std::make_unique<InlineLayout>();
    float yy = y + (first && canCollapseTop ? 0 : pending);
    float h = layoutInlineContents(e, *il, run, contentW, y0 + yy);
    run.clear();
    if (il->lines.empty()) {
      // only collapsible whitespace (but keep out-of-flow static positions)
      if (!il->items.empty()) b.inlines.push_back(std::move(il));
      return;
    }
    if (!(first && canCollapseTop)) y += pending;
    else if (first) mi.top = pending, mi.topThrough = canCollapseTop && pending != 0;
    pending = 0;
    il->y = y0 + y;
    il->h = h;
    scrollW = std::max(scrollW, il->maxRight - x0);
    if (b.baseline < 0 && !il->lines.empty()) b.baseline = il->lines.front().baseline;
    if (!il->lines.empty()) b.lastBaseline = il->lines.back().baseline;
    y += h;
    first = false;
    hasContent = true;
    b.inlines.push_back(std::move(il));
  };

  forEachLayoutChild(e, [&](Node *c) {
    if (!isDisplayed(c)) return;
    Element *ce = c->asElement();
    if (ce && isOutOfFlow(ce)) {
      if (!run.empty()) {
        run.push_back(c);  // static position within the inline run
        return;
      }
      Element *cbEl = containingBlockFor(ce);
      pendingAbs_.push_back({ce, cbEl, x0, y0 + y + pending});
      ensureBox(ce).origin = e;  // provisional: static position is relative to e
      return;
    }
    if (!isBlockLevelNode(c)) {
      run.push_back(c);
      return;
    }
    flushRun();
    // block-level child
    layoutBox(ce, childCb, contentW, false);
    Box &cbx = *ce->box;
    cbx.origin = e;
    float mt = cbx.m[0], mb = cbx.m[2];
    bool empty = cbx.h == 0 && cbx.inlines.empty() && ce->first == nullptr && !isReplaced(ce) &&
                 !ce->cs->isFlex() && !ce->cs->isGrid();
    if (empty) {
      pending = collapseMargins(collapseMargins(pending, mt), mb);
      cbx.x = x0 + cbx.m[3];
      cbx.y = y0 + y;
      return;
    }
    float top;
    if (first && canCollapseTop) {
      mi.top = collapseMargins(pending, mt);
      mi.topThrough = true;
      top = 0;
    } else {
      top = collapseMargins(pending, mt);
    }
    y += top;
    cbx.x = x0 + cbx.m[3] + cbx.relX;
    cbx.y = y0 + y + cbx.relY;
    if (b.baseline < 0 && cbx.baseline >= 0) b.baseline = (y0 + y) + cbx.baseline;
    if (cbx.lastBaseline >= 0) b.lastBaseline = (y0 + y) + cbx.lastBaseline;
    y += cbx.h;
    pending = mb;
    first = false;
    hasContent = true;
    scrollW = std::max(scrollW, cbx.m[3] + cbx.w + std::max(0.f, cbx.m[1]));
  });
  flushRun();
  if (canCollapseBottom && hasContent) {
    mi.bottom = pending;
    mi.bottomThrough = true;
  } else if (hasContent) {
    y += pending;
  } else if (canCollapseTop && canCollapseBottom) {
    mi.top = pending;
    mi.topThrough = pending != 0;
  }
  // list marker ordinal
  b.scrollW = std::max(contentW + b.p[1] + b.p[3], scrollW + b.p[3] + b.p[1]);
  b.scrollH = y + b.p[0] + b.p[2];
  return y;
}

// ============================================================ inline formatting

void LayoutEngine::addTextItem(InlineLayout &il, Element *owner, Text *t, bool &prevSpace) {
  const ComputedStyle &cs = S(owner);
  const std::string &src = t->data;
  std::string s;
  s.reserve(src.size());
  WhiteSpace ws = cs.whiteSpace;
  bool collapse = ws == WS_NORMAL || ws == WS_NOWRAP || ws == WS_PRE_LINE;
  bool keepNewlines = ws == WS_PRE || ws == WS_PRE_WRAP || ws == WS_PRE_LINE || ws == WS_BREAK_SPACES;
  for (size_t i = 0; i < src.size(); i++) {
    char c = src[i];
    if (c == '\r') continue;
    if (c == '\n' && keepNewlines) {
      if (ws == WS_PRE_LINE)
        while (!s.empty() && s.back() == ' ') s.pop_back();
      s += '\n';
      prevSpace = true;
      continue;
    }
    if (collapse && (c == ' ' || c == '\t' || c == '\n' || c == '\f')) {
      if (!prevSpace) s += ' ';
      prevSpace = true;
      continue;
    }
    if (c == '\t' && !collapse) {
      s += "    ";
      prevSpace = false;
      continue;
    }
    s += c;
    prevSpace = false;
  }
  if (s.empty()) return;
  thaiSegment(s);
  if (cs.textTransform != TT_NONE) {
    bool startWord = true;
    for (auto &c : s) {
      if (cs.textTransform == TT_UPPER) c = (char)toupper((unsigned char)c);
      else if (cs.textTransform == TT_LOWER) c = (char)tolower((unsigned char)c);
      else if (cs.textTransform == TT_CAPITALIZE) {
        if (startWord && isalpha((unsigned char)c)) c = (char)toupper((unsigned char)c);
        startWord = c == ' ' || c == '-';
      }
    }
  }
  InlineItem it;
  it.kind = InlineItem::TEXT;
  it.el = owner;
  it.text = t;
  it.font = fontFor(cs);
  ShapeOpts o;
  o.letterSpacing = cs.letterSpacing;
  o.wordSpacing = cs.wordSpacing;
  o.tabular = cs.tabularNums;
  it.width = shapeText(it.font, s.data(), s.size(), o, it.glyphs);
  for (auto &g : it.glyphs)
    if (g.flags & SG_FALLBACK) it.fallback = true;
  il.items.push_back(std::move(it));
}

void LayoutEngine::collectInlineItems(Element *container, InlineLayout &il, Node *n, bool &prevSpace, float contentW) {
  if (Text *t = n->asText()) {
    Element *owner = t->parent && t->parent->cs ? t->parent : container;
    addTextItem(il, owner, t, prevSpace);
    return;
  }
  Element *e = n->asElement();
  if (!e || !isDisplayed(e)) return;
  if (isOutOfFlow(e)) {
    InlineItem it;
    it.kind = InlineItem::OUT_OF_FLOW;
    it.el = e;
    il.items.push_back(std::move(it));
    return;
  }
  if (e->tagId() == TAG_br) {
    InlineItem it;
    it.kind = InlineItem::BREAK;
    it.el = e;
    il.items.push_back(std::move(it));
    prevSpace = true;
    return;
  }
  if (isAtomicInline(e) || e->cs->isBlockLevel()) {
    layoutBox(e, CB{contentW, -1}, contentW, true);
    InlineItem it;
    it.kind = InlineItem::ATOMIC;
    it.el = e;
    it.width = e->box->w + e->box->m[1] + e->box->m[3];
    il.items.push_back(std::move(it));
    prevSpace = false;
    return;
  }
  // inline box
  Box &b = ensureBox(e);
  b.inlines.clear();
  b.inlineRects.clear();
  resolveEdges(e, contentW);
  b.origin = container;
  InlineItem open;
  open.kind = InlineItem::OPEN;
  open.el = e;
  open.width = b.m[3] + b.b[3] + b.p[3];
  open.font = fontFor(S(e));
  il.items.push_back(std::move(open));
  forEachLayoutChild(e, [&](Node *c) { collectInlineItems(container, il, c, prevSpace, contentW); });
  InlineItem close;
  close.kind = InlineItem::CLOSE;
  close.el = e;
  close.width = b.m[1] + b.b[1] + b.p[1];
  il.items.push_back(std::move(close));
}

namespace {
struct Seg {
  uint16_t item;
  uint32_t g0, g1;  // glyph range for TEXT
  float w;
  float trailSpace;  // width of trailing spaces in this seg
};
}  // namespace

float LayoutEngine::layoutInlineContents(Element *container, InlineLayout &il, std::vector<Node *> &nodes, float contentW,
                                         float y0) {
  const ComputedStyle &cs = S(container);
  Box &cb = *container->box;
  bool prevSpace = true;
  for (Node *n : nodes) collectInlineItems(container, il, n, prevSpace, contentW);
  // anything non-collapsible?
  bool any = false;
  for (auto &it : il.items) {
    if (it.kind == InlineItem::TEXT) {
      for (auto &g : it.glyphs)
        if (!(g.flags & SG_SPACE) || cs.whiteSpace == WS_PRE || cs.whiteSpace == WS_PRE_WRAP) {
          any = true;
          break;
        }
    } else if (it.kind != InlineItem::OUT_OF_FLOW) {
      if (it.kind == InlineItem::OPEN || it.kind == InlineItem::CLOSE) {
        if (it.width > 0) any = true;
      } else {
        any = true;
      }
    }
    if (any) break;
  }
  float x0 = cb.contentX();
  if (!any) {
    for (auto &it : il.items)
      if (it.kind == InlineItem::OUT_OF_FLOW) {
        Element *cbEl = containingBlockFor(it.el);
        pendingAbs_.push_back({it.el, cbEl, x0, y0});
        ensureBox(it.el).origin = container;
      }
    return 0;
  }

  FontRef strutFont = fontFor(cs);
  float strutA = std::round(strutFont.ascent()), strutD = std::round(strutFont.descent());
  float strutLH = cs.usedLineHeight(normalLineHeight(strutFont));
  float strutHalf = (strutLH - (strutA + strutD)) / 2;

  bool wrap = cs.whiteSpace != WS_NOWRAP && cs.whiteSpace != WS_PRE;
  bool breakAnywhere = cs.wordBreak == WB_BREAK_ALL || cs.overflowWrap == OW_ANYWHERE || cs.overflowWrap == OW_BREAK_WORD ||
                       cs.wordBreak == WB_BREAK_WORD;
  bool collapsible = cs.whiteSpace == WS_NORMAL || cs.whiteSpace == WS_NOWRAP || cs.whiteSpace == WS_PRE_LINE;

  std::vector<Seg> line;  // committed segments of the current line
  float lineX = cs.textIndent;
  float avail = contentW - cs.textIndent;
  std::vector<Seg> word;
  float wordW = 0;
  bool lineHasContent = false;
  float y = 0;
  bool hardBreakEnd = false;

  struct OpenSpan {
    Element *el;
    size_t line;
    float x0, x1;
    bool openHere, closeHere;
  };
  std::vector<OpenSpan> spans;
  std::vector<Element *> openStack;

  struct PendingOOF {
    Element *el;
    size_t lineIdx;
    float x;
  };
  std::vector<PendingOOF> oofs;

  auto segIsSpaceOnly = [&](const Seg &sg) {
    const InlineItem &it = il.items[sg.item];
    if (it.kind != InlineItem::TEXT) return false;
    for (uint32_t k = sg.g0; k < sg.g1; k++)
      if (!(it.glyphs[k].flags & SG_SPACE)) return false;
    return true;
  };
  (void)segIsSpaceOnly;

  auto finishLine = [&](bool hard) {
    // trailing collapsible spaces hang
    float trail = 0;
    if (collapsible) {
      for (int k = (int)line.size() - 1; k >= 0; k--) {
        Seg &sg = line[k];
        const InlineItem &it = il.items[sg.item];
        if (it.kind == InlineItem::OPEN || it.kind == InlineItem::CLOSE) continue;
        if (it.kind != InlineItem::TEXT) break;
        trail += sg.trailSpace;
        if (sg.trailSpace < sg.w - 0.001f) break;
      }
    }
    float used = lineX - trail;
    float offset = 0;
    TextAlign ta = cs.textAlign;
    bool lastLine = hard;
    if (ta == TA_JUSTIFY && lastLine) ta = TA_START;
    if (ta == TA_RIGHT || ta == TA_END) offset = avail - used;
    else if (ta == TA_CENTER) offset = (avail - used) / 2;
    float spaceExtra = 0;
    if (ta == TA_JUSTIFY && !lastLine) {
      int spaces = 0;
      for (auto &sg : line) {
        const InlineItem &it = il.items[sg.item];
        if (it.kind != InlineItem::TEXT) continue;
        for (uint32_t k = sg.g0; k < sg.g1; k++)
          if (it.glyphs[k].flags & SG_SPACE) spaces++;
      }
      int trailingSpaces = trail > 0 ? 1 : 0;
      if (spaces - trailingSpaces > 0) spaceExtra = (avail - used) / (spaces - trailingSpaces);
    }
    if (offset < 0) offset = 0;  // a line too long for its box starts at the start edge (CSS Text, Chrome)
    // vertical metrics
    float above = strutA + strutHalf, below = strutD + strutHalf;
    struct FM {
      float a, d, shift;
    };
    std::vector<FM> fm(line.size());
    LineBox lb;
    float x = (lineX == 0 && line.empty()) ? 0 : 0;
    (void)x;
    for (size_t k = 0; k < line.size(); k++) {
      Seg &sg = line[k];
      InlineItem &it = il.items[sg.item];
      FM m{0, 0, 0};
      if (it.kind == InlineItem::TEXT) {
        const ComputedStyle &os = S(it.el);
        float A = std::round(it.font.ascent()), D = std::round(it.font.descent());
        if (it.fallback) {
          A = std::max(A, std::round(it.font.fbAscent()));
          D = std::max(D, std::round(it.font.fbDescent()));
        }
        // line-height: normal uses the metrics of the fonts actually used (incl. fallback)
        float lh = os.usedLineHeight(A + D + std::round(it.font.lineGap()));
        float half = (lh - (A + D)) / 2;
        // vertical-align of the owning inline elements
        float shift = 0;
        for (Element *p = it.el; p && p != container; p = p->parent) {
          if (!p->cs) break;
          VAlign va = p->cs->verticalAlign;
          float fs = p->parent && p->parent->cs ? p->parent->cs->fontSize : p->cs->fontSize;
          if (va == VA_SUB) shift += fs * 0.2f;
          else if (va == VA_SUPER) shift -= fs * 0.34f;
          else if (va == VA_LENGTH) shift -= p->cs->verticalAlignLen;
          if (isAtomicInline(p)) break;
        }
        m.a = A + half - shift;
        m.d = D + half + shift;
        m.shift = shift;
        above = std::max(above, m.a);
        below = std::max(below, m.d);
      } else if (it.kind == InlineItem::ATOMIC) {
        Box &ab = *it.el->box;
        const ComputedStyle &as = S(it.el);
        float H = ab.h + ab.m[0] + ab.m[2];
        float base;
        bool useBottom = isReplaced(it.el) || ab.lastBaseline < 0 || as.clips();
        Tag tg = it.el->tagId();
        if ((tg == TAG_input || tg == TAG_select || tg == TAG_textarea) && ab.baseline >= 0) useBottom = false;
        base = useBottom ? H : ab.m[0] + (as.isFlex() || as.isGrid() ? ab.baseline : ab.lastBaseline);
        if ((tg == TAG_input || tg == TAG_select) && ab.baseline >= 0) base = ab.m[0] + ab.baseline;
        if (as.verticalAlign == VA_MIDDLE) {
          float xh = strutFont.xHeight();
          base = H / 2 + xh / 2;
        }
        m.a = base;
        m.d = H - base;
        if (as.verticalAlign == VA_TOP || as.verticalAlign == VA_BOTTOM) {
          m.a = m.d = 0;
        } else {
          above = std::max(above, m.a);
          below = std::max(below, m.d);
        }
      }
      fm[k] = m;
    }
    float lineH = above + below;
    for (size_t k = 0; k < line.size(); k++) {
      InlineItem &it = il.items[line[k].item];
      if (it.kind == InlineItem::ATOMIC) {
        const ComputedStyle &as = S(it.el);
        if (as.verticalAlign == VA_TOP || as.verticalAlign == VA_BOTTOM) {
          Box &ab = *it.el->box;
          lineH = std::max(lineH, ab.h + ab.m[0] + ab.m[2]);
        }
      }
    }
    lb.y = y0 + y;
    lb.h = lineH;
    lb.baseline = lb.y + above;
    lb.f0 = (uint32_t)il.frags.size();
    // place segments
    float px = cb.contentX() + offset + (il.lines.empty() ? cs.textIndent : 0);
    size_t lineIdx = il.lines.size();
    for (size_t k = 0; k < line.size(); k++) {
      Seg &sg = line[k];
      InlineItem &it = il.items[sg.item];
      if (it.kind == InlineItem::TEXT) {
        LineFrag f;
        f.kind = LineFrag::TEXT;
        f.item = sg.item;
        f.g0 = sg.g0;
        f.g1 = sg.g1;
        f.x = px;
        f.shift = it.glyphs[sg.g0].x;
        int nsp = 0;
        for (uint32_t g = sg.g0; g < sg.g1; g++)
          if (it.glyphs[g].flags & SG_SPACE) nsp++;
        f.w = sg.w + nsp * spaceExtra;
        f.baseline = lb.baseline + fm[k].shift;
        if (spaceExtra != 0) {
          // bake justification into glyph positions of this fragment
          float extra = 0;
          for (uint32_t g = sg.g0; g < sg.g1; g++) {
            it.glyphs[g].x += extra;
            if (it.glyphs[g].flags & SG_SPACE) extra += spaceExtra;
          }
        }
        il.frags.push_back(f);
        px += f.w;
      } else if (it.kind == InlineItem::ATOMIC) {
        Box &ab = *it.el->box;
        const ComputedStyle &as = S(it.el);
        float top;
        if (as.verticalAlign == VA_TOP) top = lb.y;
        else if (as.verticalAlign == VA_BOTTOM) top = lb.y + lineH - (ab.h + ab.m[0] + ab.m[2]);
        else top = lb.baseline - fm[k].a;
        ab.x = px + ab.m[3] + ab.relX;
        ab.y = top + ab.m[0] + ab.relY;
        ab.origin = container;
        ab.inFlowInline = true;
        LineFrag f;
        f.kind = LineFrag::ATOMIC;
        f.item = sg.item;
        f.g0 = f.g1 = 0;
        f.x = px;
        f.w = sg.w;
        f.baseline = lb.baseline;
        f.shift = 0;
        il.frags.push_back(f);
        px += sg.w;
      } else if (it.kind == InlineItem::OPEN) {
        float ml = it.el->box->m[3];
        spans.push_back({it.el, lineIdx, px + ml, px + ml, true, false});
        openStack.push_back(it.el);
        px += sg.w;
      } else if (it.kind == InlineItem::CLOSE) {
        px += sg.w;
        float mr = it.el->box->m[1];
        for (auto sp = spans.rbegin(); sp != spans.rend(); ++sp) {
          if (sp->el == it.el && sp->line == lineIdx) {
            sp->x1 = px - mr;
            sp->closeHere = true;
            break;
          }
        }
        if (!openStack.empty() && openStack.back() == it.el) openStack.pop_back();
      }
      // extend open spans to cover this segment
      for (auto &sp : spans)
        if (sp.line == lineIdx && !sp.closeHere) sp.x1 = std::max(sp.x1, px);
    }
    lb.f1 = (uint32_t)il.frags.size();
    lb.width = px - cb.contentX();
    il.maxRight = std::max(il.maxRight, px - trail);
    il.lines.push_back(lb);
    // spans still open continue on the next line
    for (Element *oe : openStack) {
      bool has = false;
      for (auto &sp : spans)
        if (sp.el == oe && sp.line == lineIdx + 1) has = true;
      if (!has) spans.push_back({oe, lineIdx + 1, cb.contentX(), cb.contentX(), false, false});
    }
    for (auto &o : oofs)
      if (o.lineIdx == lineIdx) {
        Element *cbEl = containingBlockFor(o.el);
        pendingAbs_.push_back({o.el, cbEl, o.x, lb.y});
        ensureBox(o.el).origin = container;
      }
    y += lineH;
    line.clear();
    lineX = 0;
    avail = contentW;
    lineHasContent = false;
    hardBreakEnd = hard;
  };

  auto commitWord = [&]() {
    for (auto &sg : word) line.push_back(sg);
    lineX += wordW;
    if (!word.empty()) lineHasContent = true;
    word.clear();
    wordW = 0;
  };

  // drop leading collapsible spaces of the pending word (start of a line)
  auto dropLeadingSpaces = [&]() {
    if (!collapsible) return;
    while (!word.empty()) {
      Seg &sg = word.front();
      InlineItem &it = il.items[sg.item];
      if (it.kind != InlineItem::TEXT) break;
      while (sg.g0 < sg.g1 && (it.glyphs[sg.g0].flags & SG_SPACE)) {
        sg.w -= it.glyphs[sg.g0].adv;
        wordW -= it.glyphs[sg.g0].adv;
        sg.trailSpace = std::max(0.f, sg.trailSpace - it.glyphs[sg.g0].adv);
        sg.g0++;
      }
      if (sg.g0 < sg.g1) break;
      word.erase(word.begin());
    }
  };

  auto wordTrail = [&]() {
    float t = 0;
    for (int k = (int)word.size() - 1; k >= 0; k--) {
      t += word[k].trailSpace;
      if (word[k].trailSpace < word[k].w - 0.001f) break;
    }
    return collapsible ? t : 0.f;
  };

  // place the pending word, wrapping if needed
  std::function<void()> placeWord = [&]() {
    if (word.empty()) return;
    if (!lineHasContent && line.empty()) dropLeadingSpaces();
    if (word.empty()) return;
    float fitW = wordW - wordTrail();
    if (!wrap || lineX + fitW <= avail + 0.01f) {
      commitWord();
      return;
    }
    if (lineHasContent) {
      finishLine(false);
      dropLeadingSpaces();
      fitW = wordW - wordTrail();
      if (fitW <= avail + 0.01f) {
        commitWord();
        return;
      }
    }
    // the word alone is too wide: break inside it if allowed (Thai always may)
    bool thai = false;
    for (auto &sg : word) {
      InlineItem &it = il.items[sg.item];
      if (it.kind == InlineItem::TEXT && sg.g1 > sg.g0 && (it.glyphs[sg.g0].flags & SG_THAI)) thai = true;
    }
    if (!(breakAnywhere || thai)) {
      commitWord();
      return;
    }
    // greedy split at cluster boundaries
    std::vector<Seg> rest = word;
    word.clear();
    wordW = 0;
    for (size_t si = 0; si < rest.size(); si++) {
      Seg sg = rest[si];
      InlineItem &it = il.items[sg.item];
      if (it.kind != InlineItem::TEXT) {
        if (lineX + sg.w > avail && lineHasContent) finishLine(false);
        line.push_back(sg);
        lineX += sg.w;
        lineHasContent = true;
        continue;
      }
      uint32_t start = sg.g0;
      float w = 0;
      for (uint32_t g = sg.g0; g < sg.g1; g++) {
        const ShapedGlyph &gl = it.glyphs[g];
        bool clusterStart = (gl.flags & SG_CLUSTER_START) && g > start;
        if (clusterStart && lineX + w + gl.adv > avail + 0.01f && (lineHasContent || w > 0)) {
          // break before g
          Seg part{sg.item, start, g, w, 0};
          if (g > start) {
            line.push_back(part);
            lineX += w;
            lineHasContent = true;
          }
          finishLine(false);
          start = g;
          w = 0;
        }
        w += gl.adv;
      }
      Seg part{sg.item, start, sg.g1, w, 0};
      // recompute trailing spaces
      for (int g = (int)sg.g1 - 1; g >= (int)start && (it.glyphs[g].flags & SG_SPACE); g--) part.trailSpace += it.glyphs[g].adv;
      if (sg.g1 > start) {
        line.push_back(part);
        lineX += w;
        lineHasContent = true;
      }
    }
  };

  for (size_t ii = 0; ii < il.items.size(); ii++) {
    InlineItem &it = il.items[ii];
    switch (it.kind) {
      case InlineItem::TEXT: {
        uint32_t start = 0;
        float w = 0, trail = 0;
        for (uint32_t g = 0; g < it.glyphs.size(); g++) {
          const ShapedGlyph &gl = it.glyphs[g];
          w += gl.adv;
          if (gl.flags & SG_SPACE) trail += gl.adv;
          else trail = 0;
          bool brk = (gl.flags & SG_BREAK_AFTER) && wrap;
          bool hard = gl.flags & SG_HARD_BREAK;
          if (brk || hard || g + 1 == it.glyphs.size()) {
            Seg sg{(uint16_t)ii, start, g + 1, w, trail};
            if (hard) {  // the newline glyph itself has no width
              word.push_back(sg);
              wordW += w;
              placeWord();
              commitWord();
              finishLine(true);
            } else {
              word.push_back(sg);
              wordW += w;
              if (brk) placeWord();
            }
            start = g + 1;
            w = 0;
            trail = 0;
          }
        }
        break;
      }
      case InlineItem::ATOMIC: {
        placeWord();  // a break is allowed before an atomic inline
        Seg sg{(uint16_t)ii, 0, 0, it.width, 0};
        if (wrap && lineHasContent && lineX + it.width > avail + 0.01f) finishLine(false);
        line.push_back(sg);
        lineX += it.width;
        lineHasContent = true;
        break;
      }
      case InlineItem::OPEN:
      case InlineItem::CLOSE: {
        Seg sg{(uint16_t)ii, 0, 0, it.width, 0};
        word.push_back(sg);
        wordW += it.width;
        break;
      }
      case InlineItem::BREAK:
        placeWord();
        commitWord();
        finishLine(true);
        break;
      case InlineItem::OUT_OF_FLOW:
        placeWord();
        oofs.push_back({it.el, il.lines.size(), cb.contentX() + lineX});
        break;
    }
  }
  placeWord();
  commitWord();
  if (!line.empty() || il.lines.empty()) {
    bool onlyEdges = true;
    for (auto &sg : line) {
      InlineItem &it = il.items[sg.item];
      if (it.kind == InlineItem::TEXT && sg.g1 > sg.g0) onlyEdges = false;
      if (it.kind == InlineItem::ATOMIC) onlyEdges = false;
      if ((it.kind == InlineItem::OPEN || it.kind == InlineItem::CLOSE) && sg.w > 0) onlyEdges = false;
    }
    if (!line.empty() && !onlyEdges) finishLine(true);
    else if (!line.empty() && il.lines.empty()) line.clear();
  }
  (void)hardBreakEnd;
  // out-of-flow items on a trailing empty line
  for (auto &o : oofs)
    if (o.lineIdx >= il.lines.size()) {
      Element *cbEl = containingBlockFor(o.el);
      pendingAbs_.push_back({o.el, cbEl, o.x, y0 + y});
      ensureBox(o.el).origin = container;
    }

  // line clamp / ellipsis
  int clamp = cs.lineClamp;
  if (clamp > 0 && (int)il.lines.size() > clamp) {
    LineBox &last = il.lines[clamp - 1];
    il.frags.resize(il.lines[clamp].f0);
    il.lines.resize(clamp);
    y = last.y + last.h - y0;
    // ellipsis on the last kept line
    LineFrag e;
    e.kind = LineFrag::ELLIPSIS;
    e.item = 0;
    e.g0 = e.g1 = 0;
    float ew = measureText(strutFont, "\xE2\x80\xA6");
    float limit = cb.contentX() + contentW - ew;
    while (last.f1 > last.f0) {
      LineFrag &f = il.frags[last.f1 - 1];
      if (f.x + f.w <= limit) break;
      if (f.kind == LineFrag::TEXT) {
        InlineItem &it = il.items[f.item];
        while (f.g1 > f.g0 && f.x + (it.glyphs[f.g1 - 1].x - f.shift) + it.glyphs[f.g1 - 1].adv > limit) f.g1--;
        f.w = f.g1 > f.g0 ? (it.glyphs[f.g1 - 1].x - f.shift) + it.glyphs[f.g1 - 1].adv : 0;
        if (f.g1 > f.g0) break;
      }
      il.frags.pop_back();
      last.f1--;
    }
    float ex = last.f1 > last.f0 ? il.frags[last.f1 - 1].x + il.frags[last.f1 - 1].w : cb.contentX();
    e.x = ex;
    e.w = ew;
    e.baseline = last.baseline;
    e.shift = 0;
    il.frags.push_back(e);
    last.f1++;
  } else if (cs.textOverflow == TO_ELLIPSIS && cs.clips() && !il.lines.empty()) {
    float right = cb.contentX() + contentW;
    for (auto &ln : il.lines) {
      if (ln.f1 == ln.f0) continue;
      LineFrag &lf = il.frags[ln.f1 - 1];
      if (lf.x + lf.w <= right + 0.5f) continue;
      float ew = measureText(strutFont, "\xE2\x80\xA6");
      float limit = right - ew;
      uint32_t keep = ln.f1;
      while (keep > ln.f0) {
        LineFrag &f = il.frags[keep - 1];
        if (f.x + f.w <= limit) break;
        if (f.kind == LineFrag::TEXT) {
          InlineItem &it = il.items[f.item];
          while (f.g1 > f.g0 && f.x + (it.glyphs[f.g1 - 1].x - f.shift) + it.glyphs[f.g1 - 1].adv > limit) f.g1--;
          f.w = f.g1 > f.g0 ? (it.glyphs[f.g1 - 1].x - f.shift) + it.glyphs[f.g1 - 1].adv : 0;
          if (f.g1 > f.g0) break;
        } else if (f.kind == LineFrag::ATOMIC) {
          break;
        }
        f.g1 = f.g0;
        f.w = 0;
        keep--;
      }
      // hide fragments after `keep`
      for (uint32_t k = keep; k < ln.f1; k++) {
        il.frags[k].g1 = il.frags[k].g0;
        il.frags[k].w = 0;
      }
      float ex = keep > ln.f0 ? il.frags[keep - 1].x + il.frags[keep - 1].w : cb.contentX();
      LineFrag e;
      e.kind = LineFrag::ELLIPSIS;
      e.item = 0;
      e.g0 = e.g1 = 0;
      e.x = ex;
      e.w = ew;
      e.baseline = ln.baseline;
      e.shift = 0;
      // insert at the end of the line: append and extend the line range
      il.frags.insert(il.frags.begin() + ln.f1, e);
      size_t li = &ln - &il.lines[0];
      ln.f1++;
      for (size_t k = li + 1; k < il.lines.size(); k++) {
        il.lines[k].f0++;
        il.lines[k].f1++;
      }
    }
  }

  // inline element fragments (for backgrounds, borders and hit testing)
  for (auto &sp : spans) {
    if (sp.line >= il.lines.size()) continue;
    Element *el = sp.el;
    Box &eb = *el->box;
    const LineBox &ln = il.lines[sp.line];
    FontRef f = fontFor(S(el));
    float A = std::round(f.ascent()), D = std::round(f.descent());
    float x0r = sp.x0 - (sp.openHere ? 0 : 0);
    RectF r{x0r, ln.baseline - A - eb.p[0] - eb.b[0], sp.x1 - x0r, A + D + eb.p[0] + eb.p[2] + eb.b[0] + eb.b[2]};
    eb.inlineRects.push_back(r);
    eb.origin = container;
  }
  return y;
}

// ============================================================ flexbox

namespace {
struct FlexItem {
  Element *el = nullptr;
  Text *textNode = nullptr;      // anonymous item made of a text run
  InlineLayout *anon = nullptr;
  float base = 0, hypo = 0, target = 0;
  float minMain = 0, maxMain = 1e9f;
  float mMainA = 0, mMainB = 0, mCrossA = 0, mCrossB = 0;  // margins
  bool autoMainA = false, autoMainB = false, autoCrossA = false, autoCrossB = false;
  float bpMain = 0, bpCross = 0;
  float cross = 0;
  float grow = 0, shrink = 1;
  bool frozen = false;
  float pos = 0, crossPos = 0;
  float baseline = 0;
  Align align = AL_STRETCH;
  int order = 0;
  size_t idx = 0;
};
}  // namespace

float LayoutEngine::layoutFlex(Element *e, float contentW, float contentH, float minContentH, float maxContentH) {
  Box &b = *e->box;
  const ComputedStyle &s = S(e);
  bool row = s.flexDirection == FD_ROW || s.flexDirection == FD_ROW_REVERSE;
  bool reverse = s.flexDirection == FD_ROW_REVERSE || s.flexDirection == FD_COLUMN_REVERSE;
  float mainSize = row ? contentW : contentH;  // may be -1
  float crossSize = row ? contentH : contentW;
  float mainGap = (row ? s.columnGap : s.rowGap).isLen() ? (row ? s.columnGap : s.rowGap).resolve(contentW) : 0;
  float crossGap = (row ? s.rowGap : s.columnGap).isLen() ? (row ? s.rowGap : s.columnGap).resolve(contentW) : 0;
  float x0 = b.contentX(), y0 = b.contentY();
  CB childCb{contentW, contentH};

  std::vector<FlexItem> items;
  std::vector<std::unique_ptr<InlineLayout>> anons;
  forEachLayoutChild(e, [&](Node *c) {
    if (!isDisplayed(c)) return;
    if (Text *t = c->asText()) {
      bool ws = true;
      for (char ch : t->data)
        if (!isSpace(ch)) ws = false;
      if (ws) return;
      FlexItem fi;
      fi.textNode = t;
      fi.idx = items.size();
      fi.align = s.alignItems == AL_NORMAL ? AL_STRETCH : s.alignItems;
      fi.grow = 0;
      fi.shrink = 1;
      if (row) {
        InlineLayout tmp;
        bool ps = true;
        addTextItem(tmp, e, t, ps);
        float w = 0, word = 0, minw = 0;
        if (!tmp.items.empty()) {
          auto &gl = tmp.items[0].glyphs;
          w = tmp.items[0].width;
          for (int k = (int)gl.size() - 1; k >= 0 && (gl[k].flags & SG_SPACE); k--) w -= gl[k].adv;
          for (auto &g : gl) {
            if (!(g.flags & SG_SPACE)) word += g.adv;
            if (g.flags & SG_BREAK_AFTER) {
              minw = std::max(minw, word);
              word = 0;
            }
          }
          minw = std::max(minw, word);
        }
        fi.base = fi.hypo = std::max(0.f, w);
        fi.minMain = std::min(minw, fi.base);
      } else {
        auto il = std::make_unique<InlineLayout>();
        std::vector<Node *> nodes{c};
        float h = layoutInlineContents(e, *il, nodes, contentW, 0);
        fi.base = fi.hypo = fi.minMain = h;
        fi.cross = contentW;
        fi.anon = il.get();
        anons.push_back(std::move(il));
      }
      items.push_back(fi);
      return;
    }
    Element *ce = c->asElement();
    if (isOutOfFlow(ce)) {
      Element *cbEl = containingBlockFor(ce);
      pendingAbs_.push_back({ce, cbEl, x0, y0});
      ensureBox(ce).origin = e;
      return;
    }
    FlexItem fi;
    fi.el = ce;
    fi.idx = items.size();
    fi.order = ce->cs->order;
    items.push_back(fi);
  });
  std::stable_sort(items.begin(), items.end(), [](const FlexItem &a, const FlexItem &bb) { return a.order < bb.order; });

  // ---- base sizes
  for (auto &fi : items) {
    if (!fi.el) continue;
    Element *ce = fi.el;
    const ComputedStyle &cs = S(ce);
    Box &cb = ensureBox(ce);
    resolveEdges(ce, contentW);
    cb.origin = e;
    fi.grow = cs.flexGrow;
    fi.shrink = cs.flexShrink;
    fi.align = cs.alignSelf == AL_AUTO ? (s.alignItems == AL_NORMAL ? AL_STRETCH : s.alignItems) : cs.alignSelf;
    if (fi.align == AL_NORMAL) fi.align = AL_STRETCH;
    if (row) {
      fi.mMainA = cb.m[3]; fi.mMainB = cb.m[1]; fi.mCrossA = cb.m[0]; fi.mCrossB = cb.m[2];
      fi.autoMainA = cs.margin[3].isAuto(); fi.autoMainB = cs.margin[1].isAuto();
      fi.autoCrossA = cs.margin[0].isAuto(); fi.autoCrossB = cs.margin[2].isAuto();
      fi.bpMain = borderPaddingH(cb);
      fi.bpCross = borderPaddingV(cb);
    } else {
      fi.mMainA = cb.m[0]; fi.mMainB = cb.m[2]; fi.mCrossA = cb.m[3]; fi.mCrossB = cb.m[1];
      fi.autoMainA = cs.margin[0].isAuto(); fi.autoMainB = cs.margin[2].isAuto();
      fi.autoCrossA = cs.margin[3].isAuto(); fi.autoCrossB = cs.margin[1].isAuto();
      fi.bpMain = borderPaddingV(cb);
      fi.bpCross = borderPaddingH(cb);
    }
    const Len &mainProp = row ? cs.width : cs.height;
    bool box = cs.boxSizing == BOX_BORDER;
    float base = -1;
    const Len &fb = cs.flexBasis;
    if (fb.isLen() && (!fb.hasPct() || mainSize >= 0)) {
      base = fb.resolve(mainSize);
      if (!box) base += fi.bpMain;
    } else if ((fb.isAuto()) && mainProp.isLen() && (!mainProp.hasPct() || mainSize >= 0)) {
      base = mainProp.resolve(mainSize);
      if (!box) base += fi.bpMain;
    }
    if (base < 0) {
      if (row) {
        base = maxContentWidth(ce);
      } else {
        // lay out at the cross size to measure the height
        float w = -1;
        if (fi.align == AL_STRETCH && cs.width.isAuto() && !fi.autoCrossA && !fi.autoCrossB)
          w = contentW - fi.mCrossA - fi.mCrossB;
        layoutBox(ce, childCb, contentW - fi.mCrossA - fi.mCrossB, w < 0, w);
        base = cb.h;
      }
    }
    fi.base = base;
    // min/max main
    const Len &minP = row ? cs.minWidth : cs.minHeight;
    const Len &maxP = row ? cs.maxWidth : cs.maxHeight;
    if (minP.isLen() && (!minP.hasPct() || mainSize >= 0)) fi.minMain = minP.resolve(mainSize) + (box ? 0 : fi.bpMain);
    else if (minP.isAuto() && !cs.clips()) {
      // automatic minimum size = content-based minimum
      float contentMin;
      if (row) contentMin = minContentWidth(ce);
      else {
        contentMin = fi.base;
        if (cs.flexBasis.isLen() || (cs.height.isLen())) {
          // measure content height at the cross size
          float w = -1;
          if (fi.align == AL_STRETCH && cs.width.isAuto()) w = contentW - fi.mCrossA - fi.mCrossB;
          layoutBox(ce, childCb, contentW - fi.mCrossA - fi.mCrossB, w < 0, w);
          contentMin = cb.h;
        }
      }
      if (mainProp.isLen() && (!mainProp.hasPct() || mainSize >= 0)) contentMin = std::min(contentMin, mainProp.resolve(mainSize) + (box ? 0 : fi.bpMain));
      fi.minMain = contentMin;
    } else {
      fi.minMain = fi.bpMain;
    }
    if (maxP.isLen() && (!maxP.hasPct() || mainSize >= 0)) fi.maxMain = maxP.resolve(mainSize) + (box ? 0 : fi.bpMain);
    fi.minMain = std::max(fi.minMain, fi.bpMain);
    fi.hypo = std::max(fi.minMain, std::min(fi.base, fi.maxMain));
  }

  // ---- lines
  std::vector<std::pair<size_t, size_t>> lines;
  {
    size_t st = 0;
    float used = 0;
    for (size_t i = 0; i < items.size(); i++) {
      float outer = items[i].hypo + items[i].mMainA + items[i].mMainB;
      if (s.flexWrap != FW_NOWRAP && mainSize >= 0 && i > st && used + mainGap + outer > mainSize + 0.01f) {
        lines.push_back({st, i});
        st = i;
        used = 0;
      }
      used += (i > st ? mainGap : 0) + outer;
    }
    if (st < items.size() || lines.empty()) lines.push_back({st, items.size()});
  }

  // ---- resolve flexible lengths per line
  float maxLineMain = 0;
  for (auto &ln : lines) {
    size_t a = ln.first, z = ln.second;
    float sumOuter = 0;
    for (size_t i = a; i < z; i++) sumOuter += items[i].hypo + items[i].mMainA + items[i].mMainB;
    sumOuter += mainGap * (z > a ? z - a - 1 : 0);
    if (mainSize < 0) {
      for (size_t i = a; i < z; i++) items[i].target = items[i].hypo;
      maxLineMain = std::max(maxLineMain, sumOuter);
      continue;
    }
    bool growing = sumOuter < mainSize;
    for (size_t i = a; i < z; i++) {
      FlexItem &fi = items[i];
      fi.frozen = false;
      fi.target = fi.hypo;
      float factor = growing ? fi.grow : fi.shrink;
      if (factor == 0 || (growing && fi.base > fi.hypo) || (!growing && fi.base < fi.hypo)) {
        fi.frozen = true;
      }
    }
    for (int iter = 0; iter < 10; iter++) {
      float used = mainGap * (z > a ? z - a - 1 : 0);
      float sumFactor = 0, sumScaled = 0;
      bool anyUnfrozen = false;
      for (size_t i = a; i < z; i++) {
        FlexItem &fi = items[i];
        used += fi.mMainA + fi.mMainB + (fi.frozen ? fi.target : fi.base);
        if (!fi.frozen) {
          anyUnfrozen = true;
          sumFactor += growing ? fi.grow : fi.shrink;
          sumScaled += fi.shrink * fi.base;
        }
      }
      if (!anyUnfrozen) break;
      float free = mainSize - used;
      if (growing && sumFactor < 1) free *= sumFactor >= 0 ? sumFactor : 0;  // fractional grow sum
      float totalViolation = 0;
      for (size_t i = a; i < z; i++) {
        FlexItem &fi = items[i];
        if (fi.frozen) continue;
        float t;
        if (growing) t = fi.base + (sumFactor > 0 ? free * fi.grow / sumFactor : 0);
        else t = fi.base + (sumScaled > 0 ? free * (fi.shrink * fi.base) / sumScaled : 0);
        float c = std::max(fi.minMain, std::min(t, fi.maxMain));
        totalViolation += c - t;
        fi.target = c;
      }
      if (std::fabs(totalViolation) < 0.01f) break;
      for (size_t i = a; i < z; i++) {
        FlexItem &fi = items[i];
        if (fi.frozen) continue;
        if (totalViolation > 0 && fi.target > (growing ? fi.base + 0 : fi.target - 1) && fi.target == fi.minMain) fi.frozen = true;
        if (totalViolation < 0 && fi.target == fi.maxMain) fi.frozen = true;
        if (totalViolation > 0 && fi.target == fi.minMain) fi.frozen = true;
      }
    }
    float tot = mainGap * (z > a ? z - a - 1 : 0);
    for (size_t i = a; i < z; i++) tot += items[i].target + items[i].mMainA + items[i].mMainB;
    maxLineMain = std::max(maxLineMain, tot);
  }
  if (mainSize < 0) mainSize = maxLineMain;

  // ---- cross sizes (lay out items at their main size)
  for (auto &fi : items) {
    if (!fi.el) {
      if (row) {
        auto il = std::make_unique<InlineLayout>();
        std::vector<Node *> nodes{fi.textNode};
        fi.cross = layoutInlineContents(e, *il, nodes, std::max(fi.target, 1.f), 0);
        fi.anon = il.get();
        anons.push_back(std::move(il));
      }
      continue;
    }
    Element *ce = fi.el;
    const ComputedStyle &cs = S(ce);
    if (row) {
      bool stretchW = false;
      (void)stretchW;
      float fh = -1;
      layoutBox(ce, childCb, fi.target, false, fi.target, fh);
      fi.cross = ce->box->h;
      fi.baseline = ce->box->baseline >= 0 ? ce->box->baseline : ce->box->h;
    } else {
      float w = -1;
      bool stretch = fi.align == AL_STRETCH && cs.width.isAuto() && !fi.autoCrossA && !fi.autoCrossB;
      if (stretch) w = contentW - fi.mCrossA - fi.mCrossB;
      layoutBox(ce, childCb, contentW - fi.mCrossA - fi.mCrossB, w < 0, w, fi.target);
      fi.cross = ce->box->w;
    }
  }

  // ---- line cross sizes
  std::vector<float> lineCross(lines.size(), 0);
  std::vector<float> lineBase(lines.size(), 0);
  for (size_t li = 0; li < lines.size(); li++) {
    float mx = 0, maxBase = 0, maxBelow = 0;
    for (size_t i = lines[li].first; i < lines[li].second; i++) {
      FlexItem &fi = items[i];
      if (fi.align == AL_BASELINE && row) {
        maxBase = std::max(maxBase, fi.baseline + fi.mCrossA);
        maxBelow = std::max(maxBelow, fi.cross - fi.baseline + fi.mCrossB);
      }
      mx = std::max(mx, fi.cross + fi.mCrossA + fi.mCrossB);
    }
    mx = std::max(mx, maxBase + maxBelow);
    lineCross[li] = mx;
    lineBase[li] = maxBase;
  }
  if (lines.size() == 1) {
    // a single line takes the container's cross size -- or, when that is auto, its own size
    // clamped by the container's min/max (CSS Flexbox 9.4 step 8): items centre in a
    // min-height row, as in browsers
    if (crossSize >= 0) lineCross[0] = crossSize;
    else if (row) lineCross[0] = std::max(minContentH, std::min(lineCross[0], maxContentH));
  }
  // align-content (multi-line)
  float totalCross = crossGap * (lines.size() - 1);
  for (float c : lineCross) totalCross += c;
  float crossStart = 0, crossBetween = crossGap;
  if (crossSize >= 0 && lines.size() > 1) {
    float free = crossSize - totalCross;
    Align ac = s.alignContent;
    if ((ac == AL_NORMAL || ac == AL_STRETCH) && free > 0) {
      for (auto &c : lineCross) c += free / lines.size();
    } else if (ac == AL_CENTER) crossStart = free / 2;
    else if (ac == AL_END) crossStart = free;
    else if (ac == AL_SPACE_BETWEEN && lines.size() > 1) crossBetween += free / (lines.size() - 1);
    else if (ac == AL_SPACE_AROUND) {
      crossStart = free / lines.size() / 2;
      crossBetween += free / lines.size();
    } else if (ac == AL_SPACE_EVENLY) {
      crossStart = free / (lines.size() + 1);
      crossBetween += free / (lines.size() + 1);
    }
  }

  // ---- stretch and position
  float crossCursor = crossStart;
  float contentCross = 0;
  for (size_t li = 0; li < lines.size(); li++) {
    size_t a = lines[li].first, z = lines[li].second;
    float lc = lineCross[li];
    // stretch
    for (size_t i = a; i < z; i++) {
      FlexItem &fi = items[i];
      if (!fi.el) continue;
      const ComputedStyle &cs = S(fi.el);
      const Len &crossProp = row ? cs.height : cs.width;
      if (fi.align == AL_STRETCH && crossProp.isAuto() && !fi.autoCrossA && !fi.autoCrossB) {
        float target = lc - fi.mCrossA - fi.mCrossB;
        // respect max cross size
        const Len &maxC = row ? cs.maxHeight : cs.maxWidth;
        if (maxC.isLen() && !maxC.hasPct()) target = std::min(target, maxC.px + (cs.boxSizing == BOX_CONTENT ? fi.bpCross : 0));
        if (std::fabs(target - fi.cross) > 0.01f) {
          if (row) layoutBox(fi.el, childCb, fi.target, false, fi.target, target);
          else layoutBox(fi.el, childCb, target, false, target, fi.target);
          fi.cross = target;
        } else if (row) {
          // definite height for percentage children: re-layout with forced height
          layoutBox(fi.el, childCb, fi.target, false, fi.target, target);
        }
      }
    }
    // main axis
    float used = mainGap * (z > a ? z - a - 1 : 0);
    int autoMargins = 0;
    for (size_t i = a; i < z; i++) {
      used += items[i].target + items[i].mMainA + items[i].mMainB;
      autoMargins += items[i].autoMainA + items[i].autoMainB;
    }
    float free = mainSize - used;
    float start = 0, between = mainGap;
    float autoM = 0;
    if (autoMargins > 0 && free > 0) {
      autoM = free / autoMargins;
    } else {
      Align jc = s.justifyContent;
      if (free > 0 || jc == AL_CENTER || jc == AL_END || jc == AL_RIGHT) {
        size_t n = z - a;
        if (jc == AL_CENTER) start = free / 2;
        else if (jc == AL_END || (jc == AL_RIGHT && row)) start = free;
        else if (jc == AL_SPACE_BETWEEN && n > 1) between += free / (n - 1);
        else if (jc == AL_SPACE_AROUND && n > 0) {
          start = free / n / 2;
          between += free / n;
        } else if (jc == AL_SPACE_EVENLY && n > 0) {
          start = free / (n + 1);
          between += free / (n + 1);
        }
      }
    }
    float cursor = start;
    for (size_t i = a; i < z; i++) {
      FlexItem &fi = items[i];
      cursor += fi.mMainA + (fi.autoMainA ? autoM : 0);
      fi.pos = cursor;
      cursor += fi.target + fi.mMainB + (fi.autoMainB ? autoM : 0) + between;
      // cross axis
      float outerCross = fi.cross + fi.mCrossA + fi.mCrossB;
      float cfree = lc - outerCross;
      float cp = fi.mCrossA;
      if (fi.autoCrossA && fi.autoCrossB) cp += cfree / 2;
      else if (fi.autoCrossA) cp += cfree;
      else if (fi.autoCrossB) {}
      else if (fi.align == AL_CENTER) cp += cfree / 2;
      else if (fi.align == AL_END) cp += cfree;
      else if (fi.align == AL_BASELINE && row) cp = lineBase[li] - fi.baseline;
      fi.crossPos = crossCursor + cp;
    }
    crossCursor += lc + crossBetween;
    contentCross = crossCursor - crossBetween;
  }

  // ---- write positions
  float mainExtent = mainSize;
  float maxRight = 0, maxBottom = 0;
  for (auto &fi : items) {
    float mainPos = reverse ? mainExtent - fi.pos - fi.target : fi.pos;
    float px, py;
    if (row) {
      px = x0 + mainPos;
      py = y0 + fi.crossPos;
    } else {
      px = x0 + fi.crossPos;
      py = y0 + mainPos;
    }
    if (fi.el) {
      Box &cb = *fi.el->box;
      cb.x = px + cb.relX;
      cb.y = py + cb.relY;
      cb.origin = e;
      maxRight = std::max(maxRight, px + cb.w + cb.m[1] - x0);
      maxBottom = std::max(maxBottom, py + cb.h + cb.m[2] - y0);
      if (b.baseline < 0 && cb.baseline >= 0 && row) b.baseline = (py - 0) + cb.baseline;
    } else {
      // move the anonymous inline layout
      InlineLayout *il = fi.anon;
      float dx = px - b.contentX(), dy = py - 0;
      for (auto &f : il->frags) {
        f.x += dx;
        f.baseline += dy;
      }
      for (auto &l : il->lines) {
        l.y += dy;
        l.baseline += dy;
      }
      il->y = py;
      if (b.baseline < 0 && !il->lines.empty()) b.baseline = il->lines[0].baseline;
      maxRight = std::max(maxRight, px + fi.target - x0);
      maxBottom = std::max(maxBottom, py + fi.cross - y0);
    }
  }
  for (auto &a : anons) b.inlines.push_back(std::move(a));
  if (b.baseline < 0 && !items.empty() && items[0].el && items[0].el->box->baseline >= 0)
    b.baseline = items[0].el->box->y + items[0].el->box->baseline;
  b.lastBaseline = b.baseline;
  b.scrollW = std::max(contentW, maxRight) + b.p[1] + b.p[3];
  b.scrollH = maxBottom + b.p[0] + b.p[2];
  return row ? contentCross : mainSize;
}

// ============================================================ grid

namespace {
struct GItem {
  Element *el;
  int c0 = 0, c1 = 1, r0 = 0, r1 = 1;
};
struct Track {
  float base = 0, limit = 1e9f;
  float fr = 0;
  bool content = false;  // size from content
  TrackSize spec;
};
}  // namespace

static float trackFixed(const TrackSize &t, float ref, bool &ok) {
  ok = t.kind == TrackSize::FIXED && (!t.len.pct || ref >= 0);
  if (!ok) return 0;
  ResolveCtx rc;
  return computeLen(t.len, rc).resolve(ref);
}

float LayoutEngine::layoutGrid(Element *e, float contentW, float contentH) {
  Box &b = *e->box;
  const ComputedStyle &s = S(e);
  float colGap = s.columnGap.isLen() ? s.columnGap.resolve(contentW) : 0;
  float rowGap = s.rowGap.isLen() ? s.rowGap.resolve(contentH >= 0 ? contentH : 0) : 0;
  float x0 = b.contentX(), y0 = b.contentY();
  CB childCb{contentW, contentH};

  // ---- explicit column tracks
  std::vector<TrackSize> colSpecs;
  if (s.gridCols) {
    const TrackList &tl = *s.gridCols;
    if (tl.autoRepeat) {
      float fixedSum = 0;
      int fixedN = 0;
      for (auto &t : tl.before) {
        bool ok;
        fixedSum += trackFixed(t, contentW, ok);
        fixedN++;
      }
      for (auto &t : tl.after) {
        bool ok;
        fixedSum += trackFixed(t, contentW, ok);
        fixedN++;
      }
      float patW = 0;
      for (auto &t : tl.tracks) {
        bool ok;
        float v = trackFixed(t, contentW, ok);
        if (!ok && t.kind == TrackSize::MINMAX && t.mn) v = trackFixed(*t.mn, contentW, ok);
        if (!ok && t.kind == TrackSize::MINMAX && t.mx) v = trackFixed(*t.mx, contentW, ok);
        patW += std::max(v, 1.f);
      }
      int reps = 1;
      if (patW > 0) {
        float avail = contentW - fixedSum - colGap * fixedN;
        reps = std::max(1, (int)std::floor((avail + colGap) / (patW + colGap * tl.tracks.size())));
      }
      colSpecs = tl.before;
      for (int r = 0; r < reps; r++)
        for (auto &t : tl.tracks) colSpecs.push_back(t);
      for (auto &t : tl.after) colSpecs.push_back(t);
    } else {
      colSpecs = tl.tracks;
    }
  }
  std::vector<TrackSize> rowSpecs;
  if (s.gridRows) rowSpecs = s.gridRows->tracks;
  TrackSize autoRow, autoCol;
  if (s.gridAutoRows && !s.gridAutoRows->tracks.empty()) autoRow = s.gridAutoRows->tracks[0];
  if (s.gridAutoCols && !s.gridAutoCols->tracks.empty()) autoCol = s.gridAutoCols->tracks[0];

  // ---- items and placement
  std::vector<GItem> items;
  forEachLayoutChild(e, [&](Node *c) {
    Element *ce = c->asElement();
    if (!ce || !isDisplayed(ce)) return;
    if (isOutOfFlow(ce)) {
      Element *cbEl = containingBlockFor(ce);
      pendingAbs_.push_back({ce, cbEl, x0, y0});
      ensureBox(ce).origin = e;
      return;
    }
    items.push_back({ce});
  });
  int ncols = std::max<int>(1, (int)colSpecs.size());
  // explicit line numbers may extend the grid
  for (auto &it : items) {
    const ComputedStyle &cs = S(it.el);
    if (cs.gridColEnd.line > ncols + 1) ncols = cs.gridColEnd.line - 1;
    if (cs.gridColStart.line > ncols) ncols = cs.gridColStart.line;
  }
  std::vector<std::vector<bool>> occ;
  auto occupied = [&](int r, int c) { return r < (int)occ.size() && occ[r][c]; };
  auto mark = [&](int r0, int r1, int c0, int c1) {
    while ((int)occ.size() < r1) occ.push_back(std::vector<bool>(ncols, false));
    for (int r = r0; r < r1; r++)
      for (int c = c0; c < c1 && c < ncols; c++) occ[r][c] = true;
  };
  bool colFlow = s.gridFlow & GF_COLUMN;
  int curR = 0, curC = 0;
  auto resolveSpan = [&](const GridLine &st, const GridLine &en, int n, int &a, int &z) -> bool {
    int span = 1;
    bool fixed = false;
    if (st.line != 0) {
      a = st.line > 0 ? st.line - 1 : n + 1 + st.line;
      fixed = true;
      if (en.line != 0) {
        int e2 = en.line > 0 ? en.line - 1 : n + 1 + en.line;
        if (e2 < a) std::swap(e2, a);
        span = std::max(1, e2 - a);
      } else if (en.span > 0) {
        span = en.span;
      }
    } else if (en.line != 0) {
      int e2 = en.line > 0 ? en.line - 1 : n + 1 + en.line;
      span = st.span > 0 ? st.span : 1;
      a = std::max(0, e2 - span);
      fixed = true;
    } else {
      span = st.span > 0 ? st.span : (en.span > 0 ? en.span : 1);
    }
    z = a + span;
    return fixed;
  };
  for (auto &it : items) {
    const ComputedStyle &cs = S(it.el);
    int c0 = 0, c1 = 1, r0 = 0, r1 = 1;
    bool cFixed = resolveSpan(cs.gridColStart, cs.gridColEnd, ncols, c0, c1);
    bool rFixed = resolveSpan(cs.gridRowStart, cs.gridRowEnd, std::max<int>(1, (int)rowSpecs.size()), r0, r1);
    int cspan = std::min(c1 - c0, ncols), rspan = r1 - r0;
    if (cFixed && c1 > ncols) {
      c1 = ncols;
      c0 = std::max(0, c1 - cspan);
    }
    if (cFixed && rFixed) {
    } else if (cFixed) {
      int r = 0;
      while (true) {
        bool ok = true;
        for (int rr = r; rr < r + rspan && ok; rr++)
          for (int cc = c0; cc < c1 && ok; cc++)
            if (occupied(rr, cc)) ok = false;
        if (ok) break;
        r++;
      }
      r0 = r;
      r1 = r + rspan;
    } else if (rFixed) {
      int c = 0;
      while (c + cspan <= ncols) {
        bool ok = true;
        for (int rr = r0; rr < r1 && ok; rr++)
          for (int cc = c; cc < c + cspan && ok; cc++)
            if (occupied(rr, cc)) ok = false;
        if (ok) break;
        c++;
      }
      c0 = std::min(c, ncols - cspan);
      c1 = c0 + cspan;
    } else {
      // auto placement (row-major unless grid-auto-flow: column)
      while (true) {
        if (!colFlow) {
          if (curC + cspan > ncols) {
            curC = 0;
            curR++;
          }
        }
        bool ok = true;
        for (int rr = curR; rr < curR + rspan && ok; rr++)
          for (int cc = curC; cc < curC + cspan && ok; cc++)
            if (occupied(rr, cc)) ok = false;
        if (ok) break;
        if (colFlow) {
          curR++;
          if (curR > 1000) break;
        } else {
          curC++;
        }
      }
      c0 = curC;
      c1 = curC + cspan;
      r0 = curR;
      r1 = curR + rspan;
      if (!colFlow) curC += cspan;
    }
    it.c0 = c0;
    it.c1 = c1;
    it.r0 = r0;
    it.r1 = r1;
    mark(r0, r1, c0, c1);
  }
  int nrows = std::max<int>((int)occ.size(), (int)rowSpecs.size());
  nrows = std::max(nrows, 1);

  // ---- column sizing
  std::vector<Track> cols(ncols);
  for (int c = 0; c < ncols; c++) {
    TrackSize t = c < (int)colSpecs.size() ? colSpecs[c] : autoCol;
    cols[c].spec = t;
  }
  auto sizeTracks = [&](std::vector<Track> &tracks, float avail, float gap, bool isCol,
                        const std::function<float(GItem &, bool)> &contribution) {
    int n = (int)tracks.size();
    for (int i = 0; i < n; i++) {
      Track &tr = tracks[i];
      const TrackSize &t = tr.spec;
      bool ok;
      tr.fr = 0;
      tr.base = 0;
      tr.limit = 1e9f;
      tr.content = false;
      if (t.kind == TrackSize::FIXED) {
        float v = trackFixed(t, avail, ok);
        if (ok) tr.base = tr.limit = v;
        else tr.content = true;
      } else if (t.kind == TrackSize::FR) {
        tr.fr = t.fr;
        tr.content = true;  // min = auto
      } else if (t.kind == TrackSize::MINMAX) {
        float mn = t.mn ? trackFixed(*t.mn, avail, ok) : 0;
        bool mnFixed = t.mn && ok;
        if (mnFixed) tr.base = mn;
        else tr.content = true;
        if (t.mx && t.mx->kind == TrackSize::FR) tr.fr = t.mx->fr;
        else if (t.mx) {
          float mx = trackFixed(*t.mx, avail, ok);
          if (ok) tr.limit = mx;
        }
        if (mnFixed && t.mn->kind == TrackSize::FIXED && t.mn->len.px == 0 && !t.mn->len.pct) tr.content = false;
      } else {
        tr.content = true;
      }
    }
    // content contributions from single-span items
    for (auto &it : items) {
      int a = isCol ? it.c0 : it.r0, z = isCol ? it.c1 : it.r1;
      if (z - a != 1 || a >= n) continue;
      Track &tr = tracks[a];
      if (!tr.content) continue;
      bool minOnly = tr.fr > 0 || (tr.spec.kind == TrackSize::MINMAX);
      float v = contribution(it, minOnly);
      if (tr.spec.kind == TrackSize::MIN_CONTENT) v = contribution(it, true);
      tr.base = std::max(tr.base, v);
    }
    // spanning items: distribute any excess equally over content tracks
    for (auto &it : items) {
      int a = isCol ? it.c0 : it.r0, z = std::min(isCol ? it.c1 : it.r1, n);
      if (z - a <= 1) continue;
      float v = contribution(it, true);
      float have = gap * (z - a - 1);
      int ncontent = 0;
      for (int i = a; i < z; i++) {
        have += tracks[i].base;
        if (tracks[i].content && tracks[i].fr == 0) ncontent++;
      }
      if (v > have && ncontent > 0) {
        for (int i = a; i < z; i++)
          if (tracks[i].content && tracks[i].fr == 0) tracks[i].base += (v - have) / ncontent;
      }
    }
    // auto tracks grow to fill leftover space when there are no fr tracks
    float used = gap * std::max(0, n - 1);
    float sumFr = 0;
    for (auto &tr : tracks) {
      if (tr.fr > 0) sumFr += tr.fr;
      else used += tr.base;
    }
    if (avail >= 0 && sumFr > 0) {
      // fr resolution with min-content floors
      std::vector<bool> frozen(n, false);
      for (int iter = 0; iter < 5; iter++) {
        float leftover = avail - used;
        float frSum = 0;
        for (int i = 0; i < n; i++)
          if (tracks[i].fr > 0 && !frozen[i]) frSum += tracks[i].fr;
        if (frSum <= 0) break;
        float unit = std::max(0.f, leftover) / std::max(frSum, 1.f);
        if (frSum < 1) unit = std::max(0.f, leftover);
        bool changed = false;
        for (int i = 0; i < n; i++) {
          if (tracks[i].fr <= 0 || frozen[i]) continue;
          float want = unit * tracks[i].fr;
          if (want < tracks[i].base) {
            frozen[i] = true;
            used += tracks[i].base;
            changed = true;
          }
        }
        if (!changed) {
          for (int i = 0; i < n; i++)
            if (tracks[i].fr > 0 && !frozen[i]) tracks[i].base = unit * tracks[i].fr;
          break;
        }
      }
    } else if (avail >= 0 && sumFr == 0) {
      float leftover = avail - used;
      int nauto = 0;
      for (auto &tr : tracks)
        if (tr.spec.kind == TrackSize::AUTO) nauto++;
      Align jc = isCol ? s.justifyContent : s.alignContent;
      if (leftover > 0 && nauto > 0 && (jc == AL_NORMAL || jc == AL_STRETCH))
        for (auto &tr : tracks)
          if (tr.spec.kind == TrackSize::AUTO) tr.base += leftover / nauto;
    }
    for (auto &tr : tracks) tr.base = std::min(std::max(tr.base, 0.f), std::max(tr.limit, tr.base));
  };

  sizeTracks(cols, contentW, colGap, true, [&](GItem &it, bool minOnly) {
    const ComputedStyle &cs = S(it.el);
    float m = (cs.margin[1].isAuto() ? 0 : cs.margin[1].px) + (cs.margin[3].isAuto() ? 0 : cs.margin[3].px);
    return (minOnly ? minContentWidth(it.el) : maxContentWidth(it.el)) + m;
  });
  std::vector<float> colX(ncols + 1, 0);
  for (int c = 0; c < ncols; c++) colX[c + 1] = colX[c] + cols[c].base + (c + 1 < ncols ? colGap : 0);
  float gridW = colX[ncols];
  // justify-content for tracks
  float colOffset = 0, colExtraGap = 0;
  if (gridW < contentW) {
    float free = contentW - gridW;
    Align jc = s.justifyContent;
    if (jc == AL_CENTER) colOffset = free / 2;
    else if (jc == AL_END || jc == AL_RIGHT) colOffset = free;
    else if (jc == AL_SPACE_BETWEEN && ncols > 1) colExtraGap = free / (ncols - 1);
    else if (jc == AL_SPACE_AROUND) {
      colOffset = free / ncols / 2;
      colExtraGap = free / ncols;
    } else if (jc == AL_SPACE_EVENLY) {
      colOffset = free / (ncols + 1);
      colExtraGap = free / (ncols + 1);
    }
  }
  auto colStart = [&](int c) { return colOffset + colX[c] + colExtraGap * c; };
  auto areaW = [&](GItem &it) { return colStart(it.c1 - 1) + cols[it.c1 - 1].base - colStart(it.c0); };

  // ---- lay out items at their column widths to get heights
  for (auto &it : items) {
    const ComputedStyle &cs = S(it.el);
    Align js = cs.justifySelf == AL_AUTO ? (s.justifyItems == AL_NORMAL ? AL_STRETCH : s.justifyItems) : cs.justifySelf;
    float aw = areaW(it);
    bool stretch = (js == AL_STRETCH || js == AL_NORMAL) && cs.width.isAuto() && !isReplaced(it.el);
    if (stretch) layoutBox(it.el, CB{aw, -1}, aw, false);
    else layoutBox(it.el, CB{aw, -1}, aw, true);
  }
  std::vector<Track> rows(nrows);
  for (int r = 0; r < nrows; r++) rows[r].spec = r < (int)rowSpecs.size() ? rowSpecs[r] : autoRow;
  sizeTracks(rows, contentH, rowGap, false, [&](GItem &it, bool) {
    Box &ib = *it.el->box;
    return ib.h + ib.m[0] + ib.m[2];
  });
  std::vector<float> rowY(nrows + 1, 0);
  for (int r = 0; r < nrows; r++) rowY[r + 1] = rowY[r] + rows[r].base + (r + 1 < nrows ? rowGap : 0);
  float gridH = rowY[nrows];
  float rowOffset = 0;
  if (contentH >= 0 && gridH < contentH) {
    Align ac = s.alignContent;
    if (ac == AL_CENTER) rowOffset = (contentH - gridH) / 2;
    else if (ac == AL_END) rowOffset = contentH - gridH;
  }

  // ---- final placement with alignment
  float maxRight = 0, maxBottom = 0;
  for (auto &it : items) {
    const ComputedStyle &cs = S(it.el);
    Box &ib = *it.el->box;
    float ax = colStart(it.c0), aw = areaW(it);
    float ay = rowOffset + rowY[it.r0];
    float ah = rowY[std::min(it.r1, nrows)] - rowY[it.r0] - (it.r1 < nrows ? rowGap : 0);
    if (it.r1 >= nrows) ah = rowY[nrows] - rowY[it.r0];
    Align as = cs.alignSelf == AL_AUTO ? (s.alignItems == AL_NORMAL ? AL_STRETCH : s.alignItems) : cs.alignSelf;
    Align js = cs.justifySelf == AL_AUTO ? (s.justifyItems == AL_NORMAL ? AL_STRETCH : s.justifyItems) : cs.justifySelf;
    if ((as == AL_STRETCH || as == AL_NORMAL) && cs.height.isAuto() && !isReplaced(it.el)) {
      float target = ah - ib.m[0] - ib.m[2];
      if (std::fabs(target - ib.h) > 0.01f || true) {
        bool stretchW = (js == AL_STRETCH || js == AL_NORMAL) && cs.width.isAuto();
        layoutBox(it.el, CB{aw, ah}, aw, !stretchW, stretchW ? -1 : ib.w, target);
      }
    }
    float px = ax + ib.m[3], py = ay + ib.m[0];
    float freeX = aw - ib.w - ib.m[1] - ib.m[3], freeY = ah - ib.h - ib.m[0] - ib.m[2];
    if (cs.margin[3].isAuto() && cs.margin[1].isAuto()) px += freeX / 2;
    else if (js == AL_CENTER) px += freeX / 2;
    else if (js == AL_END || js == AL_RIGHT) px += freeX;
    if (cs.margin[0].isAuto() && cs.margin[2].isAuto()) py += freeY / 2;
    else if (as == AL_CENTER) py += freeY / 2;
    else if (as == AL_END) py += freeY;
    ib.x = x0 + px + ib.relX;
    ib.y = y0 + py + ib.relY;
    ib.origin = e;
    maxRight = std::max(maxRight, px + ib.w + ib.m[1]);
    maxBottom = std::max(maxBottom, py + ib.h + ib.m[2]);
    if (b.baseline < 0 && ib.baseline >= 0) b.baseline = ib.y + ib.baseline;
  }
  b.lastBaseline = b.baseline;
  b.scrollW = std::max(contentW, maxRight) + b.p[1] + b.p[3];
  b.scrollH = std::max(gridH, maxBottom) + b.p[0] + b.p[2];
  return gridH + rowOffset;
}

// ============================================================ tables

float LayoutEngine::layoutTable(Element *e, float contentW, float contentH) {
  Box &b = *e->box;
  const ComputedStyle &s = S(e);
  bool collapse = s.borderCollapse;
  float sx = collapse ? 0 : s.borderSpacingX, sy = collapse ? 0 : s.borderSpacingY;
  float x0 = b.contentX(), y0 = b.contentY();

  // collect row groups in display order: header, bodies/rows, footer
  struct Row {
    Element *tr;
    Element *group;
    std::vector<std::pair<Element *, int>> cells;  // cell, colspan
  };
  std::vector<Row> rows;
  std::vector<Element *> groups;
  Element *caption = nullptr;
  auto addRows = [&](Element *g, Element *groupEl) {
    forEachLayoutChild(g, [&](Node *c) {
      Element *tr = c->asElement();
      if (!tr || !isDisplayed(tr)) return;
      if (tr->cs->display != D_TABLE_ROW) return;
      Row r{tr, groupEl, {}};
      forEachLayoutChild(tr, [&](Node *cc) {
        Element *cell = cc->asElement();
        if (!cell || !isDisplayed(cell)) return;
        int span = std::max(1, (int)cell->attrNum(A.colspan, 1));
        r.cells.push_back({cell, span});
      });
      rows.push_back(std::move(r));
    });
  };
  std::vector<Element *> head, body, foot;
  forEachLayoutChild(e, [&](Node *c) {
    Element *ce = c->asElement();
    if (!ce || !isDisplayed(ce)) return;
    Display d = ce->cs->display;
    if (d == D_TABLE_HEADER_GROUP) head.push_back(ce);
    else if (d == D_TABLE_FOOTER_GROUP) foot.push_back(ce);
    else if (d == D_TABLE_ROW_GROUP || d == D_TABLE_ROW) body.push_back(ce);
    else if (d == D_TABLE_CAPTION) caption = ce;
  });
  auto addGroup = [&](Element *g) {
    if (g->cs->display == D_TABLE_ROW) {
      Row r{g, nullptr, {}};
      forEachLayoutChild(g, [&](Node *cc) {
        Element *cell = cc->asElement();
        if (!cell || !isDisplayed(cell)) return;
        r.cells.push_back({cell, std::max(1, (int)cell->attrNum(A.colspan, 1))});
      });
      rows.push_back(std::move(r));
    } else {
      groups.push_back(g);
      addRows(g, g);
    }
  };
  for (auto g : head) addGroup(g);
  for (auto g : body) addGroup(g);
  for (auto g : foot) addGroup(g);

  int ncols = 0;
  for (auto &r : rows) {
    int n = 0;
    for (auto &c : r.cells) n += c.second;
    ncols = std::max(ncols, n);
  }
  float captionH = 0;
  if (caption) {
    layoutBox(caption, CB{contentW, -1}, contentW, false);
    captionH = caption->box->h + caption->box->m[0] + caption->box->m[2];
    caption->box->x = x0 + caption->box->m[3];
    caption->box->y = y0 + caption->box->m[0];
    caption->box->origin = e;
  }
  if (ncols == 0) {
    b.scrollW = contentW;
    b.scrollH = captionH;
    return captionH;
  }

  // ---- column widths
  std::vector<float> colMin(ncols, 0), colMax(ncols, 0), colFixed(ncols, -1);
  for (auto &r : rows) {
    int c = 0;
    for (auto &cp : r.cells) {
      Element *cell = cp.first;
      ensureBox(cell);
      resolveEdges(cell, contentW);
      float mn = minContentWidth(cell), mx = maxContentWidth(cell);
      const ComputedStyle &cs = S(cell);
      if (cp.second == 1 && c < ncols) {
        colMin[c] = std::max(colMin[c], mn);
        colMax[c] = std::max(colMax[c], mx);
        if (cs.width.isLen() && !cs.width.hasPct()) {
          float w = cs.width.px + (cs.boxSizing == BOX_CONTENT ? borderPaddingH(*cell->box) : 0);
          colFixed[c] = std::max(colFixed[c], w);
          colMin[c] = std::max(colMin[c], w);
          colMax[c] = std::max(colMax[c], w);
        } else if (cs.width.hasPct() && contentW > 0) {
          float w = cs.width.resolve(contentW);
          colFixed[c] = std::max(colFixed[c], w);
        }
      }
      c += cp.second;
    }
  }
  // spanning cells
  for (auto &r : rows) {
    int c = 0;
    for (auto &cp : r.cells) {
      if (cp.second > 1) {
        int z = std::min(ncols, c + cp.second);
        float mn = minContentWidth(cp.first), mx = maxContentWidth(cp.first);
        float haveMn = sx * (z - c - 1), haveMx = haveMn;
        for (int k = c; k < z; k++) {
          haveMn += colMin[k];
          haveMx += colMax[k];
        }
        if (mn > haveMn)
          for (int k = c; k < z; k++) colMin[k] += (mn - haveMn) / (z - c);
        if (mx > haveMx)
          for (int k = c; k < z; k++) colMax[k] += (mx - haveMx) / (z - c);
      }
      c += cp.second;
    }
  }
  float spacing = sx * (ncols + 1);
  float sumMin = spacing, sumMax = spacing;
  for (int c = 0; c < ncols; c++) {
    sumMin += colMin[c];
    sumMax += colMax[c];
  }
  bool autoWidth = s.width.isAuto();
  float tableW = autoWidth ? std::min(contentW, std::max(sumMin, sumMax)) : contentW;
  tableW = std::max(tableW, sumMin);
  std::vector<float> colW(ncols);
  if (s.tableLayout == TL_FIXED && !autoWidth) {
    float fixedSum = 0;
    int nfree = 0;
    for (int c = 0; c < ncols; c++) {
      if (colFixed[c] >= 0) fixedSum += colFixed[c];
      else nfree++;
    }
    float rest = std::max(0.f, tableW - spacing - fixedSum);
    for (int c = 0; c < ncols; c++) colW[c] = colFixed[c] >= 0 ? colFixed[c] : (nfree ? rest / nfree : 0);
  } else if (tableW >= sumMax) {
    float extra = tableW - sumMax;
    float base = 0;
    int nAuto = 0;
    for (int c = 0; c < ncols; c++) {
      if (colFixed[c] < 0) {
        base += colMax[c];
        nAuto++;
      }
    }
    for (int c = 0; c < ncols; c++) {
      colW[c] = colMax[c];
      if (colFixed[c] < 0 && nAuto) colW[c] += base > 0 ? extra * colMax[c] / base : extra / nAuto;
    }
    if (nAuto == 0)
      for (int c = 0; c < ncols; c++) colW[c] += extra / ncols;
  } else {
    float t = (sumMax - sumMin) > 0 ? (tableW - sumMin) / (sumMax - sumMin) : 0;
    for (int c = 0; c < ncols; c++) colW[c] = colMin[c] + (colMax[c] - colMin[c]) * t;
  }
  std::vector<float> colX(ncols + 1);
  colX[0] = sx;
  for (int c = 0; c < ncols; c++) colX[c + 1] = colX[c] + colW[c] + sx;
  tableW = colX[ncols];

  // ---- rows
  float y = captionH + sy;
  for (auto &r : rows) {
    float rowH = 0;
    const ComputedStyle &rs = S(r.tr);
    if (rs.height.isLen() && !rs.height.hasPct()) rowH = rs.height.px;
    int c = 0;
    for (auto &cp : r.cells) {
      int z = std::min(ncols, c + cp.second);
      float w = colX[z] - colX[c] - sx;
      layoutBox(cp.first, CB{w, -1}, w, false, w);
      rowH = std::max(rowH, cp.first->box->h);
      c += cp.second;
    }
    // second pass: cells take the row height; vertical-align moves content
    c = 0;
    float rowBase = 0;
    for (auto &cp : r.cells) {
      Box &cb = *cp.first->box;
      if (cb.baseline >= 0) rowBase = std::max(rowBase, cb.baseline);
    }
    for (auto &cp : r.cells) {
      int z = std::min(ncols, c + cp.second);
      Element *cell = cp.first;
      Box &cb = *cell->box;
      float contentHgt = cb.h;
      VAlign va = S(cell).verticalAlign;
      // inherited vertical-align from row/group: UA sets "middle" on groups
      if (va == VA_BASELINE) {
        for (Element *p = cell->parent; p && p != e; p = p->parent)
          if (p->cs && p->cs->verticalAlign != VA_BASELINE) {
            va = p->cs->verticalAlign;
            break;
          }
      }
      float shift = 0;
      if (va == VA_MIDDLE) shift = (rowH - contentHgt) / 2;
      else if (va == VA_BOTTOM) shift = rowH - contentHgt;
      else if (va == VA_BASELINE && cb.baseline >= 0) shift = rowBase - cb.baseline;
      float w = colX[z] - colX[c] - sx;
      if (std::fabs(rowH - cb.h) > 0.01f) {
        layoutBox(cell, CB{w, rowH}, w, false, w, rowH);
      }
      // shift in-flow content
      if (shift > 0.01f) {
        for (auto &il : cb.inlines) {
          il->y += shift;
          for (auto &f : il->frags) f.baseline += shift;
          for (auto &l : il->lines) {
            l.y += shift;
            l.baseline += shift;
          }
        }
        forEachLayoutChild(cell, [&](Node *n) {
          Element *ce = n->asElement();
          if (ce && ce->box && !isOutOfFlow(ce)) ce->box->y += shift;
        });
        // atomic inlines placed by inline layouts
        for (auto &il : cb.inlines)
          for (auto &it : il->items)
            if (it.kind == InlineItem::ATOMIC && it.el->box && it.el->parent != cell) it.el->box->y += shift;
      }
      cb.x = x0 + colX[c];
      cb.y = y0 + y;
      cb.origin = e;
      c += cp.second;
    }
    // row box (for backgrounds), relative to the table
    Box &rb = ensureBox(r.tr);
    resolveEdges(r.tr, contentW);
    rb.x = x0 + sx;
    rb.y = y0 + y;
    rb.w = tableW - 2 * sx;
    rb.h = rowH;
    rb.origin = e;
    y += rowH + sy;
  }
  // row groups span their rows
  for (Element *g : groups) {
    Box &gb = ensureBox(g);
    resolveEdges(g, contentW);
    float top = 1e9f, bot = -1e9f;
    for (auto &r : rows)
      if (r.group == g) {
        top = std::min(top, r.tr->box->y);
        bot = std::max(bot, r.tr->box->y + r.tr->box->h);
      }
    if (top > bot) top = bot = y0;
    gb.x = x0 + sx;
    gb.y = top;
    gb.w = tableW - 2 * sx;
    gb.h = bot - top;
    gb.origin = e;
  }
  // cells/rows inside groups were positioned relative to the table: fix origins
  // (rows and cells keep origin = table so absolutize is a single step).
  if (autoWidth) {
    float bp = borderPaddingH(b);
    b.w = tableW + bp;
  }
  b.scrollW = tableW + b.p[1] + b.p[3];
  b.scrollH = y + b.p[0] + b.p[2];
  if (!rows.empty() && !rows[0].cells.empty()) {
    Box &first = *rows[0].cells[0].first->box;
    if (first.baseline >= 0) b.baseline = first.y + first.baseline;
  }
  b.lastBaseline = b.baseline;
  return std::max(y, contentH >= 0 ? 0.f : 0.f);
}

// ============================================================ out of flow

void LayoutEngine::finishOutOfFlow(Element *container) {
  // process (and remove) pending boxes whose containing block is `container`
  for (size_t i = 0; i < pendingAbs_.size();) {
    Pending p = pendingAbs_[i];
    if (p.cb != container) {
      i++;
      continue;
    }
    pendingAbs_.erase(pendingAbs_.begin() + i);
    // static position relative to the container's border box: walk the origin chain
    float sx = p.sx, sy = p.sy;
    Element *o = p.el->box ? p.el->box->origin : nullptr;
    while (o && o != container && o->box) {
      Box &ob = *o->box;
      sx += ob.x;
      sy += ob.y;
      o = ob.origin;
    }
    float cbW, cbH;
    if (container) {
      Box &cb = *container->box;
      cbW = cb.w - cb.b[1] - cb.b[3];
      cbH = cb.h - cb.b[0] - cb.b[2];
      sx -= cb.b[3];
      sy -= cb.b[0];
    } else {
      cbW = vw;
      cbH = vh;
    }
    layoutOutOfFlow(p.el, container, cbW, cbH, sx, sy);
    i = 0;  // list may have changed (nested boxes)
  }
}

void LayoutEngine::layoutOutOfFlow(Element *e, Element *cbEl, float cbW, float cbH, float staticX, float staticY) {
  const ComputedStyle &s = S(e);
  Box &b = ensureBox(e);
  resolveEdges(e, cbW);
  bool lAuto = s.left.isAuto(), rAuto = s.right.isAuto(), tAuto = s.top.isAuto(), bAuto = s.bottom.isAuto();
  float L = lAuto ? 0 : s.left.resolve(cbW), R = rAuto ? 0 : s.right.resolve(cbW);
  float T = tAuto ? 0 : s.top.resolve(cbH), B = bAuto ? 0 : s.bottom.resolve(cbH);
  float ml = s.margin[3].isAuto() ? 0 : b.m[3], mr = s.margin[1].isAuto() ? 0 : b.m[1];
  float mt = s.margin[0].isAuto() ? 0 : b.m[0], mb = s.margin[2].isAuto() ? 0 : b.m[2];
  // width
  float forcedW = -1;
  bool shrink = true;
  float avail = cbW - (lAuto ? 0 : L) - (rAuto ? 0 : R) - ml - mr;
  if (s.width.isAuto() && !lAuto && !rAuto && !isReplaced(e)) {
    forcedW = std::max(0.f, avail);
    shrink = false;
  }
  float forcedH = -1;
  if (s.height.isAuto() && !tAuto && !bAuto && !isReplaced(e)) forcedH = std::max(0.f, cbH - T - B - mt - mb);
  layoutBox(e, CB{cbW, cbH}, std::max(0.f, lAuto && rAuto ? cbW - staticX - ml - mr : avail), shrink, forcedW, forcedH);
  // horizontal position
  float x;
  if (lAuto && rAuto) x = staticX + ml;
  else if (lAuto) x = cbW - R - mr - b.w;
  else if (!rAuto && s.margin[3].isAuto() && s.margin[1].isAuto()) x = L + std::max(0.f, (cbW - L - R - b.w) / 2);
  else x = L + ml;
  float y;
  if (tAuto && bAuto) y = staticY + mt;
  else if (tAuto) y = cbH - B - mb - b.h;
  else if (!bAuto && s.margin[0].isAuto() && s.margin[2].isAuto()) y = T + std::max(0.f, (cbH - T - B - b.h) / 2);
  else y = T + mt;
  b.x = x;
  b.y = y;
  b.origin = cbEl;
  b.originPadding = cbEl != nullptr;
  b.fixed = s.position == POS_FIXED || cbEl == nullptr;
  b.m[3] = s.margin[3].isAuto() ? 0 : b.m[3];
}

// ============================================================ document

void LayoutEngine::layoutDocument(Document *doc) {
  doc_ = doc;
  gen_++;
  pendingAbs_.clear();
  Element *html = doc->html;
  if (!html || !html->cs || html->cs->display == D_NONE) return;
  layoutBox(html, CB{vw, vh}, vw, false);
  Box &hb = *html->box;
  hb.x = hb.m[3];
  hb.y = hb.m[0];
  hb.origin = nullptr;
  finishOutOfFlow(nullptr);
  // anything left (containing block never laid out): place against the viewport
  while (!pendingAbs_.empty()) {
    Pending p = pendingAbs_.back();
    pendingAbs_.pop_back();
    layoutOutOfFlow(p.el, nullptr, vw, vh, p.sx, p.sy);
  }
  absolutize(doc);
}

void LayoutEngine::absolutizeBox(Element *e, float px, float py) {
  (void)px;
  (void)py;
  if (!e->box || !e->cs || e->cs->display == D_NONE) return;
  Box &b = *e->box;
  if (b.fixed && !b.origin) {
    b.absX = b.x;
    b.absY = b.y;
  } else if (b.origin && b.origin->box) {
    Box &o = *b.origin->box;
    b.absX = o.absX + b.x + (b.originPadding ? o.b[3] : 0);
    b.absY = o.absY + b.y + (b.originPadding ? o.b[0] : 0);
  } else {
    b.absX = b.x;
    b.absY = b.y;
  }
  // non-atomic inline elements: bounding box of their fragments
  if (!b.inlineRects.empty() && e->cs->display == D_INLINE && !isAtomicInline(e)) {
    RectF u;
    for (auto &r : b.inlineRects) u = u.unite(r);
    Box &o = *b.origin->box;
    b.absX = o.absX + u.x;
    b.absY = o.absY + u.y;
    b.w = u.w;
    b.h = u.h;
  }
  if (e->before) absolutizeBox(e->before, 0, 0);
  for (Node *c = e->first; c; c = c->next)
    if (c->isElement()) absolutizeBox((Element *)c, 0, 0);
  if (e->after) absolutizeBox(e->after, 0, 0);
}

void LayoutEngine::absolutize(Document *doc) {
  if (doc->html) absolutizeBox(doc->html, 0, 0);
}

// ============================================================ partial relayout

// A box whose outer size is the only thing its parent takes from it: an in-flow block in
// a block container, with its width (and every ancestor's) given by the containing block
// rather than by content - so nothing outside depends on what is inside.
// A flex item whose outer size does not depend on its content: a fixed main size it is not
// flexed away from (no grow, and laid out at exactly that size: not shrunk), and a cross size
// that is fixed or stretched to the line. Laying out its inside again cannot change the
// container's result (relayout still checks the outer box comes out the same).
static bool fixedFlexItem(Element *e) {
  const ComputedStyle &s = *e->cs;
  Element *p = e->parentElement();
  if (!p || !p->cs || !p->cs->isFlex() || s.isOutOfFlow() || (e->flags & NF_PSEUDO)) return false;
  const ComputedStyle &ps = *p->cs;
  bool row = ps.flexDirection == FD_ROW || ps.flexDirection == FD_ROW_REVERSE;
  const Box &b = *e->box;
  if (s.flexGrow != 0 || !s.flexBasis.isAuto()) return false;
  const Len &mainP = row ? s.width : s.height;
  const Len &crossP = row ? s.height : s.width;
  if (!mainP.isLen() || mainP.hasPct()) return false;
  float bp = row ? b.b[1] + b.b[3] + b.p[1] + b.p[3] : b.b[0] + b.b[2] + b.p[0] + b.p[2];
  float specified = mainP.px + (s.boxSizing == BOX_CONTENT ? bp : 0);
  if (std::fabs((row ? b.w : b.h) - specified) > 0.01f) return false;  // flexed: depends on the others
  if (crossP.isLen() && !crossP.hasPct()) return true;
  Align a = s.alignSelf == AL_AUTO ? (ps.alignItems == AL_NORMAL ? AL_STRETCH : ps.alignItems) : s.alignSelf;
  return crossP.isAuto() && a == AL_STRETCH && ps.flexWrap == FW_NOWRAP;
}

bool LayoutEngine::isRelayoutBoundary(Element *e) const {
  if (!e->box || !e->cs || !e->box->inValid) return false;
  if (fixedFlexItem(e)) return true;
  const ComputedStyle &s = *e->cs;
  if (s.display == D_NONE || s.display == D_CONTENTS || !s.isBlockLevel() || s.isOutOfFlow()) return false;
  if (e->flags & NF_PSEUDO) return false;
  Element *p = e->parentElement();
  if (!p || !p->cs || !p->box) return false;
  Display pd = p->cs->display;
  if (pd != D_BLOCK && pd != D_LIST_ITEM && pd != D_FLOW_ROOT && pd != D_INLINE_BLOCK && pd != D_TABLE_CELL &&
      pd != D_TABLE_CAPTION)
    return false;
  Element *html = doc_ ? doc_->html : nullptr;
  for (Element *a = e; a && a != html; a = a->parentElement()) {
    const Box *ab = a->box;
    if (!ab || !ab->inValid || ab->inShrink || ab->inForcedW >= 0) return false;
  }
  return true;
}

Element *LayoutEngine::relayoutBoundary(Element *e) const {
  Element *html = doc_ ? doc_->html : nullptr;
  for (Element *x = e; x; x = x->parentElement()) {
    if (x == html) return html;
    if (isRelayoutBoundary(x)) return x;
  }
  return html;
}

static void resetIntrinsic(Element *e) {
  if (e->box) {
    e->box->minW = e->box->maxW = -1;
    e->box->layoutGen = 0;
  }
  if (e->before) resetIntrinsic(e->before);
  for (Node *c = e->first; c; c = c->next)
    if (c->isElement()) resetIntrinsic((Element *)c);
  if (e->after) resetIntrinsic(e->after);
}

bool LayoutEngine::relayout(Element *e, Element **root) {
  Element *html = doc_ ? doc_->html : nullptr;
  if (!html || !html->box || !laidOut() || !e || !e->connected()) return false;
  for (Element *x = relayoutBoundary(e); x && x != html; x = relayoutBoundary(x->parentElement())) {
    Box &b = *x->box;
    const float w = b.w, h = b.h, bl = b.baseline, lbl = b.lastBaseline, sw = b.scrollW, sh = b.scrollH;
    const float rx = b.relX, ry = b.relY;
    float m[4];
    memcpy(m, b.m, sizeof m);
    resetIntrinsic(x);
    pendingAbs_.clear();
    depth_ = 0;
    layoutBox(x, CB{b.inCbW, b.inCbH}, b.inAvailW, b.inShrink, b.inForcedW, b.inForcedH);
    if (!pendingAbs_.empty()) {  // out-of-flow boxes positioned against something outside
      pendingAbs_.clear();
      return false;
    }
    bool same = b.w == w && b.h == h && b.baseline == bl && b.lastBaseline == lbl && b.scrollW == sw &&
                b.scrollH == sh && b.relX == rx && b.relY == ry && memcmp(m, b.m, sizeof m) == 0;
    if (same) {
      absolutizeBox(x, 0, 0);
      if (root) *root = x;
      return true;
    }
  }
  return false;
}

}  // namespace rt
