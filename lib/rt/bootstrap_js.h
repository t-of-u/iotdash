#pragma once
// Web API surface implemented in JavaScript on top of the native __n functions (js.cpp).
static const char kBootstrapJs[] = R"BOOTSTRAP_JS(
(function (G) {
'use strict';
const N = G.__n;
delete G.__n;
const L = Symbol('listeners');
const lower = (s) => String(s).toLowerCase();

// ------------------------------------------------------------------ events
class Event {
  constructor(type, init) {
    init = init || {};
    this.type = String(type);
    this.bubbles = !!init.bubbles;
    this.cancelable = !!init.cancelable;
    this.composed = !!init.composed;
    this.defaultPrevented = false;
    this.isTrusted = false;
    this.target = null;
    this.currentTarget = null;
    this.eventPhase = 0;
    this.timeStamp = N.now();
    this._stop = false;
    this._stopImm = false;
  }
  preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
  stopPropagation() { this._stop = true; }
  stopImmediatePropagation() { this._stop = this._stopImm = true; }
  get returnValue() { return !this.defaultPrevented; }
  set returnValue(v) { if (!v) this.preventDefault(); }
  get srcElement() { return this.target; }
  composedPath() { return this._path ? this._path.slice() : []; }
}
Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;
class CustomEvent extends Event { constructor(t, i) { super(t, i); this.detail = i && i.detail !== undefined ? i.detail : null; } }
class UIEvent extends Event { constructor(t, i) { super(t, i); this.view = G; this.detail = (i && i.detail) || 0; } }
class FocusEvent extends UIEvent { constructor(t, i) { super(t, i); this.relatedTarget = (i && i.relatedTarget) || null; } }
class MouseEvent extends UIEvent {
  constructor(t, i) {
    super(t, i); i = i || {};
    this.clientX = this.pageX = this.screenX = this.x = i.clientX || 0;
    this.clientY = this.pageY = this.screenY = this.y = i.clientY || 0;
    this.button = i.button || 0; this.buttons = i.buttons || 0;
    this.ctrlKey = this.shiftKey = this.altKey = this.metaKey = false;
    this.relatedTarget = null;
  }
  get offsetX() { const r = this.target && this.target.getBoundingClientRect ? this.target.getBoundingClientRect() : {left: 0}; return this.clientX - r.left; }
  get offsetY() { const r = this.target && this.target.getBoundingClientRect ? this.target.getBoundingClientRect() : {top: 0}; return this.clientY - r.top; }
  get movementX() { return 0; } get movementY() { return 0; }
}
class PointerEvent extends MouseEvent {
  constructor(t, i) { super(t, i); i = i || {}; this.pointerId = i.pointerId || 1; this.pointerType = 'touch'; this.isPrimary = true; this.width = this.height = 1; this.pressure = t === 'pointerup' ? 0 : 0.5; }
}
class Touch { constructor(i) { Object.assign(this, i); } }
class TouchEvent extends UIEvent {
  constructor(t, i) { super(t, i); i = i || {}; this.touches = i.touches || []; this.targetTouches = i.targetTouches || this.touches; this.changedTouches = i.changedTouches || this.touches; }
}
class KeyboardEvent extends UIEvent { constructor(t, i) { super(t, i); i = i || {}; this.key = i.key || ''; this.code = i.code || ''; this.keyCode = this.which = i.keyCode || 0; this.repeat = false; this.ctrlKey = this.shiftKey = this.altKey = this.metaKey = false; } }
class InputEvent extends UIEvent { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data || null; this.inputType = i.inputType || ''; this.isComposing = false; } }
class SubmitEvent extends Event { constructor(t, i) { super(t, i); this.submitter = (i && i.submitter) || null; } }
class TransitionEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.propertyName = i.propertyName || ''; this.elapsedTime = 0; } }
class AnimationEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.animationName = i.animationName || ''; this.elapsedTime = 0; } }
class HashChangeEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.oldURL = i.oldURL || ''; this.newURL = i.newURL || ''; } }
class PopStateEvent extends Event { constructor(t, i) { super(t, i); this.state = (i && i.state) || null; } }
class ErrorEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.message = i.message || ''; this.error = i.error || null; } }

function reportError(e) {
  N.log('error', (e && e.stack) ? (e.message + '\n' + e.stack) : String(e));
  try { const ev = new ErrorEvent('error', {message: String(e && e.message || e), error: e}); dispatchOn(G, ev); } catch (_) {}
}

class EventTarget {
  addEventListener(type, fn, opts) {
    if (!fn) return;
    const capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
    const once = !!(opts && opts.once);
    let m = this[L];
    if (!m) { m = Object.create(null); Object.defineProperty(this, L, {value: m}); }
    const list = m[type] || (m[type] = []);
    for (const l of list) if (l.fn === fn && l.capture === capture) return;
    list.push({fn, capture, once});
    if (opts && opts.signal) opts.signal.addEventListener('abort', () => this.removeEventListener(type, fn, opts));
    N.listen(type);
  }
  removeEventListener(type, fn, opts) {
    const m = this[L]; if (!m || !m[type]) return;
    const capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
    m[type] = m[type].filter((l) => !(l.fn === fn && l.capture === capture));
  }
  dispatchEvent(ev) { return dispatchOn(this, ev); }
}

function parentForEvents(n) {
  if (n === G) return null;
  if (n instanceof Node) {
    const p = N.parent(n);
    if (p) return p;
    if (n.nodeType === 9) return G;
  }
  return null;
}

function handlerOf(node, type) {
  const prop = node['__on' + type];
  if (typeof prop === 'function') return prop;
  if (node instanceof Element) {
    const code = N.attr(node, 'on' + type);
    if (code) {
      const key = '__oncode' + type;
      if (!node[key] || node[key].src !== code) {
        let f;
        try { f = new Function('event', code); } catch (e) { reportError(e); return null; }
        node[key] = {src: code, fn: f};
      }
      return node[key].fn;
    }
  }
  return null;
}

function invoke(node, ev, phase) {
  ev.currentTarget = node;
  ev.eventPhase = phase;
  const m = node[L];
  const list = m && m[ev.type];
  if (list && list.length) {
    for (const l of list.slice()) {
      if (phase === 1 && !l.capture) continue;
      if (phase === 3 && l.capture) continue;
      if (l.once) node.removeEventListener(ev.type, l.fn, l.capture);
      try {
        if (typeof l.fn === 'function') l.fn.call(node, ev);
        else if (l.fn && typeof l.fn.handleEvent === 'function') l.fn.handleEvent(ev);
      } catch (e) { reportError(e); }
      if (ev._stopImm) return;
    }
  }
  if (phase !== 1) {
    const h = handlerOf(node, ev.type);
    if (h) {
      try { const r = h.call(node, ev); if (r === false) ev.preventDefault(); } catch (e) { reportError(e); }
    }
  }
}

function dispatchOn(target, ev) {
  ev.target = target;
  const path = [];
  for (let n = target; n; n = parentForEvents(n)) path.push(n);
  ev._path = path;
  for (let i = path.length - 1; i > 0 && !ev._stop; i--) invoke(path[i], ev, 1);
  if (!ev._stop) invoke(target, ev, 2);
  if (ev.bubbles) for (let i = 1; i < path.length && !ev._stop; i++) invoke(path[i], ev, 3);
  ev.currentTarget = null;
  ev.eventPhase = 0;
  return !ev.defaultPrevented;
}

// events created by the runtime (touch, click, input...)
const POINTER = new Set(['pointerdown', 'pointermove', 'pointerup', 'pointercancel']);
const MOUSE = new Set(['click', 'dblclick', 'mousedown', 'mouseup', 'mousemove', 'contextmenu']);
const TOUCH = new Set(['touchstart', 'touchmove', 'touchend', 'touchcancel']);
let lastTouch = null;
G.__dispatch = function (target, type, bubbles, cancelable, x, y, pid, detail) {
  let ev;
  const init = {bubbles, cancelable, clientX: x, clientY: y, pointerId: pid};
  if (POINTER.has(type)) ev = new PointerEvent(type, init);
  else if (MOUSE.has(type)) ev = new MouseEvent(type, Object.assign(init, {detail: 1}));
  else if (TOUCH.has(type)) {
    const t = new Touch({identifier: pid, target, clientX: x, clientY: y, pageX: x, pageY: y, screenX: x, screenY: y, radiusX: 1, radiusY: 1, force: 1});
    const touches = type === 'touchend' || type === 'touchcancel' ? [] : [t];
    ev = new TouchEvent(type, Object.assign(init, {touches, changedTouches: [t]}));
    lastTouch = t;
  } else if (type === 'keydown' || type === 'keyup') ev = new KeyboardEvent(type, Object.assign(init, detail || {}));
  else if (type === 'input' || type === 'beforeinput') ev = new InputEvent(type, init);
  else if (type === 'focus' || type === 'blur' || type === 'focusin' || type === 'focusout') ev = new FocusEvent(type, init);
  else if (type === 'transitionend') ev = new TransitionEvent(type, Object.assign(init, detail || {}));
  else if (type.startsWith('animation')) ev = new AnimationEvent(type, Object.assign(init, detail || {}));
  else if (type === 'submit') ev = new SubmitEvent(type, init);
  else ev = new CustomEvent(type, Object.assign(init, {detail}));
  ev.isTrusted = true;
  const ok = dispatchOn(target, ev);
  if (type === 'submit' && ok) { /* no navigation on the device */ }
  return ok;
};
G.__dispatchWindow = function (type, detail) {
  let ev;
  if (type === 'hashchange') ev = new HashChangeEvent(type, detail || {});
  else if (type === 'popstate') ev = new PopStateEvent(type, {state: history.state});
  else ev = new Event(type);
  ev.isTrusted = true;
  if (type === 'DOMContentLoaded') return dispatchOn(document, ev);
  return dispatchOn(G, ev);
};

