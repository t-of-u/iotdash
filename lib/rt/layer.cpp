#include "layer.h"

#include "dom.h"
#include "style.h"

namespace rt {

bool solidBackdrop(Element *e, Color &bg) {
  for (Element *p = e; p; p = p->parent) {
    if (p->nodeType != NT_ELEMENT || !p->cs) continue;
    const ComputedStyle &s = *p->cs;
    if (s.bgImages || s.opacity < 1 || s.hasTransform()) return false;
    if (s.bgColor.a == 255) {
      bg = s.bgColor;
      return true;
    }
    if (s.bgColor.a != 0) return false;
  }
  bg = kWhite;  // the page canvas (no opaque html/body background)
  return true;
}

bool paintLayered(LayerCache &lc, Element *e, const PaintCtx &pc, const RectF &docRect, uint64_t key,
                  const std::function<void(const PaintCtx &)> &paint) {
  if (pc.xf || pc.alpha != 255 || (gDebugFlags & 4)) return false;
  Color bg;
  if (!solidBackdrop(e, bg)) return false;
  float ox = pc.dx + docRect.x, oy = pc.dy + docRect.y;
  RectI outer = RectF{ox, oy, docRect.w, docRect.h}.outer();
  if (outer.w <= 0 || outer.h <= 0 || outer.w * outer.h > 400 * 480) return false;
  RectI clip = pc.clip.intersect(outer);
  if (clip.empty()) return true;
  // whole-pixel moves (scrolling) reuse the layer: only the dither phase shifts
  float fx = ox - std::floor(ox), fy = oy - std::floor(oy);
  uint16_t bg565 = bg.to565();
  std::shared_ptr<Image> L;
  {
    PaintLock lock;  // the first band (on either core) renders the layer
    if (!lc.img || lc.key != key || lc.bg != bg565 || lc.fx != fx || lc.fy != fy || lc.img->w != outer.w ||
        lc.img->h != outer.h) {
      auto img = std::make_shared<Image>();
      img->w = outer.w;
      img->h = outer.h;
      img->owned.resize((size_t)outer.w * outer.h * 2);
      uint16_t *px = (uint16_t *)img->owned.data();
      for (size_t i = 0, n = (size_t)outer.w * outer.h; i < n; i++) px[i] = bg565;
      img->px = px;
      Band b{px, outer.w, outer.x, outer.y, outer.w, outer.h};
      PaintCtx lp = pc;
      lp.band = &b;
      lp.clip = outer;
      paint(lp);
      lc.img = img;
      lc.key = key;
      lc.bg = bg565;
      lc.fx = fx;
      lc.fy = fy;
    }
    L = lc.img;
  }
  const uint16_t *src = L->px;
  for (int y = clip.y; y < clip.bottom(); y++)
    memcpy(pc.band->at(clip.x, y), src + (size_t)(y - outer.y) * outer.w + (clip.x - outer.x), (size_t)clip.w * 2);
  return true;
}

}  // namespace rt
