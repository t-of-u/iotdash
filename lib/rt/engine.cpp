#include "engine.h"

#include "layer.h"

#include "extra.h"
#include "input.h"
#include "prof.h"
#include "thai.h"
#include "widgets.h"

namespace rt {

static Engine *gEngineForLog = nullptr;

Engine::Engine(Platform &p, PanelDisplay &d) : plat_(p), disp_(d) {
  initAtoms();
  gEngineForLog = this;
  MediaEnv env;
  env.width = (float)d.width();
  env.height = (float)d.height();
  style_.init(env);
  style_.addUserAgentSheet(Widgets::internalCss());
  layout_.fonts = &fonts_;
  layout_.host = this;
  layout_.vw = env.width;
  layout_.vh = env.height;
  renderer_.layout = &layout_;
  renderer_.fonts = &fonts_;
  renderer_.host = this;
  renderer_.width = d.width();
  renderer_.height = d.height();
  widgets_ = new Widgets(*this);
  input_ = new InputController(*this);
  style_.overrideHook = [this](Element *e, ComputedStyle &nw) { anim_.styleHook(e, e->cs, nw, style_); };
  anim_.onWaapiFinish = [this](int id) {
    if (js_) js_->animationFinished(id);
  };
  anim_.onEvent = [this](Element *e, const char *type, const std::string &name) {
    if (!js_ || !e->connected()) return;
    std::string detail = std::string("{\"propertyName\":\"") + name + "\",\"animationName\":\"" + name + "\"}";
    js_->dispatch(e, type, detail, true, false);
  };
}

Engine::~Engine() {
  if (doc_) {
    doc_->observer = nullptr;
    doc_->unref();
  }
  delete input_;
  delete widgets_;
}

void Engine::log(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  plat_.log(buf);
}

// ============================================================ bundle / pages

bool Engine::loadBundle(const uint8_t *data, size_t len) {
  if (!bundle_.load(data, len)) return false;
  mounts_ = {""};
  loadFonts();
  images_.clear();
  return true;
}

bool Engine::mount(const std::string &prefix, const uint8_t *data, size_t len) {
  return mount(prefix, std::make_shared<RamSource>(data, len));
}

bool Engine::mount(const std::string &prefix, std::shared_ptr<const ImageSource> src) {
  unmount(prefix);
  if (!bundle_.mount(prefix, std::move(src))) {
    bundle_.unmount(prefix);
    return false;
  }
  mounts_.push_back(prefix);
  loadFonts();
  return true;
}

void Engine::unmount(const std::string &prefix) {
  auto it = std::find(mounts_.begin(), mounts_.end(), prefix);
  if (it == mounts_.end()) return;
  mounts_.erase(it);
  if (const ImageSource *s = bundle_.sourceOf(prefix)) fontsDropSource(s);  // its glyphs in RAM
  bundle_.unmount(prefix);
  images_.clear();
  loadFonts();
}

void Engine::setSystemCss(const std::string &css) {
  systemCss_ = css;
  MediaEnv env = style_.env();
  style_.init(env);
  style_.addUserAgentSheet(Widgets::internalCss());
  if (!css.empty()) style_.addUserAgentSheet(css);
  if (doc_) collectStyles();
  styleDirty_ = true;
}

void Engine::setDarkMode(bool dark) {
  if (style_.env().dark == dark) return;
  // media queries are resolved when sheets are parsed: parse them all again
  MediaEnv env = style_.env();
  env.dark = dark;
  style_.init(env);
  style_.addUserAgentSheet(Widgets::internalCss());
  if (!systemCss_.empty()) style_.addUserAgentSheet(systemCss_);
  if (doc_) {
    collectStyles();
    doc_->forEachElement([](Element *e) { e->flags |= NF_STYLE_DIRTY; });
    doc_->flags |= NF_DESC_STYLE_DIRTY;
  }
  styleDirty_ = listDirty_ = true;
  markLayoutDirty();
  fullRepaint_ = true;
  frameRequested_ = true;
}

// Fonts and the Thai dictionary of every mounted image (each has its own manifest.json).
void Engine::loadFonts() {
  fonts_.clear();
  std::string dict;
  // the newest mount first: an app's atlas of a size wins over the system's (it was built
  // because the app shows characters the system's atlas lacks)
  for (auto it = mounts_.rbegin(); it != mounts_.rend(); ++it) {
    const std::string &pre = *it;
    std::string d = bundle_.text(pre + "thai.dict");
    if (!d.empty()) dict += d + "\n";
    std::string m = bundle_.text(pre + "manifest.json");
    if (m.empty()) continue;
    Json j;
    if (!Json::parse(m.data(), m.size(), j)) {
      log("%smanifest.json invalid", pre.c_str());
      continue;
    }
    if (const Json *fl = j.get("fonts")) {
      for (auto &f : fl->arr) {
        std::string file = pre + f.strOr("file", "");
        const Bundle::File *bf = bundle_.find(file);
        if (!bf || !fonts_.add(*bf)) log("font %s failed", file.c_str());
      }
    }
    if (const Json *al = j.get("aliases")) {
      for (auto &kv : al->obj)
        if (kv.second.type == Json::Str) fonts_.alias(kv.first, kv.second.str);
    }
  }
  thaiLoadDict(dict.data(), dict.size());
}

void Engine::open(const std::string &urlIn, TransitionKind tr) {
  std::string url = urlIn, hash;
  size_t h = url.find('#');
  if (h != std::string::npos) {
    hash = url.substr(h + 1);
    url = url.substr(0, h);
  }
  if (url.empty()) url = url_.empty() ? "index.html" : url_;
  url = Bundle::resolve(url_, url);
  std::string html = startsWith(url, "system/private/") ? "" : bundle_.text(url);  // secrets are not pages
  if (html.empty()) {
    log("open: %s not found", url.c_str());
    return;
  }
  uint64_t t0 = plat_.micros();
  // tear down the old document
  if (doc_) {
    widgets_->closeAll();
    input_->reset();
    if (js_) js_->reset(nullptr);
    anim_.clear();  // nodes die below without telling the observer
    renderer_.reset();
    doc_->observer = nullptr;
    doc_->unref();
    doc_ = nullptr;
  }
  fonts_.releaseResident();  // nothing draws the old page any more: its fonts leave RAM
  focus_ = nullptr;
  paintDirty_.clear();
  damage_.clear();
  backdrops_.clear();
  posted_.clear();
  url_ = url;
  hash_ = hash;
  history_.resize(historyPos_ + 1 > 0 ? historyPos_ + 1 : 0);
  history_.push_back(url + (hash.empty() ? "" : "#" + hash));
  historyPos_ = (int)history_.size() - 1;
  buildDocument(html);
  if (os_) os_->documentReady(doc_);
  log("page %s parsed in %.1f ms", url.c_str(), (plat_.micros() - t0) / 1000.0);
  pendingTr_ = tr;
  if (js_) {
    js_->reset(doc_);
    runScripts();
    js_->dispatch(doc_, "DOMContentLoaded", "", true, false);
    js_->dispatchWindow("load", "");
  }
  applyHashTarget();
  fullRepaint_ = true;
  styleDirty_ = listDirty_ = true;
  markLayoutDirty();
  frameRequested_ = true;
}

void Engine::reload() {
  std::string u = url_ + (hash_.empty() ? "" : "#" + hash_);
  if (historyPos_ >= 0) history_.resize(historyPos_);
  historyPos_--;
  open(u, TR_NONE);
}

void Engine::buildDocument(const std::string &html) {
  doc_ = new Document();
  doc_->ref();
  doc_->url = url_;
  doc_->observer = this;
  building_ = true;
  parseDocument(doc_, html);
  building_ = false;
  collectStyles();
}

void Engine::collectStyles() {
  if (building_) return;
  style_.clearAuthor();
  std::vector<std::pair<std::string, std::string>> sheets;
  doc_->forEachElement([&](Element *e) {
    if (e->tag == TAG_style) {
      style_.addAuthorSheet(e->textContent());
    } else if (e->tag == TAG_link) {
      std::string rel = toLower(e->attrOr(A.rel, ""));
      if (rel.find("stylesheet") != std::string::npos) {
        std::string href = Bundle::resolve(url_, e->attrOr(A.href, ""));
        std::string css = bundle_.text(href);
        if (css.empty()) log("stylesheet %s not found", href.c_str());
        style_.addAuthorSheet(css);
      }
    }
  });
  if (doc_->html) {
    doc_->html->forEachElement([](Element *e) { e->flags |= NF_STYLE_DIRTY; });
  }
}

void Engine::runScripts() {
  std::vector<Element *> scripts;
  doc_->forEachElement([&](Element *e) {
    if (e->tag == TAG_script) scripts.push_back(e);
  });
  for (Element *s : scripts) {
    std::string type = toLower(s->attrOr(A.type, ""));
    if (!type.empty() && type != "text/javascript" && type != "module" && type != "application/javascript") continue;
    bool module = type == "module";
    std::string src = s->attrOr(A.src, "");
    std::string code, name;
    if (!src.empty()) {
      name = Bundle::resolve(url_, src);
      code = bundle_.text(name);
      if (code.empty()) {
        log("script %s not found", name.c_str());
        continue;
      }
    } else {
      code = s->textContent();
      name = url_ + "#inline";
    }
    uint64_t t0 = plat_.micros();
    js_->runScript(code, name, module);
    stats_.lastScriptUs = (uint32_t)(plat_.micros() - t0);
  }
}

// ============================================================ hash routing

void Engine::applyHashTarget() {
  if (!doc_) return;
  // pages: elements with [data-page] (or class "page" under body) - show one
  std::vector<Element *> pages;
  doc_->forEachElement([&](Element *e) {
    if (e->hasAttr(A.data_page)) pages.push_back(e);
  });
  Element *target = hash_.empty() ? nullptr : doc_->getElementById(hash_);
  doc_->forEachElement([&](Element *e) {
    bool t = e == target;
    if (e->hasState(ST_TARGET) != t) e->setState(ST_TARGET, t);
  });
  if (!pages.empty()) {
    Element *active = nullptr;
    for (Element *p : pages)
      if (p == target) active = p;
    if (!active && target) {
      for (Element *p : pages)
        if (p->isInclusiveAncestorOf(target)) active = p;
    }
    if (!active) {
      for (Element *p : pages)
        if (p->hasAttr(intern("data-default"))) active = p;
    }
    if (!active) active = pages[0];
    for (Element *p : pages) {
      bool on = p == active;
      if (on && p->hasAttr(A.hidden)) p->removeAttr(A.hidden);
      if (!on && !p->hasAttr(A.hidden)) p->setAttr(A.hidden, "");
    }
  }
  styleDirty_ = true;
}

void Engine::setHash(const std::string &hashIn, bool push, TransitionKind tr) {
  std::string hash = hashIn;
  if (!hash.empty() && hash[0] == '#') hash = hash.substr(1);
  if (hash == hash_) return;
  std::string old = hash_;
  hash_ = hash;
  if (push) {
    history_.resize(historyPos_ + 1);
    history_.push_back(url_ + "#" + hash);
    historyPos_ = (int)history_.size() - 1;
  }
  pendingTr_ = tr;
  applyHashTarget();
  if (js_) {
    std::string d = "{\"oldURL\":\"#" + old + "\",\"newURL\":\"#" + hash + "\"}";
    js_->dispatchWindow("hashchange", d);
  }
  frameRequested_ = true;
}

void Engine::navigateHistory(int delta) {
  int np = historyPos_ + delta;
  if (np < 0 || np >= (int)history_.size()) return;
  std::string target = history_[np];
  historyPos_ = np;
  std::string url = target, hash;
  size_t h = url.find('#');
  if (h != std::string::npos) {
    hash = url.substr(h + 1);
    url = url.substr(0, h);
  }
  TransitionKind tr = delta < 0 ? TR_SLIDE_RIGHT : TR_SLIDE_LEFT;
  if (url != url_) {
    // open without touching history
    std::vector<std::string> saved = history_;
    int pos = historyPos_;
    open(target, tr);
    history_ = saved;
    historyPos_ = pos;
    return;
  }
  std::string old = hash_;
  hash_ = hash;
  pendingTr_ = tr;
  applyHashTarget();
  if (js_) {
    js_->dispatchWindow("hashchange", "{\"oldURL\":\"#" + old + "\",\"newURL\":\"#" + hash + "\"}");
    js_->dispatchWindow("popstate", "{}");
  }
}

// ============================================================ DOM observer

void Engine::initElement(Element *e) {
  Tag t = e->tagId();
  if (t == TAG_input || t == TAG_textarea || t == TAG_select || t == TAG_button || t == TAG_option ||
      t == TAG_fieldset || t == TAG_details || t == TAG_dialog || t == TAG_img || t == TAG_canvas || t == TAG_svg ||
      t == TAG_progress || t == TAG_meter)
    widgets_->init(e);
  if (e->hasAttr(A.disabled) && (t == TAG_input || t == TAG_textarea || t == TAG_select || t == TAG_button ||
                                 t == TAG_option || t == TAG_fieldset || t == TAG_optgroup))
    e->state |= ST_DISABLED;
}

void Engine::nodeInserted(Node *n) {
  styleDirty_ = true;
  markLayoutDirty(n->parent);
  frameRequested_ = true;
  if (Element *e = n->asElement()) {
    e->forEachElement([&](Element *d) {
      d->flags |= NF_STYLE_DIRTY | NF_SUBTREE_STYLE;
      initElement(d);
    });
  }
  Element *p = n->parent;
  if (p) {
    // structural pseudo-classes of siblings may change
    p->flags |= NF_STYLE_DIRTY | NF_SUBTREE_STYLE;
    for (Element *a = p; a; a = a->parent) a->flags |= NF_DESC_STYLE_DIRTY;
    paintDirty_.insert(p);
  }
  if (js_ && n->connected()) js_->nodeConnected(n, true);
  // <style> added at runtime
  if (Element *e = n->asElement())
    if ((e->tag == TAG_style || e->tag == TAG_link) && n->connected()) collectStyles();
}

void Engine::forgetSubtree(Node *n) {
  if (Element *e = n->asElement()) {
    e->forEachElement([&](Element *d) {
      if (d->box && d->box->painted) {
        if (!d->box->lastInk.empty()) damage_.push_back(d->box->lastInk);
        d->box->painted = false;
      }
      renderer_.forget(d);
      anim_.cancel(d);
      paintDirty_.erase(d);
      if (focus_ == d) focus_ = nullptr;
      input_->forget(d);
      widgets_->forget(d);
      auto bd = backdrops_.find(d);
      if (bd != backdrops_.end()) {
        Element *b = bd->second;
        backdrops_.erase(bd);
        post([b]() { b->remove(); });
      }
      if (d->before && d->before->box && d->before->box->painted) damage_.push_back(d->before->box->lastInk);
      if (d->after && d->after->box && d->after->box->painted) damage_.push_back(d->after->box->lastInk);
      // out of the document, an element keeps no computed style (over a kilobyte each): scripts
      // hold on to the screens they replaced until the GC comes. Back in, it is computed again.
      delete d->cs;
      d->cs = nullptr;
      d->flags |= NF_STYLE_DIRTY;
    });
  }
}

void Engine::nodeRemoved(Node *n, Element *oldParent) {
  forgetSubtree(n);
  styleDirty_ = listDirty_ = true;
  markLayoutDirty(oldParent);
  frameRequested_ = true;
  if (oldParent) {
    oldParent->flags |= NF_STYLE_DIRTY | NF_SUBTREE_STYLE;
    for (Element *a = oldParent; a; a = a->parent) a->flags |= NF_DESC_STYLE_DIRTY;
    paintDirty_.insert(oldParent);
  }
  if (js_) js_->nodeConnected(n, false);
  if (Element *e = n->asElement())
    if (e->tag == TAG_style || e->tag == TAG_link) collectStyles();
}

void Engine::attrChanged(Element *e, Atom name, const std::string *oldValue) {
  (void)oldValue;
  if (name == A.style) {
    delete e->inlineStyle;
    e->inlineStyle = nullptr;
    const std::string *v = e->getAttr(A.style);
    if (v && !v->empty()) {
      e->inlineStyle = new DeclBlock();
      parseDeclarations(v->data(), v->size(), *e->inlineStyle);
    }
    e->flags |= NF_STYLE_DIRTY;
  } else {
    // attribute selectors / id / class: restyle subtree (and following siblings, when a
    // sibling combinator could reach them)
    e->flags |= NF_STYLE_DIRTY | NF_SUBTREE_STYLE;
    if (gSiblingSelectors)
      for (Element *s = e->nextElementSibling(); s; s = s->nextElementSibling()) s->flags |= NF_STYLE_DIRTY | NF_SUBTREE_STYLE;
    if (e->parent) e->parent->flags |= NF_STYLE_DIRTY;  // :has() and friends (cheap enough)
  }
  for (Element *a = e->parent; a; a = a->parent) a->flags |= NF_DESC_STYLE_DIRTY;
  styleDirty_ = true;
  frameRequested_ = true;
  widgets_->attrChanged(e, name);
  if (e->tagId() >= TAG_svg && e->tagId() <= TAG_use) svgDirty(e);
  if (name == A.disabled) e->setState(ST_DISABLED, e->hasAttr(A.disabled));
  if (js_ && e->js) static_cast<ScriptHost *>(js_)->attributeChanged(e, name);
}

void Engine::textChanged(Text *t) {
  markLayoutDirty(t);
  frameRequested_ = true;
  for (Element *p = t->parent; p; p = p->parent) {
    paintDirty_.insert(p);
    if (p->cs && p->cs->isBlockLevel()) break;
  }
  if (t->parent && (t->parent->tag == TAG_style)) collectStyles();
  if (t->parent && t->parent->tagId() == TAG_textarea) widgets_->attrChanged(t->parent, A.value);
}

void Engine::nodeDestroyed(Node *n) {
  if (js_) js_->nodeDestroyed(n);
  if (Element *e = n->asElement()) {
    renderer_.forget(e);
    anim_.cancel(e);
    paintDirty_.erase(e);
    if (focus_ == e) focus_ = nullptr;
    input_->forget(e);
    widgets_->forget(e);
  }
}

// ============================================================ hosts

bool Engine::intrinsicSize(Element *e, float &w, float &h) { return widgets_->intrinsic(e, w, h); }

std::string Engine::controlText(Element *e) { return currentValue(e); }

bool Engine::paintReplaced(Element *e, const PaintCtx &pc) { return widgets_->paint(e, pc); }

std::shared_ptr<Image> Engine::bgImage(const std::string &url) { return loadImage(url); }

std::shared_ptr<Image> Engine::loadImage(const std::string &srcIn) {
  PaintLock lock;  // also called while painting (possibly on both cores)
  bool card = startsWith(srcIn, "sd:");
  std::string src = card ? srcIn : Bundle::resolve(url_, srcIn);
  auto it = images_.find(src);
  if (it != images_.end()) return it->second;
  std::shared_ptr<Image> img;
  if (card) img = os_ ? os_->image(src) : nullptr;  // a file on the SD card (app icons)
  else if (const Bundle::File *f = bundle_.find(src)) {
    if (f->data) {
      img = Image::fromImg1(f->data, f->size);  // in RAM: in place
    } else if (auto buf = bundle_.bytes(src)) {  // slow storage: read once into RAM
      img = Image::fromImg1(buf->data(), buf->size());
      if (img) img->keep = buf;
    }
  }
  if (!img) log("image %s not found", src.c_str());
  images_[src] = img;
  return img;
}

// ============================================================ form state

std::string Engine::currentValue(Element *e) {
  Tag t = e->tagId();
  if (t == TAG_select) return widgets_->selectValue(e);
  if (e->x && e->x->valueDirty) return e->x->value;
  if (t == TAG_textarea) return e->textContent();
  if (t == TAG_option) return e->hasAttr(A.value) ? e->attrOr(A.value, "") : trim(e->textContent());
  std::string type = toLower(e->attrOr(A.type, "text"));
  if (t == TAG_input && (type == "checkbox" || type == "radio")) return e->attrOr(A.value, "on");
  if (t == TAG_input && type == "range" && !e->hasAttr(A.value)) {
    float mn = e->attrNum(A.min, 0), mx = e->attrNum(A.max, 100);
    char buf[32];
    snprintf(buf, sizeof buf, "%g", mn + (mx - mn) / 2);
    return buf;
  }
  if (t == TAG_input && type == "color" && !e->hasAttr(A.value)) return "#000000";
  return e->attrOr(A.value, "");
}

void Engine::setValue(Element *e, const std::string &v, bool fromUser) {
  (void)fromUser;
  if (e->tagId() == TAG_select) {
    widgets_->selectSetValue(e, v);
    return;
  }
  ElementExtra &x = e->extra();
  if (x.valueDirty && x.value == v) return;
  x.value = v;
  x.valueDirty = true;
  if ((size_t)x.caret > v.size()) x.caret = (int)v.size();
  refreshFormState(e);
  invalidateElement(e);
  if (e->tagId() == TAG_input) {
    std::string type = toLower(e->attrOr(A.type, "text"));
    if (type == "range" || type == "progress") invalidateElement(e);
  }
}

void Engine::setChecked(Element *e, bool on, bool fromUser) {
  (void)fromUser;
  e->extra().checkedDirty = true;
  if (e->hasState(ST_CHECKED) == on) return;
  e->setState(ST_CHECKED, on);
  if (on && toLower(e->attrOr(A.type, "")) == "radio") {
    std::string name = e->attrOr(A.name, "");
    if (!name.empty() && doc_) {
      doc_->forEachElement([&](Element *o) {
        if (o != e && o->tagId() == TAG_input && toLower(o->attrOr(A.type, "")) == "radio" && o->attrOr(A.name, "") == name &&
            o->hasState(ST_CHECKED)) {
          o->setState(ST_CHECKED, false);
          o->extra().checkedDirty = true;
          invalidateElement(o);
        }
      });
    }
  }
  styleDirty_ = true;
  invalidateElement(e);
}

void Engine::refreshFormState(Element *e) {
  Tag t = e->tagId();
  if (t == TAG_input || t == TAG_textarea) {
    bool ph = e->hasAttr(A.placeholder) && currentValue(e).empty();
    if (e->hasState(ST_PLACEHOLDER) != ph) {
      e->setState(ST_PLACEHOLDER, ph);
      styleDirty_ = true;
    }
    bool invalid = e->hasAttr(A.required) && currentValue(e).empty();
    if (e->hasState(ST_INVALID) != invalid) {
      e->setState(ST_INVALID, invalid);
      styleDirty_ = true;
    }
  }
}

void Engine::focus(Element *e) {
  if (e == focus_) return;
  Element *old = focus_;
  if (old) {
    old->setState(ST_FOCUS, false);
    old->setState(ST_FOCUS_VISIBLE, false);
    for (Element *p = old->parent; p; p = p->parent)
      if (p->hasState(ST_FOCUS_WITHIN)) p->setState(ST_FOCUS_WITHIN, false);
  }
  focus_ = e;
  if (e) {
    e->setState(ST_FOCUS, true);
    Tag t = e->tagId();
    if (t == TAG_input || t == TAG_textarea) e->setState(ST_FOCUS_VISIBLE, true);
    for (Element *p = e->parent; p; p = p->parent) p->setState(ST_FOCUS_WITHIN, true);
  }
  styleDirty_ = true;
  frameRequested_ = true;
  widgets_->focusChanged(old, e);
  if (js_) {  // the system UI's own fields (internal) are not the page's business
    if (old && old->connected() && !widgets_->isInternal(old)) {
      js_->dispatch(old, "blur", "", false, false);
      js_->dispatch(old, "focusout", "", true, false);
    }
    if (e && !widgets_->isInternal(e)) {
      js_->dispatch(e, "focus", "", false, false);
      js_->dispatch(e, "focusin", "", true, false);
    }
  }
}

void Engine::invalidateElement(Element *e) {
  paintDirty_.insert(e);
  frameRequested_ = true;
}

void Engine::scrolled(Element *e) {
  listDirty_ = true;
  frameRequested_ = true;
  // repaint the scroll container's visible area
  if (e->box) {
    const PaintItem *it = renderer_.itemFor(e);
    if (e->tag == TAG_html) damage_.push_back({0, 0, disp_.width(), disp_.height()});
    else if (it) damage_.push_back(it->ink);
  }
  if (js_) js_->dispatch(e->tag == TAG_html ? (Element *)doc_ : e, "scroll", "", false, false);
}

// ============================================================ main loop

void Engine::markLayoutDirty(Node *hint) {
  layoutDirty_ = true;
  if (fullLayout_) return;
  Element *e = hint ? (hint->nodeType == NT_ELEMENT ? (Element *)hint : hint->parent) : nullptr;
  if (!e || e->nodeType != NT_ELEMENT || layoutHints_.size() >= 32) {
    fullLayout_ = true;
    for (Element *h : layoutHints_) h->unref();
    layoutHints_.clear();
    return;
  }
  if (!layoutHints_.empty() && layoutHints_.back() == e) return;
  e->ref();
  layoutHints_.push_back(e);
}

void Engine::flushLayout() {
  if (!doc_ || !doc_->html) return;
  if (styleDirty_ || (doc_->flags & (NF_DESC_STYLE_DIRTY | NF_STYLE_DIRTY)) ||
      (doc_->html->flags & (NF_DESC_STYLE_DIRTY | NF_STYLE_DIRTY))) {
    RT_PROF(PF_STYLE);
    uint64_t t0 = plat_.micros();
    gRestyleCount = 0;
    style_.recalc(doc_->html, false, [this](Element *e, uint8_t d) { styleChanged(e, d); });
    stats_.lastRestyled = gRestyleCount;
    styleDirty_ = false;
    // dirty marks propagate up to the document node, which recalc (from <html>) never clears
    doc_->flags &= ~(NF_DESC_STYLE_DIRTY | NF_STYLE_DIRTY);
    stats_.lastStyleUs = (uint32_t)(plat_.micros() - t0);
  }
  if (layoutDirty_) {
    RT_PROF(PF_LAYOUT);
    uint64_t t0 = plat_.micros();
    bool full = fullLayout_ || !layout_.laidOut();
    if (!full) {
      // lay out only around what changed (e.g. a clock's text): subtrees whose outer size
      // stays the same leave the rest of the page untouched
      std::vector<Element *> roots;
      for (Element *e : layoutHints_) {
        if (!e->connected()) continue;  // removed: its old parent is hinted as well
        bool covered = false;
        for (Element *r : roots)
          if (r->isInclusiveAncestorOf(e)) covered = true;
        if (covered) continue;
        Element *root = nullptr;
        if (!layout_.relayout(e, &root)) {
          full = true;
          break;
        }
        roots.push_back(root);
      }
    }
    for (Element *e : layoutHints_) e->unref();
    layoutHints_.clear();
    if (full) layout_.layoutDocument(doc_);
    else if (checkLayout_) verifyPartialLayout();
    stats_.partialLayouts += full ? 0 : 1;
    stats_.lastLayoutFull = full;
    fullLayout_ = false;
    layoutDirty_ = false;
    listDirty_ = true;
    stats_.lastLayoutUs = (uint32_t)(plat_.micros() - t0);
    widgets_->afterLayout();
    input_->clampScrolls();
    static const bool dump = getenv("RT_DUMP_LAYOUT") != nullptr;
    if (dump) dumpLayout();
  }
}

// Debug (RT_CHECK_LAYOUT): a partial layout must give exactly what a full one gives.
void Engine::verifyPartialLayout() {
  struct G {
    Element *e;
    float x, y, w, h;
  };
  std::vector<G> before;
  doc_->html->forEachElement([&](Element *e) {
    if (e->box && e->cs && e->cs->display != D_NONE) before.push_back({e, e->box->absX, e->box->absY, e->box->w, e->box->h});
  });
  layout_.layoutDocument(doc_);
  int bad = 0;
  for (auto &g : before) {
    Box &b = *g.e->box;
    if (std::fabs(b.absX - g.x) > 0.01f || std::fabs(b.absY - g.y) > 0.01f || std::fabs(b.w - g.w) > 0.01f ||
        std::fabs(b.h - g.h) > 0.01f) {
      if (bad++ < 5)
        log("partial layout mismatch <%s%s> %.1f,%.1f %.1fx%.1f vs full %.1f,%.1f %.1fx%.1f", g.e->tagName().c_str(),
            g.e->idAtom ? (" #" + atomStr(g.e->idAtom)).c_str() : "", g.x, g.y, g.w, g.h, b.absX, b.absY, b.w, b.h);
    }
  }
  checkCount_++;
  if (bad) checkBad_++;
}

void Engine::dumpLayout() {
  std::function<void(Element *, int)> dump = [&](Element *e, int depth) {
    if (!e->box || !e->cs || e->cs->display == D_NONE) return;
    Box &b = *e->box;
    std::string ind(depth * 2, ' ');
    log("%s<%s%s%s> %.1f,%.1f %.1fx%.1f", ind.c_str(), e->tagName().c_str(), e->idAtom ? (" #" + atomStr(e->idAtom)).c_str() : "",
        e->classes.empty() ? "" : (" ." + atomStr(e->classes[0])).c_str(), b.absX, b.absY, b.w, b.h);
    for (auto &il : b.inlines)
      for (auto &ln : il->lines) {
        std::string t;
        for (uint32_t fi = ln.f0; fi < ln.f1; fi++) {
          LineFrag &f = il->frags[fi];
          char buf[64];
          snprintf(buf, sizeof buf, " [%c x%.1f w%.1f]", f.kind == LineFrag::TEXT ? 'T' : f.kind == LineFrag::ATOMIC ? 'A' : 'E',
                   b.absX + f.x, f.w);
          t += buf;
        }
        log("%s  line y%.1f h%.1f base%.1f%s", ind.c_str(), b.absY + ln.y, ln.h, b.absY + ln.baseline, t.c_str());
      }
    LayoutEngine::forEachLayoutChild(e, [&](Node *c) {
      if (c->isElement()) dump((Element *)c, depth + 1);
    });
  };
  if (doc_->html) dump(doc_->html, 0);
}

void Engine::svgDirty(Element *e) {
  for (Element *p = e; p && p->nodeType == NT_ELEMENT; p = p->parent)
    if (p->tagId() == TAG_svg) {
      if (p->x) {
        p->x->svg.reset();
        p->x->layer.reset();  // a new tree may reuse the old one's address (the cache key)
      }
      invalidateElement(p);
    }
}

void Engine::styleChanged(Element *e, uint8_t d) {
  if (e->tagId() >= TAG_svg && e->tagId() <= TAG_use) svgDirty(e);
  if (d & SD_LAYOUT) markLayoutDirty(e);
  if (d & (SD_STACKING | SD_PAINT)) listDirty_ = true;
  if (d & SD_PAINT) {
    paintDirty_.insert(e);
    // inline elements are painted by their block container
    if (e->cs && e->cs->display == D_INLINE)
      for (Element *p = e->parent; p; p = p->parent) {
        paintDirty_.insert(p);
        if (p->cs && p->cs->display != D_INLINE) break;
      }
  }
}

static void mergeRects(std::vector<RectI> &v, int align, int w, int h) {
  // align horizontally, clip to screen and merge overlapping rects
  std::vector<RectI> out;
  for (RectI r : v) {
    r = r.intersect({0, 0, w, h});
    if (r.empty()) continue;
    if (align > 1) {
      int x0 = r.x & ~(align - 1), x1 = std::min(w, (r.right() + align - 1) & ~(align - 1));
      r.x = x0;
      r.w = x1 - x0;
    }
    bool merged = true;
    while (merged) {
      merged = false;
      for (size_t i = 0; i < out.size(); i++) {
        RectI g{r.x - 2, r.y - 2, r.w + 4, r.h + 4};
        if (g.intersects(out[i])) {
          r = r.unite(out[i]);
          out.erase(out.begin() + i);
          merged = true;
          break;
        }
      }
    }
    out.push_back(r);
  }
  if (out.size() > 12) {
    RectI u;
    for (auto &r : out) u = u.unite(r);
    out = {u};
  }
  v = out;
}

bool Engine::render() {
  int W = disp_.width(), H = disp_.height();
  uint64_t t0 = plat_.micros();
  flushLayout();
  if (listDirty_) {
    RT_PROF(PF_BUILD);
    renderer_.build(doc_);
    listDirty_ = false;
  }
  if (fullRepaint_) {
    damage_.clear();
    damage_.push_back({0, 0, W, H});
    // still mark items as painted
    std::vector<RectI> tmp;
    renderer_.collectDamage(tmp, [](Element *) { return true; });
    fullRepaint_ = false;
  } else {
    renderer_.collectDamage(damage_, [this](Element *e) { return paintDirty_.count(e) > 0; });
  }
  paintDirty_.clear();
  mergeRects(damage_, disp_.alignX(), W, H);
  std::vector<RectI> todo = damage_;
  for (auto &r : prevDamage_) todo.push_back(r);
  mergeRects(todo, disp_.alignX(), W, H);
  if (todo.empty() && pendingTr_ == TR_NONE) {
    prevDamage_.clear();
    damageHistory_.clear();
    damage_.clear();
    return false;
  }
  static const bool damageDebug = getenv("RT_DAMAGE_DEBUG") != nullptr;
  if (damageDebug) {
    RectI u;
    for (auto &r : todo) u = u.empty() ? r : u.unite(r);
    log("render: todo %d rects bbox %d,%d %dx%d hist %d (new %d)", (int)todo.size(), u.x, u.y, u.w, u.h,
        (int)damageHistory_.size(), (int)damage_.size());
  }
  uint32_t px = 0;
  gPhase = PH_PAINT;
  for (auto &r : todo) {
    px += r.w * r.h;
    disp_.render(r, [&](const Band &b) { renderer_.paint(b, r.intersect(b.rect())); });
  }
  stats_.lastPaintUs = (uint32_t)(plat_.micros() - t0);
  stats_.lastDamagePx = px;
  if (pendingTr_ != TR_NONE && pendingTr_ != TR_FADE) {
    TransitionKind tr = pendingTr_;
    pendingTr_ = TR_NONE;
    // whole page was re-rendered? make sure: transition needs a complete back buffer
    RectI area{0, 0, W, H};
    if (disp_.transition(tr, 320, area)) {
      // the other buffers are stale
      damageHistory_.assign(std::max(1, disp_.bufferCount() - 1), std::vector<RectI>{{0, 0, W, H}});
      prevDamage_ = {{0, 0, W, H}};
      damage_.clear();
      stats_.frames++;
      return true;
    }
  }
  pendingTr_ = TR_NONE;
  disp_.present();
  damageHistory_.insert(damageHistory_.begin(), damage_);
  damageHistory_.resize(std::max(1, disp_.bufferCount() - 1));
  prevDamage_.clear();
  for (auto &h : damageHistory_) prevDamage_.insert(prevDamage_.end(), h.begin(), h.end());
  damage_.clear();
  stats_.frames++;
  stats_.lastFrameUs = (uint32_t)(plat_.micros() - t0);
  return true;
}

bool Engine::update() {
  uint64_t now = plat_.micros();
  double nowMs = now / 1000.0;
  if (js_) js_->armWatchdog(kScriptWatchdogMs);
  plat_.pollAsync();
  if (!posted_.empty()) {
    gPhase = PH_SCRIPT;
    std::vector<std::function<void()>> q;
    q.swap(posted_);
    for (auto &f : q) f();
  }
  gPhase = PH_OS;
  if (os_) os_->tick(nowMs);
  gPhase = PH_IDLE;
  if (!doc_) return false;
  gPhase = PH_SCRIPT;
  input_->tick(nowMs);
  widgets_->tick(nowMs);
  if (js_) js_->tick((uint64_t)nowMs);
  if (!pendingOpen_.empty()) {
    gPhase = PH_LOAD;
    std::string u = pendingOpen_;
    pendingOpen_.clear();
    open(u, pendingTr_);
  }
  gPhase = PH_LAYOUT;
  anim_.setNow(nowMs);
  if (anim_.active()) {
    anim_.tick(nowMs, [this](Element *e, uint8_t f) {
      if (f & 0x80) {
        e->flags |= NF_STYLE_DIRTY;
        for (Element *a = e->parent; a; a = a->parent) a->flags |= NF_DESC_STYLE_DIRTY;
        styleDirty_ = true;
      }
      styleChanged(e, f & 0x7F);
      frameRequested_ = true;
    });
  }
  bool styleDirty = styleDirty_ || (doc_->html && (doc_->html->flags & (NF_DESC_STYLE_DIRTY | NF_STYLE_DIRTY))) ||
                    (doc_->flags & NF_DESC_STYLE_DIRTY);
  bool want = frameRequested_ || styleDirty || layoutDirty_ || listDirty_ || !paintDirty_.empty() || !damage_.empty() ||
              !prevDamage_.empty() || fullRepaint_ || pendingTr_ != TR_NONE || (js_ && js_->wantsFrame());
  if (!want) {
    gPhase = PH_IDLE;
    return false;
  }
  static const bool wantDebug = getenv("RT_WANT_DEBUG") != nullptr;
  if (wantDebug)
    log("want: req %d style %d layout %d list %d paint %d dmg %d prev %d full %d tr %d js %d", frameRequested_, styleDirty,
        layoutDirty_, listDirty_, (int)paintDirty_.size(), (int)damage_.size(), (int)prevDamage_.size(), fullRepaint_,
        pendingTr_ != TR_NONE, js_ && js_->wantsFrame());
  if (js_ && js_->wantsFrame()) {
    gPhase = PH_SCRIPT;
    js_->animationFrame(nowMs);
    gPhase = PH_LAYOUT;
  }
  frameRequested_ = false;
  bool drew = render();
  gPhase = PH_IDLE;
  if (!drew) return false;
  // fps
  fpsFrames_++;
  if (now - fpsT0_ >= 1000000) {
    stats_.fps = fpsFrames_ * 1e6f / (float)(now - fpsT0_);
    fpsT0_ = now;
    fpsFrames_ = 0;
  }
  return true;
}

uint32_t Engine::idleMs() const {
  if (!doc_) return 50;
  if (frameRequested_ || anim_.active() || input_->busy() || !prevDamage_.empty() || (js_ && js_->wantsFrame())) return 0;
  uint64_t now = plat_.micros() / 1000;
  uint64_t next = js_ ? js_->nextTimerMs() : UINT64_MAX;
  uint64_t wnext = widgets_->nextWakeMs();
  next = std::min(next, wnext);
  if (next == UINT64_MAX) return 50;
  return next <= now ? 0 : (uint32_t)std::min<uint64_t>(next - now, 50);
}

// ============================================================ activation

void Engine::activate(Element *target, float x, float y) {
  if (!target) return;
  if (widgets_->activate(target, x, y)) return;
  for (Element *p = target; p; p = p->parent) {
    if (p->nodeType != NT_ELEMENT) break;
    if (p->tagId() == TAG_a && p->hasAttr(A.href)) {
      std::string href = p->attrOr(A.href, "");
      std::string trn = toLower(p->hasAttr(A.transition) ? p->attrOr(A.transition, "slide")
                                                         : p->attrOr(intern("data-transition"), "slide"));
      bool back = p->hasAttr(intern("data-back")) || href == "#back";
      TransitionKind tk = trn == "none" ? TR_NONE : trn == "up" ? TR_SLIDE_UP : trn == "down" ? TR_SLIDE_DOWN
                        : trn == "fade" ? TR_FADE : back ? TR_SLIDE_RIGHT : TR_SLIDE_LEFT;
      if (href == "#back" || href == "javascript:history.back()") navigateHistory(-1);
      else if (!href.empty() && href[0] == '#') setHash(href.substr(1), true, tk);
      else if (href.find("://") == std::string::npos && !startsWith(href, "javascript:") && !startsWith(href, "mailto:")) {
        if (!os_ || !os_->linkActivated(Bundle::resolve(url_, href))) openLater(href, tk);
      }
      return;
    }
  }
  // tapping outside a focused text field blurs it (closing the keyboard)
  if (focus_ && !focus_->isInclusiveAncestorOf(target) && !widgets_->isInternal(target)) focus(nullptr);
}

void Engine::setModal(Element *d, bool open) {
  if (!doc_ || !doc_->html) return;
  auto it = backdrops_.find(d);
  if (open) {
    d->extra().modal = true;
    if (!d->hasAttr(A.open)) d->setAttr(A.open, "");
    if (it == backdrops_.end()) {
      Color c{15, 23, 42, 110};
      auto &ps = d->extra().pseudoStyles[PE_BACKDROP];
      if (ps && ps->bgColor.a) c = ps->bgColor;
      char css[160];
      snprintf(css, sizeof css, "position:fixed;left:0;top:0;right:0;bottom:0;z-index:999;background:rgba(%d,%d,%d,%.3f)",
               c.r, c.g, c.b, c.a / 255.0);
      Element *b = doc_->createElement(TAG_div);
      b->flags |= NF_INTERNAL;
      b->setAttr(A.style, css);
      b->setAttr(intern("data-backdrop"), "");
      doc_->html->appendChild(b);
      backdrops_[d] = b;
    }
  } else {
    if (d->x) d->x->modal = false;
    if (d->hasAttr(A.open)) d->removeAttr(A.open);
    if (it != backdrops_.end()) {
      Element *b = it->second;
      backdrops_.erase(it);
      b->remove();
    }
  }
  requestFrame();
}

// ============================================================ input passthrough

void Engine::pointer(int id, PointerPhase phase, float x, float y) {
  if (js_) js_->armWatchdog(kScriptWatchdogMs);  // app handlers run from here
  if (os_ && os_->pointer(id, phase, x, y)) {
    if (gDebugFlags & DBG_INPUT) log("input: %s %.0f,%.0f -> system", phase == PTR_DOWN ? "down" : phase == PTR_UP ? "up" : phase == PTR_MOVE ? "move" : "cancel", x, y);
    return;
  }
  input_->pointer(id, phase, x, y);
}
void Engine::typeText(const std::string &utf8) { widgets_->typeText(utf8); }
void Engine::keyDown(const std::string &key) { widgets_->key(key); }

// ============================================================ hot reload

void Engine::hotUpdate(const std::string &path, const uint8_t *data, size_t len) {
  bundle_.put(path, data, len);
  std::string ext = path.substr(path.rfind('.') == std::string::npos ? path.size() : path.rfind('.'));
  if (ext == ".css") {
    // restyle in place: keep the DOM and script state
    collectStyles();
    styleDirty_ = true;
    markLayoutDirty();
    fullRepaint_ = true;
    log("hot: %s (styles)", path.c_str());
  } else if (ext == ".fnt" || path == "manifest.json") {
    loadFonts();
    markLayoutDirty();
    fullRepaint_ = true;
  } else if (ext == ".img" || ext == ".png" || ext == ".jpg" || ext == ".svg") {
    images_.clear();
    markLayoutDirty();
    fullRepaint_ = true;
  } else if (startsWith(path, "components/") && ext == ".html") {
    // components: <include src="components/x.html"> instances are re-instantiated
    std::string html((const char *)data, len);
    int n = 0;
    if (doc_)
      doc_->forEachElement([&](Element *e) {
        if (e->tag == TAG_include && Bundle::resolve(url_, e->attrOr(A.src, "")) == path) {
          e->removeAllChildren();
          Element *frag = parseFragment(doc_, html, TAG_div);
          frag->ref();
          e->appendChild(frag);
          frag->unref();
          n++;
        }
      });
    log("hot: %s (%d instances)", path.c_str(), n);
  } else {
    // html / js: reload the page, keeping the route
    log("hot: %s (reload)", path.c_str());
    if (path == url_ || ext == ".js" || ext == ".html") pendingOpen_ = url_ + (hash_.empty() ? "" : "#" + hash_);
  }
  frameRequested_ = true;
}

int Engine::replaceComponent(const std::string &selector, const std::string &html, bool outer) {
  if (!doc_) return 0;
  SelectorList sl;
  if (!parseSelectorList(selector, sl)) return 0;
  std::vector<Element *> matches;
  doc_->forEachElement([&](Element *e) {
    if (e->isElement() && matchesAny(sl, e)) matches.push_back(e);
  });
  for (Element *e : matches) {
    Element *frag = parseFragment(doc_, html, e->tag);
    frag->ref();
    if (outer && e->parent) {
      e->parent->insertBefore(frag, e);
      e->remove();
    } else {
      e->removeAllChildren();
      e->appendChild(frag);
    }
    frag->unref();
  }
  return (int)matches.size();
}

}  // namespace rt