// ------------------------------------------------------------------ DOMTokenList / dataset / style
class DOMTokenList {
  constructor(el, attr) { this._el = el; this._attr = attr; }
  _get() { return (N.attr(this._el, this._attr) || '').split(/\s+/).filter(Boolean); }
  _set(a) { N.setAttr(this._el, this._attr, a.join(' ')); }
  get length() { return this._get().length; }
  get value() { return N.attr(this._el, this._attr) || ''; }
  set value(v) { N.setAttr(this._el, this._attr, String(v)); }
  item(i) { return this._get()[i] || null; }
  contains(c) { return this._get().includes(String(c)); }
  add(...cs) { const a = this._get(); let ch = false; for (const c of cs) if (!a.includes(c)) { a.push(String(c)); ch = true; } if (ch) this._set(a); }
  remove(...cs) { const a = this._get(); const b = a.filter((x) => !cs.includes(x)); if (b.length !== a.length) this._set(b); }
  toggle(c, force) {
    const has = this.contains(c);
    const want = force === undefined ? !has : !!force;
    if (want && !has) this.add(c); else if (!want && has) this.remove(c);
    return want;
  }
  replace(a, b) { const l = this._get(); const i = l.indexOf(a); if (i < 0) return false; l[i] = b; this._set(l); return true; }
  forEach(fn, t) { this._get().forEach(fn, t); }
  entries() { return this._get().entries(); }
  keys() { return this._get().keys(); }
  values() { return this._get().values(); }
  [Symbol.iterator]() { return this._get()[Symbol.iterator](); }
  toString() { return this.value; }
}

const camelToKebab = (s) => s.startsWith('--') ? s : s.replace(/[A-Z]/g, (m) => '-' + m.toLowerCase()).replace(/^(webkit|moz|ms)-/, '-$1-');
const kebabToCamel = (s) => s.replace(/-([a-z])/g, (_, c) => c.toUpperCase());

function makeStyle(el) {
  const decl = {
    getPropertyValue: (p) => N.getStyle(el, p),
    setProperty: (p, v, pri) => N.setStyle(el, p, v == null ? '' : String(v), pri || ''),
    removeProperty: (p) => { const old = N.getStyle(el, p); N.setStyle(el, p, ''); return old; },
    getPropertyPriority: () => '',
    item: (i) => N.styleNames(el)[i] || '',
  };
  return new Proxy(decl, {
    get(t, k) {
      if (typeof k === 'symbol') return t[k];
      if (k in t) return t[k];
      if (k === 'cssText') return N.cssText(el);
      if (k === 'length') return N.styleNames(el).length;
      if (/^\d+$/.test(k)) return N.styleNames(el)[+k] || '';
      return N.getStyle(el, camelToKebab(k));
    },
    set(t, k, v) {
      if (k === 'cssText') { N.setCssText(el, String(v)); return true; }
      N.setStyle(el, camelToKebab(String(k)), v == null ? '' : String(v), '');
      return true;
    },
    has(t, k) { return true; },
  });
}

function makeComputed(el) {
  return new Proxy({}, {
    get(t, k) {
      if (k === 'getPropertyValue') return (p) => N.computed(el, p);
      if (typeof k === 'symbol') return undefined;
      return N.computed(el, camelToKebab(k));
    },
  });
}

function makeDataset(el) {
  return new Proxy({}, {
    get(t, k) { if (typeof k === 'symbol') return undefined; const v = N.attr(el, 'data-' + camelToKebab(k)); return v === null ? undefined : v; },
    set(t, k, v) { N.setAttr(el, 'data-' + camelToKebab(k), String(v)); return true; },
    deleteProperty(t, k) { N.removeAttr(el, 'data-' + camelToKebab(k)); return true; },
    has(t, k) { return N.attr(el, 'data-' + camelToKebab(k)) !== null; },
    ownKeys() { return N.attrNames(el).filter((n) => n.startsWith('data-')).map((n) => kebabToCamel(n.slice(5))); },
    getOwnPropertyDescriptor(t, k) { const v = N.attr(el, 'data-' + camelToKebab(k)); return v === null ? undefined : {value: v, enumerable: true, configurable: true, writable: true}; },
  });
}

class DOMRect {
  constructor(x = 0, y = 0, w = 0, h = 0) { this.x = x; this.y = y; this.width = w; this.height = h; }
  get left() { return this.x; } get top() { return this.y; }
  get right() { return this.x + this.width; } get bottom() { return this.y + this.height; }
  toJSON() { return {x: this.x, y: this.y, width: this.width, height: this.height, top: this.top, left: this.left, right: this.right, bottom: this.bottom}; }
}

// ------------------------------------------------------------------ nodes
function toNodes(args) {
  return args.map((a) => (a instanceof Node ? a : N.text(String(a))));
}

class Node extends EventTarget {
  get nodeType() { return N.nodeType(this); }
  get nodeName() { return N.nodeName(this); }
  get parentNode() { return N.parent(this); }
  get parentElement() { const p = N.parent(this); return p && p.nodeType === 1 ? p : null; }
  get childNodes() { return N.childNodes(this); }
  get firstChild() { return N.first(this); }
  get lastChild() { return N.last(this); }
  get nextSibling() { return N.next(this); }
  get previousSibling() { return N.prev(this); }
  get textContent() { return this.nodeType === 9 ? null : N.getText(this); }
  set textContent(v) { N.setText(this, v == null ? '' : String(v)); }
  get nodeValue() { const t = this.nodeType; return t === 3 || t === 8 ? N.getText(this) : null; }
  set nodeValue(v) { const t = this.nodeType; if (t === 3 || t === 8) N.setText(this, String(v)); }
  get ownerDocument() { return this.nodeType === 9 ? null : document; }
  get isConnected() { let n = this; while (N.parent(n)) n = N.parent(n); return n.nodeType === 9; }
  hasChildNodes() { return !!N.first(this); }
  appendChild(c) { return N.insert(this, c, null); }
  insertBefore(c, ref) { return N.insert(this, c, ref || null); }
  removeChild(c) { return N.removeChild(this, c); }
  replaceChild(n, o) { N.insert(this, n, o); N.removeChild(this, o); return o; }
  cloneNode(deep) { const c = N.clone(this, !!deep); return c; }
  contains(o) { return !!o && N.contains(this, o); }
  getRootNode() { let n = this; while (N.parent(n)) n = N.parent(n); return n; }
  isSameNode(o) { return this === o; }
  isEqualNode(o) { return !!o && o.nodeType === this.nodeType && (this.nodeType === 1 ? o.outerHTML === this.outerHTML : o.textContent === this.textContent); }
  compareDocumentPosition(o) {
    if (o === this) return 0;
    if (N.contains(this, o)) return 20;
    if (N.contains(o, this)) return 10;
    const all = document.querySelectorAll('*');
    return all.indexOf(o) > all.indexOf(this) ? 4 : 2;
  }
  normalize() {}
}
Node.ELEMENT_NODE = 1; Node.TEXT_NODE = 3; Node.COMMENT_NODE = 8; Node.DOCUMENT_NODE = 9; Node.DOCUMENT_FRAGMENT_NODE = 11;

class CharacterData extends Node {
  get data() { return N.getText(this); }
  set data(v) { N.setText(this, String(v)); }
  get length() { return N.getText(this).length; }
  appendData(s) { this.data = this.data + s; }
  remove() { N.remove(this); }
  before(...n) { const p = this.parentNode; if (p) for (const x of toNodes(n)) N.insert(p, x, this); }
  after(...n) { const p = this.parentNode; if (p) { const ref = this.nextSibling; for (const x of toNodes(n)) N.insert(p, x, ref); } }
  replaceWith(...n) { this.before(...n); this.remove(); }
  get nextElementSibling() { let n = N.next(this); while (n && n.nodeType !== 1) n = N.next(n); return n; }
  get previousElementSibling() { let n = N.prev(this); while (n && n.nodeType !== 1) n = N.prev(n); return n; }
}
class Text extends CharacterData {
  constructor(s = '') { super(); return N.text(String(s)); }
  get wholeText() { return this.data; }
  splitText(off) { const d = this.data; this.data = d.slice(0, off); const t = N.text(d.slice(off)); this.after(t); return t; }
}
class Comment extends CharacterData { constructor(s = '') { super(); return N.comment(String(s)); } }

