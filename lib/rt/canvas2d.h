#pragma once

#include "layer.h"
// <canvas> 2D context. Drawing calls are recorded as resolution-independent commands (paths
// already in canvas pixels) and replayed into each SRAM band while the page is painted - the
// CPU never writes a canvas bitmap in PSRAM. A full clearRect() resets the list; very long
// lists are flattened into a bitmap.

#include "image.h"
#include "raster.h"
#include "render.h"

extern "C" {
#include "quickjs.h"
}

namespace rt {

class JsHost;
class Engine;

struct CanvasPaint {
  enum Kind : uint8_t { COLOR, GRADIENT, PATTERN } kind = COLOR;
  Color color = kBlack;
  std::shared_ptr<GradientPaint> grad;  // geometry in canvas pixels
  std::shared_ptr<Image> pattern;
  bool repeatX = true, repeatY = true;
};

class Canvas2D {
 public:
  Canvas2D(Engine &eng, Element *el);
  Element *element() const { return el_; }
  int width() const { return w_; }
  int height() const { return h_; }
  void resize(int w, int h);  // clears

  // ---- state
  struct State {
    Affine ctm;
    CanvasPaint fill, stroke;
    float lineWidth = 1, miter = 10, alpha = 1, dashOffset = 0;
    LineCap cap = CAP_BUTT;
    LineJoin join = JOIN_MITER;
    std::vector<float> dash;
    std::string font = "10px sans-serif";
    FontRef fontRef;
    int textAlign = 0;     // 0 start 1 left 2 center 3 right 4 end
    int textBaseline = 0;  // 0 alphabetic 1 top 2 middle 3 bottom 4 hanging 5 ideographic
    Color shadowColor = kTransparent;
    float shadowBlur = 0, shadowX = 0, shadowY = 0;
    bool smoothing = true;
    RectF clip{-1e9f, -1e9f, 2e9f, 2e9f};  // canvas px (axis aligned)
    bool composite = true;                 // source-over
  };
  State st;
  std::vector<State> stack;
  Path path;  // current path in canvas px (transformed at record time)

  void save() { stack.push_back(st); }
  void restore() {
    if (!stack.empty()) {
      st = stack.back();
      stack.pop_back();
    }
  }
  void setFont(const std::string &css);

  // ---- drawing
  void fillRect(float x, float y, float w, float h);
  void strokeRect(float x, float y, float w, float h);
  void clearRect(float x, float y, float w, float h);
  void fill(const Path &p, FillRule rule);
  void stroke(const Path &p);
  void clip(const Path &p);
  void fillText(const std::string &text, float x, float y, float maxWidth, bool strokeIt);
  float measureText(const std::string &text, float &ascent, float &descent);
  void drawImage(std::shared_ptr<Image> img, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh);
  void putImageData(const uint8_t *rgba, int w, int h, int dx, int dy);
  std::vector<uint8_t> getImageData(int x, int y, int w, int h);

  // ---- rendering into a band
  // Paint into a band. When the canvas sits 1:1 on a plain opaque background (the usual
  // chart on a card) its pixels are rendered once into a cached layer and copied per band;
  // otherwise the command list is replayed.
  void paint(const PaintCtx &pc, const RectF &contentBox);

 private:
  struct Cmd {
    enum Kind : uint8_t { FILL, TEXT, IMAGE, BG } kind = FILL;
    Path path;  // canvas px
    FillRule rule = FR_NONZERO;
    CanvasPaint paint;
    float alpha = 1;
    RectF clip;
    // text
    struct G {
      const FntGlyph *g;
      float x, y;
    };
    std::vector<G> glyphs;
    FontRef font;
    Affine xf;  // glyph space -> canvas px
    // image
    std::shared_ptr<Image> img;
    RectF src, dst;  // dst in canvas px before xf
    bool smooth = true;
    // cached screen geometry (and gradient), rebuilt when the canvas moves
    Path screen;
    Affine screenKey;
    std::shared_ptr<GradientPaint> screenGrad;
    bool haveScreen = false;
  };
  void push(Cmd &&c);
  void invalidate();
  Color effectiveBackground() const;
  void flatten();
  void addShadow(const Cmd &c);
  bool paintFromStyle(Cmd &c, const CanvasPaint &p);

  Engine &eng_;
  Element *el_;
  int w_ = 300, h_ = 150;
  std::vector<Cmd> cmds_;
  std::shared_ptr<Image> flat_;  // flattened earlier commands (opaque, effective bg)
  size_t pathBytes_ = 0;
  uint32_t version_ = 0;  // bumped on every change to the drawing
  LayerCache layer_;  // rendered pixels over a solid backdrop (see layer.h)
  void paintCommands(const PaintCtx &pc, const RectF &contentBox);
};

JSClassID registerCanvasClass(JSRuntime *rt);
JSValue canvasContext(JSContext *ctx, JsHost *host, Element *canvasEl);

}  // namespace rt
