#include "js.h"

#include <chrono>

#include "bootstrap_js.h"
#include "canvas2d.h"
#include "extra.h"
#include "input.h"
#include "widgets.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "os.h"

namespace rt {

// Platform memory hooks (the player routes these to PSRAM).
__attribute__((weak)) void *jsMalloc(size_t n) { return malloc(n); }
__attribute__((weak)) void *jsCalloc(size_t c, size_t n) { return calloc(c, n); }
__attribute__((weak)) void *jsRealloc(void *p, size_t n) { return realloc(p, n); }
__attribute__((weak)) void jsFree(void *p) { free(p); }

static void *qjsCalloc(void *, size_t c, size_t n) { return jsCalloc(c, n); }
static void *qjsMalloc(void *, size_t n) { return jsMalloc(n); }
static void qjsFree(void *, void *p) { jsFree(p); }
static void *qjsRealloc(void *, void *p, size_t n) { return jsRealloc(p, n); }
static size_t qjsUsable(const void *) { return 0; }
static const JSMallocFunctions kMalloc = {qjsCalloc, qjsMalloc, qjsFree, qjsRealloc, qjsUsable};

#define HOST(ctx) ((JsHost *)JS_GetContextOpaque(ctx))
#define ENG(ctx) (HOST(ctx)->engine())
#define NATIVE(name) static JSValue name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)

static std::string str(JSContext *ctx, JSValueConst v) {
  size_t len;
  const char *s = JS_ToCStringLen(ctx, &len, v);
  std::string r = s ? std::string(s, len) : std::string();
  if (s) JS_FreeCString(ctx, s);
  return r;
}
static JSValue jstr(JSContext *ctx, const std::string &s) { return JS_NewStringLen(ctx, s.data(), s.size()); }
static double num(JSContext *ctx, JSValueConst v, double d = 0) {
  double r;
  if (JS_IsUndefined(v) || JS_ToFloat64(ctx, &r, v)) return d;
  return r;
}
static Node *argNode(JSContext *ctx, JSValueConst v) { return HOST(ctx)->unwrap(v); }
static Element *argEl(JSContext *ctx, JSValueConst v) {
  Node *n = argNode(ctx, v);
  return n ? n->asElement() : nullptr;
}
static JSValue arr(JSContext *ctx, const std::vector<JSValue> &v) {
  JSValue a = JS_NewArray(ctx);
  for (uint32_t i = 0; i < v.size(); i++) JS_SetPropertyUint32(ctx, a, i, v[i]);
  return a;
}
static JSValue farr(JSContext *ctx, std::initializer_list<double> v) {
  JSValue a = JS_NewArray(ctx);
  uint32_t i = 0;
  for (double d : v) JS_SetPropertyUint32(ctx, a, i++, JS_NewFloat64(ctx, d));
  return a;
}

// ============================================================ node wrappers

static JSClassID gNodeClass = 0;

static void nodeFinalizer(JSRuntime *, JSValueConst val) {
  Node *n = (Node *)JS_GetOpaque(val, gNodeClass);
  if (!n) return;
  n->js = nullptr;
  n->flags &= ~NF_JS_PINNED;
  n->unref();
}

JSValue JsHost::wrap(Node *n) {
  if (!n || !ctx_) return JS_NULL;
  if ((n->flags & NF_INTERNAL) && n->isElement()) return JS_NULL;
  if (n->js) return JS_DupValue(ctx_, JS_MKPTR(JS_TAG_OBJECT, n->js));
  std::string key;
  switch (n->nodeType) {
    case NT_TEXT: key = "#text"; break;
    case NT_COMMENT: key = "#comment"; break;
    case NT_DOCUMENT: key = "#document"; break;
    case NT_FRAGMENT: key = "#fragment"; break;
    default: key = ((Element *)n)->tagName(); break;
  }
  auto it = protos_.find(key);
  if (it == protos_.end() && n->nodeType == NT_ELEMENT) {
    const std::string &t = ((Element *)n)->tagName();
    it = protos_.find(t.find('-') != std::string::npos ? "*custom" : "*");
  }
  JSValue proto = it != protos_.end() ? it->second : JS_NULL;
  JSValue obj = JS_NewObjectProtoClass(ctx_, proto, nodeClass_);
  if (JS_IsException(obj)) return obj;
  JS_SetOpaque(obj, n);
  n->ref();
  n->js = JS_VALUE_GET_PTR(obj);
  if (n->connected()) pin(n);
  return obj;
}

Node *JsHost::unwrap(JSValueConst v) const { return (Node *)JS_GetOpaque(v, nodeClass_); }

void JsHost::pin(Node *n) {
  if (!n->js || (n->flags & NF_JS_PINNED)) return;
  JS_DupValue(ctx_, JS_MKPTR(JS_TAG_OBJECT, n->js));
  n->flags |= NF_JS_PINNED;
  pinned_.push_back(n);
}

void JsHost::unpin(Node *n) {
  if (!(n->flags & NF_JS_PINNED)) return;
  n->flags &= ~NF_JS_PINNED;
  auto it = std::find(pinned_.begin(), pinned_.end(), n);
  if (it != pinned_.end()) pinned_.erase(it);
  if (n->js) JS_FreeValue(ctx_, JS_MKPTR(JS_TAG_OBJECT, n->js));
}

void JsHost::nodeConnected(Node *n, bool connected) {
  if (!ctx_) return;
  // pin/unpin wrappers of the subtree
  uint32_t nodes = 0;
  bool wrapped = false;
  std::function<void(Node *)> walk = [&](Node *x) {
    if (connected) pin(x);
    else unpin(x);
    nodes++;
    if (x->js) wrapped = true;
    if (Element *e = x->asElement())
      for (Node *c = e->first; c; c = c->next) walk(c);
  };
  n->ref();
  walk(n);
  if (!connected && wrapped) {  // a wrapper keeps its subtree, maybe till the GC
    detached_ += nodes;
    lastDetachUs_ = eng_.platform().micros();
  }
  // custom element callbacks
  if (Element *e = n->asElement()) {
    std::vector<Element *> customs;
    e->forEachElement([&](Element *d) {
      if (d->tagName().find('-') != std::string::npos && !(d->flags & NF_INTERNAL)) customs.push_back(d);
    });
    for (Element *d : customs) {
      JSValue w = wrap(d);
      JSValue args[2] = {w, JS_NewBool(ctx_, connected)};
      JSValue r = JS_Call(ctx_, connectedFn_, JS_UNDEFINED, 2, args);
      if (JS_IsException(r)) reportException("connectedCallback");
      JS_FreeValue(ctx_, r);
      JS_FreeValue(ctx_, w);
    }
  }
  n->unref();
}

void JsHost::attributeChanged(Element *e, Atom name) {  // custom elements
  if (!ctx_ || e->tagName().find('-') == std::string::npos || !e->js) return;
  JSValue w = wrap(e);
  JSValue args[2] = {w, jstr(ctx_, atomStr(name))};
  JSValue r = JS_Call(ctx_, attrFn_, JS_UNDEFINED, 2, args);
  if (JS_IsException(r)) reportException("attributeChangedCallback");
  JS_FreeValue(ctx_, r);
  JS_FreeValue(ctx_, args[1]);
  JS_FreeValue(ctx_, w);
}

void JsHost::nodeDestroyed(Node *n) { (void)n; }

void JsHost::setProto(const std::string &key, JSValueConst proto) {
  auto it = protos_.find(key);
  if (it != protos_.end()) JS_FreeValue(ctx_, it->second);
  protos_[key] = JS_DupValue(ctx_, proto);
}

// ============================================================ natives: tree

NATIVE(n_nodeType) {
  Node *n = argNode(ctx, argv[0]);
  return JS_NewInt32(ctx, n ? n->nodeType : 0);
}
NATIVE(n_nodeName) {
  Node *n = argNode(ctx, argv[0]);
  if (!n) return JS_NULL;
  switch (n->nodeType) {
    case NT_TEXT: return jstr(ctx, "#text");
    case NT_COMMENT: return jstr(ctx, "#comment");
    case NT_DOCUMENT: return jstr(ctx, "#document");
    case NT_FRAGMENT: return jstr(ctx, "#document-fragment");
    default: {
      std::string t = ((Element *)n)->tagName();
      for (auto &c : t) c = (char)toupper((unsigned char)c);
      return jstr(ctx, t);
    }
  }
}
static Node *visibleNext(Node *n) {
  while (n && (n->flags & NF_INTERNAL)) n = n->next;
  return n;
}
static Node *visiblePrev(Node *n) {
  while (n && (n->flags & NF_INTERNAL)) n = n->prev;
  return n;
}
NATIVE(n_parent) {
  Node *n = argNode(ctx, argv[0]);
  return n && n->parent ? HOST(ctx)->wrap(n->parent) : JS_NULL;
}
NATIVE(n_first) {
  Element *e = argEl(ctx, argv[0]);
  return e ? HOST(ctx)->wrap(visibleNext(e->first)) : JS_NULL;
}
NATIVE(n_last) {
  Element *e = argEl(ctx, argv[0]);
  return e ? HOST(ctx)->wrap(visiblePrev(e->last)) : JS_NULL;
}
NATIVE(n_next) {
  Node *n = argNode(ctx, argv[0]);
  return n ? HOST(ctx)->wrap(visibleNext(n->next)) : JS_NULL;
}
NATIVE(n_prev) {
  Node *n = argNode(ctx, argv[0]);
  return n ? HOST(ctx)->wrap(visiblePrev(n->prev)) : JS_NULL;
}
NATIVE(n_childNodes) {
  Element *e = argEl(ctx, argv[0]);
  std::vector<JSValue> v;
  if (e)
    for (Node *c = e->first; c; c = c->next)
      if (!(c->flags & NF_INTERNAL)) v.push_back(HOST(ctx)->wrap(c));
  return arr(ctx, v);
}
NATIVE(n_children) {
  Element *e = argEl(ctx, argv[0]);
  std::vector<JSValue> v;
  if (e)
    for (Node *c = e->first; c; c = c->next)
      if (c->isElement() && !(c->flags & NF_INTERNAL)) v.push_back(HOST(ctx)->wrap(c));
  return arr(ctx, v);
}
NATIVE(n_insert) {
  Element *p = argEl(ctx, argv[0]);
  Node *c = argNode(ctx, argv[1]);
  Node *ref = argc > 2 ? argNode(ctx, argv[2]) : nullptr;
  if (!p || !c) return JS_ThrowTypeError(ctx, "insert: not a node");
  if (c->isContainer() && c != p && c->isInclusiveAncestorOf(p)) return JS_ThrowTypeError(ctx, "HierarchyRequestError");
  p->insertBefore(c, ref);
  return JS_DupValue(ctx, argv[1]);
}
NATIVE(n_removeChild) {
  Element *p = argEl(ctx, argv[0]);
  Node *c = argNode(ctx, argv[1]);
  if (!p || !c || c->parent != p) return JS_ThrowTypeError(ctx, "NotFoundError: not a child");
  c->ref();
  p->removeChild(c);
  JSValue r = JS_DupValue(ctx, argv[1]);
  c->unref();
  return r;
}
NATIVE(n_remove) {
  Node *n = argNode(ctx, argv[0]);
  if (n && n->parent) {
    n->ref();
    n->parent->removeChild(n);
    n->unref();
  }
  return JS_UNDEFINED;
}
NATIVE(n_clone) {
  Node *n = argNode(ctx, argv[0]);
  if (!n) return JS_NULL;
  Node *c = cloneNode(n, JS_ToBool(ctx, argv[1]), ENG(ctx).document());
  c->ref();
  // form state follows
  if (Element *ce = c->asElement())
    ce->forEachElement([&](Element *d) {
      d->flags |= NF_STYLE_DIRTY;
    });
  JSValue r = HOST(ctx)->wrap(c);
  c->unref();
  return r;
}
NATIVE(n_contains) {
  Node *a = argNode(ctx, argv[0]), *b = argNode(ctx, argv[1]);
  return JS_NewBool(ctx, a && b && a->isInclusiveAncestorOf(b));
}
NATIVE(n_create) {
  Document *d = ENG(ctx).document();
  if (!d) return JS_NULL;
  Element *e = d->createElement(str(ctx, argv[0]));
  e->ref();
  JSValue r = HOST(ctx)->wrap(e);
  e->unref();
  return r;
}
NATIVE(n_text) {
  Document *d = ENG(ctx).document();
  if (!d) return JS_NULL;
  Text *t = d->createText(str(ctx, argv[0]));
  t->ref();
  JSValue r = HOST(ctx)->wrap(t);
  t->unref();
  return r;
}
NATIVE(n_comment) {
  Comment *c = new Comment(str(ctx, argv[0]));
  c->doc = ENG(ctx).document();
  c->ref();
  JSValue r = HOST(ctx)->wrap(c);
  c->unref();
  return r;
}
NATIVE(n_fragment) {
  Element *f = new Element(0, NT_FRAGMENT);
  f->doc = ENG(ctx).document();
  f->ref();
  JSValue r = HOST(ctx)->wrap(f);
  f->unref();
  return r;
}
NATIVE(n_getText) {
  Node *n = argNode(ctx, argv[0]);
  if (!n) return JS_NULL;
  if (n->nodeType == NT_COMMENT) return jstr(ctx, ((Comment *)n)->data);
  std::string s = n->textContent();
  // hide the zero-width spaces the compiler may have inserted? (they are not inserted into the DOM)
  return jstr(ctx, s);
}
NATIVE(n_setText) {
  Node *n = argNode(ctx, argv[0]);
  std::string s = str(ctx, argv[1]);
  if (!n) return JS_UNDEFINED;
  if (Text *t = n->asText()) {
    t->setData(s);
  } else if (n->nodeType == NT_COMMENT) {
    ((Comment *)n)->data = s;
  } else if (Element *e = n->asElement()) {
    // fast path: a single text child is updated in place
    if (e->first && e->first == e->last && e->first->isText()) {
      ((Text *)e->first)->setData(s);
    } else {
      e->removeAllChildren();
      if (!s.empty()) e->appendChild((Document::alive(e->doc) ? e->doc : ENG(ctx).document())->createText(s));
    }
  }
  return JS_UNDEFINED;
}

// ============================================================ attributes

NATIVE(n_attr) {
  Element *e = argEl(ctx, argv[0]);
  if (!e) return JS_NULL;
  std::string name = toLower(str(ctx, argv[1]));
  Atom a = findAtom(name.data(), name.size());
  const std::string *v = a ? e->getAttr(a) : nullptr;
  return v ? jstr(ctx, *v) : JS_NULL;
}
NATIVE(n_setAttr) {
  Element *e = argEl(ctx, argv[0]);
  if (e) e->setAttr(internLower(str(ctx, argv[1])), str(ctx, argv[2]));
  return JS_UNDEFINED;
}
NATIVE(n_removeAttr) {
  Element *e = argEl(ctx, argv[0]);
  if (!e) return JS_FALSE;
  std::string name = toLower(str(ctx, argv[1]));
  Atom a = findAtom(name.data(), name.size());
  return JS_NewBool(ctx, a && e->removeAttr(a));
}
NATIVE(n_attrNames) {
  Element *e = argEl(ctx, argv[0]);
  std::vector<JSValue> v;
  if (e)
    for (auto &a : e->attrs) v.push_back(jstr(ctx, atomStr(a.name)));
  return arr(ctx, v);
}

// ============================================================ HTML

NATIVE(n_innerHTML) {
  Node *n = argNode(ctx, argv[0]);
  return n ? jstr(ctx, serializeNode(n, false)) : JS_NULL;
}
NATIVE(n_outerHTML) {
  Node *n = argNode(ctx, argv[0]);
  return n ? jstr(ctx, serializeNode(n, true)) : JS_NULL;
}
NATIVE(n_setInnerHTML) {
  Element *e = argEl(ctx, argv[0]);
  if (!e) return JS_UNDEFINED;
  std::string html = str(ctx, argv[1]);
  Element *frag = parseFragment(ENG(ctx).document(), html, e->tag);
  frag->ref();
  e->removeAllChildren();
  e->appendChild(frag);
  frag->unref();
  return JS_UNDEFINED;
}
NATIVE(n_setOuterHTML) {
  Element *e = argEl(ctx, argv[0]);
  if (!e || !e->parent) return JS_UNDEFINED;
  Element *frag = parseFragment(ENG(ctx).document(), str(ctx, argv[1]), e->parent->tag);
  frag->ref();
  e->ref();
  e->parent->insertBefore(frag, e);
  e->remove();
  e->unref();
  frag->unref();
  return JS_UNDEFINED;
}
NATIVE(n_insertHTML) {
  Element *e = argEl(ctx, argv[0]);
  if (!e) return JS_UNDEFINED;
  std::string pos = toLower(str(ctx, argv[1]));
  Element *ctxEl = (pos == "beforebegin" || pos == "afterend") ? e->parent : e;
  if (!ctxEl) return JS_UNDEFINED;
  Element *frag = parseFragment(ENG(ctx).document(), str(ctx, argv[2]), ctxEl->tag);
  frag->ref();
  if (pos == "beforebegin") e->parent->insertBefore(frag, e);
  else if (pos == "afterbegin") e->insertBefore(frag, e->first);
  else if (pos == "beforeend") e->appendChild(frag);
  else if (pos == "afterend") e->parent->insertBefore(frag, e->next);
  frag->unref();
  return JS_UNDEFINED;
}
NATIVE(n_parseHTML) {  // DOMParser / template content: returns a fragment
  Element *frag = parseFragment(ENG(ctx).document(), str(ctx, argv[0]), TAG_div);
  frag->ref();
  JSValue r = HOST(ctx)->wrap(frag);
  frag->unref();
  return r;
}

// ============================================================ selectors

static bool selectorFor(JSContext *ctx, JSValueConst v, SelectorList &out) {
  std::string s = str(ctx, v);
  if (!parseSelectorList(s, out)) {
    JS_ThrowSyntaxError(ctx, "'%s' is not a valid selector", s.c_str());
    return false;
  }
  return true;
}
NATIVE(n_qs) {
  Element *root = argEl(ctx, argv[0]);
  SelectorList sl;
  if (!root) return JS_NULL;
  if (!selectorFor(ctx, argv[1], sl)) return JS_EXCEPTION;
  Element *found = nullptr;
  Element *scope = root->nodeType == NT_ELEMENT ? root : nullptr;
  for (Node *c = root->first; c && !found; c = c->next) {
    if (!c->isElement() || (c->flags & NF_INTERNAL)) continue;
    ((Element *)c)->forEachElement([&](Element *e) {
      if (!found && !(e->flags & NF_INTERNAL) && matchesAny(sl, e, scope)) found = e;
    });
  }
  return HOST(ctx)->wrap(found);
}
NATIVE(n_qsa) {
  Element *root = argEl(ctx, argv[0]);
  SelectorList sl;
  if (!root) return JS_NewArray(ctx);
  if (!selectorFor(ctx, argv[1], sl)) return JS_EXCEPTION;
  std::vector<JSValue> v;
  Element *scope = root->nodeType == NT_ELEMENT ? root : nullptr;
  for (Node *c = root->first; c; c = c->next) {
    if (!c->isElement() || (c->flags & NF_INTERNAL)) continue;
    ((Element *)c)->forEachElement([&](Element *e) {
      if (!(e->flags & NF_INTERNAL) && matchesAny(sl, e, scope)) v.push_back(HOST(ctx)->wrap(e));
    });
  }
  return arr(ctx, v);
}
NATIVE(n_matches) {
  Element *e = argEl(ctx, argv[0]);
  SelectorList sl;
  if (!e) return JS_FALSE;
  if (!selectorFor(ctx, argv[1], sl)) return JS_EXCEPTION;
  return JS_NewBool(ctx, matchesAny(sl, e));
}
NATIVE(n_closest) {
  Element *e = argEl(ctx, argv[0]);
  SelectorList sl;
  if (!e) return JS_NULL;
  if (!selectorFor(ctx, argv[1], sl)) return JS_EXCEPTION;
  for (Element *p = e; p && p->nodeType == NT_ELEMENT; p = p->parent)
    if (matchesAny(sl, p)) return HOST(ctx)->wrap(p);
  return JS_NULL;
}
NATIVE(n_byId) {
  Document *d = ENG(ctx).document();
  return d ? HOST(ctx)->wrap(d->getElementById(str(ctx, argv[0]))) : JS_NULL;
}
NATIVE(n_document) { return HOST(ctx)->wrap(ENG(ctx).document()); }

// ============================================================ style

static std::string kebab(const std::string &n) {
  if (startsWith(n, "--")) return n;
  std::string r;
  for (size_t i = 0; i < n.size(); i++) {
    char c = n[i];
    if (c >= 'A' && c <= 'Z') {
      r += '-';
      r += (char)(c + 32);
    } else {
      r += c;
    }
  }
  if (startsWith(r, "webkit-") || startsWith(r, "moz-")) r = "-" + r;
  if (r == "css-float") r = "float";
  return r;
}

static void syncStyleAttr(Element *e) {
  // keep the style attribute in sync with the declaration block (for getAttribute/outerHTML)
  std::string s;
  if (e->inlineStyle)
    for (auto &d : e->inlineStyle->decls) {
      if (!s.empty()) s += " ";
      s += (d.prop == P_CUSTOM || d.prop == 0xFFFD ? atomStr(d.customName) : std::string(propName((Prop)d.prop))) + ": " +
           serializeValue((Prop)d.prop, d.v) + (d.important ? " !important" : "") + ";";
    }
  // bypass re-parsing in attrChanged by setting the attribute directly
  bool had = false;
  for (auto &a : e->attrs)
    if (a.name == A.style) {
      a.value = s;
      had = true;
    }
  if (!had && !s.empty()) e->attrs.push_back({A.style, s});
}

NATIVE(n_getStyle) {
  Element *e = argEl(ctx, argv[0]);
  if (!e || !e->inlineStyle) return jstr(ctx, "");
  std::string name = kebab(str(ctx, argv[1]));
  if (startsWith(name, "--")) {
    const Decl *d = e->inlineStyle->find(P_CUSTOM, intern(name));
    return jstr(ctx, d ? d->v.str() : "");
  }
  int p = propByName(name);
  if (p < 0) {
    // shorthand: reconstruct from the first longhand we know about
    std::vector<Decl> tmp;
    return jstr(ctx, "");
  }
  const Decl *d = e->inlineStyle->find((uint16_t)p);
  return jstr(ctx, d ? (d->v.str().empty() ? serializeValue((Prop)p, d->v) : d->v.str()) : "");
}
NATIVE(n_setStyle) {
  Element *e = argEl(ctx, argv[0]);
  if (!e) return JS_UNDEFINED;
  std::string name = kebab(str(ctx, argv[1]));
  std::string value = trim(str(ctx, argv[2]));
  bool important = argc > 3 && str(ctx, argv[3]) == "important";
  if (!e->inlineStyle) e->inlineStyle = new DeclBlock();
  if (value.empty()) {
    // remove the property (and its longhands for shorthands)
    std::vector<Decl> probe;
    if (startsWith(name, "--")) e->inlineStyle->remove(P_CUSTOM, intern(name));
    else if (parsePropertyValue(name, "initial", probe))
      for (auto &d : probe) e->inlineStyle->remove(d.prop, d.customName);
  } else {
    std::vector<Decl> decls;
    if (!parsePropertyValue(name, value, decls, important)) return JS_UNDEFINED;  // invalid: ignored like browsers
    for (auto &d : decls) e->inlineStyle->set(std::move(d));
  }
  syncStyleAttr(e);
  e->flags |= NF_STYLE_DIRTY;
  for (Element *a = e->parent; a; a = a->parent) a->flags |= NF_DESC_STYLE_DIRTY;
  ENG(ctx).requestFrame();
  return JS_UNDEFINED;
}
NATIVE(n_cssText) {
  Element *e = argEl(ctx, argv[0]);
  return jstr(ctx, e ? e->attrOr(A.style, "") : "");
}
NATIVE(n_setCssText) {
  Element *e = argEl(ctx, argv[0]);
  if (e) e->setAttr(A.style, str(ctx, argv[1]));
  return JS_UNDEFINED;
}
NATIVE(n_styleNames) {
  Element *e = argEl(ctx, argv[0]);
  std::vector<JSValue> v;
  if (e && e->inlineStyle)
    for (auto &d : e->inlineStyle->decls)
      v.push_back(jstr(ctx, d.prop == P_CUSTOM ? atomStr(d.customName) : std::string(propName((Prop)d.prop))));
  return arr(ctx, v);
}

static std::string colorCss(Color c) {
  char b[64];
  if (c.a == 255) snprintf(b, sizeof b, "rgb(%d, %d, %d)", c.r, c.g, c.b);
  else snprintf(b, sizeof b, "rgba(%d, %d, %d, %g)", c.r, c.g, c.b, std::round(c.a / 255.0 * 1000) / 1000);
  return b;
}
static std::string pxCss(float v) {
  char b[32];
  snprintf(b, sizeof b, "%gpx", std::round(v * 100) / 100);
  return b;
}

NATIVE(n_computed) {
  Element *e = argEl(ctx, argv[0]);
  if (!e) return jstr(ctx, "");
  ENG(ctx).flushLayout();
  if (!e->cs) return jstr(ctx, "");
  const ComputedStyle &s = *e->cs;
  std::string p = kebab(str(ctx, argv[1]));
  Box *b = e->box;
  if (startsWith(p, "--")) {
    if (s.customProps)
      for (auto &kv : *s.customProps)
        if (atomStr(kv.first) == p) return jstr(ctx, kv.second);
    return jstr(ctx, "");
  }
  static const char *displayNames[] = {"none", "block", "inline", "inline-block", "flex", "inline-flex", "grid", "inline-grid",
                                       "table", "inline-table", "table-row", "table-cell", "table-row-group",
                                       "table-header-group", "table-footer-group", "table-caption", "table-column",
                                       "table-column-group", "list-item", "contents", "flow-root"};
  static const char *posNames[] = {"static", "relative", "absolute", "fixed", "sticky"};
  if (p == "display") return jstr(ctx, displayNames[s.display]);
  if (p == "position") return jstr(ctx, posNames[s.position]);
  if (p == "color") return jstr(ctx, colorCss(s.color));
  if (p == "background-color") return jstr(ctx, colorCss(s.bgColor));
  if (p == "opacity") return jstr(ctx, std::to_string(s.opacity).substr(0, 5));
  if (p == "visibility") return jstr(ctx, s.visibility == VIS_VISIBLE ? "visible" : "hidden");
  if (p == "font-size") return jstr(ctx, pxCss(s.fontSize));
  if (p == "font-weight") return jstr(ctx, std::to_string(s.fontWeight));
  if (p == "line-height") return jstr(ctx, s.lineHeightIsNum ? pxCss(s.lineHeightNum * s.fontSize) : s.lineHeight > 0 ? pxCss(s.lineHeight) : "normal");
  if (p == "font-family" && s.fontFamily) {
    std::string r;
    for (auto &f : *s.fontFamily) r += (r.empty() ? "" : ", ") + f;
    return jstr(ctx, r);
  }
  if (b) {
    if (p == "width") return jstr(ctx, pxCss(s.boxSizing == BOX_BORDER ? b->w : b->contentW()));
    if (p == "height") return jstr(ctx, pxCss(s.boxSizing == BOX_BORDER ? b->h : b->contentH()));
    if (p == "margin-top") return jstr(ctx, pxCss(b->m[0]));
    if (p == "margin-right") return jstr(ctx, pxCss(b->m[1]));
    if (p == "margin-bottom") return jstr(ctx, pxCss(b->m[2]));
    if (p == "margin-left") return jstr(ctx, pxCss(b->m[3]));
    if (p == "padding-top") return jstr(ctx, pxCss(b->p[0]));
    if (p == "padding-right") return jstr(ctx, pxCss(b->p[1]));
    if (p == "padding-bottom") return jstr(ctx, pxCss(b->p[2]));
    if (p == "padding-left") return jstr(ctx, pxCss(b->p[3]));
    if (p == "border-top-width") return jstr(ctx, pxCss(b->b[0]));
    if (p == "left") return jstr(ctx, pxCss(b->x));
    if (p == "top") return jstr(ctx, pxCss(b->y));
  }
  if (p == "transform") {
    if (!s.hasTransform()) return jstr(ctx, "none");
    Affine m = elementTransform(e);
    char buf[160];
    snprintf(buf, sizeof buf, "matrix(%g, %g, %g, %g, %g, %g)", m.a, m.b, m.c, m.d, m.e, m.f);
    return jstr(ctx, buf);
  }
  if (p == "overflow" || p == "overflow-y") return jstr(ctx, s.overflowY == OV_VISIBLE ? "visible" : s.overflowY == OV_HIDDEN ? "hidden" : s.overflowY == OV_SCROLL ? "scroll" : "auto");
  if (p == "z-index") return jstr(ctx, s.zAuto ? "auto" : std::to_string(s.zIndex));
  if (p == "pointer-events") return jstr(ctx, s.pointerEvents == PE_NONE_ ? "none" : "auto");
  return jstr(ctx, "");
}

// ============================================================ geometry & scrolling

NATIVE(n_rect) {
  Element *e = argEl(ctx, argv[0]);
  if (!e) return farr(ctx, {0, 0, 0, 0});
  ENG(ctx).flushLayout();
  ENG(ctx).renderer().build(ENG(ctx).document());
  if (!e->box || !e->cs || e->cs->display == D_NONE) return farr(ctx, {0, 0, 0, 0});
  RectF r = ENG(ctx).renderer().screenRect(e);
  return farr(ctx, {r.x, r.y, r.w, r.h});
}
NATIVE(n_offset) {
  Element *e = argEl(ctx, argv[0]);
  if (!e) return farr(ctx, {0, 0, 0, 0});
  ENG(ctx).flushLayout();
  if (!e->box || !e->cs || e->cs->display == D_NONE) return farr(ctx, {0, 0, 0, 0});
  Box &b = *e->box;
  Element *op = nullptr;
  for (Element *p = e->parentElement(); p; p = p->parentElement())
    if (p->cs && (p->cs->position != POS_STATIC || p->tagId() == TAG_body || p->tagId() == TAG_td || p->tagId() == TAG_table)) {
      op = p;
      break;
    }
  float ox = 0, oy = 0;
  if (op && op->box) {
    ox = op->box->absX + op->box->b[3];
    oy = op->box->absY + op->box->b[0];
    if (op->tagId() == TAG_body) ox = oy = 0;
  }
  JSValue a = farr(ctx, {std::round(b.absX - ox), std::round(b.absY - oy), std::round(b.w), std::round(b.h)});
  JS_SetPropertyUint32(ctx, a, 4, HOST(ctx)->wrap(op));
  return a;
}
NATIVE(n_client) {
  Element *e = argEl(ctx, argv[0]);
  if (!e) return farr(ctx, {0, 0, 0, 0});
  ENG(ctx).flushLayout();
  if (!e->box) return farr(ctx, {0, 0, 0, 0});
  Box &b = *e->box;
  if (e->tagId() == TAG_html) return farr(ctx, {0, 0, (double)ENG(ctx).display().width(), (double)ENG(ctx).display().height()});
  return farr(ctx, {b.b[3], b.b[0], b.w - b.b[1] - b.b[3], b.h - b.b[0] - b.b[2]});
}
NATIVE(n_scroll) {
  Element *e = argEl(ctx, argv[0]);
  if (!e) return farr(ctx, {0, 0, 0, 0});
  ENG(ctx).flushLayout();
  float sx = e->x ? e->x->scrollX : 0, sy = e->x ? e->x->scrollY : 0;
  float sw = e->box ? std::max(e->box->scrollW, e->box->w - e->box->b[1] - e->box->b[3]) : 0;
  float sh = e->box ? std::max(e->box->scrollH, e->box->h - e->box->b[0] - e->box->b[2]) : 0;
  if (e->tagId() == TAG_html && e->box) sh = std::max(sh, e->box->h + e->box->m[0] + e->box->m[2]);
  return farr(ctx, {sx, sy, sw, sh});
}
NATIVE(n_setScroll) {
  Element *e = argEl(ctx, argv[0]);
  if (!e) return JS_UNDEFINED;
  ENG(ctx).flushLayout();
  ENG(ctx).input().scrollTo(e, (float)num(ctx, argv[1]), (float)num(ctx, argv[2]), argc > 3 && JS_ToBool(ctx, argv[3]));
  return JS_UNDEFINED;
}
NATIVE(n_scrollIntoView) {
  Element *e = argEl(ctx, argv[0]);
  if (e) ENG(ctx).input().scrollIntoView(e, argc > 1 && JS_ToBool(ctx, argv[1]));
  return JS_UNDEFINED;
}
NATIVE(n_elementFromPoint) {
  ENG(ctx).flushLayout();
  ENG(ctx).renderer().build(ENG(ctx).document());
  Element *e = ENG(ctx).renderer().hitTest((float)num(ctx, argv[0]), (float)num(ctx, argv[1]), false);
  return HOST(ctx)->wrap(e);
}

// ============================================================ focus & forms

NATIVE(n_focus) {
  Element *e = argEl(ctx, argv[0]);
  ENG(ctx).focus(e);
  return JS_UNDEFINED;
}
NATIVE(n_blur) {
  Element *e = argEl(ctx, argv[0]);
  if (e && ENG(ctx).focused() == e) ENG(ctx).focus(nullptr);
  return JS_UNDEFINED;
}
NATIVE(n_active) { return HOST(ctx)->wrap(ENG(ctx).focused()); }
NATIVE(n_value) {
  Element *e = argEl(ctx, argv[0]);
  return e ? jstr(ctx, ENG(ctx).currentValue(e)) : jstr(ctx, "");
}
NATIVE(n_setValue) {
  Element *e = argEl(ctx, argv[0]);
  if (!e) return JS_UNDEFINED;
  std::string v = str(ctx, argv[1]);
  ENG(ctx).setValue(e, v, false);
  if (e->x) e->x->caret = (int)v.size();
  return JS_UNDEFINED;
}
NATIVE(n_checked) {
  Element *e = argEl(ctx, argv[0]);
  return JS_NewBool(ctx, e && e->hasState(ST_CHECKED));
}
NATIVE(n_setChecked) {
  Element *e = argEl(ctx, argv[0]);
  if (e) ENG(ctx).setChecked(e, JS_ToBool(ctx, argv[1]), false);
  return JS_UNDEFINED;
}
NATIVE(n_state) {  // generic state bit get/set: (el, bitName, [value])
  Element *e = argEl(ctx, argv[0]);
  std::string n = str(ctx, argv[1]);
  uint16_t bit = n == "indeterminate" ? ST_INDETERMINATE : n == "open" ? ST_OPEN : n == "invalid" ? ST_INVALID : 0;
  if (!e || !bit) return JS_FALSE;
  if (argc > 2) {
    e->setState(bit, JS_ToBool(ctx, argv[2]));
    ENG(ctx).invalidateElement(e);
    ENG(ctx).requestFrame();
  }
  return JS_NewBool(ctx, e->hasState(bit));
}
NATIVE(n_selectedIndex) {
  Element *e = argEl(ctx, argv[0]);
  return JS_NewInt32(ctx, e ? ENG(ctx).widgets().selectedIndex(e) : -1);
}
NATIVE(n_setSelectedIndex) {
  Element *e = argEl(ctx, argv[0]);
  if (e) ENG(ctx).widgets().setSelectedIndex(e, (int)num(ctx, argv[1], -1), false);
  return JS_UNDEFINED;
}
NATIVE(n_options) {
  Element *e = argEl(ctx, argv[0]);
  std::vector<JSValue> v;
  if (e)
    for (Element *o : ENG(ctx).widgets().options(e)) v.push_back(HOST(ctx)->wrap(o));
  return arr(ctx, v);
}
NATIVE(n_activate) {  // default action of a click (element.click())
  Element *e = argEl(ctx, argv[0]);
  if (e) ENG(ctx).activate(e);
  return JS_UNDEFINED;
}
NATIVE(n_naturalSize) {
  Element *e = argEl(ctx, argv[0]);
  auto img = e && e->x ? e->x->image : nullptr;
  return farr(ctx, {img ? (double)img->w : 0, img ? (double)img->h : 0});
}
NATIVE(n_modal) {  // dialog.showModal()/close(): backdrop + top layer
  Element *e = argEl(ctx, argv[0]);
  if (e) ENG(ctx).setModal(e, JS_ToBool(ctx, argv[1]));
  return JS_UNDEFINED;
}

// ============================================================ animations

NATIVE(n_animate) {
  Element *e = argEl(ctx, argv[0]);
  if (!e) return JS_NewInt32(ctx, 0);
  ENG(ctx).flushLayout();
  // frames: array of [offset, [[prop, value], ...]]
  std::vector<std::pair<float, std::vector<std::pair<std::string, std::string>>>> frames;
  int64_t n = 0;
  JS_GetLength(ctx, argv[1], &n);
  for (int64_t i = 0; i < n; i++) {
    JSValue f = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)i);
    JSValue off = JS_GetPropertyUint32(ctx, f, 0);
    JSValue props = JS_GetPropertyUint32(ctx, f, 1);
    std::pair<float, std::vector<std::pair<std::string, std::string>>> fr;
    fr.first = (float)num(ctx, off, n > 1 ? (double)i / (n - 1) : 1);
    int64_t pn = 0;
    JS_GetLength(ctx, props, &pn);
    for (int64_t k = 0; k < pn; k++) {
      JSValue kv = JS_GetPropertyUint32(ctx, props, (uint32_t)k);
      JSValue key = JS_GetPropertyUint32(ctx, kv, 0), val = JS_GetPropertyUint32(ctx, kv, 1);
      fr.second.push_back({kebab(str(ctx, key)), str(ctx, val)});
      JS_FreeValue(ctx, key);
      JS_FreeValue(ctx, val);
      JS_FreeValue(ctx, kv);
    }
    frames.push_back(fr);
    JS_FreeValue(ctx, off);
    JS_FreeValue(ctx, props);
    JS_FreeValue(ctx, f);
  }
  double dur = num(ctx, argv[2], 0), delay = num(ctx, argv[3], 0);
  std::string easing = str(ctx, argv[4]);
  TimingFn tf;
  {
    std::vector<Tok> toks;
    cssTokenize(easing.data(), easing.size(), toks);
    std::vector<Tok> clean;
    for (auto &t : toks)
      if (t.t != TK_WS) clean.push_back(t);
    size_t i = 0;
    if (!parseTimingFn(clean, i, tf)) tf = TimingFn{TimingFn::LINEAR};
  }
  double iters = num(ctx, argv[5], 1);
  int dir = (int)num(ctx, argv[6], 0), fill = (int)num(ctx, argv[7], 0);
  int id = ENG(ctx).animator().animate(e, frames, dur, delay, tf, (float)(iters < 0 ? INFINITY : iters), (uint8_t)dir,
                                       (uint8_t)fill, ENG(ctx).style());
  ENG(ctx).requestFrame();
  return JS_NewInt32(ctx, id);
}
NATIVE(n_animCtl) {
  int id = (int)num(ctx, argv[0]);
  std::string op = str(ctx, argv[1]);
  Animator &a = ENG(ctx).animator();
  if (op == "cancel") a.cancelAnimation(id);
  else if (op == "finish") a.finishAnimation(id);
  else if (op == "pause") a.pauseAnimation(id, true);
  else if (op == "play") a.pauseAnimation(id, false);
  else if (op == "finished") return JS_NewBool(ctx, a.animationFinished(id));
  ENG(ctx).requestFrame();
  return JS_UNDEFINED;
}

