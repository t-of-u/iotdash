#include "input.h"

#include "engine.h"
#include "extra.h"
#include "widgets.h"

namespace rt {

static const float kSlop = 9;  // px before a touch becomes a drag

float InputController::maxScrollX(Element *e) const {
  if (!e || !e->box) return 0;
  Box &b = *e->box;
  if (e->tag == TAG_html) return 0;
  float view = b.w - b.b[1] - b.b[3];
  return std::max(0.f, b.scrollW - view);
}

float InputController::maxScrollY(Element *e) const {
  if (!e || !e->box) return 0;
  Box &b = *e->box;
  if (e->tag == TAG_html) {
    float docH = std::max(b.h + b.m[0] + b.m[2], b.y + b.scrollH);
    // content inside body that overflows (absolutely positioned etc.)
    return std::max(0.f, docH - eng_.display().height());
  }
  float view = b.h - b.b[0] - b.b[2];
  return std::max(0.f, b.scrollH - view);
}

bool InputController::canScroll(Element *e, bool vertical) const {
  if (!e || !e->cs || !e->box) return false;
  if (e->tag == TAG_html) {
    if (!vertical) return false;
    const ComputedStyle &s = *e->cs;
    Element *body = e->doc ? e->doc->body : nullptr;
    if (s.overflowY == OV_HIDDEN || (body && body->cs && body->cs->overflowY == OV_HIDDEN && s.overflowY == OV_VISIBLE)) return false;
    return maxScrollY(e) > 0.5f;
  }
  const ComputedStyle &s = *e->cs;
  if (vertical) return (s.overflowY == OV_AUTO || s.overflowY == OV_SCROLL) && maxScrollY(e) > 0.5f;
  return (s.overflowX == OV_AUTO || s.overflowX == OV_SCROLL) && maxScrollX(e) > 0.5f;
}

Element *InputController::scrollerFor(Element *t, bool vertical, float delta) const {
  for (Element *p = t; p; p = p->parent) {
    if (!p->isElement() && p->nodeType != NT_ELEMENT) continue;
    if (canScroll(p, vertical)) {
      // prefer a scroller that can still move in the drag direction
      float pos = p->x ? (vertical ? p->x->scrollY : p->x->scrollX) : 0;
      float mx = vertical ? maxScrollY(p) : maxScrollX(p);
      bool canMove = delta < 0 ? pos < mx - 0.5f : pos > 0.5f;
      if (canMove || p->tag == TAG_html) return p;
    }
    if (p->tag == TAG_html) break;
  }
  return nullptr;
}

void InputController::setActive(Element *e, bool on) {
  for (Element *p = e; p; p = p->parent) {
    if (p->nodeType != NT_ELEMENT) break;
    p->setState(ST_ACTIVE, on);
  }
  eng_.requestFrame();
}

void InputController::dispatch(Element *t, const char *type, float x, float y, bool cancelable) {
  ScriptHost *js = eng_.script();
  if (!js || !t) return;
  if (eng_.widgets().isInternal(t)) return;
  js->dispatch(t, type, "", true, cancelable, x, y, id_);
}

void InputController::setScroll(Element *s, float x, float y) {
  ElementExtra &ex = s->extra();
  x = clampf(x, 0, maxScrollX(s));
  y = clampf(y, 0, maxScrollY(s));
  if (ex.scrollX == x && ex.scrollY == y) return;
  ex.scrollX = x;
  ex.scrollY = y;
  ex.scrollbarAlpha = 1;
  ex.scrollbarUntil = (uint32_t)(eng_.nowMs() + 700);
  eng_.scrolled(s);
}

void InputController::pointer(int id, uint8_t phase, float x, float y) {
  double now = eng_.nowMs();
  Widgets &w = eng_.widgets();
  if (phase == PTR_DOWN) {
    // a new touch stops momentum under the finger
    anims_.clear();
    down_ = true;
    id_ = id;
    x0_ = x_ = x;
    y0_ = y_ = y;
    t0_ = now;
    scrolling_ = captured_ = cancelledClick_ = swipeBack_ = false;
    samples_.clear();
    samples_.push_back({now, x, y});
    eng_.flushLayout();
    target_ = eng_.renderer().hitTest(x, y);
    if (!target_) target_ = eng_.document() ? eng_.document()->body : nullptr;
    if (gDebugFlags & DBG_INPUT) {
      std::string cls = target_ ? target_->attrOr(intern("class"), "") : "";
      eng_.log("input: down %.0f,%.0f -> <%s%s%s> (%d ms late)", x, y, target_ ? target_->tagName().c_str() : "-",
               cls.empty() ? "" : " class=", cls.c_str(), (int)(eng_.platform().micros() / 1000 - (uint64_t)now));
    }
    active_ = target_;
    if (active_) setActive(active_, true);
    if (target_ && !w.isInternal(target_)) {
      dispatch(target_, "pointerdown", x, y);
      dispatch(target_, "touchstart", x, y);
      dispatch(target_, "mousedown", x, y);
      captured_ = w.pointerDown(target_, x, y);
    }
    swipeBack_ = x < 16 && !captured_;
    return;
  }
  if (!down_ || id != id_) {
    if (gDebugFlags & DBG_INPUT) eng_.log("input: %s %.0f,%.0f ignored: no touch down", phase == PTR_MOVE ? "move" : "up", x, y);
    return;
  }
  if (phase == PTR_MOVE) {
    x_ = x;
    y_ = y;
    samples_.push_back({now, x, y});
    while (samples_.size() > 2 && now - samples_.front().t > 120) samples_.erase(samples_.begin());
    if (captured_) {
      w.pointerMove(x, y);
      dispatch(target_, "pointermove", x, y);
      return;
    }
    float dx = x - x0_, dy = y - y0_;
    if (!scrolling_ && (std::fabs(dx) > kSlop || std::fabs(dy) > kSlop)) {
      if (swipeBack_ && dx > 0 && std::fabs(dx) > std::fabs(dy) * 1.5f) {
        // edge swipe: handled on release
      } else {
        swipeBack_ = false;
        vertical_ = std::fabs(dy) >= std::fabs(dx);
        Element *s = target_ ? scrollerFor(target_, vertical_, vertical_ ? dy : dx) : nullptr;
        if (!s && target_) s = scrollerFor(target_, !vertical_, vertical_ ? dx : dy), vertical_ = s ? !vertical_ : vertical_;
        if (s) {
          scrolling_ = true;
          scroller_ = s;
          sx0_ = s->x ? s->x->scrollX : 0;
          sy0_ = s->x ? s->x->scrollY : 0;
          // the gesture is a scroll: cancel the press
          if (active_) setActive(active_, false);
          active_ = nullptr;
          cancelledClick_ = true;
          dispatch(target_, "pointercancel", x, y, false);
          dispatch(target_, "touchcancel", x, y, false);
          // follow the finger from where the slop was crossed
          x0_ = x;
          y0_ = y;
        } else {
          // not scrollable: keep delivering moves; a large move cancels the click
          if (active_ && (std::fabs(dx) > 24 || std::fabs(dy) > 24)) {
            setActive(active_, false);
            active_ = nullptr;
            cancelledClick_ = true;
          }
        }
      }
    }
    if (scrolling_ && scroller_) {
      float nx = sx0_, ny = sy0_;
      if (vertical_) ny = sy0_ - (y - y0_);
      else nx = sx0_ - (x - x0_);
      setScroll(scroller_, nx, ny);
    } else if (target_) {
      dispatch(target_, "pointermove", x, y);
      dispatch(target_, "touchmove", x, y);
      dispatch(target_, "mousemove", x, y);
    }
    return;
  }
  // PTR_UP / PTR_CANCEL
  down_ = false;
  bool cancel = phase == PTR_CANCEL;
  if (captured_) {
    w.pointerUp(x, y, cancel);
    dispatch(target_, cancel ? "pointercancel" : "pointerup", x, y);
    if (active_) setActive(active_, false);
    active_ = nullptr;
    captured_ = false;
    return;
  }
  if (active_) setActive(active_, false);
  active_ = nullptr;
  if (swipeBack_ && x - x0_ > 90) {
    swipeBack_ = false;
    eng_.navigateHistory(-1);
    return;
  }
  if (scrolling_) {
    scrolling_ = false;
    // velocity over the last samples
    float vx = 0, vy = 0;
    if (samples_.size() >= 2) {
      const Sample &a = samples_.front(), &b = samples_.back();
      double dt = (b.t - a.t) / 1000.0;
      if (dt > 0.005 && now - b.t < 80) {
        vx = (float)((b.x - a.x) / dt);
        vy = (float)((b.y - a.y) / dt);
      }
    }
    if (scroller_) {
      Anim an{scroller_, vertical_ ? 0 : -vx, vertical_ ? -vy : 0, 0, 0, false, now};
      const ComputedStyle *cs = scroller_->cs;
      if (cs && (cs->scrollSnapX || cs->scrollSnapY)) snap(scroller_, an.vx, an.vy);
      else if (std::fabs(an.vx) > 40 || std::fabs(an.vy) > 40) anims_.push_back(an);
    }
    scroller_ = nullptr;
    return;
  }
  const bool dbg = gDebugFlags & DBG_INPUT;
  if (cancel || !target_) {
    if (dbg) eng_.log("input: up %.0f,%.0f: %s", x, y, cancel ? "cancelled" : "no target");
    return;
  }
  dispatch(target_, "pointerup", x, y);
  dispatch(target_, "touchend", x, y);
  dispatch(target_, "mouseup", x, y);
  if (cancelledClick_) {
    if (dbg) eng_.log("input: up %.0f,%.0f: no click (moved %.0f,%.0f)", x, y, x - x0_, y - y0_);
    return;
  }
  // click: the release must still be over the pressed element (or a descendant)
  Element *up = eng_.renderer().hitTest(x, y);
  if (up && !target_->isInclusiveAncestorOf(up) && !up->isInclusiveAncestorOf(target_)) {
    if (dbg) eng_.log("input: up %.0f,%.0f: no click (released over <%s>)", x, y, up->tagName().c_str());
    return;
  }
  if (dbg) eng_.log("input: click <%s>", target_->tagName().c_str());
  if (w.isInternal(target_)) {
    w.internalTap(target_);
    return;
  }
  bool proceed = true;
  ScriptHost *js = eng_.script();
  if (js) proceed = js->dispatch(target_, "click", "", true, true, x, y, id_);
  if (!proceed) return;
  // default actions (links, form controls, labels, summary, blur)
  eng_.activate(target_, x, y);
}

void InputController::snap(Element *s, float vx, float vy) {
  // choose the snap point nearest to where momentum would carry us
  ElementExtra &ex = s->extra();
  float projX = ex.scrollX + vx * 0.3f, projY = ex.scrollY + vy * 0.3f;
  float bestX = ex.scrollX, bestY = ex.scrollY;
  float bestD = 1e9f;
  Box &sb = *s->box;
  float viewW = sb.w - sb.b[1] - sb.b[3], viewH = sb.h - sb.b[0] - sb.b[2];
  float originX = sb.absX + sb.b[3], originY = sb.absY + sb.b[0];
  s->forEachElement([&](Element *c) {
    if (c == s || !c->cs || !c->box || c->cs->scrollSnapAlign == 0) return;
    Box &cb = *c->box;
    float px = cb.absX - originX, py = cb.absY - originY;
    if (c->cs->scrollSnapAlign == 2) {
      px -= (viewW - cb.w) / 2;
      py -= (viewH - cb.h) / 2;
    } else if (c->cs->scrollSnapAlign == 3) {
      px -= viewW - cb.w;
      py -= viewH - cb.h;
    }
    float d = s->cs->scrollSnapX ? std::fabs(px - projX) : std::fabs(py - projY);
    if (d < bestD) {
      bestD = d;
      bestX = s->cs->scrollSnapX ? px : ex.scrollX;
      bestY = s->cs->scrollSnapY ? py : ex.scrollY;
    }
  });
  scrollTo(s, bestX, bestY, true);
}

void InputController::scrollTo(Element *e, float x, float y, bool smooth) {
  if (!e) return;
  for (size_t i = 0; i < anims_.size();)
    if (anims_[i].el == e) anims_.erase(anims_.begin() + i);
    else i++;
  x = clampf(x, 0, maxScrollX(e));
  y = clampf(y, 0, maxScrollY(e));
  if (!smooth) {
    setScroll(e, x, y);
    return;
  }
  anims_.push_back({e, 0, 0, x, y, true, (double)eng_.nowMs()});
}

void InputController::scrollIntoView(Element *e, bool smooth) {
  eng_.flushLayout();
  if (!e->box) return;
  for (Element *p = e->parent; p; p = p->parent) {
    if (!p->cs) continue;
    bool sv = canScroll(p, true), sh = canScroll(p, false);
    if (!sv && !sh) continue;
    Box &pb = *p->box;
    float viewTop = p->tag == TAG_html ? 0 : pb.absY + pb.b[0];
    float viewH = p->tag == TAG_html ? (float)eng_.display().height() : pb.h - pb.b[0] - pb.b[2];
    float viewLeft = p->tag == TAG_html ? 0 : pb.absX + pb.b[3];
    float viewW = p->tag == TAG_html ? (float)eng_.display().width() : pb.w - pb.b[1] - pb.b[3];
    Box &b = *e->box;
    float sx = p->x ? p->x->scrollX : 0, sy = p->x ? p->x->scrollY : 0;
    float relY = b.absY - viewTop, relX = b.absX - viewLeft;
    float ny = sy, nx = sx;
    if (sv) {
      if (relY < sy) ny = relY;
      else if (relY + b.h > sy + viewH) ny = relY + b.h - viewH;
    }
    if (sh) {
      if (relX < sx) nx = relX;
      else if (relX + b.w > sx + viewW) nx = relX + b.w - viewW;
    }
    scrollTo(p, nx, ny, smooth);
    break;
  }
}

void InputController::tick(double now) {
  double dtMs = lastTick_ > 0 ? now - lastTick_ : 16;
  lastTick_ = now;
  float dt = (float)std::min(dtMs, 50.0) / 1000.f;
  for (size_t i = 0; i < anims_.size();) {
    Anim &a = anims_[i];
    if (!a.el->box) {
      anims_.erase(anims_.begin() + i);
      continue;
    }
    ElementExtra &ex = a.el->extra();
    bool done = false;
    if (a.smooth) {
      // critically damped approach to the target
      float k = 1 - std::exp(-dt * 14);
      float nx = ex.scrollX + (a.tx - ex.scrollX) * k, ny = ex.scrollY + (a.ty - ex.scrollY) * k;
      if (std::fabs(a.tx - nx) < 0.5f && std::fabs(a.ty - ny) < 0.5f) {
        nx = a.tx;
        ny = a.ty;
        done = true;
      }
      setScroll(a.el, nx, ny);
    } else {
      float decay = std::exp(-dt / 0.325f);
      float nx = ex.scrollX + a.vx * dt, ny = ex.scrollY + a.vy * dt;
      a.vx *= decay;
      a.vy *= decay;
      float mx = maxScrollX(a.el), my = maxScrollY(a.el);
      if (nx <= 0 || nx >= mx) a.vx = 0;
      if (ny <= 0 || ny >= my) a.vy = 0;
      setScroll(a.el, nx, ny);
      if (std::fabs(a.vx) < 20 && std::fabs(a.vy) < 20) done = true;
    }
    if (done) anims_.erase(anims_.begin() + i);
    else i++;
  }
  // scrollbar fade-out
  Document *doc = eng_.document();
  if (doc) {
    uint32_t nowMs = (uint32_t)now;
    std::function<void(Element *)> fade;
    // only elements that scrolled recently have extras with alpha > 0: walk cheaply via html
    if (doc->html) {
      doc->html->forEachElement([&](Element *e) {
        if (!e->x || e->x->scrollbarAlpha <= 0) return;
        if (nowMs > e->x->scrollbarUntil) {
          e->x->scrollbarAlpha = std::max(0.f, e->x->scrollbarAlpha - dt * 4);
          eng_.scrolled(e);
        }
      });
    }
  }
  // page shift (keyboard)
  if (std::fabs(pageShift_ - pageShiftTarget_) > 0.3f) {
    float k = 1 - std::exp(-dt * 16);
    pageShift_ += (pageShiftTarget_ - pageShift_) * k;
    if (std::fabs(pageShift_ - pageShiftTarget_) <= 0.3f) pageShift_ = pageShiftTarget_;
    eng_.renderer().pageShiftY = pageShift_;
    eng_.markListDirty();
    eng_.requestFrame();
  }
}

void InputController::setPageShift(float y) { pageShiftTarget_ = std::max(0.f, y); }

bool InputController::busy() const {
  if (!anims_.empty()) return true;
  if (std::fabs(pageShift_ - pageShiftTarget_) > 0.3f) return true;
  return false;
}

void InputController::reset() {
  down_ = false;
  target_ = active_ = scroller_ = nullptr;
  anims_.clear();
  pageShift_ = pageShiftTarget_ = 0;
  eng_.renderer().pageShiftY = 0;
}

void InputController::forget(Element *e) {
  if (target_ == e) target_ = nullptr;
  if (active_ == e) active_ = nullptr;
  if (scroller_ == e) scroller_ = nullptr;
  for (size_t i = 0; i < anims_.size();)
    if (anims_[i].el == e) anims_.erase(anims_.begin() + i);
    else i++;
}

void InputController::clampScrolls() {
  Document *doc = eng_.document();
  if (!doc || !doc->html) return;
  doc->html->forEachElement([&](Element *e) {
    if (!e->x || (e->x->scrollX == 0 && e->x->scrollY == 0)) return;
    float mx = maxScrollX(e), my = maxScrollY(e);
    if (e->x->scrollX > mx || e->x->scrollY > my) {
      e->x->scrollX = std::min(e->x->scrollX, mx);
      e->x->scrollY = std::min(e->x->scrollY, my);
      eng_.markListDirty();
    }
  });
}

}  // namespace rt
