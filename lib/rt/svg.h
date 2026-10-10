#pragma once
// Inline <svg>: shapes are flattened into paths once (in viewBox units) and rasterized with
// the same anti-aliased scanline fill as canvas, so icons stay crisp at any size.

#include "render.h"

namespace rt {

bool parseSvgPath(const std::string &d, Path &out);
bool parseSvgTransform(const std::string &s, Affine &out);

struct SvgShape {
  Path path;  // viewBox units (element transforms applied)
  FillRule rule = FR_NONZERO;
  bool fill = true, stroke = false;
  Color fillColor = kBlack, strokeColor = kBlack;
  bool fillCurrent = false, strokeCurrent = false;
  float strokeWidth = 1;
  LineCap cap = CAP_BUTT;
  LineJoin join = JOIN_MITER;
  std::vector<float> dash;
  float opacity = 1;
};

struct SvgCache {
  std::vector<SvgShape> shapes;
  float vbX = 0, vbY = 0, vbW = 0, vbH = 0;
  bool hasViewBox = false;
  int align = 5;      // preserveAspectRatio: 0 none, 1..9 xMin/Mid/Max x yMin/Mid/Max
  bool slice = false;
  // cached screen paths (valid when ready and key matches)
  Affine key;
  std::vector<Path> fills, strokes;
  bool ready = false;
};

std::shared_ptr<SvgCache> buildSvg(Element *svg);
void paintSvg(Element *svg, SvgCache &cache, const PaintCtx &pc, const RectF &contentBox, Color current);

}  // namespace rt