// ============================================================ timers & misc

NATIVE(n_now) { return JS_NewFloat64(ctx, ENG(ctx).platform().micros() / 1000.0); }
NATIVE(n_timer) {
  if (!JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
  std::vector<JSValue> args;
  for (int i = 3; i < argc; i++) args.push_back(JS_DupValue(ctx, argv[i]));
  double d = num(ctx, argv[1], 0);
  return JS_NewInt32(ctx, HOST(ctx)->addTimer(argv[0], (uint32_t)std::max(0.0, d), JS_ToBool(ctx, argv[2]), args));
}
NATIVE(n_clearTimer) {
  HOST(ctx)->clearTimer((int)num(ctx, argv[0]));
  return JS_UNDEFINED;
}
NATIVE(n_raf) {
  if (!JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
  return JS_NewInt32(ctx, HOST(ctx)->addRaf(argv[0]));
}
NATIVE(n_cancelRaf) {
  HOST(ctx)->cancelRaf((int)num(ctx, argv[0]));
  return JS_UNDEFINED;
}
NATIVE(n_log) {
  std::string level = str(ctx, argv[0]);
  std::string msg = str(ctx, argv[1]);
  ENG(ctx).log("%s%s", level == "log" || level == "info" ? "" : (level + ": ").c_str(), msg.c_str());
  return JS_UNDEFINED;
}
NATIVE(n_listen) {
  HOST(ctx)->listen(str(ctx, argv[0]));
  return JS_UNDEFINED;
}
NATIVE(n_setProto) {
  HOST(ctx)->setProto(str(ctx, argv[0]), argv[1]);
  return JS_UNDEFINED;
}
NATIVE(n_matchMedia) {
  std::string q = str(ctx, argv[0]);
  StyleSheet tmp;
  uint32_t order = 0;
  MediaEnv env = ENG(ctx).style().env();
  parseStyleSheet("@media " + q + " { a { color: red } }", tmp, 1, order, env);
  return JS_NewBool(ctx, !tmp.rules.empty());
}

// ============================================================ OS natives

static JSValue throwMsg(JSContext *ctx, const std::string &msg) {
  JSValue e = JS_NewError(ctx);
  JS_SetPropertyStr(ctx, e, "message", jstr(ctx, msg));
  return JS_Throw(ctx, e);
}

static Os *osOf(JSContext *ctx) { return HOST(ctx)->os(); }
#define NEED_OS()                                          \
  Os *os = osOf(ctx);                                      \
  if (!os) return throwMsg(ctx, "not running on the OS");

static JSValue appJson(JSContext *ctx, const OsApp &a) {
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "id", jstr(ctx, a.id));
  JS_SetPropertyStr(ctx, o, "title", jstr(ctx, a.title));
  JS_SetPropertyStr(ctx, o, "version", jstr(ctx, a.version));
  JS_SetPropertyStr(ctx, o, "icon", jstr(ctx, a.icon));
  JS_SetPropertyStr(ctx, o, "system", JS_NewBool(ctx, a.system));
  JS_SetPropertyStr(ctx, o, "source", jstr(ctx, a.system ? "system" : a.source));
  if (!a.storeVersionId.empty()) JS_SetPropertyStr(ctx, o, "versionId", jstr(ctx, a.storeVersionId));
  if (a.installed > 0) JS_SetPropertyStr(ctx, o, "installed", JS_NewFloat64(ctx, a.installed));
  return o;
}

NATIVE(n_os) {
  NEED_OS();
  std::string op = str(ctx, argv[0]);
  std::string err;
  if (op == "page") {
    JSValue o = appJson(ctx, os->current());
    JS_SetPropertyStr(ctx, o, "page", jstr(ctx, os->page()));
    JS_SetPropertyStr(ctx, o, "depth", JS_NewInt32(ctx, os->depth()));
    return o;
  }
  if (op == "apps") {
    std::vector<JSValue> v;
    for (auto &a : os->apps(true)) v.push_back(appJson(ctx, a));
    return arr(ctx, v);
  }
  if (op == "launch") {
    if (!os->launch(str(ctx, argv[1]), err)) return throwMsg(ctx, err);
    return JS_TRUE;
  }
  if (op == "home") {
    os->userHome(JS_ToBool(ctx, argv[1]));  // kiosk mode: the password first
    return JS_UNDEFINED;
  }
  if (op == "kiosk") {  // {app} while kiosk mode is on, else null
    if (os->kioskApp().empty()) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "app", jstr(ctx, os->kioskApp()));
    return o;
  }
  if (op == "kioskStart") {
    if (!os->kioskStart(str(ctx, argv[1]), str(ctx, argv[2]), err)) return throwMsg(ctx, err);
    return JS_TRUE;
  }
  if (op == "back") {
    os->back();
    return JS_UNDEFINED;
  }
  if (op == "navigate") {
    double d;
    if (JS_IsNumber(argv[1]) && JS_ToFloat64(ctx, &d, argv[1]) == 0) {
      if (d < 0) return JS_NewBool(ctx, os->navigateBack());
      return JS_FALSE;
    }
    if (!os->navigate(str(ctx, argv[1]), err)) return throwMsg(ctx, err);
    return JS_TRUE;
  }
  if (op == "confirm") {  // (title, message, ok, cancel) -> answered through the event bus
    JsHost *h = HOST(ctx);
    static int seq = 0;
    std::string name = "#confirm" + std::to_string(++seq);
    h->busOn(name, argv[4]);
    os->confirm(str(ctx, argv[1]), str(ctx, argv[2]), str(ctx, argv[3]), str(ctx, argv[5]),
                [h, name](bool ok) { h->busEmit(name, ok ? "true" : "false"); });
    return JS_UNDEFINED;
  }
  if (op == "privileged") return JS_NewBool(ctx, os->privileged());
  if (op == "version") return jstr(ctx, Os::kVersion);
  if (op == "processes") return HOST(ctx)->processList(ctx);
  if (op == "sdInfo") {
    SdInfo i = os->sdInfo();
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "mounted", JS_NewBool(ctx, i.mounted));
    JS_SetPropertyStr(ctx, o, "total", JS_NewFloat64(ctx, (double)i.totalBytes));
    JS_SetPropertyStr(ctx, o, "free", JS_NewFloat64(ctx, (double)i.freeBytes));
    JS_SetPropertyStr(ctx, o, "type", jstr(ctx, i.type));
    JS_SetPropertyStr(ctx, o, "name", jstr(ctx, i.name));
    return o;
  }
  if (op == "sdFormat") {
    if (!os->sdFormat(err)) return throwMsg(ctx, err);
    return JS_TRUE;
  }
  if (op == "uninstall") {
    if (!os->uninstall(str(ctx, argv[1]), err)) return throwMsg(ctx, err);
    return JS_TRUE;
  }
  return throwMsg(ctx, "os: unknown operation " + op);
}