// shared by Element / Document / DocumentFragment
const ParentMixin = {
  get children() { return N.children(this); },
  get firstElementChild() { return N.children(this)[0] || null; },
  get lastElementChild() { const c = N.children(this); return c[c.length - 1] || null; },
  get childElementCount() { return N.children(this).length; },
  querySelector(s) { return N.qs(this, String(s)); },
  querySelectorAll(s) { return N.qsa(this, String(s)); },
  getElementsByClassName(c) { return N.qsa(this, String(c).trim().split(/\s+/).map((x) => '.' + x).join('')); },
  getElementsByTagName(t) { return N.qsa(this, t === '*' ? '*' : String(t)); },
  append(...n) { for (const x of toNodes(n)) N.insert(this, x, null); },
  prepend(...n) { const f = N.first(this); for (const x of toNodes(n)) N.insert(this, x, f); },
  replaceChildren(...n) { while (N.first(this)) N.removeChild(this, N.first(this)); this.append(...n); },
};
function mixin(cls, obj) {
  for (const k of Object.getOwnPropertyNames(obj)) Object.defineProperty(cls.prototype, k, Object.getOwnPropertyDescriptor(obj, k));
}

let upgrading = null;
class Element extends Node {
  get tagName() { return N.nodeName(this); }
  get localName() { return lower(N.nodeName(this)); }
  get id() { return N.attr(this, 'id') || ''; }
  set id(v) { N.setAttr(this, 'id', String(v)); }
  get className() { return N.attr(this, 'class') || ''; }
  set className(v) { N.setAttr(this, 'class', String(v)); }
  get classList() { return this.__cl || (this.__cl = new DOMTokenList(this, 'class')); }
  set classList(v) { this.className = v; }
  getAttribute(n) { return N.attr(this, lower(n)); }
  setAttribute(n, v) { N.setAttr(this, lower(n), String(v)); }
  removeAttribute(n) { N.removeAttr(this, lower(n)); }
  hasAttribute(n) { return N.attr(this, lower(n)) !== null; }
  hasAttributes() { return N.attrNames(this).length > 0; }
  toggleAttribute(n, force) {
    const has = this.hasAttribute(n);
    const want = force === undefined ? !has : !!force;
    if (want && !has) this.setAttribute(n, ''); else if (!want && has) this.removeAttribute(n);
    return want;
  }
  getAttributeNames() { return N.attrNames(this); }
  get attributes() {
    const names = N.attrNames(this);
    const a = names.map((n) => ({name: n, localName: n, value: N.attr(this, n), nodeName: n}));
    a.getNamedItem = (n) => a.find((x) => x.name === lower(n)) || null;
    a.item = (i) => a[i] || null;
    return a;
  }
  getAttributeNS(ns, n) { return this.getAttribute(n); }
  setAttributeNS(ns, n, v) { this.setAttribute(n.includes(':') ? n.split(':')[1] : n, v); }
  removeAttributeNS(ns, n) { this.removeAttribute(n); }
  get innerHTML() { return N.innerHTML(this); }
  set innerHTML(v) { N.setInnerHTML(this, v == null ? '' : String(v)); }
  get outerHTML() { return N.outerHTML(this); }
  set outerHTML(v) { N.setOuterHTML(this, String(v)); }
  get innerText() { return N.getText(this); }
  set innerText(v) { N.setText(this, v == null ? '' : String(v)); }
  get outerText() { return this.innerText; }
  insertAdjacentHTML(pos, html) { N.insertHTML(this, String(pos), String(html)); }
  insertAdjacentElement(pos, el) {
    pos = lower(pos);
    if (pos === 'beforebegin') this.before(el); else if (pos === 'afterbegin') this.prepend(el);
    else if (pos === 'beforeend') this.append(el); else if (pos === 'afterend') this.after(el);
    return el;
  }
  insertAdjacentText(pos, t) { this.insertAdjacentElement(pos, N.text(String(t))); }
  before(...n) { const p = this.parentNode; if (p) for (const x of toNodes(n)) N.insert(p, x, this); }
  after(...n) { const p = this.parentNode; if (p) { const ref = this.nextSibling; for (const x of toNodes(n)) N.insert(p, x, ref); } }
  replaceWith(...n) { const p = this.parentNode; if (!p) return; for (const x of toNodes(n)) N.insert(p, x, this); N.remove(this); }
  remove() { N.remove(this); }
  get nextElementSibling() { let n = N.next(this); while (n && n.nodeType !== 1) n = N.next(n); return n; }
  get previousElementSibling() { let n = N.prev(this); while (n && n.nodeType !== 1) n = N.prev(n); return n; }
  matches(s) { return N.matches(this, String(s)); }
  webkitMatchesSelector(s) { return this.matches(s); }
  closest(s) { return N.closest(this, String(s)); }
  get style() { return this.__st || (this.__st = makeStyle(this)); }
  set style(v) { N.setCssText(this, String(v)); }
  get dataset() { return this.__ds || (this.__ds = makeDataset(this)); }
  getBoundingClientRect() { const r = N.rect(this); return new DOMRect(r[0], r[1], r[2], r[3]); }
  getClientRects() { return [this.getBoundingClientRect()]; }
  get offsetLeft() { return N.offset(this)[0]; } get offsetTop() { return N.offset(this)[1]; }
  get offsetWidth() { return N.offset(this)[2]; } get offsetHeight() { return N.offset(this)[3]; }
  get offsetParent() { return N.offset(this)[4]; }
  get clientLeft() { return N.client(this)[0]; } get clientTop() { return N.client(this)[1]; }
  get clientWidth() { return Math.round(N.client(this)[2]); } get clientHeight() { return Math.round(N.client(this)[3]); }
  get scrollLeft() { return N.scroll(this)[0]; } set scrollLeft(v) { N.setScroll(this, +v, N.scroll(this)[1], false); }
  get scrollTop() { return N.scroll(this)[1]; } set scrollTop(v) { N.setScroll(this, N.scroll(this)[0], +v, false); }
  get scrollWidth() { return Math.round(N.scroll(this)[2]); } get scrollHeight() { return Math.round(N.scroll(this)[3]); }
  scrollTo(x, y) {
    if (typeof x === 'object' && x) { const s = N.scroll(this); N.setScroll(this, x.left !== undefined ? +x.left : s[0], x.top !== undefined ? +x.top : s[1], x.behavior === 'smooth'); }
    else N.setScroll(this, +x || 0, +y || 0, false);
  }
  scroll(x, y) { this.scrollTo(x, y); }
  scrollBy(x, y) {
    const s = N.scroll(this);
    if (typeof x === 'object' && x) N.setScroll(this, s[0] + (x.left || 0), s[1] + (x.top || 0), x.behavior === 'smooth');
    else N.setScroll(this, s[0] + (+x || 0), s[1] + (+y || 0), false);
  }
  scrollIntoView(o) { N.scrollIntoView(this, !!(o && typeof o === 'object' && o.behavior === 'smooth')); }
  focus() { N.focus(this); }
  blur() { N.blur(this); }
  click() { if (G.__dispatch(this, 'click', true, true, 0, 0, 0)) N.activate(this); }
  get hidden() { return this.hasAttribute('hidden'); }
  set hidden(v) { this.toggleAttribute('hidden', !!v); }
  get title() { return this.getAttribute('title') || ''; } set title(v) { this.setAttribute('title', v); }
  get lang() { return this.getAttribute('lang') || ''; } set lang(v) { this.setAttribute('lang', v); }
  get dir() { return this.getAttribute('dir') || ''; } set dir(v) { this.setAttribute('dir', v); }
  get tabIndex() { return +(this.getAttribute('tabindex') || -1); } set tabIndex(v) { this.setAttribute('tabindex', v); }
  get slot() { return ''; }
  get shadowRoot() { return this.__shadow || null; }
  attachShadow() { this.__shadow = this; return this; }
  setPointerCapture() {} releasePointerCapture() {} hasPointerCapture() { return false; }
  requestFullscreen() { return Promise.resolve(); }
  animate(keyframes, options) { return new Animation(this, keyframes, options); }
  getAnimations() { return []; }
  get isContentEditable() { return false; }
}
mixin(Element, ParentMixin);

// on<event> handler properties
const EVENTS = ['click', 'dblclick', 'input', 'change', 'submit', 'reset', 'focus', 'blur', 'focusin', 'focusout', 'keydown', 'keyup',
  'keypress', 'pointerdown', 'pointermove', 'pointerup', 'pointercancel', 'touchstart', 'touchmove', 'touchend', 'touchcancel',
  'mousedown', 'mouseup', 'mousemove', 'scroll', 'load', 'error', 'transitionend', 'animationend', 'animationstart',
  'animationiteration', 'toggle', 'close', 'cancel', 'contextmenu', 'wheel', 'select', 'invalid', 'hashchange', 'popstate',
  'DOMContentLoaded', 'resize', 'message', 'beforeunload', 'unload'];
function defineHandlers(proto) {
  for (const t of EVENTS) {
    Object.defineProperty(proto, 'on' + lower(t), {
      get() { return this['__on' + t] || null; },
      set(f) { this['__on' + t] = typeof f === 'function' ? f : null; if (f) N.listen(t); },
      configurable: true,
    });
  }
}

class HTMLElement extends Element {
  constructor() {
    super();
    if (upgrading) { const e = upgrading; upgrading = null; return e; }
    const tag = registryByClass.get(new.target);
    if (!tag) throw new TypeError('Illegal constructor');
    const e = N.create(tag);
    Object.setPrototypeOf(e, new.target.prototype);
    return e;
  }
}
defineHandlers(HTMLElement.prototype);

