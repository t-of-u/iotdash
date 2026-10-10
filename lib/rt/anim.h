#pragma once
// CSS transitions, @keyframes animations and Element.animate(): interpolated values are
// written straight into the element's computed style each frame.

#include "style.h"

namespace rt {

enum AnimProp : uint8_t {
  AP_OPACITY, AP_TRANSFORM, AP_COLOR, AP_BG_COLOR, AP_BORDER_TOP_COLOR, AP_BORDER_RIGHT_COLOR,
  AP_BORDER_BOTTOM_COLOR, AP_BORDER_LEFT_COLOR, AP_OUTLINE_COLOR, AP_WIDTH, AP_HEIGHT, AP_MIN_WIDTH,
  AP_MIN_HEIGHT, AP_MAX_WIDTH, AP_MAX_HEIGHT, AP_TOP, AP_RIGHT, AP_BOTTOM, AP_LEFT, AP_MARGIN_TOP,
  AP_MARGIN_RIGHT, AP_MARGIN_BOTTOM, AP_MARGIN_LEFT, AP_PADDING_TOP, AP_PADDING_RIGHT, AP_PADDING_BOTTOM,
  AP_PADDING_LEFT, AP_RADIUS_TL, AP_RADIUS_TR, AP_RADIUS_BR, AP_RADIUS_BL, AP_FONT_SIZE, AP_LETTER_SPACING,
  AP_BOX_SHADOW, AP_ROW_GAP, AP_COLUMN_GAP, AP_FLEX_GROW, AP_FLEX_BASIS, AP_VISIBILITY, AP_BORDER_TOP_WIDTH,
  AP_BORDER_RIGHT_WIDTH, AP_BORDER_BOTTOM_WIDTH, AP_BORDER_LEFT_WIDTH, AP_OUTLINE_WIDTH, AP_OUTLINE_OFFSET,
  AP_LINE_HEIGHT, AP_TEXT_SHADOW, AP_BG_POSITION_X, AP_BG_POSITION_Y, AP_FILTER, AP_ACCENT_COLOR,
  AP_TEXT_DECO_COLOR, AP_Z_INDEX, AP_DISPLAY,
  AP__COUNT
};

int animPropByName(const std::string &cssName);  // -1 if not animatable
bool animPropAffectsLayout(AnimProp p);

struct AnimValue {
  float f = 0;
  Len len;
  Color color;
  std::shared_ptr<std::vector<TransformFn>> tf;
  std::shared_ptr<std::vector<Shadow>> sh;
  std::shared_ptr<std::vector<FilterFn>> filter;
  int i = 0;
};

struct Running {
  Element *el = nullptr;
  AnimProp prop;
  bool keyframes = false;
  bool held = false;     // finished with fill: forwards
  bool waapi = false;    // created by Element.animate()
  int id = 0;            // groups the tracks of one animation
  Atom name = 0;         // @keyframes name
  // transition
  AnimValue from, to;
  // keyframes track
  struct Key {
    float offset;
    AnimValue v;
    bool hasTiming = false;
    TimingFn timing;
  };
  std::vector<Key> keys;
  double start = 0, duration = 0, delay = 0;
  TimingFn timing;
  float iterations = 1;
  uint8_t direction = 0, fill = 0;
  bool paused = false;
  double pausedAt = 0;
  float playbackRate = 1;
  AnimValue current;
  bool started = false;
};

class Animator {
 public:
  // Called by the style engine with the element's previous and freshly cascaded style.
  void styleHook(Element *e, const ComputedStyle *old, ComputedStyle &nw, const StyleEngine &se);
  // Advance animations. Returns bit flags (SD_PAINT/SD_LAYOUT/SD_STACKING) of what changed.
  uint8_t tick(double nowMs, const std::function<void(Element *, uint8_t)> &dirty);
  bool active() const { return !runs_.empty() && anyRunning(); }
  bool anyRunning() const;
  void cancel(Element *e);  // element removed
  void clear() { runs_.clear(); }  // the document goes away
  void setNow(double now) { now_ = now; }
  // Web Animations API: keyframes as (offset, prop name, value) triples
  int animate(Element *e, const std::vector<std::pair<float, std::vector<std::pair<std::string, std::string>>>> &frames,
              double duration, double delay, const TimingFn &easing, float iterations, uint8_t direction, uint8_t fill,
              const StyleEngine &se);
  void cancelAnimation(int id);
  void finishAnimation(int id);
  void pauseAnimation(int id, bool pause);
  bool animationFinished(int id) const;
  // events: (element, "transitionend"/"animationend"/"animationstart"/"animationiteration", name)
  std::function<void(Element *, const char *, const std::string &)> onEvent;
  std::function<void(int)> onWaapiFinish;

 private:
  void startKeyframes(Element *e, const AnimationSpec &spec, const ComputedStyle &base, const StyleEngine &se);
  bool computeValue(Running &r, double now, bool &finished);
  std::vector<Running> runs_;
  double now_ = 0;
  int nextId_ = 1;
};

AnimValue animGet(const ComputedStyle &cs, AnimProp p);
void animSet(ComputedStyle &cs, AnimProp p, const AnimValue &v);
AnimValue animLerp(AnimProp p, const AnimValue &a, const AnimValue &b, float t);
bool animEqual(AnimProp p, const AnimValue &a, const AnimValue &b);

}  // namespace rt