// ---- connectivity: N.net("state") answers at once; any other operation answers later
// through a one-shot bus channel: fn(err, json)
NATIVE(n_net) {
  NEED_OS();
  std::string op = str(ctx, argv[0]);
  if (op == "state") return jstr(ctx, os->netState());
  JsHost *h = HOST(ctx);
  static int seq = 0;
  std::string name = "#net" + std::to_string(++seq);
  h->busOn(name, argv[2]);
  os->net(op, argc > 1 && !JS_IsUndefined(argv[1]) ? str(ctx, argv[1]) : "{}",
          [h, name](const std::string &err, const std::string &json) {
            h->busEmit(name, "{\"err\":" + Json::quote(err) + ",\"v\":" + (json.empty() ? "null" : json) + "}");
          });
  return JS_UNDEFINED;
}

NATIVE(n_reg) {
  NEED_OS();
  std::string op = str(ctx, argv[0]), err;
  if (op == "get") {
    std::string v = os->regGet(str(ctx, argv[1]));
    return v.empty() ? JS_NULL : jstr(ctx, v);
  }
  if (op == "keys") {
    std::vector<JSValue> v;
    for (auto &k : os->regKeys()) v.push_back(jstr(ctx, k));
    return arr(ctx, v);
  }
  if (op == "all") return jstr(ctx, os->regDump());
  bool ok = op == "set"      ? os->regSet(str(ctx, argv[1]), str(ctx, argv[2]), err)
            : op == "remove" ? os->regRemove(str(ctx, argv[1]), err)
            : op == "clear"  ? os->regClear(err)
                             : (err = "registry: unknown operation " + op, false);
  if (!ok) return throwMsg(ctx, err);
  return JS_TRUE;
}