class HTMLInputElement extends HTMLElement {
  get value() { return N.value(this); } set value(v) { N.setValue(this, v == null ? '' : String(v)); }
  get defaultValue() { return this.getAttribute('value') || ''; } set defaultValue(v) { this.setAttribute('value', v); }
  get valueAsNumber() { const v = parseFloat(this.value); return isNaN(v) ? NaN : v; }
  set valueAsNumber(v) { this.value = String(v); }
  get checked() { return N.checked(this); } set checked(v) { N.setChecked(this, !!v); }
  get defaultChecked() { return this.hasAttribute('checked'); } set defaultChecked(v) { this.toggleAttribute('checked', !!v); }
  get indeterminate() { return N.state(this, 'indeterminate'); } set indeterminate(v) { N.state(this, 'indeterminate', !!v); }
  get type() { return lower(this.getAttribute('type') || 'text'); } set type(v) { this.setAttribute('type', v); }
  get name() { return this.getAttribute('name') || ''; } set name(v) { this.setAttribute('name', v); }
  get placeholder() { return this.getAttribute('placeholder') || ''; } set placeholder(v) { this.setAttribute('placeholder', v); }
  get disabled() { return this.hasAttribute('disabled'); } set disabled(v) { this.toggleAttribute('disabled', !!v); }
  get readOnly() { return this.hasAttribute('readonly'); } set readOnly(v) { this.toggleAttribute('readonly', !!v); }
  get required() { return this.hasAttribute('required'); } set required(v) { this.toggleAttribute('required', !!v); }
  get min() { return this.getAttribute('min') || ''; } set min(v) { this.setAttribute('min', v); }
  get max() { return this.getAttribute('max') || ''; } set max(v) { this.setAttribute('max', v); }
  get step() { return this.getAttribute('step') || ''; } set step(v) { this.setAttribute('step', v); }
  get maxLength() { return +(this.getAttribute('maxlength') || -1); } set maxLength(v) { this.setAttribute('maxlength', v); }
  get pattern() { return this.getAttribute('pattern') || ''; }
  get form() { return this.closest('form'); }
  get labels() { return this.id ? document.querySelectorAll('label[for="' + this.id + '"]') : []; }
  get validity() { const v = this.checkValidity(); return {valid: v, valueMissing: this.required && !this.value}; }
  get validationMessage() { return this.checkValidity() ? '' : 'Please fill out this field.'; }
  get files() { return []; }
  checkValidity() {
    if (this.required && !this.value && this.type !== 'checkbox' && this.type !== 'radio') return false;
    if (this.required && (this.type === 'checkbox') && !this.checked) return false;
    if (this.pattern && this.value && !new RegExp('^(?:' + this.pattern + ')$').test(this.value)) return false;
    if (this.type === 'email' && this.value && !/^[^@\s]+@[^@\s]+$/.test(this.value)) return false;
    return true;
  }
  reportValidity() { return this.checkValidity(); }
  setCustomValidity() {}
  select() {} setSelectionRange() {}
  stepUp(n = 1) { const s = +this.step || 1; this.value = String((+this.value || 0) + s * n); }
  stepDown(n = 1) { this.stepUp(-n); }
  showPicker() { this.click(); }
}
class HTMLTextAreaElement extends HTMLElement {
  get value() { return N.value(this); } set value(v) { N.setValue(this, v == null ? '' : String(v)); }
  get defaultValue() { return this.textContent; } set defaultValue(v) { this.textContent = v; }
  get name() { return this.getAttribute('name') || ''; }
  get placeholder() { return this.getAttribute('placeholder') || ''; } set placeholder(v) { this.setAttribute('placeholder', v); }
  get disabled() { return this.hasAttribute('disabled'); } set disabled(v) { this.toggleAttribute('disabled', !!v); }
  get readOnly() { return this.hasAttribute('readonly'); } set readOnly(v) { this.toggleAttribute('readonly', !!v); }
  get form() { return this.closest('form'); }
  get type() { return 'textarea'; }
  checkValidity() { return !(this.hasAttribute('required') && !this.value); }
  reportValidity() { return this.checkValidity(); }
  select() {} setSelectionRange() {}
}
class HTMLSelectElement extends HTMLElement {
  get value() { return N.value(this); } set value(v) { N.setValue(this, String(v)); }
  get selectedIndex() { return N.selectedIndex(this); } set selectedIndex(i) { N.setSelectedIndex(this, +i); }
  get options() { const o = N.options(this); o.selectedIndex = this.selectedIndex; o.item = (i) => o[i] || null; o.add = (opt, before) => this.add(opt, before); return o; }
  get selectedOptions() { const i = this.selectedIndex; const o = N.options(this); return i >= 0 && o[i] ? [o[i]] : []; }
  get length() { return N.options(this).length; }
  get name() { return this.getAttribute('name') || ''; }
  get disabled() { return this.hasAttribute('disabled'); } set disabled(v) { this.toggleAttribute('disabled', !!v); }
  get multiple() { return this.hasAttribute('multiple'); }
  get form() { return this.closest('form'); }
  get type() { return 'select-one'; }
  add(opt, before) { if (typeof before === 'number') before = N.options(this)[before] || null; this.insertBefore(opt, before || null); }
  remove(i) { if (i === undefined) return Element.prototype.remove.call(this); const o = N.options(this)[i]; if (o) o.remove(); }
  item(i) { return N.options(this)[i] || null; }
  checkValidity() { return true; }
}
class HTMLOptionElement extends HTMLElement {
  constructor(text, value, defSel, sel) {
    const o = N.create('option');
    Object.setPrototypeOf(o, HTMLOptionElement.prototype);
    if (text !== undefined) o.textContent = String(text);
    if (value !== undefined) o.setAttribute('value', String(value));
    if (defSel) o.setAttribute('selected', '');
    return o;
  }
  get value() { const v = this.getAttribute('value'); return v !== null ? v : this.textContent.trim(); }
  set value(v) { this.setAttribute('value', v); }
  get text() { return this.textContent.trim(); } set text(v) { this.textContent = v; }
  get label() { return this.getAttribute('label') || this.text; }
  get disabled() { return this.hasAttribute('disabled'); } set disabled(v) { this.toggleAttribute('disabled', !!v); }
  get index() { const s = this.closest('select'); return s ? N.options(s).indexOf(this) : 0; }
  get selected() { const s = this.closest('select'); return s ? s.selectedIndex === this.index : this.hasAttribute('selected'); }
  set selected(v) { const s = this.closest('select'); if (s && v) s.selectedIndex = this.index; }
  get defaultSelected() { return this.hasAttribute('selected'); }
}
class HTMLButtonElement extends HTMLElement {
  get type() { return lower(this.getAttribute('type') || 'submit'); } set type(v) { this.setAttribute('type', v); }
  get disabled() { return this.hasAttribute('disabled'); } set disabled(v) { this.toggleAttribute('disabled', !!v); }
  get value() { return this.getAttribute('value') || ''; } set value(v) { this.setAttribute('value', v); }
  get name() { return this.getAttribute('name') || ''; }
  get form() { return this.closest('form'); }
}
class HTMLFormElement extends HTMLElement {
  get elements() {
    const els = this.querySelectorAll('input,select,textarea,button,fieldset,output');
    for (const e of els) { const n = e.getAttribute('name') || e.id; if (n && !(n in els)) els[n] = e; }
    els.namedItem = (n) => els[n] || null;
    return els;
  }
  get length() { return this.elements.length; }
  get action() { return this.getAttribute('action') || ''; }
  get method() { return lower(this.getAttribute('method') || 'get'); }
  submit() { /* no navigation */ }
  requestSubmit(s) { if (this.checkValidity()) G.__dispatch(this, 'submit', true, true, 0, 0, 0); }
  reset() {
    for (const e of this.elements) {
      if (e.type === 'checkbox' || e.type === 'radio') e.checked = e.defaultChecked;
      else if ('defaultValue' in e) e.value = e.defaultValue;
      else if (e instanceof HTMLSelectElement) { const o = N.options(e); const d = o.findIndex((x) => x.defaultSelected); e.selectedIndex = d >= 0 ? d : 0; }
    }
    G.__dispatch(this, 'reset', true, false, 0, 0, 0);
  }
  checkValidity() { return this.elements.every((e) => !e.checkValidity || e.checkValidity()); }
  reportValidity() { return this.checkValidity(); }
}
class HTMLAnchorElement extends HTMLElement {
  get href() { return this.getAttribute('href') || ''; } set href(v) { this.setAttribute('href', v); }
  get hash() { const h = this.href; const i = h.indexOf('#'); return i >= 0 ? h.slice(i) : ''; }
  get pathname() { return this.href.split('#')[0].split('?')[0]; }
  get target() { return this.getAttribute('target') || ''; }
  get text() { return this.textContent; }
}
class HTMLImageElement extends HTMLElement {
  constructor(w, h) {
    const i = N.create('img');
    Object.setPrototypeOf(i, HTMLImageElement.prototype);
    if (w !== undefined) i.setAttribute('width', w);
    if (h !== undefined) i.setAttribute('height', h);
    return i;
  }
  get src() { return this.getAttribute('src') || ''; }
  set src(v) {
    this.setAttribute('src', String(v));
    setTimeout(() => { const s = N.naturalSize(this); G.__dispatch(this, s[0] ? 'load' : 'error', false, false, 0, 0, 0); }, 0);
  }
  get alt() { return this.getAttribute('alt') || ''; } set alt(v) { this.setAttribute('alt', v); }
  get width() { return this.offsetWidth || +(this.getAttribute('width') || 0); } set width(v) { this.setAttribute('width', v); }
  get height() { return this.offsetHeight || +(this.getAttribute('height') || 0); } set height(v) { this.setAttribute('height', v); }
  get naturalWidth() { return N.naturalSize(this)[0]; }
  get naturalHeight() { return N.naturalSize(this)[1]; }
  get complete() { return true; }
  decode() { return Promise.resolve(); }
}
class HTMLCanvasElement extends HTMLElement {
  get width() { return +(this.getAttribute('width') || 300); } set width(v) { this.setAttribute('width', String(v | 0)); }
  get height() { return +(this.getAttribute('height') || 150); } set height(v) { this.setAttribute('height', String(v | 0)); }
  getContext(type) { return type === '2d' ? (this.__ctx || (this.__ctx = N.canvas(this))) : null; }
  toDataURL() { return 'data:,'; }
  toBlob(cb) { cb && cb(null); }
}
class HTMLDialogElement extends HTMLElement {
  get open() { return this.hasAttribute('open'); }
  set open(v) { this.toggleAttribute('open', !!v); }
  show() { this.setAttribute('open', ''); }
  showModal() { this.returnValue = ''; N.modal(this, true); const f = this.querySelector('[autofocus]'); if (f) f.focus(); }
  close(rv) {
    if (!this.open) return;
    if (rv !== undefined) this.returnValue = String(rv);
    N.modal(this, false);
    G.__dispatch(this, 'close', false, false, 0, 0, 0);
  }
  requestClose(rv) { this.close(rv); }
}
class HTMLDetailsElement extends HTMLElement {
  get open() { return this.hasAttribute('open'); } set open(v) { this.toggleAttribute('open', !!v); }
}
class HTMLTemplateElement extends HTMLElement {
  get content() {
    const f = N.fragment();
    for (const c of N.childNodes(this)) N.insert(f, N.clone(c, true), null);
    return f;
  }
}
class HTMLProgressElement extends HTMLElement {
  get value() { return +(this.getAttribute('value') || 0); } set value(v) { this.setAttribute('value', String(v)); }
  get max() { return +(this.getAttribute('max') || 1); } set max(v) { this.setAttribute('max', String(v)); }
  get position() { return this.hasAttribute('value') ? this.value / this.max : -1; }
}
class HTMLMeterElement extends HTMLProgressElement {
  get min() { return +(this.getAttribute('min') || 0); } set min(v) { this.setAttribute('min', String(v)); }
}
class HTMLLabelElement extends HTMLElement {
  get htmlFor() { return this.getAttribute('for') || ''; } set htmlFor(v) { this.setAttribute('for', v); }
  get control() { const f = this.htmlFor; return f ? document.getElementById(f) : this.querySelector('input,select,textarea'); }
}
class HTMLOutputElement extends HTMLElement {
  get value() { return this.textContent; } set value(v) { this.textContent = String(v); }
}
class HTMLTableElement extends HTMLElement {
  get rows() { return this.querySelectorAll('tr'); }
  get tBodies() { return this.querySelectorAll('tbody'); }
  get tHead() { return this.querySelector('thead'); }
  insertRow(i = -1) {
    const tb = this.querySelector('tbody') || this.appendChild(document.createElement('tbody'));
    const tr = document.createElement('tr');
    const rows = tb.querySelectorAll('tr');
    tb.insertBefore(tr, i >= 0 && rows[i] ? rows[i] : null);
    return tr;
  }
  deleteRow(i) { const r = this.rows[i < 0 ? this.rows.length - 1 : i]; if (r) r.remove(); }
}
class HTMLTableSectionElement extends HTMLElement {
  get rows() { return this.querySelectorAll('tr'); }
  insertRow(i = -1) { const tr = document.createElement('tr'); const rows = this.querySelectorAll('tr'); this.insertBefore(tr, i >= 0 && rows[i] ? rows[i] : null); return tr; }
  deleteRow(i) { const r = this.rows[i < 0 ? this.rows.length - 1 : i]; if (r) r.remove(); }
}
class HTMLTableRowElement extends HTMLElement {
  get cells() { return N.children(this).filter((c) => c.tagName === 'TD' || c.tagName === 'TH'); }
  get rowIndex() { const t = this.closest('table'); return t ? t.rows.indexOf(this) : -1; }
  insertCell(i = -1) { const td = document.createElement('td'); const c = this.cells; this.insertBefore(td, i >= 0 && c[i] ? c[i] : null); return td; }
  deleteCell(i) { const c = this.cells[i]; if (c) c.remove(); }
}
class SVGElement extends Element {}
defineHandlers(SVGElement.prototype);

