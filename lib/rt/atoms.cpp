#include "atoms.h"

namespace rt {

Attrs A;

namespace {
struct Table {
  std::unordered_map<std::string, Atom> map;
  std::vector<std::string> strs{""};
};
Table &table() {
  static Table *t = new Table();
  return *t;
}
}  // namespace

Atom intern(const char *s, size_t n) {
  Table &t = table();
  std::string k(s, n);
  auto it = t.map.find(k);
  if (it != t.map.end()) return it->second;
  Atom a = (Atom)t.strs.size();
  t.strs.push_back(k);
  t.map.emplace(std::move(k), a);
  return a;
}

Atom internLower(const char *s, size_t n) {
  char buf[64];
  if (n < sizeof(buf)) {
    for (size_t i = 0; i < n; i++) buf[i] = lowerAscii(s[i]);
    return intern(buf, n);
  }
  std::string k(s, n);
  for (auto &c : k) c = lowerAscii(c);
  return intern(k);
}

Atom findAtom(const char *s, size_t n) {
  Table &t = table();
  auto it = t.map.find(std::string(s, n));
  return it == t.map.end() ? 0 : it->second;
}

const std::string &atomStr(Atom a) {
  Table &t = table();
  return a < t.strs.size() ? t.strs[a] : t.strs[0];
}

void initAtoms() {
  static bool done = false;
  if (done) return;
  done = true;
  // Tags first so their atom equals their enum value.
#define RT_TAG_INIT(name, ...)                                \
  {                                                           \
    std::string n = #name;                                    \
    if (!n.empty() && n.back() == '_') n.pop_back();          \
    Atom a = intern(n);                                       \
    (void)a;                                                  \
  }
  RT_TAGS(RT_TAG_INIT)
#undef RT_TAG_INIT
  auto I = [](const char *s) { return intern(s); };
  A.id = I("id"); A.class_ = I("class"); A.style = I("style"); A.href = I("href"); A.src = I("src");
  A.type = I("type"); A.value = I("value"); A.name = I("name"); A.checked = I("checked");
  A.disabled = I("disabled"); A.selected = I("selected"); A.placeholder = I("placeholder");
  A.min = I("min"); A.max = I("max"); A.step = I("step"); A.width = I("width"); A.height = I("height");
  A.alt = I("alt"); A.multiple = I("multiple"); A.readonly = I("readonly"); A.required = I("required");
  A.maxlength = I("maxlength"); A.rows = I("rows"); A.cols = I("cols"); A.colspan = I("colspan");
  A.rowspan = I("rowspan"); A.open = I("open"); A.hidden = I("hidden"); A.for_ = I("for");
  A.tabindex = I("tabindex"); A.d = I("d"); A.cx = I("cx"); A.cy = I("cy"); A.r = I("r");
  A.rx = I("rx"); A.ry = I("ry"); A.x = I("x"); A.y = I("y"); A.x1 = I("x1"); A.y1 = I("y1");
  A.x2 = I("x2"); A.y2 = I("y2"); A.points = I("points"); A.fill = I("fill"); A.stroke = I("stroke");
  A.stroke_width = I("stroke-width"); A.viewBox = I("viewbox"); A.transform = I("transform");
  A.opacity = I("opacity"); A.fill_opacity = I("fill-opacity"); A.stroke_opacity = I("stroke-opacity");
  A.stroke_linecap = I("stroke-linecap"); A.stroke_linejoin = I("stroke-linejoin");
  A.fill_rule = I("fill-rule"); A.stroke_dasharray = I("stroke-dasharray"); A.label = I("label");
  A.contenteditable = I("contenteditable"); A.autofocus = I("autofocus"); A.inputmode = I("inputmode");
  A.pattern = I("pattern"); A.accept = I("accept"); A.action = I("action"); A.method = I("method");
  A.page = I("page"); A.transition = I("transition"); A.onclick = I("onclick"); A.content = I("content");
  A.charset = I("charset"); A.rel = I("rel"); A.lang = I("lang"); A.dir = I("dir");
  A.data_page = I("data-page"); A.target = I("target"); A.download = I("download"); A.size = I("size");
  A.low = I("low"); A.high = I("high"); A.optimum = I("optimum"); A.list = I("list"); A.form = I("form");
  A.xmlns = I("xmlns"); A.preserveAspectRatio = I("preserveaspectratio"); A.clip_path = I("clip-path");
  A.font_size = I("font-size"); A.text_anchor = I("text-anchor"); A.font_weight = I("font-weight");
  A.font_family = I("font-family"); A.dominant_baseline = I("dominant-baseline");
  A.stroke_miterlimit = I("stroke-miterlimit"); A.stroke_dashoffset = I("stroke-dashoffset");
  A.gradientTransform = I("gradienttransform"); A.offset = I("offset"); A.stop_color = I("stop-color");
  A.stop_opacity = I("stop-opacity"); A.autocomplete = I("autocomplete"); A.spellcheck = I("spellcheck");
  A.wrap = I("wrap");
}

}  // namespace rt