NATIVE(n_bg) {
  JsHost *h = HOST(ctx);
  std::string op = str(ctx, argv[0]);
  if (op == "set") {
    std::string scope = toLower(str(ctx, argv[2]));
    if (scope != "app" && scope != "page") return throwMsg(ctx, "setBackgroundProcess: scope is \"APP\" or \"PAGE\"");
    if (!JS_IsFunction(ctx, argv[3])) return throwMsg(ctx, "setBackgroundProcess: fn must be a function");
    double iv = num(ctx, argv[4], 1000);
    return JS_NewInt32(ctx, h->setProcess(str(ctx, argv[1]), scope == "app", argv[3], (uint32_t)std::max(1.0, iv)));
  }
  if (op == "rm") return JS_NewBool(ctx, h->removeProcess(str(ctx, argv[1])));
  if (op == "clear") {
    std::string w = toLower(str(ctx, argv[1]));
    return JS_NewInt32(ctx, h->clearProcesses(w == "app" ? 1 : w == "page" ? 2 : 0));
  }
  if (op == "list") return h->processList(ctx);
  return JS_UNDEFINED;
}

NATIVE(n_bus) {
  JsHost *h = HOST(ctx);
  std::string op = str(ctx, argv[0]);
  if (op == "on") {
    if (!JS_IsFunction(ctx, argv[2])) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, h->busOn(str(ctx, argv[1]), argv[2]));
  }
  if (op == "off") h->busOff((int)num(ctx, argv[1]));
  else if (op == "emit") h->busEmit(str(ctx, argv[1]), JS_IsUndefined(argv[2]) ? "" : str(ctx, argv[2]));
  return JS_UNDEFINED;
}

