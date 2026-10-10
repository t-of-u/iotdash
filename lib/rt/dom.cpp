#include "dom.h"

#include <unordered_set>

#include "css.h"
#include "extra.h"
#include "layout.h"
#include "style.h"

namespace rt {

static void setConnected(Node *n, bool on) {
  if (on) n->flags |= NF_CONNECTED;
  else n->flags &= ~NF_CONNECTED;
  if (Element *e = n->asElement()) {
    for (Node *c = e->first; c; c = c->next) setConnected(c, on);
  }
}

static void setDoc(Node *n, Document *d) {
  n->doc = d;
  if (Element *e = n->asElement())
    for (Node *c = e->first; c; c = c->next) setDoc(c, d);
}

void Node::unref() {
  if (--refs <= 0) {
    if (doc && (Node *)doc != this && Document::alive(doc) && doc->observer) doc->observer->nodeDestroyed(this);
    delete this;
  }
}

static std::unordered_set<const Document *> &liveDocuments() {
  static auto *s = new std::unordered_set<const Document *>();
  return *s;
}

Document::Document() : Element(0, NT_DOCUMENT) {
  doc = this;
  flags |= NF_CONNECTED;
  liveDocuments().insert(this);
}

Document::~Document() { liveDocuments().erase(this); }

bool Document::alive(const Document *d) { return d && liveDocuments().count(d); }

void Node::remove() {
  if (parent) parent->removeChild(this);
}

Element *Node::parentElement() const {
  return parent && parent->nodeType == NT_ELEMENT ? parent : nullptr;
}

bool Node::isInclusiveAncestorOf(const Node *n) const {
  for (; n; n = n->parent)
    if (n == this) return true;
  return false;
}

static void collectText(const Node *n, std::string &out) {
  if (n->nodeType == NT_TEXT) {
    out += ((const Text *)n)->data;
    return;
  }
  if (n->isContainer()) {
    for (Node *c = ((const Element *)n)->first; c; c = c->next) collectText(c, out);
  }
}

std::string Node::textContent() const {
  std::string s;
  collectText(this, s);
  return s;
}

void Text::setData(std::string d) {
  if (d == data) return;
  data = std::move(d);
  flags |= NF_TEXT_DIRTY;
  if (doc && Document::alive(doc) && doc->observer) doc->observer->textChanged(this);
}

Element::~Element() {
  Node *c = first;
  while (c) {
    Node *n = c->next;
    c->parent = nullptr;
    c->prev = c->next = nullptr;
    c->flags &= ~NF_CONNECTED;
    c->unref();
    c = n;
  }
  first = last = nullptr;
  delete inlineStyle;
  delete cs;
  delete box;
  delete x;
  if (before) before->unref();
  if (after) after->unref();
}

const std::string *Element::getAttr(Atom name) const {
  for (auto &a : attrs)
    if (a.name == name) return &a.value;
  return nullptr;
}

static void parseClasses(const std::string &v, std::vector<Atom> &out) {
  out.clear();
  size_t i = 0, n = v.size();
  while (i < n) {
    while (i < n && isSpace(v[i])) i++;
    size_t s = i;
    while (i < n && !isSpace(v[i])) i++;
    if (i > s) {
      Atom a = intern(v.data() + s, i - s);
      if (std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
    }
  }
}

void Element::setAttr(Atom name, const std::string &value) {
  std::string old;
  bool had = false;
  for (auto &a : attrs) {
    if (a.name == name) {
      if (a.value == value) return;
      old = a.value;
      had = true;
      a.value = value;
      break;
    }
  }
  if (!had) attrs.push_back({name, value});
  if (name == A.id) idAtom = value.empty() ? 0 : intern(value);
  else if (name == A.class_) parseClasses(value, classes);
  if (doc && doc->observer) doc->observer->attrChanged(this, name, had ? &old : nullptr);
}

bool Element::removeAttr(Atom name) {
  for (size_t i = 0; i < attrs.size(); i++) {
    if (attrs[i].name == name) {
      std::string old = std::move(attrs[i].value);
      attrs.erase(attrs.begin() + i);
      if (name == A.id) idAtom = 0;
      else if (name == A.class_) classes.clear();
      if (doc && doc->observer) doc->observer->attrChanged(this, name, &old);
      return true;
    }
  }
  return false;
}

float Element::attrNum(Atom name, float d) const {
  const std::string *v = getAttr(name);
  if (!v || v->empty()) return d;
  char *end;
  float f = strtof(v->c_str(), &end);
  return end == v->c_str() ? d : f;
}

bool Element::hasClass(Atom c) const {
  for (Atom a : classes)
    if (a == c) return true;
  return false;
}

void Element::setClassList(const std::vector<Atom> &cls) {
  std::string v;
  for (Atom a : cls) {
    if (!v.empty()) v += ' ';
    v += atomStr(a);
  }
  setAttr(A.class_, v);
}

void Element::appendChild(Node *n) { insertBefore(n, nullptr); }

void Element::insertBefore(Node *n, Node *ref) {
  if (!n || n == this) return;
  if (ref && ref->parent != this) ref = nullptr;
  if (n->nodeType == NT_FRAGMENT) {
    Element *frag = (Element *)n;
    while (frag->first) insertBefore(frag->first, ref);
    return;
  }
  if (n->isContainer() && n->isInclusiveAncestorOf(this)) return;  // would create a cycle
  n->ref();  // keep alive while moving
  if (n->parent) n->parent->removeChild(n);
  n->parent = this;
  if (ref) {
    n->next = ref;
    n->prev = ref->prev;
    if (ref->prev) ref->prev->next = n;
    else first = n;
    ref->prev = n;
  } else {
    n->prev = last;
    n->next = nullptr;
    if (last) last->next = n;
    else first = n;
    last = n;
  }
  if (doc && n->doc != doc) setDoc(n, doc);
  setConnected(n, connected());
  if (doc && doc->observer) doc->observer->nodeInserted(n);
}

void Element::removeChild(Node *n) {
  if (!n || n->parent != this) return;
  if (n->prev) n->prev->next = n->next;
  else first = n->next;
  if (n->next) n->next->prev = n->prev;
  else last = n->prev;
  n->prev = n->next = nullptr;
  n->parent = nullptr;
  setConnected(n, false);
  if (doc && doc->observer) doc->observer->nodeRemoved(n, this);
  n->unref();
}

void Element::removeAllChildren() {
  while (first) removeChild(first);
}

Element *Element::firstElementChild() const {
  for (Node *c = first; c; c = c->next)
    if (c->nodeType == NT_ELEMENT) return (Element *)c;
  return nullptr;
}

Element *Element::lastElementChild() const {
  for (Node *c = last; c; c = c->prev)
    if (c->nodeType == NT_ELEMENT) return (Element *)c;
  return nullptr;
}

Element *Element::nextElementSibling() const {
  for (Node *c = next; c; c = c->next)
    if (c->nodeType == NT_ELEMENT) return (Element *)c;
  return nullptr;
}

Element *Element::prevElementSibling() const {
  for (Node *c = prev; c; c = c->prev)
    if (c->nodeType == NT_ELEMENT) return (Element *)c;
  return nullptr;
}

int Element::childElementCount() const {
  int n = 0;
  for (Node *c = first; c; c = c->next)
    if (c->nodeType == NT_ELEMENT) n++;
  return n;
}

int Element::elementIndex() const {
  int i = 0;
  for (Node *c = prev; c; c = c->prev)
    if (c->nodeType == NT_ELEMENT) i++;
  return i;
}

void Element::setState(uint16_t bit, bool on) {
  uint16_t ns = on ? (state | bit) : (state & ~bit);
  if (ns == state) return;
  state = ns;
  flags |= NF_STYLE_DIRTY | (gStateDescendants ? NF_SUBTREE_STYLE : 0);
  // Sibling combinators (+, ~) can depend on our state: restyle following siblings too.
  if (gSiblingSelectors)
    for (Element *s = nextElementSibling(); s; s = s->nextElementSibling()) s->flags |= NF_STYLE_DIRTY | NF_SUBTREE_STYLE;
  for (Element *p = parent; p; p = p->parent) p->flags |= NF_DESC_STYLE_DIRTY;
}

ElementExtra &Element::extra() {
  // painting may run on two cores: create once (double-checked)
  if (ElementExtra *p = __atomic_load_n(&x, __ATOMIC_ACQUIRE)) return *p;
  PaintLock lock;
  if (!x) __atomic_store_n(&x, new ElementExtra(), __ATOMIC_RELEASE);
  return *x;
}

Element *Document::createElement(const std::string &tagName) { return createElement(internLower(tagName)); }

Element *Document::createElement(Atom tag) {
  Element *e = new Element(tag);
  e->doc = this;
  return e;
}

Text *Document::createText(const std::string &s) {
  Text *t = new Text(s);
  t->doc = this;
  return t;
}

Element *Document::getElementById(const std::string &id) {
  Atom a = findAtom(id.data(), id.size());
  if (!a) return nullptr;
  Element *found = nullptr;
  Element *root = html ? html : firstElementChild();
  if (!root) return nullptr;
  root->forEachElement([&](Element *e) {
    if (!found && e->idAtom == a) found = e;
  });
  return found;
}

void Document::findStructure() {
  html = head = body = nullptr;
  for (Node *c = first; c; c = c->next)
    if (c->isElement() && ((Element *)c)->tag == TAG_html) html = (Element *)c;
  if (!html) return;
  for (Node *c = html->first; c; c = c->next) {
    if (!c->isElement()) continue;
    Element *e = (Element *)c;
    if (e->tag == TAG_head && !head) head = e;
    if (e->tag == TAG_body && !body) body = e;
  }
}

Node *cloneNode(Node *n, bool deep, Document *d) {
  if (Text *t = n->asText()) {
    Text *c = new Text(t->data);
    c->doc = d;
    return c;
  }
  if (n->nodeType == NT_COMMENT) {
    Comment *c = new Comment(((Comment *)n)->data);
    c->doc = d;
    return c;
  }
  Element *e = (Element *)n;
  Element *c = new Element(e->tag, n->nodeType == NT_FRAGMENT ? NT_FRAGMENT : NT_ELEMENT);
  c->doc = d;
  for (auto &a : e->attrs) c->setAttr(a.name, a.value);
  c->state = e->state & (ST_CHECKED | ST_DISABLED | ST_OPEN);
  if (deep)
    for (Node *ch = e->first; ch; ch = ch->next) c->appendChild(cloneNode(ch, true, d));
  return c;
}

}  // namespace rt