class DocumentFragment extends Node {
  constructor() { super(); return N.fragment(); }
  getElementById(id) { return N.qs(this, '#' + id); }
}
mixin(DocumentFragment, ParentMixin);

class Document extends Node {
  get documentElement() { return N.children(this).find((c) => c.tagName === 'HTML') || null; }
  get head() { const h = this.documentElement; return h ? N.children(h).find((c) => c.tagName === 'HEAD') || null : null; }
  get body() { const h = this.documentElement; return h ? N.children(h).find((c) => c.tagName === 'BODY') || null : null; }
  get title() { const t = N.qs(this, 'title'); return t ? t.textContent : ''; }
  set title(v) { let t = N.qs(this, 'title'); if (!t) { t = this.createElement('title'); this.head && this.head.append(t); } t.textContent = v; }
  getElementById(id) { return N.byId(String(id)); }
  createElement(t) { const tag = lower(t); const e = N.create(tag); const C = registry.get(tag); if (C) { upgrading = e; Object.setPrototypeOf(e, C.prototype); try { new C(); } finally { upgrading = null; } } return e; }
  createElementNS(ns, t) { return this.createElement(t); }
  createTextNode(s) { return N.text(String(s)); }
  createComment(s) { return N.comment(String(s)); }
  createDocumentFragment() { return N.fragment(); }
  createEvent(t) { const e = new Event(''); e.initEvent = (type, b, c) => { e.type = type; e.bubbles = !!b; e.cancelable = !!c; }; e.initCustomEvent = (type, b, c, d) => { e.initEvent(type, b, c); e.detail = d; }; return e; }
  createRange() { return {selectNodeContents() {}, getBoundingClientRect() { return new DOMRect(); }, getClientRects() { return []; }, createContextualFragment: (h) => N.parseHTML(h)}; }
  get activeElement() { return N.active() || this.body; }
  get readyState() { return 'complete'; }
  get defaultView() { return G; }
  get location() { return location; }
  set location(v) { location.href = v; }
  get URL() { return location.href; }
  get documentURI() { return location.href; }
  get cookie() { return ''; } set cookie(v) {}
  get hidden() { return false; }
  get visibilityState() { return 'visible'; }
  get fonts() { return {ready: Promise.resolve(), load: () => Promise.resolve([]), check: () => true, addEventListener() {}}; }
  get forms() { return this.querySelectorAll('form'); }
  get images() { return this.querySelectorAll('img'); }
  get links() { return this.querySelectorAll('a[href]'); }
  get scrollingElement() { return this.documentElement; }
  get characterSet() { return 'UTF-8'; }
  elementFromPoint(x, y) { return N.elementFromPoint(x, y); }
  elementsFromPoint(x, y) { const e = N.elementFromPoint(x, y); return e ? [e] : []; }
  hasFocus() { return true; }
  execCommand() { return false; }
  getSelection() { return {removeAllRanges() {}, addRange() {}, toString: () => ''}; }
  importNode(n, deep) { return n.cloneNode(deep); }
  adoptNode(n) { return n; }
}
mixin(Document, ParentMixin);
defineHandlers(Document.prototype);

// prototypes for native wrappers
const TAGS = {
  input: HTMLInputElement, textarea: HTMLTextAreaElement, select: HTMLSelectElement, option: HTMLOptionElement,
  button: HTMLButtonElement, form: HTMLFormElement, a: HTMLAnchorElement, img: HTMLImageElement, canvas: HTMLCanvasElement,
  dialog: HTMLDialogElement, details: HTMLDetailsElement, template: HTMLTemplateElement, progress: HTMLProgressElement,
  meter: HTMLMeterElement, label: HTMLLabelElement, output: HTMLOutputElement, table: HTMLTableElement,
  tbody: HTMLTableSectionElement, thead: HTMLTableSectionElement, tfoot: HTMLTableSectionElement, tr: HTMLTableRowElement,
};
for (const k in TAGS) N.setProto(k, TAGS[k].prototype);
for (const k of ['svg', 'g', 'path', 'circle', 'ellipse', 'line', 'polyline', 'polygon', 'rect', 'text', 'defs', 'use'])
  N.setProto(k, SVGElement.prototype);
N.setProto('*', HTMLElement.prototype);
N.setProto('*custom', HTMLElement.prototype);
N.setProto('#text', Text.prototype);
N.setProto('#comment', Comment.prototype);
N.setProto('#document', Document.prototype);
N.setProto('#fragment', DocumentFragment.prototype);

