#pragma once
// Interned strings. Tag names, attribute names, class names and ids are compared as small
// integers. Known HTML tags get fixed ids (the Tag enum) so the engine can switch on them.

#include "rt_base.h"

namespace rt {

#define RT_TAGS(X)                                                                                  \
  X(html) X(head) X(body) X(title) X(meta) X(link) X(style) X(script) X(template_, "template")    \
  X(div) X(span) X(p) X(a) X(img) X(br) X(hr) X(wbr) X(h1) X(h2) X(h3) X(h4) X(h5) X(h6)          \
  X(ul) X(ol) X(li) X(dl) X(dt) X(dd) X(section) X(header) X(footer) X(nav) X(main) X(article)     \
  X(aside) X(figure) X(figcaption) X(blockquote) X(pre) X(code) X(b) X(strong) X(i) X(em) X(u)    \
  X(s) X(small) X(sub) X(sup) X(mark) X(label) X(input) X(button) X(select) X(option)             \
  X(optgroup) X(textarea) X(form) X(fieldset) X(legend) X(table) X(caption) X(thead) X(tbody)     \
  X(tfoot) X(tr) X(td) X(th) X(col) X(colgroup) X(canvas) X(svg) X(g) X(path) X(circle)           \
  X(ellipse) X(line) X(polyline) X(polygon) X(rect) X(text) X(defs) X(use) X(progress) X(meter)   \
  X(dialog) X(details) X(summary) X(output) X(time) X(address) X(center) X(iframe) X(video)       \
  X(audio) X(source) X(picture) X(noscript) X(slot) X(include) X(datalist) X(abbr) X(cite)        \
  X(kbd) X(var) X(samp) X(q) X(del) X(ins) X(menu) X(search) X(base) X(area) X(embed) X(param)    \
  X(track) X(object) X(map)

enum Tag : uint16_t {
  TAG_NONE = 0,
#define RT_TAG_ENUM(name, ...) TAG_##name,
  RT_TAGS(RT_TAG_ENUM)
#undef RT_TAG_ENUM
  TAG__COUNT,
  TAG_PSEUDO_BEFORE = 0xFFF0,  // generated ::before / ::after boxes
  TAG_PSEUDO_AFTER = 0xFFF1,
};

using Atom = uint32_t;  // 0 = none

Atom intern(const char *s, size_t n);
inline Atom intern(const std::string &s) { return intern(s.data(), s.size()); }
inline Atom intern(const char *s) { return intern(s, strlen(s)); }
Atom internLower(const char *s, size_t n);  // case-folded (tag/attribute names)
inline Atom internLower(const std::string &s) { return internLower(s.data(), s.size()); }
const std::string &atomStr(Atom a);
Atom findAtom(const char *s, size_t n);  // 0 if never interned; safe while painting (intern is not)

// Known tags are atoms 1..TAG__COUNT-1, so a tag atom below TAG__COUNT *is* its Tag.
inline Tag tagOf(Atom a) { return a < TAG__COUNT ? (Tag)a : TAG_NONE; }
void initAtoms();

// Frequently used attribute names (interned at startup).
struct Attrs {
  Atom id, class_, style, href, src, type, value, name, checked, disabled, selected, placeholder,
      min, max, step, width, height, alt, multiple, readonly, required, maxlength, rows, cols,
      colspan, rowspan, open, hidden, for_, tabindex, d, cx, cy, r, rx, ry, x, y, x1, y1, x2, y2,
      points, fill, stroke, stroke_width, viewBox, transform, opacity, fill_opacity,
      stroke_opacity, stroke_linecap, stroke_linejoin, fill_rule, stroke_dasharray, label,
      contenteditable, autofocus, inputmode, pattern, accept, action, method, page, transition,
      onclick, content, charset, rel, lang, dir, data_page, target, download, size, low, high,
      optimum, list, form, xmlns, preserveAspectRatio, clip_path, font_size, text_anchor,
      font_weight, font_family, dominant_baseline, stroke_miterlimit, stroke_dashoffset,
      gradientTransform, offset, stop_color, stop_opacity, autocomplete, spellcheck, wrap;
};
extern Attrs A;

}  // namespace rt
