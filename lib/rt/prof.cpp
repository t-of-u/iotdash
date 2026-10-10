#include "prof.h"

namespace rt {

bool profEnabled = false;
thread_local bool profMuted = false;
uint64_t profTotal[PF__COUNT];
uint32_t profCalls[PF__COUNT];

std::string profReport(bool reset) {
  static const char *names[] = {"paint", "band-bg", "item-box", "shadow", "background", "border", "text", "replaced",
                                "dma-wait", "style", "layout", "build", "damage", "script", "shadow-mask", "shadow-px", "canvas", "svg", "widget", "image", "path", "grad", "glyph", "bg-fill", "blit", "msync", "frame"};
  std::string out;
  char buf[96];
  for (int i = 0; i < PF__COUNT; i++) {
    if (!profCalls[i]) continue;
    snprintf(buf, sizeof buf, "%s %.1f/%u\n", names[i], profTotal[i] / 240000.0, profCalls[i]);
    out += buf;
  }
  if (reset) {
    memset(profTotal, 0, sizeof profTotal);
    memset(profCalls, 0, sizeof profCalls);
  }
  return out;
}

}  // namespace rt
