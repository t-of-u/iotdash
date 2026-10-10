#pragma once
// Layout: block flow, inline formatting (line breaking), flexbox, grid, tables, positioned
// boxes and scroll containers. Results live in Element::box.

#include "dom.h"
#include "font.h"
#include "style.h"

namespace rt {

struct InlineItem {
  enum Kind : uint8_t { TEXT, ATOMIC, OPEN, CLOSE, BREAK, OUT_OF_FLOW } kind;
  Element *el = nullptr;  // owner element (style) / atomic / inline element
  Text *text = nullptr;
  FontRef font;
  std::vector<ShapedGlyph> glyphs;
  float width = 0;
  bool fallback = false;  // uses the fallback (Thai) face metrics
};

struct LineFrag {
  enum Kind : uint8_t { TEXT, ATOMIC, ELLIPSIS } kind;
  uint16_t item;     // index into InlineLayout::items
  uint32_t g0, g1;   // glyph range (TEXT)
  float x;           // left edge, relative to the container's border box
  float w;
  float baseline;    // relative to the container's border box
  float shift;       // pen offset to subtract from glyph x (glyph.x - shift = position in frag)
};

struct LineBox {
  float y = 0, h = 0, baseline = 0;  // baseline relative to the container's border box
  uint32_t f0 = 0, f1 = 0;
  float width = 0;
};

struct InlineLayout {
  std::vector<InlineItem> items;
  std::vector<LineFrag> frags;
  std::vector<LineBox> lines;
  float y = 0, h = 0;  // vertical extent relative to the container's border box
  float maxRight = 0;  // widest line (for scrollable overflow)
};

struct Box {
  // Geometry. x/y: border box relative to origin's border box (or, for absolutely
  // positioned boxes, to origin's padding box).
  float x = 0, y = 0, w = 0, h = 0;
  Element *origin = nullptr;
  bool originPadding = false;  // x/y relative to origin's padding box (abs/fixed)
  bool fixed = false;          // relative to the viewport
  float absX = 0, absY = 0;    // document position (scroll offsets not applied)
  float m[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0}, p[4] = {0, 0, 0, 0};  // t r b l
  float relX = 0, relY = 0;    // position: relative offsets (already in x/y)
  float scrollW = 0, scrollH = 0;  // scrollable overflow extent (padding box origin)
  float baseline = -1;         // first baseline from the border-box top (-1: none)
  float lastBaseline = -1;
  bool inFlowInline = false;   // laid out by an inline formatting context
  std::vector<std::unique_ptr<InlineLayout>> inlines;  // owned inline formatting contexts
  std::vector<RectF> inlineRects;  // inline element fragments (relative to origin border box)
  // paint bookkeeping
  RectI lastInk;
  bool painted = false;
  // intrinsic widths cache (border box)
  float minW = -1, maxW = -1;
  uint32_t layoutGen = 0;
  // inputs of the last layoutBox() call, so the subtree can be laid out again alone
  float inCbW = 0, inCbH = -1, inAvailW = 0, inForcedW = -1, inForcedH = -1;
  bool inShrink = false, inValid = false;

  float contentX() const { return b[3] + p[3]; }
  float contentY() const { return b[0] + p[0]; }
  float contentW() const { return w - b[1] - b[3] - p[1] - p[3]; }
  float contentH() const { return h - b[0] - b[2] - p[0] - p[2]; }
  RectF borderRect() const { return {absX, absY, w, h}; }
  RectF paddingRect() const { return {absX + b[3], absY + b[0], w - b[1] - b[3], h - b[0] - b[2]}; }
  RectF contentRect() const { return {absX + contentX(), absY + contentY(), contentW(), contentH()}; }
  RectF marginRect() const { return {absX - m[3], absY - m[0], w + m[1] + m[3], h + m[0] + m[2]}; }
};

// Hooks the layout needs from the rest of the engine (images, form controls).
struct LayoutHost {
  virtual bool intrinsicSize(Element *e, float &w, float &h) = 0;  // replaced content natural size
  virtual std::string controlText(Element *e) = 0;                 // text shown in a form control
  virtual ~LayoutHost() {}
};

class LayoutEngine {
 public:
  FontManager *fonts = nullptr;
  LayoutHost *host = nullptr;
  float vw = 800, vh = 480;

  void layoutDocument(Document *doc);
  // Lay out again after a change inside e, touching as little as possible: the subtree of
  // the nearest ancestor whose width cannot depend on its content, widened while its outer
  // size changes. Returns false when the whole document must be laid out instead.
  // *root receives the subtree that was laid out.
  bool relayout(Element *e, Element **root);
  bool laidOut() const { return gen_ != 0; }
  // Recompute absolute positions (after layout or a relative-offset change).
  void absolutize(Document *doc);

  FontRef fontFor(const ComputedStyle &cs) const;
  static bool isReplaced(Element *e);
  static bool isAtomicInline(Element *e);
  static float normalLineHeight(const FontRef &f);

  // Layout children for a given element (including ::before/::after) in order.
  template <class F>
  static void forEachLayoutChild(Element *e, F &&f) {
    if (e->before) f((Node *)e->before);
    for (Node *c = e->first; c; c = c->next) {
      if (c->isElement()) {
        Element *ce = (Element *)c;
        if (ce->cs && ce->cs->display == D_CONTENTS) {
          forEachLayoutChild(ce, f);
          continue;
        }
      }
      f(c);
    }
    if (e->after) f((Node *)e->after);
  }

 private:
  struct CB {        // containing block for in-flow children
    float w = 0;     // content width
    float h = -1;    // content height if definite, else -1
  };
  struct MarginInfo {
    float top = 0, bottom = 0;      // collapsed margins passing through this box
    bool topThrough = false, bottomThrough = false;
  };

  void layoutBox(Element *e, const CB &cb, float availW, bool shrinkToFit, float forcedW = -1, float forcedH = -1);
  void computeHorizontal(Element *e, const CB &cb, float availW, bool shrink, float forcedW);
  float layoutBlockContents(Element *e, float contentW, float contentH, MarginInfo &mi);
  float layoutInlineContents(Element *container, InlineLayout &il, std::vector<Node *> &nodes, float contentW, float y0);
  void collectInlineItems(Element *container, InlineLayout &il, Node *n, bool &prevSpace, float contentW);
  void addTextItem(InlineLayout &il, Element *owner, Text *t, bool &prevSpace);
  float layoutFlex(Element *e, float contentW, float contentH, float minContentH = 0, float maxContentH = 1e9f);
  float layoutGrid(Element *e, float contentW, float contentH);
  float layoutTable(Element *e, float contentW, float contentH);
  void layoutOutOfFlow(Element *e, Element *cbEl, float cbW, float cbH, float staticX, float staticY);
  void layoutReplaced(Element *e, float contentW, float contentH);
  void finishOutOfFlow(Element *container);

  float minContentWidth(Element *e);  // border-box width
  float maxContentWidth(Element *e);
  void intrinsicInline(Element *e, float &minW, float &maxW);
  float inlineItemsWidth(Element *container, bool minMode);

  void absolutizeBox(Element *e, float px, float py);
  bool isRelayoutBoundary(Element *e) const;
  Element *relayoutBoundary(Element *e) const;

  struct Pending {
    Element *el;
    Element *cb;
    float sx, sy;  // static position relative to cb's border box
  };
  std::vector<Pending> pendingAbs_;
  Document *doc_ = nullptr;
  uint32_t gen_ = 0;
  int depth_ = 0;
};

Element *containingBlockFor(Element *e);

}  // namespace rt