// ------------------------------------------------------------------ custom elements
const registry = new Map();
const registryByClass = new Map();
const customElements = {
  define(name, cls, opts) {
    name = lower(name);
    if (registry.has(name)) throw new Error('NotSupportedError: ' + name + ' already defined');
    registry.set(name, cls);
    registryByClass.set(cls, name);
    N.setProto(name, cls.prototype);
    for (const el of document.querySelectorAll(name)) upgrade(el, cls);
    const w = whenDefined.get(name);
    if (w) { w.resolve(cls); whenDefined.delete(name); }
  },
  get(name) { return registry.get(lower(name)); },
  whenDefined(name) {
    name = lower(name);
    if (registry.has(name)) return Promise.resolve(registry.get(name));
    let w = whenDefined.get(name);
    if (!w) { let r; const p = new Promise((res) => (r = res)); w = {promise: p, resolve: r}; whenDefined.set(name, w); }
    return w.promise;
  },
  upgrade(root) { for (const [n, c] of registry) for (const el of root.querySelectorAll(n)) upgrade(el, c); },
};
const whenDefined = new Map();
function upgrade(el, cls) {
  if (el.__upgraded) return;
  el.__upgraded = true;
  upgrading = el;
  Object.setPrototypeOf(el, cls.prototype);
  try { new cls(); } catch (e) { reportError(e); } finally { upgrading = null; }
  if (cls.observedAttributes && el.attributeChangedCallback)
    for (const a of cls.observedAttributes) { const v = el.getAttribute(a); if (v !== null) el.attributeChangedCallback(a, null, v); }
  if (el.isConnected && el.connectedCallback) { try { el.connectedCallback(); } catch (e) { reportError(e); } }
}
G.__connected = function (el, connected) {
  const cls = registry.get(el.localName);
  if (!cls) return;
  if (!el.__upgraded) { if (connected) upgrade(el, cls); return; }
  try {
    if (connected && el.connectedCallback) el.connectedCallback();
    if (!connected && el.disconnectedCallback) el.disconnectedCallback();
  } catch (e) { reportError(e); }
};
const attrCache = new WeakMap();
G.__attributeChanged = function (el, name) {
  const cls = registry.get(el.localName);
  if (!cls || !el.__upgraded || !el.attributeChangedCallback) return;
  const obs = cls.observedAttributes || [];
  if (!obs.includes(name)) return;
  let m = attrCache.get(el); if (!m) { m = {}; attrCache.set(el, m); }
  const nv = el.getAttribute(name);
  const ov = name in m ? m[name] : null;
  m[name] = nv;
  try { el.attributeChangedCallback(name, ov, nv); } catch (e) { reportError(e); }
};

// ------------------------------------------------------------------ Web Animations
class Animation extends EventTarget {
  constructor(el, keyframes, options) {
    super();
    if (typeof options === 'number') options = {duration: options};
    options = options || {};
    let frames = [];
    if (Array.isArray(keyframes)) {
      frames = keyframes.map((f, i) => [f.offset != null ? f.offset : (keyframes.length > 1 ? i / (keyframes.length - 1) : 1),
        Object.keys(f).filter((k) => k !== 'offset' && k !== 'easing' && k !== 'composite').map((k) => [k, String(f[k])])]);
    } else if (keyframes) {
      const keys = Object.keys(keyframes).filter((k) => k !== 'offset' && k !== 'easing');
      const n = Math.max(...keys.map((k) => (Array.isArray(keyframes[k]) ? keyframes[k].length : 1)));
      for (let i = 0; i < n; i++)
        frames.push([n > 1 ? i / (n - 1) : 1, keys.map((k) => [k, String(Array.isArray(keyframes[k]) ? keyframes[k][Math.min(i, keyframes[k].length - 1)] : keyframes[k])])]);
    }
    const dirs = {normal: 0, reverse: 1, alternate: 2, 'alternate-reverse': 3};
    const fills = {none: 0, forwards: 1, backwards: 2, both: 3, auto: 0};
    const it = options.iterations === Infinity ? -1 : (options.iterations != null ? options.iterations : 1);
    this.effect = {target: el, getTiming: () => options};
    this._id = N.animate(el, frames, +options.duration || 0, +options.delay || 0, options.easing || 'linear', it,
      dirs[options.direction] || 0, fills[options.fill] || 0);
    this.playState = 'running';
    let res;
    this.finished = new Promise((r) => (res = r));
    this._res = res;
    this.onfinish = null;
    this.oncancel = null;
    animations.set(this._id, this);
  }
  cancel() { N.animCtl(this._id, 'cancel'); this.playState = 'idle'; animations.delete(this._id); if (this.oncancel) this.oncancel(); }
  finish() { N.animCtl(this._id, 'finish'); }
  pause() { N.animCtl(this._id, 'pause'); this.playState = 'paused'; }
  play() { N.animCtl(this._id, 'play'); this.playState = 'running'; }
  reverse() {}
  commitStyles() {}
  persist() {}
}
const animations = new Map();
G.__waapiDone = function (id) {
  const a = animations.get(id);
  if (!a) return;
  animations.delete(id);
  a.playState = 'finished';
  a._res(a);
  if (a.onfinish) try { a.onfinish({type: 'finish'}); } catch (e) { reportError(e); }
  a.dispatchEvent(new Event('finish'));
};

// ------------------------------------------------------------------ window
const document = N.document();
Object.defineProperty(G, 'document', {value: document, enumerable: true});
G.window = G.self = G.globalThis = G.top = G.parent = G.frames = G;
Object.setPrototypeOf(G, EventTarget.prototype);
defineHandlers(G);
G.innerWidth = G.outerWidth = N.W;
G.innerHeight = G.outerHeight = N.H;
G.devicePixelRatio = 1;
G.screen = {width: N.W, height: N.H, availWidth: N.W, availHeight: N.H, orientation: {type: 'landscape-primary', angle: 0}};
G.navigator = {userAgent: 'Mozilla/5.0 (iotdash; ESP32-S3) AppleWebKit/537.36 (KHTML, like Gecko)', language: 'th-TH',
  languages: ['th-TH', 'th', 'en'], onLine: true, platform: 'iotdash', maxTouchPoints: 1, hardwareConcurrency: 2,
  vendor: 'iotdash', clipboard: {writeText: () => Promise.resolve(), readText: () => Promise.resolve('')},
  vibrate: () => false, sendBeacon: () => true};
G.performance = {now: N.now, timeOrigin: 0, mark() {}, measure() {}, getEntriesByName: () => []};
G.console = {};
for (const lv of ['log', 'info', 'warn', 'error', 'debug', 'trace']) {
  G.console[lv] = (...a) => N.log(lv, a.map((x) => {
    if (typeof x === 'string') return x;
    if (x instanceof Error) return x.message + (x.stack ? '\n' + x.stack : '');
    if (x instanceof Node) return '<' + lower(x.nodeName) + '>';
    try { return JSON.stringify(x); } catch (_) { return String(x); }
  }).join(' '));
}
G.console.assert = (c, ...a) => { if (!c) G.console.error('Assertion failed:', ...a); };
G.console.table = G.console.log; G.console.dir = G.console.log; G.console.group = G.console.groupEnd = G.console.time = G.console.timeEnd = () => {};
G.setTimeout = (fn, ms, ...a) => N.timer(typeof fn === 'string' ? new Function(fn) : fn, +ms || 0, false, ...a);
G.setInterval = (fn, ms, ...a) => N.timer(typeof fn === 'string' ? new Function(fn) : fn, +ms || 0, true, ...a);
G.clearTimeout = G.clearInterval = (id) => N.clearTimer(+id || 0);
G.requestAnimationFrame = (fn) => N.raf(fn);
G.cancelAnimationFrame = (id) => N.cancelRaf(+id || 0);
G.requestIdleCallback = (fn) => N.timer(() => fn({timeRemaining: () => 10, didTimeout: false}), 1, false);
G.cancelIdleCallback = (id) => N.clearTimer(id);
G.queueMicrotask = (fn) => Promise.resolve().then(fn);
G.getComputedStyle = (el) => makeComputed(el);
G.matchMedia = (q) => ({matches: N.matchMedia(String(q)), media: String(q), addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {}, onchange: null});
G.scrollTo = (x, y) => document.documentElement.scrollTo(x, y);
G.scrollBy = (x, y) => document.documentElement.scrollBy(x, y);
G.scroll = G.scrollTo;
Object.defineProperty(G, 'scrollX', {get: () => document.documentElement.scrollLeft});
Object.defineProperty(G, 'scrollY', {get: () => document.documentElement.scrollTop});
Object.defineProperty(G, 'pageXOffset', {get: () => document.documentElement.scrollLeft});
Object.defineProperty(G, 'pageYOffset', {get: () => document.documentElement.scrollTop});
G.alert = (m) => N.log('alert', String(m));
G.confirm = (m) => { N.log('confirm', String(m)); return true; };
G.prompt = (m, d) => { N.log('prompt', String(m)); return d === undefined ? '' : String(d); };
G.print = () => {};
G.close = () => {};
G.open = (u) => { if (u) location.href = u; return null; };
G.postMessage = (msg) => { setTimeout(() => { const e = new Event('message'); e.data = msg; dispatchOn(G, e); }, 0); };
G.reportError = reportError;
G.structuredClone = (v) => (v === undefined ? undefined : JSON.parse(JSON.stringify(v)));
G.customElements = customElements;
G.Image = HTMLImageElement;
G.Option = HTMLOptionElement;
G.addEventListener('unhandledrejection', () => {});

