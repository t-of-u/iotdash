#pragma once
// Optional per-element state that most elements never need.

#include "css.h"

namespace rt {

struct ComputedStyle;
struct Image;
class Canvas2D;
struct SvgCache;
struct LayerCache;

struct ElementExtra {
  // scrolling (overflow: auto/scroll, and the viewport on <html>)
  float scrollX = 0, scrollY = 0;
  float velX = 0, velY = 0;       // momentum (px/s)
  float scrollbarAlpha = 0;       // fading scroll indicator
  uint32_t scrollbarUntil = 0;    // ms timestamp until which the indicator stays

  // pseudo-element styles for form controls (::placeholder, slider parts, ::backdrop, ::marker)
  std::shared_ptr<ComputedStyle> pseudoStyles[PE_UNKNOWN + 1];

  // form controls
  std::string value;          // current value (input/textarea), when dirty
  bool valueDirty = false;    // value differs from the value attribute / text
  int caret = 0;              // byte offset of the caret
  float textScroll = 0;       // horizontal scroll of a single-line input
  float caretX = 0, caretY = 0, caretH = 0;  // last painted caret (screen)
  int selectedIndex = -2;     // <select>: -2 = derive from options
  bool checkedDirty = false;  // checkbox/radio checkedness changed by the user/script

  // images (<img>, background-image urls)
  std::shared_ptr<Image> image;
  std::string imageSrc;

  // <canvas>
  std::shared_ptr<Canvas2D> canvas;

  // <svg>
  std::shared_ptr<SvgCache> svg;
  std::shared_ptr<LayerCache> layer;  // cached pixels (canvas keeps its own)

  // list item ordinal (computed during layout)
  int ordinal = 0;

  // dialog / details
  bool modal = false;
};

}  // namespace rt