// ---- files: paths are relative to the running app's data/ folder

static bool bytesOf(JSContext *ctx, JSValueConst v, std::string &out) {
  size_t n = 0;
  if (uint8_t *p = JS_GetArrayBuffer(ctx, &n, v)) {
    out.assign((const char *)p, n);
    return true;
  }
  JS_FreeValue(ctx, JS_GetException(ctx));  // not an ArrayBuffer
  if (uint8_t *p = JS_GetUint8Array(ctx, &n, v)) {
    out.assign((const char *)p, n);
    return true;
  }
  JS_FreeValue(ctx, JS_GetException(ctx));
  out = str(ctx, v);
  return true;
}

static JSValue readAll(JSContext *ctx, FILE *f, bool binary, long limit) {
  std::string out;
  char buf[4096];
  while (limit < 0 || (long)out.size() < limit) {
    size_t want = sizeof buf;
    if (limit >= 0) want = std::min<size_t>(want, (size_t)(limit - (long)out.size()));
    size_t n = fread(buf, 1, want, f);
    if (n == 0) break;
    out.append(buf, n);
  }
  if (binary) return JS_NewArrayBufferCopy(ctx, (const uint8_t *)out.data(), out.size());
  return jstr(ctx, out);
}

NATIVE(n_fs) {
  NEED_OS();
  JsHost *h = HOST(ctx);
  std::string op = str(ctx, argv[0]), err;
  auto path = [&](int i) { return os->dataPath(str(ctx, argv[i]), err); };
  if (op == "open") {  // (path, mode): r w a r+ w+ a+, with b for bytes
    std::string mode = argc > 2 && !JS_IsUndefined(argv[2]) ? str(ctx, argv[2]) : "r";
    bool binary = mode.find('b') != std::string::npos;
    std::string m;
    for (char c : mode)
      if (c != 'b' && c != 't') m += c;
    if (m != "r" && m != "w" && m != "a" && m != "r+" && m != "w+" && m != "a+")
      return throwMsg(ctx, "open: mode must be r, w, a, r+, w+ or a+ (with b for bytes)");
    std::string p = path(1);
    if (p.empty()) return throwMsg(ctx, err);
    if (m != "r" && m != "r+") {
      size_t slash = p.rfind('/');
      if (slash != std::string::npos) Os::makeDirs(p.substr(0, slash));
    }
    FILE *f = fopen(p.c_str(), (m + "b").c_str());
    if (!f) return throwMsg(ctx, "open: cannot open " + str(ctx, argv[1]) + " (" + m + ")");
    return JS_NewInt32(ctx, h->fileOpen(f, binary));
  }
  if (op == "read" || op == "readLine" || op == "write" || op == "seek" || op == "tell" || op == "size" ||
      op == "close" || op == "flush") {
    int id = (int)num(ctx, argv[1]);
    bool binary = false;
    FILE *f = h->file(id, &binary);
    if (!f) return throwMsg(ctx, "file is closed");
    if (op == "read") {
      double n = argc > 2 && !JS_IsUndefined(argv[2]) ? num(ctx, argv[2], -1) : -1;
      return readAll(ctx, f, binary, n < 0 ? -1 : (long)n);
    }
    if (op == "readLine") {
      std::string line;
      int c;
      bool any = false;
      while ((c = fgetc(f)) != EOF) {
        any = true;
        if (c == '\n') break;
        line += (char)c;
      }
      if (!any) return JS_NULL;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return jstr(ctx, line);
    }
    if (op == "write") {
      std::string data;
      bytesOf(ctx, argv[2], data);
      size_t n = fwrite(data.data(), 1, data.size(), f);
      return JS_NewInt32(ctx, (int)n);
    }
    if (op == "seek") {
      int whence = (int)num(ctx, argv[3], 0);
      fseek(f, (long)num(ctx, argv[2]), whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET);
      return JS_NewFloat64(ctx, (double)ftell(f));
    }
    if (op == "tell") return JS_NewFloat64(ctx, (double)ftell(f));
    if (op == "size") {
      long cur = ftell(f);
      fseek(f, 0, SEEK_END);
      long n = ftell(f);
      fseek(f, cur, SEEK_SET);
      return JS_NewFloat64(ctx, (double)n);
    }
    if (op == "flush") {
      fflush(f);
      return JS_UNDEFINED;
    }
    h->fileClose(id);
    return JS_UNDEFINED;
  }
  std::string p = path(1);
  if (p.empty()) return throwMsg(ctx, err);
  struct stat st;
  if (op == "readFile") {
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return throwMsg(ctx, "no such file: " + str(ctx, argv[1]));
    JSValue r = readAll(ctx, f, JS_ToBool(ctx, argv[2]), -1);
    fclose(f);
    return r;
  }
  if (op == "writeFile") {
    std::string data;
    bytesOf(ctx, argv[2], data);
    size_t slash = p.rfind('/');
    if (slash != std::string::npos) Os::makeDirs(p.substr(0, slash));
    FILE *f = fopen(p.c_str(), JS_ToBool(ctx, argv[3]) ? "ab" : "wb");
    if (!f) return throwMsg(ctx, "cannot write " + str(ctx, argv[1]));
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    if (fclose(f) != 0 || !ok) return throwMsg(ctx, "write failed: " + str(ctx, argv[1]));
    return JS_TRUE;
  }
  if (op == "exists") return JS_NewBool(ctx, stat(p.c_str(), &st) == 0);
  if (op == "stat") {
    if (stat(p.c_str(), &st) != 0) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "size", JS_NewFloat64(ctx, (double)st.st_size));
    JS_SetPropertyStr(ctx, o, "dir", JS_NewBool(ctx, S_ISDIR(st.st_mode)));
    JS_SetPropertyStr(ctx, o, "mtime", JS_NewFloat64(ctx, (double)st.st_mtime * 1000));
    return o;
  }
  if (op == "list") {
    std::vector<JSValue> v;
    DIR *d = opendir(p.c_str());
    if (!d) return throwMsg(ctx, "no such folder: " + str(ctx, argv[1]));
    std::vector<std::string> names;
    while (dirent *e = readdir(d))
      if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
    closedir(d);
    std::sort(names.begin(), names.end());
    for (auto &n : names) {
      JSValue o = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, o, "name", jstr(ctx, n));
      bool dir = stat((p + "/" + n).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
      JS_SetPropertyStr(ctx, o, "dir", JS_NewBool(ctx, dir));
      JS_SetPropertyStr(ctx, o, "size", JS_NewFloat64(ctx, dir ? 0 : (double)st.st_size));
      v.push_back(o);
    }
    return arr(ctx, v);
  }
  if (op == "mkdir") return JS_NewBool(ctx, Os::makeDirs(p));
  if (op == "remove") {
    if (stat(p.c_str(), &st) != 0) return JS_FALSE;
    if (S_ISDIR(st.st_mode) && !JS_ToBool(ctx, argv[2])) {
      if (rmdir(p.c_str()) != 0) return throwMsg(ctx, "folder not empty: " + str(ctx, argv[1]));
      return JS_TRUE;
    }
    return JS_NewBool(ctx, Os::removeTree(p));
  }
  if (op == "rename") {
    std::string q = path(2);
    if (q.empty()) return throwMsg(ctx, err);
    return JS_NewBool(ctx, rename(p.c_str(), q.c_str()) == 0);
  }
  return throwMsg(ctx, "fs: unknown operation " + op);
}

// ---- fetch
NATIVE(n_fetch) {
  FetchRequest req;
  req.url = str(ctx, argv[0]);
  req.method = toLower(str(ctx, argv[1])) == "" ? "GET" : str(ctx, argv[1]);
  for (auto &c : req.method) c = (char)toupper((unsigned char)c);
  int64_t n = 0;
  JS_GetLength(ctx, argv[2], &n);
  for (int64_t i = 0; i + 1 < n; i += 2) {
    JSValue k = JS_GetPropertyUint32(ctx, argv[2], (uint32_t)i), v = JS_GetPropertyUint32(ctx, argv[2], (uint32_t)i + 1);
    req.headers.push_back({str(ctx, k), str(ctx, v)});
    JS_FreeValue(ctx, k);
    JS_FreeValue(ctx, v);
  }
  if (!JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3])) req.body = str(ctx, argv[3]);
  req.timeoutMs = (int)num(ctx, argv[4], 15000);
  // relative URLs read from the bundle (like fetching static assets)
  JSValue funcs[2];
  JSValue promise = JS_NewPromiseCapability(ctx, funcs);
  JsHost *h = HOST(ctx);
  int id = h->newFetch(funcs[0], funcs[1]);
  if (req.url.find("://") == std::string::npos) {
    std::string path = Bundle::resolve(ENG(ctx).url(), req.url);
    FetchResponse r;
    r.url = path;
    if (ENG(ctx).bundle().has(path) && !startsWith(path, "system/private/")) {  // secrets stay native
      r.status = 200;
      r.statusText = "OK";
      r.body = ENG(ctx).bundle().text(path);
    } else {
      r.status = 404;
      r.statusText = "Not Found";
    }
    // resolve asynchronously like a real fetch
    ENG(ctx).post([h, id, r]() { h->completeFetch(id, r); });
    return promise;
  }
  ENG(ctx).platform().fetch(req, [h, id](FetchResponse r) { h->completeFetch(id, r); });
  return promise;
}

// ---- storage
NATIVE(n_storage) {
  std::string op = str(ctx, argv[0]);
  if (Os *os = osOf(ctx)) {  // memory.json of the running app
    if (op == "get") {
      std::string v;
      return os->memGet(str(ctx, argv[1]), v) ? jstr(ctx, v) : JS_NULL;
    }
    if (op == "set") os->memSet(str(ctx, argv[1]), str(ctx, argv[2]));
    else if (op == "remove") os->memRemove(str(ctx, argv[1]));
    else if (op == "clear") os->memClear();
    else if (op == "keys") {
      std::vector<JSValue> v;
      for (auto &k : os->memKeys()) v.push_back(jstr(ctx, k));
      return arr(ctx, v);
    }
    return JS_UNDEFINED;
  }
  Platform &p = ENG(ctx).platform();
  if (op == "get") {
    std::string v;
    if (!p.storageGet(str(ctx, argv[1]), v)) return JS_NULL;
    return jstr(ctx, v);
  }
  if (op == "set") {
    p.storageSet(str(ctx, argv[1]), str(ctx, argv[2]));
    return JS_UNDEFINED;
  }
  if (op == "remove") {
    p.storageRemove(str(ctx, argv[1]));
    return JS_UNDEFINED;
  }
  if (op == "keys") {
    std::vector<JSValue> v;
    for (auto &k : p.storageKeys()) v.push_back(jstr(ctx, k));
    return arr(ctx, v);
  }
  if (op == "clear") {
    for (auto &k : p.storageKeys()) p.storageRemove(k);
  }
  return JS_UNDEFINED;
}

// ---- navigation
NATIVE(n_nav) {
  std::string op = str(ctx, argv[0]);
  Engine &e = ENG(ctx);
  if (op == "url") return jstr(ctx, e.url() + (e.hash().empty() ? "" : "#" + e.hash()));
  if (op == "hash") return jstr(ctx, e.hash().empty() ? "" : "#" + e.hash());
  if (op == "setHash") {
    std::string t = argc > 3 ? str(ctx, argv[3]) : "";
    TransitionKind tk = t == "none" ? TR_NONE : t == "back" ? TR_SLIDE_RIGHT : t == "up" ? TR_SLIDE_UP : t == "down" ? TR_SLIDE_DOWN : TR_SLIDE_LEFT;
    e.setHash(str(ctx, argv[1]), argc > 2 ? JS_ToBool(ctx, argv[2]) : true, tk);
  } else if (op == "open") {
    std::string t = argc > 2 ? str(ctx, argv[2]) : "";
    TransitionKind tk = t == "none" ? TR_NONE : t == "back" ? TR_SLIDE_RIGHT : t == "up" ? TR_SLIDE_UP : TR_SLIDE_LEFT;
    e.openLater(str(ctx, argv[1]), tk);
  } else if (op == "go") {
    e.navigateHistory((int)num(ctx, argv[1], -1));
  } else if (op == "reload") {
    e.openLater(e.url() + (e.hash().empty() ? "" : "#" + e.hash()), TR_NONE);
  }
  return JS_UNDEFINED;
}