Object.assign(G, {Event, CustomEvent, UIEvent, FocusEvent, MouseEvent, PointerEvent, TouchEvent, Touch, KeyboardEvent, InputEvent,
  SubmitEvent, TransitionEvent, AnimationEvent, HashChangeEvent, PopStateEvent, ErrorEvent, EventTarget, Node, CharacterData,
  Text, Comment, Element, HTMLElement, HTMLInputElement, HTMLTextAreaElement, HTMLSelectElement, HTMLOptionElement,
  HTMLButtonElement, HTMLFormElement, HTMLAnchorElement, HTMLImageElement, HTMLCanvasElement, HTMLDialogElement,
  HTMLDetailsElement, HTMLTemplateElement, HTMLProgressElement, HTMLMeterElement, HTMLLabelElement, HTMLOutputElement,
  HTMLTableElement, HTMLTableSectionElement, HTMLTableRowElement, SVGElement, Document, DocumentFragment, DOMTokenList,
  DOMRect, Animation});
for (const t of ['Div', 'Span', 'Paragraph', 'Heading', 'UList', 'OList', 'LI', 'Pre', 'BR', 'HR', 'Body', 'Head', 'Html',
  'Style', 'Script', 'Link', 'Meta', 'Title', 'Video', 'Audio', 'Media', 'Unknown', 'Slot', 'Data', 'Time', 'Picture', 'Source'])
  G['HTML' + t + 'Element'] = HTMLElement;

// ------------------------------------------------------------------ location / history
const location = {
  get href() { return N.nav('url'); },
  set href(u) { u = String(u); if (u.startsWith('#')) N.nav('setHash', u.slice(1), true); else N.nav('open', u); },
  get hash() { return N.nav('hash'); },
  set hash(h) { h = String(h); N.nav('setHash', h.startsWith('#') ? h.slice(1) : h, true); },
  get pathname() { return '/' + N.nav('url').split('#')[0]; },
  get search() { return ''; },
  get origin() { return 'http://iotdash.local'; },
  get host() { return 'iotdash.local'; }, get hostname() { return 'iotdash.local'; }, get port() { return ''; },
  get protocol() { return 'http:'; },
  assign(u) { this.href = u; },
  replace(u) { u = String(u); if (u.startsWith('#')) N.nav('setHash', u.slice(1), false); else N.nav('open', u); },
  reload() { N.nav('reload'); },
  toString() { return this.href; },
};
let historyState = null;
const history = {
  get length() { return 1; },
  get state() { return historyState; },
  back() { N.nav('go', -1); },
  forward() { N.nav('go', 1); },
  go(n) { N.nav('go', n || 0); },
  pushState(st, t, url) { historyState = st; if (url) { url = String(url); const i = url.indexOf('#'); if (i >= 0) N.nav('setHash', url.slice(i + 1), true, 'none'); } },
  replaceState(st, t, url) { historyState = st; if (url) { url = String(url); const i = url.indexOf('#'); if (i >= 0) N.nav('setHash', url.slice(i + 1), false, 'none'); } },
  scrollRestoration: 'auto',
};
G.location = location;
G.history = history;
// navigate with an explicit transition: iot.go('#page', 'up')
G.iot = {
  go(target, transition) {
    target = String(target);
    if (target.startsWith('#')) N.nav('setHash', target.slice(1), true, transition || 'slide');
    else N.nav('open', target, transition || 'slide');
  },
  back() { N.nav('go', -1); },
  replace(selector, html, outer) { return N.replace(String(selector), String(html), !!outer); },
  stats: () => N.stats(),
  gc: () => N.gc(),
};

// ------------------------------------------------------------------ storage
function makeStorage(persist) {
  const mem = new Map();
  const S = {
    getItem: (k) => { k = String(k); if (persist) return N.storage('get', k); return mem.has(k) ? mem.get(k) : null; },
    setItem: (k, v) => { if (persist) N.storage('set', String(k), String(v)); else mem.set(String(k), String(v)); },
    removeItem: (k) => { if (persist) N.storage('remove', String(k)); else mem.delete(String(k)); },
    clear: () => { if (persist) N.storage('clear'); else mem.clear(); },
    key: (i) => (persist ? N.storage('keys') : [...mem.keys()])[i] || null,
    get length() { return (persist ? N.storage('keys') : [...mem.keys()]).length; },
  };
  return new Proxy(S, {
    get(t, k) { if (k in t) return t[k]; if (typeof k === 'symbol') return undefined; const v = t.getItem(k); return v === null ? undefined : v; },
    set(t, k, v) { t.setItem(k, v); return true; },
    deleteProperty(t, k) { t.removeItem(k); return true; },
  });
}
G.localStorage = makeStorage(true);
G.sessionStorage = makeStorage(false);

// ------------------------------------------------------------------ fetch
class Headers {
  constructor(init) {
    this._h = new Map();
    if (init instanceof Headers) init.forEach((v, k) => this.append(k, v));
    else if (Array.isArray(init)) for (const [k, v] of init) this.append(k, v);
    else if (init) for (const k of Object.keys(init)) this.append(k, init[k]);
  }
  append(k, v) { k = lower(k); const o = this._h.get(k); this._h.set(k, o !== undefined ? o + ', ' + v : String(v)); }
  set(k, v) { this._h.set(lower(k), String(v)); }
  get(k) { const v = this._h.get(lower(k)); return v === undefined ? null : v; }
  has(k) { return this._h.has(lower(k)); }
  delete(k) { this._h.delete(lower(k)); }
  forEach(fn, t) { for (const [k, v] of this._h) fn.call(t, v, k, this); }
  entries() { return this._h.entries(); }
  keys() { return this._h.keys(); }
  values() { return this._h.values(); }
  [Symbol.iterator]() { return this._h.entries(); }
}
class Response {
  constructor(body, init) {
    init = init || {};
    this._body = body == null ? '' : String(body);
    this.status = init.status !== undefined ? init.status : 200;
    this.statusText = init.statusText || '';
    this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
    this.ok = this.status >= 200 && this.status < 300;
    this.url = init.url || '';
    this.redirected = false;
    this.type = 'basic';
    this.bodyUsed = false;
  }
  text() { this.bodyUsed = true; return Promise.resolve(this._body); }
  json() { this.bodyUsed = true; try { return Promise.resolve(JSON.parse(this._body)); } catch (e) { return Promise.reject(e); } }
  arrayBuffer() { this.bodyUsed = true; return Promise.resolve(new TextEncoder().encode(this._body).buffer); }
  blob() { return Promise.resolve({size: this._body.length, type: this.headers.get('content-type') || '', text: () => Promise.resolve(this._body)}); }
  clone() { return new Response(this._body, {status: this.status, statusText: this.statusText, headers: this.headers, url: this.url}); }
  static json(d, i) { return new Response(JSON.stringify(d), Object.assign({headers: {'content-type': 'application/json'}}, i)); }
}
G.__response = (status, statusText, hdrs, body, url) => {
  const h = new Headers();
  for (let i = 0; i + 1 < hdrs.length; i += 2) h.append(hdrs[i], hdrs[i + 1]);
  return new Response(body, {status, statusText, headers: h, url});
};
class Request {
  constructor(url, init) { init = init || {}; this.url = String(url instanceof Request ? url.url : url); this.method = (init.method || 'GET').toUpperCase(); this.headers = new Headers(init.headers); this.body = init.body; this.signal = init.signal || null; }
}
class AbortSignal extends EventTarget {
  constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
  throwIfAborted() { if (this.aborted) throw this.reason; }
  static timeout(ms) { const c = new AbortController(); setTimeout(() => c.abort(new Error('TimeoutError')), ms); return c.signal; }
}
class AbortController {
  constructor() { this.signal = new AbortSignal(); }
  abort(reason) { const s = this.signal; if (s.aborted) return; s.aborted = true; s.reason = reason || new Error('AbortError'); const e = new Event('abort'); s.dispatchEvent(e); if (s.onabort) s.onabort(e); }
}
G.fetch = function (input, init) {
  init = init || {};
  const req = input instanceof Request ? input : new Request(input, init);
  const headers = new Headers(init.headers || req.headers);
  let body = init.body !== undefined ? init.body : req.body;
  if (body instanceof URLSearchParams) { body = body.toString(); if (!headers.has('content-type')) headers.set('content-type', 'application/x-www-form-urlencoded'); }
  else if (body instanceof FormData) { body = body._urlencoded(); if (!headers.has('content-type')) headers.set('content-type', 'application/x-www-form-urlencoded'); }
  else if (body && typeof body === 'object' && !(body instanceof ArrayBuffer)) body = JSON.stringify(body);
  const flat = [];
  headers.forEach((v, k) => flat.push(k, v));
  const signal = init.signal || req.signal;
  if (signal && signal.aborted) return Promise.reject(signal.reason || new Error('AbortError'));
  const p = N.fetch(req.url, init.method || req.method || 'GET', flat, body === undefined ? null : body, init.timeout || 15000);
  if (!signal) return p;
  return new Promise((res, rej) => { signal.addEventListener('abort', () => rej(signal.reason || new Error('AbortError'))); p.then(res, rej); });
};
Object.assign(G, {Headers, Response, Request, AbortController, AbortSignal});

