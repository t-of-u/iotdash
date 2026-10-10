#pragma once
// Touch input: pointer/touch/mouse/click events, :active, drag scrolling with momentum and
// scroll snapping, slider capture, swipe-back.

#include "render.h"

namespace rt {

class Engine;

class InputController {
 public:
  explicit InputController(Engine &e) : eng_(e) {}
  void pointer(int id, uint8_t phase, float x, float y);
  void tick(double nowMs);
  bool busy() const;
  void reset();
  void forget(Element *e);
  void clampScrolls();
  // programmatic scrolling (element.scrollTo / scrollIntoView / window.scrollTo)
  void scrollTo(Element *e, float x, float y, bool smooth);
  void scrollIntoView(Element *e, bool smooth);
  float maxScrollX(Element *e) const;
  float maxScrollY(Element *e) const;
  bool canScroll(Element *e, bool vertical) const;
  void setPageShift(float y);

 private:
  struct Sample {
    double t;
    float x, y;
  };
  Element *scrollerFor(Element *target, bool vertical, float delta) const;
  void setActive(Element *e, bool on);
  void startMomentum(Element *s);
  void snap(Element *s, float vx, float vy);
  void setScroll(Element *s, float x, float y);
  void dispatch(Element *t, const char *type, float x, float y, bool cancelable = true);

  Engine &eng_;
  bool down_ = false;
  int id_ = 0;
  float x0_ = 0, y0_ = 0, x_ = 0, y_ = 0;
  double t0_ = 0;
  Element *target_ = nullptr;
  Element *active_ = nullptr;
  Element *scroller_ = nullptr;
  bool scrolling_ = false, vertical_ = true, captured_ = false, cancelledClick_ = false;
  bool swipeBack_ = false;
  float sx0_ = 0, sy0_ = 0;  // scroll position at drag start
  std::vector<Sample> samples_;
  // momentum / smooth scroll animations
  struct Anim {
    Element *el;
    float vx, vy;      // momentum velocity px/s
    float tx, ty;      // smooth target (when smooth)
    bool smooth;
    double last;
  };
  std::vector<Anim> anims_;
  double lastTick_ = 0;
  float pageShift_ = 0, pageShiftTarget_ = 0;
};

}  // namespace rt
