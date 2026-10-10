#pragma once
// Paint order (stacking contexts), element painting into bands, hit testing and damage
// tracking.

#include "image.h"
#include "layout.h"

namespace rt {

struct PaintItem {
  Element *el;
  RectI clip;        // screen clip from ancestor overflow (and the viewport)
  float dx, dy;      // screen = document + (dx, dy) (scroll offsets)
  uint8_t alpha;     // accumulated opacity
  int16_t xf;        // transform index (-1 = none)
  RectI ink;         // screen bounds this item paints (for culling / damage)
};

// Element-specific painting supplied by the engine (form controls, images, canvas, svg).
struct PaintHost {
  // Paint replaced/widget content inside the content box. Return true if handled.
  virtual bool paintReplaced(Element *e, const struct PaintCtx &pc) = 0;
  virtual std::shared_ptr<Image> bgImage(const std::string &url) = 0;
  virtual ~PaintHost() {}
};

struct PaintCtx {
  const Band *band;
  RectI clip;        // intersection of item clip, band and dirty area
  float dx, dy;
  uint8_t alpha;
  const Affine *xf;  // nullptr: translation only
  FontManager *fonts;
  LayoutEngine *layout;
  RasterTarget target() const { return RasterTarget{band, clip}; }
  // map a document rect to screen (translation only)
  RectF map(const RectF &r) const { return r.translated(dx, dy); }
};

class Renderer {
 public:
  LayoutEngine *layout = nullptr;
  FontManager *fonts = nullptr;
  PaintHost *host = nullptr;
  int width = 800, height = 480;
  float pageShiftY = 0;  // page moved up to keep a focused field above the keyboard

  // Rebuild the paint list after layout/scroll/transform/opacity changes.
  void build(Document *doc);
  // Damage: rects that changed since the last build (old and new ink of moved/changed items).
  void collectDamage(std::vector<RectI> &out, const std::function<bool(Element *)> &paintDirty);
  // Paint everything intersecting `area` into the band.
  void paint(const Band &band, const RectI &area);
  // Topmost element at a screen point (nullptr = none).
  Element *hitTest(float x, float y, bool forPointer = true) const;
  // Screen rect of an element (border box, after scroll; transforms approximated by bounds).
  RectF screenRect(Element *e) const;
  const std::vector<PaintItem> &items() const { return items_; }
  const PaintItem *itemFor(Element *e) const;
  void forget(Element *e);  // element removed: drop references
  void reset();             // the document goes away: drop every reference
  // Paint a single element's own decorations (used by widgets).
  void paintElementBox(Element *e, const PaintCtx &pc);
  void paintText(Element *container, InlineLayout &il, const PaintCtx &pc);

 private:
  struct St {
    RectI clip;
    float dx, dy;
    float alpha;
    int16_t xf;
    Affine m;  // accumulated transform (document -> screen before dx/dy)
    bool hasXf;
  };
  void buildSC(Element *root, const St &st, bool rootIsSC);
  void walkFlow(Element *e, const St &st, std::vector<std::pair<Element *, St>> &positioned,
                std::vector<std::pair<Element *, St>> &negZ, std::vector<std::pair<Element *, St>> &posZ);
  St childState(Element *parent, const St &st) const;
  St selfState(Element *e, const St &st);
  void addItem(Element *e, const St &st);
  RectI inkRect(Element *e, const St &st) const;
  void paintItem(const PaintItem &it, const Band &band, const RectI &area);
  void paintBackground(Element *e, const PaintCtx &pc, const RectF &border, const float radii[4]);
  void paintBorder(Element *e, const PaintCtx &pc, const RectF &border, const float radii[4]);
  void paintInlineDecorations(Element *container, InlineLayout &il, const PaintCtx &pc);
  void paintMarker(Element *e, const PaintCtx &pc);
  void paintScrollbar(Element *e, const PaintCtx &pc);

  std::vector<PaintItem> items_;
  std::vector<Affine> xforms_;
  std::unordered_map<Element *, size_t> index_;
  std::vector<RectI> gone_;  // painted last time, not in the list any more (display: none)
};

void borderRadii(Element *e, const RectF &border, float out[4]);
void insetRadii(const float in[4], float l, float t, float r, float b, float out[4]);
Affine elementTransform(Element *e);  // CSS transform about transform-origin, in document coords

}  // namespace rt