// ------------------------------------------------------------------ URL / encoding helpers
class URLSearchParams {
  constructor(init) {
    this._p = [];
    if (typeof init === 'string') {
      for (const part of init.replace(/^\?/, '').split('&')) {
        if (!part) continue;
        const i = part.indexOf('=');
        const d = (s) => decodeURIComponent(s.replace(/\+/g, ' '));
        this._p.push(i < 0 ? [d(part), ''] : [d(part.slice(0, i)), d(part.slice(i + 1))]);
      }
    } else if (Array.isArray(init)) this._p = init.map(([k, v]) => [String(k), String(v)]);
    else if (init) for (const k of Object.keys(init)) this._p.push([k, String(init[k])]);
  }
  append(k, v) { this._p.push([String(k), String(v)]); }
  set(k, v) { this.delete(k); this.append(k, v); }
  get(k) { const e = this._p.find((x) => x[0] === k); return e ? e[1] : null; }
  getAll(k) { return this._p.filter((x) => x[0] === k).map((x) => x[1]); }
  has(k) { return this._p.some((x) => x[0] === k); }
  delete(k) { this._p = this._p.filter((x) => x[0] !== k); }
  forEach(fn) { for (const [k, v] of this._p) fn(v, k, this); }
  entries() { return this._p[Symbol.iterator](); }
  keys() { return this._p.map((x) => x[0])[Symbol.iterator](); }
  values() { return this._p.map((x) => x[1])[Symbol.iterator](); }
  [Symbol.iterator]() { return this._p[Symbol.iterator](); }
  get size() { return this._p.length; }
  toString() { return this._p.map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v)).join('&'); }
}
class URL {
  constructor(u, base) {
    u = String(u);
    if (!/^[a-z][a-z0-9+.-]*:/i.test(u)) {
      const b = base ? String(base) : 'http://iotdash.local/' + N.nav('url');
      const bm = b.match(/^([a-z]+:\/\/[^/]+)(\/[^?#]*)?/i);
      const origin = bm ? bm[1] : 'http://iotdash.local';
      const dir = bm && bm[2] ? bm[2].replace(/[^/]*$/, '') : '/';
      u = u.startsWith('/') ? origin + u : origin + dir + u;
    }
    const m = u.match(/^([a-z][a-z0-9+.-]*:)(?:\/\/(?:([^:@/]*)(?::([^@/]*))?@)?([^:/?#]*)(?::(\d+))?)?([^?#]*)(\?[^#]*)?(#.*)?$/i);
    if (!m) throw new TypeError('Invalid URL: ' + u);
    this.protocol = m[1]; this.username = m[2] || ''; this.password = m[3] || ''; this.hostname = m[4] || '';
    this.port = m[5] || ''; this.pathname = m[6] || '/'; this.hash = m[8] || '';
    this.searchParams = new URLSearchParams(m[7] || '');
  }
  get search() { const s = this.searchParams.toString(); return s ? '?' + s : ''; }
  set search(v) { this.searchParams = new URLSearchParams(String(v)); }
  get host() { return this.hostname + (this.port ? ':' + this.port : ''); }
  get origin() { return this.protocol + '//' + this.host; }
  get href() { return this.origin + this.pathname + this.search + this.hash; }
  toString() { return this.href; }
  toJSON() { return this.href; }
  static canParse(u, b) { try { new URL(u, b); return true; } catch (_) { return false; } }
}
class FormData {
  constructor(form) {
    this._p = [];
    if (form) for (const e of form.elements) {
      const name = e.getAttribute('name');
      if (!name || e.disabled) continue;
      if ((e.type === 'checkbox' || e.type === 'radio') && !e.checked) continue;
      if (e.type === 'submit' || e.type === 'button' || e.type === 'reset') continue;
      this._p.push([name, e.value]);
    }
  }
  append(k, v) { this._p.push([String(k), String(v)]); }
  set(k, v) { this.delete(k); this.append(k, v); }
  get(k) { const e = this._p.find((x) => x[0] === k); return e ? e[1] : null; }
  getAll(k) { return this._p.filter((x) => x[0] === k).map((x) => x[1]); }
  has(k) { return this._p.some((x) => x[0] === k); }
  delete(k) { this._p = this._p.filter((x) => x[0] !== k); }
  entries() { return this._p[Symbol.iterator](); }
  keys() { return this._p.map((x) => x[0])[Symbol.iterator](); }
  values() { return this._p.map((x) => x[1])[Symbol.iterator](); }
  forEach(fn) { for (const [k, v] of this._p) fn(v, k, this); }
  [Symbol.iterator]() { return this._p[Symbol.iterator](); }
  _urlencoded() { return new URLSearchParams(this._p).toString(); }
}
class TextEncoder {
  get encoding() { return 'utf-8'; }
  encode(s = '') {
    s = String(s); const out = [];
    for (const ch of s) {
      const c = ch.codePointAt(0);
      if (c < 0x80) out.push(c);
      else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
      else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
      else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
    }
    return new Uint8Array(out);
  }
}
class TextDecoder {
  get encoding() { return 'utf-8'; }
  decode(b) {
    if (!b) return '';
    const a = b instanceof ArrayBuffer ? new Uint8Array(b) : new Uint8Array(b.buffer || b, b.byteOffset || 0, b.byteLength);
    let s = '', i = 0;
    while (i < a.length) {
      const c = a[i++];
      if (c < 0x80) s += String.fromCharCode(c);
      else if (c < 0xe0) s += String.fromCharCode(((c & 31) << 6) | (a[i++] & 63));
      else if (c < 0xf0) s += String.fromCharCode(((c & 15) << 12) | ((a[i++] & 63) << 6) | (a[i++] & 63));
      else s += String.fromCodePoint(((c & 7) << 18) | ((a[i++] & 63) << 12) | ((a[i++] & 63) << 6) | (a[i++] & 63));
    }
    return s;
  }
}
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
G.btoa = (s) => {
  s = String(s); let o = '';
  for (let i = 0; i < s.length; i += 3) {
    const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
    const n = (a << 16) | ((b || 0) << 8) | (c || 0);
    o += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + (isNaN(b) ? '=' : B64[(n >> 6) & 63]) + (isNaN(c) ? '=' : B64[n & 63]);
  }
  return o;
};
G.atob = (s) => {
  s = String(s).replace(/[^A-Za-z0-9+/]/g, ''); let o = '';
  for (let i = 0; i < s.length; i += 4) {
    const n = (B64.indexOf(s[i]) << 18) | (B64.indexOf(s[i + 1]) << 12) | ((B64.indexOf(s[i + 2]) & 63) << 6) | (B64.indexOf(s[i + 3]) & 63);
    o += String.fromCharCode((n >> 16) & 255);
    if (s[i + 2] !== undefined) o += String.fromCharCode((n >> 8) & 255);
    if (s[i + 3] !== undefined) o += String.fromCharCode(n & 255);
  }
  return o;
};
class DOMParser {
  parseFromString(s) {
    const f = N.parseHTML(String(s));
    return {body: f, documentElement: f, querySelector: (q) => f.querySelector(q), querySelectorAll: (q) => f.querySelectorAll(q),
      getElementById: (id) => f.querySelector('#' + id)};
  }
}
class ImageData {
  constructor(a, w, h) {
    if (typeof a === 'number') { h = w; w = a; a = new Uint8ClampedArray(w * h * 4); }
    else if (a instanceof ArrayBuffer) a = new Uint8ClampedArray(a);
    this.data = a; this.width = w; this.height = h || (a.length / 4 / w); this.colorSpace = 'srgb';
  }
}
G.ImageData = ImageData;
class MutationObserver { constructor(cb) { this._cb = cb; } observe() {} disconnect() {} takeRecords() { return []; } }
class ResizeObserver {
  constructor(cb) { this._cb = cb; this._els = []; }
  observe(el) { this._els.push(el); setTimeout(() => { try { this._cb(this._els.map((e) => { const r = e.getBoundingClientRect(); return {target: e, contentRect: r, borderBoxSize: [{inlineSize: r.width, blockSize: r.height}]}; }), this); } catch (e) { reportError(e); } }, 0); }
  unobserve(el) { this._els = this._els.filter((e) => e !== el); }
  disconnect() { this._els = []; }
}
class IntersectionObserver {
  constructor(cb, opts) { this._cb = cb; }
  observe(el) { setTimeout(() => { try { const r = el.getBoundingClientRect(); this._cb([{target: el, isIntersecting: true, intersectionRatio: 1, boundingClientRect: r}], this); } catch (e) { reportError(e); } }, 0); }
  unobserve() {} disconnect() {} takeRecords() { return []; }
}
Object.assign(G, {URL, URLSearchParams, FormData, TextEncoder, TextDecoder, DOMParser, MutationObserver, ResizeObserver,
  IntersectionObserver, CSS: {supports: () => true, escape: (s) => String(s).replace(/([^\w-])/g, '\\$1')}});
})(globalThis);
)BOOTSTRAP_JS";