// ---- engine extras
NATIVE(n_replace) {
  return JS_NewInt32(ctx, ENG(ctx).replaceComponent(str(ctx, argv[0]), str(ctx, argv[1]), argc > 2 && JS_ToBool(ctx, argv[2])));
}
NATIVE(n_stats) {
  EngineStats &s = ENG(ctx).stats();
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "fps", JS_NewFloat64(ctx, s.fps));
  JS_SetPropertyStr(ctx, o, "frames", JS_NewInt32(ctx, (int32_t)s.frames));
  JS_SetPropertyStr(ctx, o, "styleMs", JS_NewFloat64(ctx, s.lastStyleUs / 1000.0));
  JS_SetPropertyStr(ctx, o, "layoutMs", JS_NewFloat64(ctx, s.lastLayoutUs / 1000.0));
  JS_SetPropertyStr(ctx, o, "paintMs", JS_NewFloat64(ctx, s.lastPaintUs / 1000.0));
  JS_SetPropertyStr(ctx, o, "frameMs", JS_NewFloat64(ctx, s.lastFrameUs / 1000.0));
  JS_SetPropertyStr(ctx, o, "damagePx", JS_NewInt32(ctx, (int32_t)s.lastDamagePx));
  JS_SetPropertyStr(ctx, o, "jsKB", JS_NewInt32(ctx, (int32_t)(HOST(ctx)->memoryUsed() / 1024)));
  return o;
}
NATIVE(n_gc) {
  JS_RunGC(JS_GetRuntime(ctx));
  return JS_UNDEFINED;
}

// ---- canvas
NATIVE(n_canvas) {
  Element *e = argEl(ctx, argv[0]);
  if (!e || e->tagId() != TAG_canvas) return JS_NULL;
  return canvasContext(ctx, HOST(ctx), e);
}

// ============================================================ host lifecycle

JsHost::JsHost(Engine &e) : eng_(e) {
  dispatchFn_ = dispatchWinFn_ = connectedFn_ = attrFn_ = responseFn_ = JS_UNDEFINED;
}

JsHost::~JsHost() { endSession(); }

static JSModuleDef *moduleLoader(JSContext *ctx, const char *name, void *opaque) {
  JsHost *h = (JsHost *)opaque;
  std::string src = h->engine().bundle().text(name);
  if (src.empty()) {
    JS_ThrowReferenceError(ctx, "module %s not found", name);
    return nullptr;
  }
  JSValue fn = JS_Eval(ctx, src.data(), src.size(), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(fn)) return nullptr;
  JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(fn);
  JS_FreeValue(ctx, fn);
  return m;
}

static char *moduleNormalize(JSContext *ctx, const char *base, const char *name, void *) {
  std::string r = Bundle::resolve(base, name);
  char *out = (char *)js_malloc(ctx, r.size() + 1);
  memcpy(out, r.c_str(), r.size() + 1);
  return out;
}

// Free everything of the app session (all realms, timers, processes, files) and the runtime.
void JsHost::endSession() {
  if (!rt_) return;
  retirePage();
  alive_ = false;
  dropScope(kScopeApp);
  for (auto &f : files_) fclose(f.f);
  files_.clear();
  JS_RunGC(rt_);
  JS_FreeRuntime(rt_);
  rt_ = nullptr;
  procs_.clear();
  bus_.clear();
  busQueue_.clear();
}

// Real time, not the platform clock (the simulator's stands still while a script loops).
static uint64_t wallUs() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int JsHost::onInterrupt(JSContext *, void *opaque) {
  auto *h = static_cast<JsHost *>(opaque);
  if (!h->deadlineUs_ || wallUs() < h->deadlineUs_) return 0;
  h->deadlineUs_ = 0;  // one interrupt per arm
  h->interrupts_++;
  h->interrupted_ = true;
  h->eng_.log("app script stopped: it ran for more than %u ms without returning", (unsigned)kScriptWatchdogMs);
  return 1;
}

void JsHost::armWatchdog(uint32_t ms) { deadlineUs_ = wallUs() + (uint64_t)ms * 1000; }

bool JsHost::overBudget() const { return eng_.platform().micros() > budgetEndUs_; }

bool JsHost::takeInterrupted() {
  bool r = interrupted_;
  interrupted_ = false;
  return r;
}

void JsHost::createRuntime() {
  rt_ = JS_NewRuntime2(&kMalloc, nullptr);
  JS_SetInterruptHandler(rt_, onInterrupt, this);
  // nested event dispatch (keydown -> click() -> handler -> blur() -> blur listener) goes ~16
  // script frames deep through the bootstrap's dispatch: 24 KB was not enough for that
  static const size_t stack = getenv("RT_JS_STACK") ? (size_t)atoi(getenv("RT_JS_STACK")) * 1024 : 32 * 1024;
  JS_SetMaxStackSize(rt_, stack);  // sanitizer builds need more (bigger frames)
  JS_UpdateStackTop(rt_);
  JS_SetModuleLoaderFunc(rt_, moduleNormalize, moduleLoader, this);
  JSClassDef nodeDef = {};
  nodeDef.class_name = "NativeNode";
  nodeDef.finalizer = nodeFinalizer;
  // class ids come from the runtime's own count: allocate them again for every runtime (a
  // stale id from the previous runtime would collide with the canvas classes below)
  nodeClass_ = 0;
  JS_NewClassID(rt_, &nodeClass_);
  JS_NewClass(rt_, nodeClass_, &nodeDef);
  gNodeClass = nodeClass_;
  canvasClass_ = registerCanvasClass(rt_);
  alive_ = true;
}

// Release `scope`'s timers, frames, fetches, processes, listeners and files.
void JsHost::dropScope(uint32_t scope) {
  if (!rt_) return;
  for (size_t i = 0; i < timers_.size();) {
    if (timers_[i].scope != scope) {
      i++;
      continue;
    }
    JS_FreeValueRT(rt_, timers_[i].fn);
    for (auto &a : timers_[i].args) JS_FreeValueRT(rt_, a);
    timers_.erase(timers_.begin() + i);
  }
  for (size_t i = 0; i < raf_.size();) {
    if (raf_[i].scope != scope) {
      i++;
      continue;
    }
    JS_FreeValueRT(rt_, raf_[i].fn);
    raf_.erase(raf_.begin() + i);
  }
  for (size_t i = 0; i < fetches_.size();) {
    if (fetches_[i].scope != scope) {
      i++;
      continue;
    }
    JS_FreeValueRT(rt_, fetches_[i].resolve);
    JS_FreeValueRT(rt_, fetches_[i].reject);
    fetches_.erase(fetches_.begin() + i);
  }
  for (auto &p : procs_)
    if (p.scope == scope && !p.dead) {
      p.dead = true;
      JS_FreeValueRT(rt_, p.fn);
      p.fn = JS_UNDEFINED;
    }
  procs_.erase(std::remove_if(procs_.begin(), procs_.end(), [](const Proc &p) { return p.dead && !p.busy; }), procs_.end());
  for (size_t i = 0; i < bus_.size();) {
    if (bus_[i].scope != scope) {
      i++;
      continue;
    }
    JS_FreeValueRT(rt_, bus_[i].fn);
    bus_.erase(bus_.begin() + i);
  }
  for (size_t i = 0; i < files_.size();) {
    if (files_[i].scope != scope) {
      i++;
      continue;
    }
    fclose(files_[i].f);
    files_.erase(files_.begin() + i);
  }
}

// The page is left: its scope ends and its context is released. A context stays alive only
// while app-scope functions created in it (background processes) still reference it.
void JsHost::retirePage() {
  if (!ctx_) return;
  dropScope(pageGen_);
  for (auto &p : protos_) JS_FreeValue(ctx_, p.second);
  protos_.clear();
  JS_FreeValue(ctx_, dispatchFn_);
  JS_FreeValue(ctx_, dispatchWinFn_);
  JS_FreeValue(ctx_, connectedFn_);
  JS_FreeValue(ctx_, attrFn_);
  JS_FreeValue(ctx_, responseFn_);
  dispatchFn_ = dispatchWinFn_ = connectedFn_ = attrFn_ = responseFn_ = JS_UNDEFINED;
  // unpin every wrapper so the GC can collect them
  std::vector<Node *> pins = pinned_;
  for (Node *n : pins) {
    n->ref();
    unpin(n);
  }
  pinned_.clear();
  JS_FreeContext(ctx_);
  ctx_ = nullptr;
  JS_RunGC(rt_);
  // drop the temporary refs taken above (wrappers are gone now)
  for (Node *n : pins) n->unref();
  listened_.clear();
  curScope_ = kScopeApp;
}

