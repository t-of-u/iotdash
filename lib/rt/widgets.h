#pragma once
// Form controls (painting + behaviour), images/canvas/svg hooks, and the runtime's own
// overlays: on-screen keyboard (Thai Kedmanee / English / numeric), <select> popup and
// date/time/colour pickers. Overlays are ordinary internal DOM styled by internalCss().

#include "render.h"

namespace rt {

class Engine;

class Widgets {
 public:
  explicit Widgets(Engine &e) : eng_(e) {}
  static const char *internalCss();

  void init(Element *e);
  void attrChanged(Element *e, Atom name);
  void forget(Element *e);
  bool intrinsic(Element *e, float &w, float &h);
  bool paint(Element *e, const PaintCtx &pc);
  void afterLayout();
  void tick(double nowMs);
  uint64_t nextWakeMs() const;
  void closeAll();

  void focusChanged(Element *old, Element *nw);
  void typeText(const std::string &utf8);
  void key(const std::string &key);

  // pointer: returns true when the widget captures the gesture (slider drag)
  bool pointerDown(Element *target, float x, float y);
  void pointerMove(float x, float y);
  void pointerUp(float x, float y, bool cancelled);
  bool dragging() const { return drag_ != nullptr; }
  // default action on click; returns true if handled
  bool activate(Element *target, float x, float y);
  bool isInternal(Element *e) const;
  bool internalTap(Element *target);
  bool keyboardOpen() const { return kb_ != nullptr && kbOpen_; }
  float keyboardHeight() const;

  // <select>
  std::vector<Element *> options(Element *sel) const;
  int selectedIndex(Element *sel) const;
  void setSelectedIndex(Element *sel, int i, bool fromUser);
  std::string selectValue(Element *sel) const;
  void selectSetValue(Element *sel, const std::string &v);

  static std::string inputType(Element *e);
  static bool isTextInput(Element *e);

 private:
  // text editing
  void insertText(Element *e, const std::string &s);
  void backspace(Element *e);
  void moveCaret(Element *e, int dir);
  void placeCaret(Element *e, float x, float y);
  void ensureCaretVisible(Element *e);
  void fireInput(Element *e);
  void fireChange(Element *e);
  std::string displayText(Element *e, bool &placeholder);
  void paintTextControl(Element *e, const PaintCtx &pc);
  void paintTextarea(Element *e, const PaintCtx &pc);
  void paintCheck(Element *e, const PaintCtx &pc, bool radio);
  void paintRange(Element *e, const PaintCtx &pc);
  void paintProgress(Element *e, const PaintCtx &pc, bool meter);
  void paintSelect(Element *e, const PaintCtx &pc);
  void paintColor(Element *e, const PaintCtx &pc);
  void paintImage(Element *e, const PaintCtx &pc);
  void paintButtonLabel(Element *e, const PaintCtx &pc, const std::string &label);
  void paintCenteredText(Element *e, const PaintCtx &pc, const std::string &text, Color c, float left = 0);
  float rangeValue(Element *e, float &mn, float &mx, float &step);
  void setRangeFromX(Element *e, float x);

  // overlays
  void openKeyboard(Element *forInput);
  void closeKeyboard();
  void buildKeyboard();
  void openSelect(Element *sel);
  void openDatePicker(Element *input);
  void openTimePicker(Element *input);
  void openColorPicker(Element *input);
  void closePopup();
  Element *makeOverlay(const std::string &html);
  void shiftForKeyboard();

  Engine &eng_;
  Element *drag_ = nullptr;
  float dragStartVal_ = 0;
  // caret blink
  double caretT0_ = 0;
  bool caretOn_ = true;
  double nextBlink_ = 0;
  // keyboard
  Element *kb_ = nullptr;
  bool kbOpen_ = false;
  int kbLayout_ = 0;  // 0 thai, 1 english, 2 symbols, 3 numeric
  bool kbShift_ = false;
  Element *kbFor_ = nullptr;
  // popups
  Element *popup_ = nullptr;
  Element *popupFor_ = nullptr;
  int popupKind_ = 0;  // 1 select 2 date 3 time 4 colour
  int pickYear_ = 2026, pickMonth_ = 1, pickHour_ = 12, pickMin_ = 0;
};

}  // namespace rt
