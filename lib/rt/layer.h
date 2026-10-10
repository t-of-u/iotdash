#pragma once
// Cached pixels for replaced content (canvas, svg) that sits 1:1 on a plain opaque
// background: rendered once into a bitmap, then copied into each band, instead of
// rasterizing the paths again for every band of every frame.

#include "image.h"
#include "render.h"

namespace rt {

struct LayerCache {
  std::shared_ptr<Image> img;
  uint64_t key = 0;
  uint16_t bg = 0;
  float fx = -1, fy = -1;  // sub-pixel screen offset it was rendered at
};

// The backdrop of e is one opaque colour: nothing between e and the nearest ancestor with
// an opaque background paints anything (other overlapping boxes are not considered).
bool solidBackdrop(Element *e, Color &bg);

// Paint docRect of e through the cache. `key` identifies the content (a change re-renders);
// `paint` draws it into the given context. Returns false when caching does not apply
// (transformed, translucent, busy backdrop, too big): the caller then paints directly.
bool paintLayered(LayerCache &lc, Element *e, const PaintCtx &pc, const RectF &docRect, uint64_t key,
                  const std::function<void(const PaintCtx &)> &paint);

}  // namespace rt
