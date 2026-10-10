#pragma once
// DOM tree: Document > Element/Text. Nodes are reference counted: the parent holds one
// reference, a JS wrapper holds another, so a node removed from the tree lives on while
// script still points at it.

#include "atoms.h"

namespace rt {

struct Element;
struct Text;
struct Document;
struct DeclBlock;
struct ComputedStyle;
struct Box;
struct ElementExtra;

enum NodeType : uint8_t { NT_ELEMENT = 1, NT_TEXT = 3, NT_COMMENT = 8, NT_DOCUMENT = 9, NT_FRAGMENT = 11 };

enum NodeFlag : uint32_t {
  NF_CONNECTED = 1u << 0,         // reachable from the document
  NF_STYLE_DIRTY = 1u << 1,       // recompute this element's style
  NF_DESC_STYLE_DIRTY = 1u << 2,  // some descendant needs restyle
  NF_SUBTREE_STYLE = 1u << 3,     // restyle the whole subtree
  NF_PSEUDO = 1u << 4,            // generated ::before/::after element
  NF_INTERNAL = 1u << 5,          // runtime-owned UI (keyboard, popups)
  NF_TEXT_DIRTY = 1u << 6,        // text changed: re-shape
  NF_HAS_LISTENERS = 1u << 7,     // JS registered event listeners
  NF_INLINE_HANDLER = 1u << 8,    // has on* attribute handlers
  NF_JS_PINNED = 1u << 9,         // the JS wrapper is kept alive by the node (connected)
};

// Element interaction state (pseudo-classes).
enum ElemState : uint16_t {
  ST_ACTIVE = 1 << 0,
  ST_HOVER = 1 << 1,
  ST_FOCUS = 1 << 2,
  ST_FOCUS_WITHIN = 1 << 3,
  ST_CHECKED = 1 << 4,
  ST_DISABLED = 1 << 5,
  ST_TARGET = 1 << 6,
  ST_OPEN = 1 << 7,
  ST_INVALID = 1 << 8,
  ST_PLACEHOLDER = 1 << 9,  // :placeholder-shown
  ST_INDETERMINATE = 1 << 10,
  ST_FOCUS_VISIBLE = 1 << 11,
};

struct Node {
  NodeType nodeType;
  uint32_t flags = 0;
  Document *doc = nullptr;
  Element *parent = nullptr;
  Node *prev = nullptr, *next = nullptr;
  int32_t refs = 0;
  void *js = nullptr;  // JS wrapper object (weak; cleared by its finalizer)

  explicit Node(NodeType t) : nodeType(t) {}
  virtual ~Node() {}

  bool isElement() const { return nodeType == NT_ELEMENT; }
  bool isText() const { return nodeType == NT_TEXT; }
  Element *asElement() { return (nodeType == NT_ELEMENT || nodeType == NT_DOCUMENT || nodeType == NT_FRAGMENT) ? (Element *)this : nullptr; }
  Text *asText() { return nodeType == NT_TEXT ? (Text *)this : nullptr; }
  bool connected() const { return flags & NF_CONNECTED; }
  bool isContainer() const { return nodeType == NT_ELEMENT || nodeType == NT_DOCUMENT || nodeType == NT_FRAGMENT; }

  void ref() { refs++; }
  void unref();
  void remove();  // detach from parent (if any)
  Element *parentElement() const;
  bool isInclusiveAncestorOf(const Node *n) const;
  std::string textContent() const;
};

struct Text : Node {
  std::string data;
  explicit Text(std::string d) : Node(NT_TEXT), data(std::move(d)) {}
  void setData(std::string d);
};

struct Comment : Node {
  std::string data;
  explicit Comment(std::string d) : Node(NT_COMMENT), data(std::move(d)) {}
};

struct Attr {
  Atom name;
  std::string value;
};

struct Element : Node {
  Atom tag = 0;  // lower-case tag name; < TAG__COUNT means a known Tag
  Node *first = nullptr, *last = nullptr;
  std::vector<Attr> attrs;
  Atom idAtom = 0;
  std::vector<Atom> classes;
  DeclBlock *inlineStyle = nullptr;  // parsed style="" (owned)
  ComputedStyle *cs = nullptr;       // computed style (owned)
  Box *box = nullptr;                // layout results (owned)
  ElementExtra *x = nullptr;         // optional per-element state (owned)
  Element *before = nullptr, *after = nullptr;  // generated content (owned)
  uint16_t state = 0;

  explicit Element(Atom t, NodeType nt = NT_ELEMENT) : Node(nt), tag(t) {}
  ~Element() override;

  Tag tagId() const { return tagOf(tag); }
  const std::string &tagName() const { return atomStr(tag); }

  // attributes
  const std::string *getAttr(Atom name) const;
  bool hasAttr(Atom name) const { return getAttr(name) != nullptr; }
  void setAttr(Atom name, const std::string &value);
  bool removeAttr(Atom name);
  std::string attrOr(Atom name, const char *d) const {
    const std::string *v = getAttr(name);
    return v ? *v : std::string(d);
  }
  float attrNum(Atom name, float d) const;
  bool hasClass(Atom c) const;
  void setClassList(const std::vector<Atom> &cls);  // updates the class attribute too

  // tree
  void appendChild(Node *n);
  void insertBefore(Node *n, Node *ref);  // ref == nullptr appends
  void removeChild(Node *n);
  void removeAllChildren();
  Element *firstElementChild() const;
  Element *lastElementChild() const;
  Element *nextElementSibling() const;
  Element *prevElementSibling() const;
  int childElementCount() const;
  int elementIndex() const;  // index among element siblings

  void setState(uint16_t bit, bool on);
  bool hasState(uint16_t bit) const { return state & bit; }
  ElementExtra &extra();

  // Walk the subtree in document order (pre-order), including this element.
  template <class F>
  void forEachElement(F &&f) {
    Element *e = this;
    while (e) {
      f(e);
      Element *n = e->firstElementChild();
      if (n) {
        e = n;
        continue;
      }
      while (e && e != this) {
        Element *s = e->nextElementSibling();
        if (s) {
          e = s;
          break;
        }
        e = e->parent;
      }
      if (e == this) break;
    }
  }
};

// Mutation observer for the engine (style/layout invalidation, JS wrapper pinning).
struct DomObserver {
  virtual void nodeInserted(Node *n) = 0;
  virtual void nodeRemoved(Node *n, Element *oldParent) = 0;
  virtual void attrChanged(Element *e, Atom name, const std::string *oldValue) = 0;
  virtual void textChanged(Text *t) = 0;
  virtual void nodeDestroyed(Node *n) = 0;
  virtual ~DomObserver() {}
};

struct Document : Element {
  Element *html = nullptr, *head = nullptr, *body = nullptr;
  DomObserver *observer = nullptr;
  std::string url;  // e.g. "index.html"
  Document();
  ~Document() override;
  // Nodes can outlive their document (wrappers kept by background processes): their doc
  // pointer is only followed while the document is still alive.
  static bool alive(const Document *d);
  Element *createElement(const std::string &tagName);
  Element *createElement(Atom tag);
  Text *createText(const std::string &s);
  Element *getElementById(const std::string &id);
  void findStructure();  // sets html/head/body after parsing
};

// Recursively deep-copy a node (without JS state).
Node *cloneNode(Node *n, bool deep, Document *intoDoc);

}  // namespace rt
