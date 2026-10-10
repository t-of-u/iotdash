#pragma once
// Decoded images (RGB565 + optional A8 alpha). Bundled images are pre-converted by the
// compiler ("IMG1"); remote images (fetch) are decoded at runtime when a decoder exists.

#include "raster.h"

namespace rt {

#pragma pack(push, 1)
struct ImgHeader {
  char magic[4];  // "IMG1"
  uint16_t w, h;
  uint16_t flags;  // IMGF_*
  uint16_t reserved;
};
#pragma pack(pop)
enum ImgFlags : uint16_t { IMGF_ALPHA = 1, IMGF_MASK = 2 /* A8 only: tinted with the CSS colour */ };

struct Image {
  int w = 0, h = 0;
  bool mask = false;
  const uint16_t *px = nullptr;
  const uint8_t *alpha = nullptr;
  std::vector<uint8_t> owned;  // when decoded at runtime
  std::shared_ptr<void> keep;  // backing data read from a file (px/alpha point into it)
  ImageView view() const {
    ImageView v;
    v.px = px;
    v.alpha = alpha;
    v.w = w;
    v.h = h;
    v.stride = w;
    return v;
  }
  static std::shared_ptr<Image> fromImg1(const uint8_t *data, size_t len);
};

}  // namespace rt