void JsHost::reset(Document *doc) {
  retirePage();
  if (!doc) return;
  if (!rt_) createRuntime();
  pageGen_ = nextGen_++;
  if (nextGen_ == 0) nextGen_ = 1;  // 0 is the app scope
  curScope_ = pageGen_;
  ctx_ = JS_NewContext(rt_);
  JS_SetContextOpaque(ctx_, this);

  // natives
  JSValue g = JS_GetGlobalObject(ctx_);
  JSValue n = JS_NewObject(ctx_);
  struct F {
    const char *name;
    JSCFunction *fn;
    int len;
  };
  static const F fns[] = {
      {"nodeType", n_nodeType, 1}, {"nodeName", n_nodeName, 1}, {"parent", n_parent, 1}, {"first", n_first, 1},
      {"last", n_last, 1}, {"next", n_next, 1}, {"prev", n_prev, 1}, {"childNodes", n_childNodes, 1},
      {"children", n_children, 1}, {"insert", n_insert, 3}, {"removeChild", n_removeChild, 2}, {"remove", n_remove, 1},
      {"clone", n_clone, 2}, {"contains", n_contains, 2}, {"create", n_create, 1}, {"text", n_text, 1},
      {"comment", n_comment, 1}, {"fragment", n_fragment, 0}, {"getText", n_getText, 1}, {"setText", n_setText, 2},
      {"attr", n_attr, 2}, {"setAttr", n_setAttr, 3}, {"removeAttr", n_removeAttr, 2}, {"attrNames", n_attrNames, 1},
      {"innerHTML", n_innerHTML, 1}, {"outerHTML", n_outerHTML, 1}, {"setInnerHTML", n_setInnerHTML, 2},
      {"setOuterHTML", n_setOuterHTML, 2}, {"insertHTML", n_insertHTML, 3}, {"parseHTML", n_parseHTML, 1},
      {"qs", n_qs, 2}, {"qsa", n_qsa, 2}, {"matches", n_matches, 2}, {"closest", n_closest, 2}, {"byId", n_byId, 1},
      {"document", n_document, 0}, {"getStyle", n_getStyle, 2}, {"setStyle", n_setStyle, 4}, {"cssText", n_cssText, 1},
      {"setCssText", n_setCssText, 2}, {"styleNames", n_styleNames, 1}, {"computed", n_computed, 2}, {"rect", n_rect, 1},
      {"offset", n_offset, 1}, {"client", n_client, 1}, {"scroll", n_scroll, 1}, {"setScroll", n_setScroll, 4},
      {"scrollIntoView", n_scrollIntoView, 2}, {"elementFromPoint", n_elementFromPoint, 2}, {"focus", n_focus, 1},
      {"blur", n_blur, 1}, {"active", n_active, 0}, {"value", n_value, 1}, {"setValue", n_setValue, 2},
      {"checked", n_checked, 1}, {"setChecked", n_setChecked, 2}, {"state", n_state, 3},
      {"selectedIndex", n_selectedIndex, 1}, {"setSelectedIndex", n_setSelectedIndex, 2}, {"options", n_options, 1},
      {"activate", n_activate, 1}, {"naturalSize", n_naturalSize, 1}, {"modal", n_modal, 2}, {"animate", n_animate, 8},
      {"animCtl", n_animCtl, 2}, {"now", n_now, 0}, {"timer", n_timer, 3}, {"clearTimer", n_clearTimer, 1},
      {"raf", n_raf, 1}, {"cancelRaf", n_cancelRaf, 1}, {"log", n_log, 2}, {"listen", n_listen, 1},
      {"setProto", n_setProto, 2}, {"matchMedia", n_matchMedia, 1}, {"fetch", n_fetch, 5}, {"storage", n_storage, 3},
      {"nav", n_nav, 4}, {"replace", n_replace, 3}, {"stats", n_stats, 0}, {"gc", n_gc, 0}, {"canvas", n_canvas, 1},
      {"os", n_os, 6}, {"net", n_net, 3}, {"reg", n_reg, 3}, {"bg", n_bg, 5}, {"bus", n_bus, 3}, {"fs", n_fs, 4},
  };
  for (auto &f : fns) JS_SetPropertyStr(ctx_, n, f.name, JS_NewCFunction(ctx_, f.fn, f.name, f.len));
  JS_SetPropertyStr(ctx_, n, "W", JS_NewInt32(ctx_, eng_.display().width()));
  JS_SetPropertyStr(ctx_, n, "H", JS_NewInt32(ctx_, eng_.display().height()));
  JS_SetPropertyStr(ctx_, g, "__n", JS_DupValue(ctx_, n));  // the bootstrap takes it off the global
  // bootstrap
  JSValue r = JS_Eval(ctx_, kBootstrapJs, strlen(kBootstrapJs), "<bootstrap>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(r)) reportException("bootstrap");
  JS_FreeValue(ctx_, r);
  dispatchFn_ = JS_GetPropertyStr(ctx_, g, "__dispatch");
  dispatchWinFn_ = JS_GetPropertyStr(ctx_, g, "__dispatchWindow");
  connectedFn_ = JS_GetPropertyStr(ctx_, g, "__connected");
  attrFn_ = JS_GetPropertyStr(ctx_, g, "__attributeChanged");
  responseFn_ = JS_GetPropertyStr(ctx_, g, "__response");
  JS_FreeValue(ctx_, g);
  // the document wrapper is pinned (the page's root)
  JSValue d = wrap(doc);
  JS_FreeValue(ctx_, d);
  // the OS script API (navigate, open, registry, os.*) from the system image; it gets the
  // natives as an argument (page code never sees them)
  if (os_) {
    std::string api = eng_.bundle().text("system/os.js");
    if (api.empty()) api = eng_.bundle().text("boot/os.js");  // the boot screen, before the system is up
    if (!api.empty()) {
      std::string src = "(function (N) {\n" + api + "\n})";
      JSValue fn = JS_Eval(ctx_, src.data(), src.size(), "system/os.js", JS_EVAL_TYPE_GLOBAL);
      if (JS_IsException(fn)) {
        reportException("os.js");
      } else {
        JSValue r2 = JS_Call(ctx_, fn, JS_UNDEFINED, 1, &n);
        if (JS_IsException(r2)) reportException("os.js");
        JS_FreeValue(ctx_, r2);
      }
      JS_FreeValue(ctx_, fn);
    }
  }
  JS_FreeValue(ctx_, n);
}

void JsHost::reportException(const char *where) {
  if (!ctx_) return;
  JSValue e = JS_GetException(ctx_);
  std::string msg = str(ctx_, e);
  JSValue stack = JS_GetPropertyStr(ctx_, e, "stack");
  std::string st = JS_IsUndefined(stack) ? "" : str(ctx_, stack);
  JS_FreeValue(ctx_, stack);
  JS_FreeValue(ctx_, e);
  eng_.log("JS error (%s): %s\n%s", where, msg.c_str(), st.c_str());
}

void JsHost::runJobs() {
  if (!rt_) return;
  JSContext *c;
  for (int i = 0; i < 1000; i++) {
    int r = JS_ExecutePendingJob(rt_, &c);
    if (r <= 0) {
      if (r < 0) reportException("promise job");
      break;
    }
  }
}

void JsHost::runScript(const std::string &src, const std::string &name, bool module) {
  if (!ctx_) return;
  JSValue r = JS_Eval(ctx_, src.data(), src.size(), name.c_str(), module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(r)) reportException(name.c_str());
  JS_FreeValue(ctx_, r);
  runJobs();
}

std::string JsHost::eval(const std::string &src) {
  if (!ctx_) return "(no page)";
  armWatchdog(kScriptWatchdogMs);
  JSValue r = JS_Eval(ctx_, src.data(), src.size(), "<eval>", JS_EVAL_TYPE_GLOBAL);
  std::string out;
  if (JS_IsException(r)) {
    JSValue e = JS_GetException(ctx_);
    out = "Error: " + str(ctx_, e);
    JS_FreeValue(ctx_, e);
  } else {
    JSValue j = JS_JSONStringify(ctx_, r, JS_UNDEFINED, JS_UNDEFINED);
    out = JS_IsUndefined(j) || JS_IsException(j) ? str(ctx_, r) : str(ctx_, j);
    JS_FreeValue(ctx_, j);
  }
  JS_FreeValue(ctx_, r);
  runJobs();
  return out;
}

bool JsHost::interested(const std::string &type, Element *target) const {
  if (listened_.count(type)) return true;
  // inline on<type> attributes on the target or its ancestors
  std::string attr = "on" + type;
  Atom a = findAtom(attr.data(), attr.size());
  if (!a) return false;
  for (Element *p = target; p; p = p->parent)
    if (p->hasAttr(a)) return true;
  return false;
}

bool JsHost::dispatch(Element *target, const std::string &type, const std::string &detailJson, bool bubbles, bool cancelable,
                      float x, float y, int pointerId) {
  if (!ctx_ || !target || !interested(type, target)) return true;
  if (target->flags & NF_INTERNAL) return true;
  JSValue t = wrap(target);
  JSValue detail = JS_UNDEFINED;
  if (!detailJson.empty()) detail = JS_ParseJSON(ctx_, detailJson.data(), detailJson.size(), "<detail>");
  if (JS_IsException(detail)) {
    JS_FreeValue(ctx_, JS_GetException(ctx_));
    detail = JS_UNDEFINED;
  }
  JSValue args[8] = {t,
                     jstr(ctx_, type),
                     JS_NewBool(ctx_, bubbles),
                     JS_NewBool(ctx_, cancelable),
                     JS_NewFloat64(ctx_, x),
                     JS_NewFloat64(ctx_, y),
                     JS_NewInt32(ctx_, pointerId),
                     detail};
  JSValue r = JS_Call(ctx_, dispatchFn_, JS_UNDEFINED, 8, args);
  bool ok = true;
  if (JS_IsException(r)) reportException(type.c_str());
  else ok = JS_ToBool(ctx_, r);
  JS_FreeValue(ctx_, r);
  for (auto &a : args) JS_FreeValue(ctx_, a);
  runJobs();
  return ok;
}

void JsHost::dispatchWindow(const std::string &type, const std::string &detailJson) {
  if (!ctx_) return;
  JSValue detail = JS_UNDEFINED;
  if (!detailJson.empty()) detail = JS_ParseJSON(ctx_, detailJson.data(), detailJson.size(), "<detail>");
  JSValue args[2] = {jstr(ctx_, type), detail};
  JSValue r = JS_Call(ctx_, dispatchWinFn_, JS_UNDEFINED, 2, args);
  if (JS_IsException(r)) reportException(type.c_str());
  JS_FreeValue(ctx_, r);
  JS_FreeValue(ctx_, args[0]);
  JS_FreeValue(ctx_, args[1]);
  runJobs();
}

// ---- timers

int JsHost::addTimer(JSValueConst fn, uint32_t delay, bool repeat, std::vector<JSValue> args) {
  uint64_t now = eng_.platform().micros() / 1000;
  Timer t{nextTimer_++, now + delay, repeat ? std::max<uint32_t>(delay, 1) : 0, JS_DupValue(ctx_, fn), std::move(args),
          curScope_};
  timers_.push_back(std::move(t));
  return timers_.back().id;
}

void JsHost::clearTimer(int id) {
  for (size_t i = 0; i < timers_.size(); i++)
    if (timers_[i].id == id) {
      JS_FreeValue(ctx_, timers_[i].fn);
      for (auto &a : timers_[i].args) JS_FreeValue(ctx_, a);
      timers_.erase(timers_.begin() + i);
      return;
    }
}

int JsHost::addRaf(JSValueConst fn) {
  raf_.push_back({nextRaf_++, JS_DupValue(ctx_, fn), curScope_});
  eng_.requestFrame();
  return raf_.back().id;
}

void JsHost::cancelRaf(int id) {
  for (size_t i = 0; i < raf_.size(); i++)
    if (raf_[i].id == id) {
      JS_FreeValue(ctx_, raf_[i].fn);
      raf_.erase(raf_.begin() + i);
      return;
    }
}

uint64_t JsHost::nextTimerMs() const {
  uint64_t n = UINT64_MAX;
  for (auto &t : timers_) n = std::min(n, t.when);
  for (auto &p : procs_)
    if (!p.busy && !p.dead) n = std::min(n, p.next);
  if (!busQueue_.empty()) n = 0;
  return n;
}

// ---- background processes

int JsHost::setProcess(const std::string &name, bool appScope, JSValueConst fn, uint32_t intervalMs) {
  removeProcess(name);
  Proc p;
  p.id = nextProc_++;
  p.name = name;
  p.scope = appScope ? kScopeApp : pageGen_;
  p.fn = JS_DupValue(ctx_, fn);
  p.interval = std::max<uint32_t>(intervalMs, 1);
  p.next = eng_.platform().micros() / 1000;
  procs_.push_back(p);
  eng_.requestFrame();
  return p.id;
}

bool JsHost::removeProcess(const std::string &name) {
  bool found = false;
  for (auto &p : procs_)
    if (!p.dead && p.name == name) {
      p.dead = true;
      JS_FreeValueRT(rt_, p.fn);
      p.fn = JS_UNDEFINED;
      found = true;
    }
  procs_.erase(std::remove_if(procs_.begin(), procs_.end(), [](const Proc &p) { return p.dead && !p.busy; }), procs_.end());
  return found;
}

int JsHost::clearProcesses(int which) {
  int n = 0;
  for (auto &p : procs_) {
    bool app = p.scope == kScopeApp;
    if (p.dead || (which == 1 && !app) || (which == 2 && app)) continue;
    p.dead = true;
    JS_FreeValueRT(rt_, p.fn);
    p.fn = JS_UNDEFINED;
    n++;
  }
  procs_.erase(std::remove_if(procs_.begin(), procs_.end(), [](const Proc &p) { return p.dead && !p.busy; }), procs_.end());
  return n;
}

JSValue JsHost::processList(JSContext *ctx) {
  JSValue a = JS_NewArray(ctx);
  uint32_t i = 0;
  for (auto &p : procs_) {
    if (p.dead) continue;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "name", JS_NewStringLen(ctx, p.name.data(), p.name.size()));
    JS_SetPropertyStr(ctx, o, "scope", JS_NewString(ctx, p.scope == kScopeApp ? "APP" : "PAGE"));
    JS_SetPropertyStr(ctx, o, "interval", JS_NewInt32(ctx, (int)p.interval));
    JS_SetPropertyStr(ctx, o, "runs", JS_NewInt32(ctx, (int)p.runs));
    JS_SetPropertyStr(ctx, o, "running", JS_NewBool(ctx, p.busy));
    JS_SetPropertyUint32(ctx, a, i++, o);
  }
  return a;
}

// what a process run returned: a delay, false (stop) or anything else (keep the interval)
static void scheduleAfter(JSContext *ctx, JSValueConst v, uint64_t now, uint32_t interval, uint64_t &next, bool &stop) {
  stop = false;
  if (JS_IsBool(v) && !JS_ToBool(ctx, v)) {
    stop = true;
    return;
  }
  double d;
  if (JS_IsNumber(v) && JS_ToFloat64(ctx, &d, v) == 0 && d >= 0) {
    next = now + (uint64_t)d;
    return;
  }
  next = now + interval;
}

static JSValue procSettledFn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv, int magic, JSValueConst *data) {
  int32_t id = 0;
  JS_ToInt32(ctx, &id, data[0]);
  HOST(ctx)->processSettled(ctx, (int)id, argc ? argv[0] : JS_UNDEFINED, magic == 1);
  return JS_UNDEFINED;
}

void JsHost::processSettled(JSContext *ctx, int id, JSValueConst value, bool rejected) {
  uint64_t now = eng_.platform().micros() / 1000;
  for (auto &p : procs_) {
    if (p.id != id) continue;
    p.busy = false;
    if (p.dead) break;
    if (rejected) {
      eng_.log("process %s failed: %s", p.name.c_str(), str(ctx, value).c_str());
      p.next = now + p.interval;
    } else {
      bool stop;
      scheduleAfter(ctx, value, now, p.interval, p.next, stop);
      if (stop) {
        p.dead = true;
        JS_FreeValueRT(rt_, p.fn);
        p.fn = JS_UNDEFINED;
      }
    }
    break;
  }
  procs_.erase(std::remove_if(procs_.begin(), procs_.end(), [](const Proc &p) { return p.dead && !p.busy; }), procs_.end());
  eng_.requestFrame();
}

