#pragma once
// Tiny cycle-accurate profiler: RT_PROF(slot) times the enclosing scope. Totals are read and
// reset with profReport(). Enabled at runtime (profEnabled) so it costs a branch when off.

#include "rt_base.h"

namespace rt {

enum ProfSlot : uint8_t {
  PF_PAINT, PF_BAND_BG, PF_ITEM_BOX, PF_SHADOW, PF_BACKGROUND, PF_BORDER, PF_TEXT, PF_REPLACED, PF_DMA_WAIT,
  PF_STYLE, PF_LAYOUT, PF_BUILD, PF_DAMAGE, PF_SCRIPT, PF_SHADOW_MASK, PF_SHADOW_PX,
  PF_CANVAS, PF_SVG, PF_WIDGET, PF_IMAGE, PF_PATH, PF_GRAD, PF_GLYPH, PF_BGFILL, PF_BLIT, PF_MSYNC, PF_FRAME, PF__COUNT
};

uint32_t cycles();  // platform hook (CPU cycle counter or ns on the host)
extern bool profEnabled;
extern thread_local bool profMuted;  // set on helper threads (counters are not atomic)
extern uint64_t profTotal[PF__COUNT];
extern uint32_t profCalls[PF__COUNT];
std::string profReport(bool reset);

struct ProfScope {
  uint8_t slot;
  uint32_t t0;
  bool on;
  explicit ProfScope(uint8_t s) : slot(s), t0(0), on(profEnabled && !profMuted) {
    if (on) t0 = cycles();
  }
  ~ProfScope() {
    if (on) {
      profTotal[slot] += cycles() - t0;
      profCalls[slot]++;
    }
  }
};
#define RT_PROF(slot) ::rt::ProfScope _prof_##slot(::rt::slot)

}  // namespace rt