void JsHost::runProcesses(uint64_t now) {
  // index loop: a process may start or remove processes while it runs
  for (size_t i = 0; i < procs_.size() && ctx_; i++) {
    if (procs_[i].dead || procs_[i].busy || procs_[i].next > now) continue;
    if (overBudget()) break;  // the rest are still due: next tick
    int id = procs_[i].id;
    std::string name = procs_[i].name;
    uint32_t interval = procs_[i].interval;
    procs_[i].runs++;
    procs_[i].busy = true;  // also guards against re-entry
    curScope_ = procs_[i].scope;
    JSValue fn = JS_DupValue(ctx_, procs_[i].fn);
    JSValue info = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, info, "name", jstr(ctx_, name));
    JS_SetPropertyStr(ctx_, info, "runs", JS_NewInt32(ctx_, (int)procs_[i].runs));
    JSValue r = JS_Call(ctx_, fn, JS_UNDEFINED, 1, &info);
    JS_FreeValue(ctx_, info);
    JS_FreeValue(ctx_, fn);
    Proc *p = nullptr;
    for (auto &q : procs_)
      if (q.id == id) p = &q;
    if (JS_IsException(r)) {
      reportException(("process " + name).c_str());
      if (p && takeInterrupted()) {
        // stopped by the watchdog: it would hang the app again at its next run -- end it
        eng_.log("process %s ended: it did not return", name.c_str());
        p->busy = false;
        p->dead = true;
        JS_FreeValueRT(rt_, p->fn);
        p->fn = JS_UNDEFINED;
      } else if (p) {
        p->busy = false;
        p->next = now + interval;
      }
    } else if (JS_IsPromise(r)) {
      // settles later: the next run is planned from what it resolves to
      JSValue idv = JS_NewInt32(ctx_, id);
      JSValue ok = JS_NewCFunctionData(ctx_, procSettledFn, 1, 0, 1, &idv);
      JSValue bad = JS_NewCFunctionData(ctx_, procSettledFn, 1, 1, 1, &idv);
      JSValue then = JS_GetPropertyStr(ctx_, r, "then");
      JSValue args[2] = {ok, bad};
      JSValue t = JS_Call(ctx_, then, r, 2, args);
      JS_FreeValue(ctx_, t);
      JS_FreeValue(ctx_, then);
      JS_FreeValue(ctx_, ok);
      JS_FreeValue(ctx_, bad);
    } else if (p) {
      p->busy = false;
      bool stop;
      scheduleAfter(ctx_, r, now, interval, p->next, stop);
      if (stop && !p->dead) {
        p->dead = true;
        JS_FreeValueRT(rt_, p->fn);
        p->fn = JS_UNDEFINED;
      }
    }
    JS_FreeValue(ctx_, r);
    runJobs();
    curScope_ = pageGen_;
  }
  procs_.erase(std::remove_if(procs_.begin(), procs_.end(), [](const Proc &p) { return p.dead && !p.busy; }), procs_.end());
}

// ---- OS event bus

int JsHost::busOn(const std::string &name, JSValueConst fn) {
  bus_.push_back({nextBus_++, name, curScope_, JS_DupValue(ctx_, fn)});
  return bus_.back().id;
}

void JsHost::busOff(int id) {
  for (size_t i = 0; i < bus_.size(); i++)
    if (bus_[i].id == id) {
      JS_FreeValueRT(rt_, bus_[i].fn);
      bus_.erase(bus_.begin() + i);
      return;
    }
}

void JsHost::busEmit(const std::string &name, const std::string &json) {
  busQueue_.push_back({name, json});
  eng_.requestFrame();
}

void JsHost::deliverBus() {
  if (busQueue_.empty()) return;
  auto q = std::move(busQueue_);
  busQueue_.clear();
  for (auto &ev : q) {
    std::vector<int> ids;
    for (auto &l : bus_)
      if (l.name == ev.first || l.name == "*") ids.push_back(l.id);
    for (int id : ids) {
      if (!ctx_) return;
      JSValue fn = JS_UNDEFINED;
      uint32_t scope = pageGen_;
      for (auto &l : bus_)
        if (l.id == id) {
          fn = JS_DupValue(ctx_, l.fn);
          scope = l.scope;
        }
      if (JS_IsUndefined(fn)) continue;
      JSValue data = ev.second.empty() ? JS_UNDEFINED : JS_ParseJSON(ctx_, ev.second.data(), ev.second.size(), "<event>");
      if (JS_IsException(data)) {
        JS_FreeValue(ctx_, JS_GetException(ctx_));
        data = JS_UNDEFINED;
      }
      JSValue args[2] = {data, jstr(ctx_, ev.first)};
      curScope_ = scope;
      if (ev.first[0] == '#') busOff(id);  // reply channels (dialogs) answer once
      JSValue r = JS_Call(ctx_, fn, JS_UNDEFINED, 2, args);
      if (JS_IsException(r)) reportException(("event " + ev.first).c_str());
      JS_FreeValue(ctx_, r);
      JS_FreeValue(ctx_, fn);
      JS_FreeValue(ctx_, args[0]);
      JS_FreeValue(ctx_, args[1]);
      runJobs();
      curScope_ = pageGen_;
    }
  }
}

// ---- files opened by scripts

int JsHost::fileOpen(FILE *f, bool binary) {
  files_.push_back({nextFile_++, f, binary, curScope_});
  return files_.back().id;
}

FILE *JsHost::file(int id, bool *binary) {
  for (auto &f : files_)
    if (f.id == id) {
      if (binary) *binary = f.binary;
      return f.f;
    }
  return nullptr;
}

void JsHost::fileClose(int id) {
  for (size_t i = 0; i < files_.size(); i++)
    if (files_[i].id == id) {
      fclose(files_[i].f);
      files_.erase(files_.begin() + i);
      return;
    }
}

void JsHost::fireTimers(uint64_t now) {
  // run due timers in deadline order; timers added while running wait for the next tick
  int limit = nextTimer_;
  for (int guard = 0; guard < 256; guard++) {
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < timers_.size(); i++)
      if (timers_[i].when <= now && timers_[i].id < limit && (best == SIZE_MAX || timers_[i].when < timers_[best].when)) best = i;
    if (best == SIZE_MAX) break;
    Timer t = timers_[best];
    JSValue fn = JS_DupValue(ctx_, t.fn);
    std::vector<JSValue> args;
    for (auto &a : t.args) args.push_back(JS_DupValue(ctx_, a));
    if (t.interval) {
      timers_[best].when = std::max(now, t.when + t.interval);
      if (timers_[best].when <= now) timers_[best].when = now + t.interval;
    } else {
      JS_FreeValue(ctx_, timers_[best].fn);
      for (auto &a : timers_[best].args) JS_FreeValue(ctx_, a);
      timers_.erase(timers_.begin() + best);
    }
    curScope_ = t.scope;  // whatever it starts belongs to the same scope
    JSValue r = JS_Call(ctx_, fn, JS_UNDEFINED, (int)args.size(), args.data());
    if (JS_IsException(r)) {
      reportException("timer");
      if (takeInterrupted() && t.interval) {  // a repeating timer that hangs: cleared
        eng_.log("setInterval %d cleared: its callback did not return", t.id);
        for (size_t i = 0; i < timers_.size(); i++)
          if (timers_[i].id == t.id) {
            JS_FreeValue(ctx_, timers_[i].fn);
            for (auto &a : timers_[i].args) JS_FreeValue(ctx_, a);
            timers_.erase(timers_.begin() + i);
            break;
          }
      }
    }
    JS_FreeValue(ctx_, r);
    JS_FreeValue(ctx_, fn);
    for (auto &a : args) JS_FreeValue(ctx_, a);
    runJobs();
    curScope_ = pageGen_;
    if (!ctx_ || overBudget()) return;  // the rest are still due: next tick
  }
}

void JsHost::tick(uint64_t nowMs) {
  if (!ctx_) return;
  budgetEndUs_ = eng_.platform().micros() + kTickBudgetUs;
  fireTimers(nowMs);
  if (ctx_) runProcesses(nowMs);
  if (ctx_) deliverBus();
  if (ctx_) runJobs();
  // Wrappers of nodes a script took out of the page often sit in cycles (a listener closure
  // that names its element) only the GC frees, and until then their nodes keep layout and
  // paint caches: memory QuickJS does not count toward its next collection. A screen's worth
  // dropped: collect (~35 ms on the board) once the page has settled, not mid-transition.
  if (ctx_ && detached_ >= 150 && nowMs - lastGcMs_ >= 2000 && eng_.platform().micros() - lastDetachUs_ >= 600000) {
    detached_ = 0;
    lastGcMs_ = nowMs;
    JS_RunGC(rt_);
  }
}

void JsHost::animationFrame(double ts) {
  if (!ctx_ || raf_.empty()) return;
  std::vector<Raf> run;
  run.swap(raf_);
  for (auto &r : run) {
    if (!ctx_) {
      JS_FreeValueRT(rt_, r.fn);
      continue;
    }
    curScope_ = r.scope;
    JSValue a = JS_NewFloat64(ctx_, ts);
    JSValue res = JS_Call(ctx_, r.fn, JS_UNDEFINED, 1, &a);
    if (JS_IsException(res)) reportException("requestAnimationFrame");
    JS_FreeValue(ctx_, res);
    JS_FreeValue(ctx_, r.fn);
    curScope_ = pageGen_;
  }
  if (!ctx_) return;
  runJobs();
}

size_t JsHost::memoryUsed() const {
  if (!rt_) return 0;
  JSMemoryUsage mu;
  JS_ComputeMemoryUsage(rt_, &mu);
  return (size_t)mu.malloc_size;
}

// ---- fetch completion

int JsHost::newFetch(JSValue resolve, JSValue reject) {
  fetches_.push_back({nextFetch_++, resolve, reject, curScope_});
  return fetches_.back().id;
}

void JsHost::completeFetch(int id, const FetchResponse &r) {
  if (!ctx_) return;
  for (size_t i = 0; i < fetches_.size(); i++) {
    if (fetches_[i].id != id) continue;
    PendingFetch f = fetches_[i];
    fetches_.erase(fetches_.begin() + i);
    curScope_ = f.scope;
    if (r.status == 0) {
      JSValue err = JS_NewError(ctx_);
      JS_SetPropertyStr(ctx_, err, "message", jstr(ctx_, "Failed to fetch: " + (r.error.empty() ? r.url : r.error)));
      JSValue res = JS_Call(ctx_, f.reject, JS_UNDEFINED, 1, &err);
      JS_FreeValue(ctx_, res);
      JS_FreeValue(ctx_, err);
    } else {
      std::vector<JSValue> hv;
      for (auto &h : r.headers) {
        hv.push_back(jstr(ctx_, h.first));
        hv.push_back(jstr(ctx_, h.second));
      }
      JSValue args[5] = {JS_NewInt32(ctx_, r.status), jstr(ctx_, r.statusText), arr(ctx_, hv), jstr(ctx_, r.body), jstr(ctx_, r.url)};
      JSValue resp = JS_Call(ctx_, responseFn_, JS_UNDEFINED, 5, args);
      for (auto &a : args) JS_FreeValue(ctx_, a);
      JSValue res = JS_Call(ctx_, f.resolve, JS_UNDEFINED, 1, &resp);
      JS_FreeValue(ctx_, res);
      JS_FreeValue(ctx_, resp);
    }
    JS_FreeValue(ctx_, f.resolve);
    JS_FreeValue(ctx_, f.reject);
    runJobs();
    curScope_ = pageGen_;
    return;
  }
}

void JsHost::animationFinished(int id) {
  if (!ctx_) return;
  JSValue g = JS_GetGlobalObject(ctx_);
  JSValue fn = JS_GetPropertyStr(ctx_, g, "__waapiDone");
  JSValue a = JS_NewInt32(ctx_, id);
  JSValue r = JS_Call(ctx_, fn, JS_UNDEFINED, 1, &a);
  if (JS_IsException(r)) reportException("animation");
  JS_FreeValue(ctx_, r);
  JS_FreeValue(ctx_, fn);
  JS_FreeValue(ctx_, g);
  runJobs();
}

void JsHost::imageLoaded(Element *img, bool ok) { dispatch(img, ok ? "load" : "error", "", false, false, 0, 0, 0); }

}  // namespace rt
