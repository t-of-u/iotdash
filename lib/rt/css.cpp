// CSS tokenizer, value parsers, property table, shorthands.
#include "css.h"

#include "style.h"

namespace rt {

// ============================================================ tokenizer

static bool isNameStart(unsigned char c) { return isalpha(c) || c == '_' || c >= 0x80; }
static bool isNameChar(unsigned char c) { return isalnum(c) || c == '_' || c == '-' || c >= 0x80; }

void cssTokenize(const char *s, size_t n, std::vector<Tok> &out) {
  size_t i = 0;
  auto readName = [&](size_t &j) {
    std::string r;
    while (j < n) {
      unsigned char c = s[j];
      if (isNameChar(c)) {
        r += (char)c;
        j++;
      } else if (c == '\\' && j + 1 < n) {
        r += s[j + 1];
        j += 2;
      } else {
        break;
      }
    }
    return r;
  };
  while (i < n) {
    unsigned char c = s[i];
    if (isSpace(c)) {
      while (i < n && isSpace(s[i])) i++;
      if (out.empty() || out.back().t != TK_WS) out.push_back({TK_WS});
      continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '*') {
      const char *e = strstr(s + i + 2, "*/");
      i = e ? (size_t)(e - s) + 2 : n;
      continue;
    }
    if (c == '"' || c == '\'') {
      char q = c;
      i++;
      Tok t{TK_STRING};
      while (i < n && s[i] != q) {
        if (s[i] == '\\' && i + 1 < n) {
          i++;
          if (isxdigit((unsigned char)s[i])) {
            size_t st = i;
            while (i < n && i - st < 6 && isxdigit((unsigned char)s[i])) i++;
            utf8Append(t.s, (uint32_t)strtoul(std::string(s + st, i - st).c_str(), nullptr, 16));
            if (i < n && s[i] == ' ') i++;
            continue;
          }
        }
        t.s += s[i++];
      }
      i++;
      out.push_back(t);
      continue;
    }
    // number?
    bool numStart = isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1])) ||
                    ((c == '-' || c == '+') && i + 1 < n &&
                     (isdigit((unsigned char)s[i + 1]) || (s[i + 1] == '.' && i + 2 < n && isdigit((unsigned char)s[i + 2]))));
    if (numStart) {
      size_t st = i;
      if (s[i] == '-' || s[i] == '+') i++;
      bool isInt = true;
      while (i < n && isdigit((unsigned char)s[i])) i++;
      if (i < n && s[i] == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1])) {
        isInt = false;
        i++;
        while (i < n && isdigit((unsigned char)s[i])) i++;
      }
      if (i < n && (s[i] == 'e' || s[i] == 'E') && i + 1 < n &&
          (isdigit((unsigned char)s[i + 1]) || ((s[i + 1] == '-' || s[i + 1] == '+') && i + 2 < n && isdigit((unsigned char)s[i + 2])))) {
        isInt = false;
        i += 2;
        while (i < n && isdigit((unsigned char)s[i])) i++;
      }
      Tok t;
      t.n = strtof(std::string(s + st, i - st).c_str(), nullptr);
      t.isInt = isInt;
      if (i < n && s[i] == '%') {
        t.t = TK_PCT;
        i++;
      } else if (i < n && isNameStart((unsigned char)s[i])) {
        t.t = TK_DIM;
        t.s = toLower(readName(i));
      } else {
        t.t = TK_NUM;
      }
      out.push_back(t);
      continue;
    }
    if (isNameStart(c) || (c == '-' && i + 1 < n && (isNameStart((unsigned char)s[i + 1]) || s[i + 1] == '-')) || c == '\\') {
      std::string name = readName(i);
      if (i < n && s[i] == '(') {
        i++;
        std::string lname = toLower(name);
        if (lname == "url") {
          // url( unquoted ) or url("...")
          while (i < n && isSpace(s[i])) i++;
          Tok t{TK_URL};
          if (i < n && (s[i] == '"' || s[i] == '\'')) {
            char q = s[i++];
            while (i < n && s[i] != q) t.s += s[i++];
            i++;
            while (i < n && s[i] != ')') i++;
            i++;
          } else {
            while (i < n && s[i] != ')') t.s += s[i++];
            i++;
            t.s = trim(t.s);
          }
          out.push_back(t);
          continue;
        }
        Tok t{TK_FUNC};
        t.s = lname;
        out.push_back(t);
        continue;
      }
      Tok t{TK_IDENT};
      t.s = name;
      out.push_back(t);
      continue;
    }
    if (c == '@') {
      i++;
      Tok t{TK_AT};
      t.s = toLower(readName(i));
      out.push_back(t);
      continue;
    }
    if (c == '#') {
      i++;
      Tok t{TK_HASH};
      t.s = readName(i);
      out.push_back(t);
      continue;
    }
    i++;
    switch (c) {
      case ':': out.push_back({TK_COLON}); break;
      case ';': out.push_back({TK_SEMI}); break;
      case ',': out.push_back({TK_COMMA}); break;
      case '[': out.push_back({TK_LBRACK}); break;
      case ']': out.push_back({TK_RBRACK}); break;
      case '(': out.push_back({TK_LPAREN}); break;
      case ')': out.push_back({TK_RPAREN}); break;
      case '{': out.push_back({TK_LBRACE}); break;
      case '}': out.push_back({TK_RBRACE}); break;
      default: {
        Tok t{TK_DELIM};
        t.c = (char)c;
        out.push_back(t);
      }
    }
  }
}

static std::string numStr(float v) {
  char b[32];
  if (std::fabs(v - std::round(v)) < 1e-4f) snprintf(b, sizeof b, "%d", (int)std::lround(v));
  else snprintf(b, sizeof b, "%g", v);
  return b;
}

std::string tokensToString(const std::vector<Tok> &t, size_t a, size_t b) {
  std::string r;
  int depth = 0;
  for (size_t i = a; i < b && i < t.size(); i++) {
    const Tok &k = t[i];
    switch (k.t) {
      case TK_IDENT: r += k.s; break;
      case TK_FUNC: r += k.s + "("; depth++; break;
      case TK_AT: r += "@" + k.s; break;
      case TK_HASH: r += "#" + k.s; break;
      case TK_STRING: r += "\"" + k.s + "\""; break;
      case TK_NUM: r += numStr(k.n); break;
      case TK_PCT: r += numStr(k.n) + "%"; break;
      case TK_DIM: r += numStr(k.n) + k.s; break;
      case TK_WS: r += " "; break;
      case TK_COLON: r += ":"; break;
      case TK_SEMI: r += ";"; break;
      case TK_COMMA: r += ","; break;
      case TK_LBRACK: r += "["; break;
      case TK_RBRACK: r += "]"; break;
      case TK_LPAREN: r += "("; depth++; break;
      case TK_RPAREN: r += ")"; depth--; break;
      case TK_LBRACE: r += "{"; break;
      case TK_RBRACE: r += "}"; break;
      case TK_DELIM: r += k.c; break;
      case TK_URL: r += "url(" + k.s + ")"; break;
      default: break;
    }
  }
  (void)depth;
  return trim(r);
}

// ============================================================ token stream helper

namespace {
struct TS {
  std::vector<Tok> t;  // whitespace removed
  size_t i = 0;
  explicit TS(const std::vector<Tok> &src) {
    for (auto &k : src)
      if (k.t != TK_WS) t.push_back(k);
  }
  bool end() const { return i >= t.size(); }
  const Tok &peek(size_t k = 0) const {
    static Tok eof;
    return i + k < t.size() ? t[i + k] : eof;
  }
  const Tok &next() {
    static Tok eof;
    return i < t.size() ? t[i++] : eof;
  }
  bool ident(const char *s) {
    if (peek().t == TK_IDENT && iequals(peek().s, s)) {
      i++;
      return true;
    }
    return false;
  }
  bool comma() {
    if (peek().t == TK_COMMA) {
      i++;
      return true;
    }
    return false;
  }
  bool delim(char c) {
    if (peek().t == TK_DELIM && peek().c == c) {
      i++;
      return true;
    }
    return false;
  }
  // Skip a function's arguments (after TK_FUNC) up to the matching ')'.
  void skipArgs() {
    int d = 1;
    while (!end() && d > 0) {
      TokType k = next().t;
      if (k == TK_FUNC || k == TK_LPAREN) d++;
      else if (k == TK_RPAREN) d--;
    }
  }
  bool closeParen() {
    if (peek().t == TK_RPAREN) {
      i++;
      return true;
    }
    return false;
  }
};
}  // namespace

// ============================================================ colours

struct NamedColor {
  const char *n;
  uint32_t rgb;
};
static const NamedColor kNamed[] = {
    {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff}, {"aquamarine", 0x7fffd4},
    {"azure", 0xf0ffff}, {"beige", 0xf5f5dc}, {"bisque", 0xffe4c4}, {"black", 0x000000},
    {"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff}, {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a},
    {"burlywood", 0xdeb887}, {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00}, {"chocolate", 0xd2691e},
    {"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed}, {"cornsilk", 0xfff8dc}, {"crimson", 0xdc143c},
    {"cyan", 0x00ffff}, {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b}, {"darkgoldenrod", 0xb8860b},
    {"darkgray", 0xa9a9a9}, {"darkgreen", 0x006400}, {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b},
    {"darkmagenta", 0x8b008b}, {"darkolivegreen", 0x556b2f}, {"darkorange", 0xff8c00}, {"darkorchid", 0x9932cc},
    {"darkred", 0x8b0000}, {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f}, {"darkslateblue", 0x483d8b},
    {"darkslategray", 0x2f4f4f}, {"darkslategrey", 0x2f4f4f}, {"darkturquoise", 0x00ced1}, {"darkviolet", 0x9400d3},
    {"deeppink", 0xff1493}, {"deepskyblue", 0x00bfff}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1e90ff}, {"firebrick", 0xb22222}, {"floralwhite", 0xfffaf0}, {"forestgreen", 0x228b22},
    {"fuchsia", 0xff00ff}, {"gainsboro", 0xdcdcdc}, {"ghostwhite", 0xf8f8ff}, {"gold", 0xffd700},
    {"goldenrod", 0xdaa520}, {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xadff2f},
    {"grey", 0x808080}, {"honeydew", 0xf0fff0}, {"hotpink", 0xff69b4}, {"indianred", 0xcd5c5c},
    {"indigo", 0x4b0082}, {"ivory", 0xfffff0}, {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa},
    {"lavenderblush", 0xfff0f5}, {"lawngreen", 0x7cfc00}, {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6},
    {"lightcoral", 0xf08080}, {"lightcyan", 0xe0ffff}, {"lightgoldenrodyellow", 0xfafad2}, {"lightgray", 0xd3d3d3},
    {"lightgreen", 0x90ee90}, {"lightgrey", 0xd3d3d3}, {"lightpink", 0xffb6c1}, {"lightsalmon", 0xffa07a},
    {"lightseagreen", 0x20b2aa}, {"lightskyblue", 0x87cefa}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xb0c4de}, {"lightyellow", 0xffffe0}, {"lime", 0x00ff00}, {"limegreen", 0x32cd32},
    {"linen", 0xfaf0e6}, {"magenta", 0xff00ff}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66cdaa},
    {"mediumblue", 0x0000cd}, {"mediumorchid", 0xba55d3}, {"mediumpurple", 0x9370db}, {"mediumseagreen", 0x3cb371},
    {"mediumslateblue", 0x7b68ee}, {"mediumspringgreen", 0x00fa9a}, {"mediumturquoise", 0x48d1cc}, {"mediumvioletred", 0xc71585},
    {"midnightblue", 0x191970}, {"mintcream", 0xf5fffa}, {"mistyrose", 0xffe4e1}, {"moccasin", 0xffe4b5},
    {"navajowhite", 0xffdead}, {"navy", 0x000080}, {"oldlace", 0xfdf5e6}, {"olive", 0x808000},
    {"olivedrab", 0x6b8e23}, {"orange", 0xffa500}, {"orangered", 0xff4500}, {"orchid", 0xda70d6},
    {"palegoldenrod", 0xeee8aa}, {"palegreen", 0x98fb98}, {"paleturquoise", 0xafeeee}, {"palevioletred", 0xdb7093},
    {"papayawhip", 0xffefd5}, {"peachpuff", 0xffdab9}, {"peru", 0xcd853f}, {"pink", 0xffc0cb},
    {"plum", 0xdda0dd}, {"powderblue", 0xb0e0e6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399},
    {"red", 0xff0000}, {"rosybrown", 0xbc8f8f}, {"royalblue", 0x4169e1}, {"saddlebrown", 0x8b4513},
    {"salmon", 0xfa8072}, {"sandybrown", 0xf4a460}, {"seagreen", 0x2e8b57}, {"seashell", 0xfff5ee},
    {"sienna", 0xa0522d}, {"silver", 0xc0c0c0}, {"skyblue", 0x87ceeb}, {"slateblue", 0x6a5acd},
    {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xfffafa}, {"springgreen", 0x00ff7f},
    {"steelblue", 0x4682b4}, {"tan", 0xd2b48c}, {"teal", 0x008080}, {"thistle", 0xd8bfd8},
    {"tomato", 0xff6347}, {"turquoise", 0x40e0d0}, {"violet", 0xee82ee}, {"wheat", 0xf5deb3},
    {"white", 0xffffff}, {"whitesmoke", 0xf5f5f5}, {"yellow", 0xffff00}, {"yellowgreen", 0x9acd32},
    // system colours (light theme)
    {"canvas", 0xffffff}, {"canvastext", 0x000000}, {"buttonface", 0xefefef}, {"buttontext", 0x000000},
    {"field", 0xffffff}, {"fieldtext", 0x000000}, {"highlight", 0x3390ff}, {"highlighttext", 0xffffff},
    {"graytext", 0x808080}, {"linktext", 0x0000ee}, {"accentcolor", 0x2563eb}, {"accentcolortext", 0xffffff},
    {"buttonborder", 0x767676},
};

static float clamp255(float v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static float hue2rgb(float p, float q, float t) {
  if (t < 0) t += 1;
  if (t > 1) t -= 1;
  if (t < 1.f / 6) return p + (q - p) * 6 * t;
  if (t < 0.5f) return q;
  if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
  return p;
}

static Color hsl(float h, float s, float l, float a) {
  h = std::fmod(h, 360.f);
  if (h < 0) h += 360;
  h /= 360;
  float r, g, b;
  if (s == 0) {
    r = g = b = l;
  } else {
    float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
    float p = 2 * l - q;
    r = hue2rgb(p, q, h + 1.f / 3);
    g = hue2rgb(p, q, h);
    b = hue2rgb(p, q, h - 1.f / 3);
  }
  return {(uint8_t)std::lround(r * 255), (uint8_t)std::lround(g * 255), (uint8_t)std::lround(b * 255),
          (uint8_t)std::lround(clampf(a, 0, 1) * 255)};
}

static float angleDeg(const Tok &k, bool &ok) {
  ok = true;
  if (k.t == TK_NUM) return k.n;  // unitless (hsl hue / 0)
  if (k.t == TK_DIM) {
    if (k.s == "deg") return k.n;
    if (k.s == "rad") return k.n * 57.2957795f;
    if (k.s == "grad") return k.n * 0.9f;
    if (k.s == "turn") return k.n * 360;
  }
  ok = false;
  return 0;
}

static bool parseColorTS(TS &ts, ColorSpec &out) {
  const Tok &k = ts.peek();
  if (k.t == TK_IDENT) {
    std::string n = toLower(k.s);
    if (n == "transparent") {
      ts.i++;
      out = {kTransparent, false};
      return true;
    }
    if (n == "currentcolor") {
      ts.i++;
      out = {kBlack, true};
      return true;
    }
    for (auto &c : kNamed) {
      if (n == c.n) {
        ts.i++;
        out = {{(uint8_t)(c.rgb >> 16), (uint8_t)(c.rgb >> 8), (uint8_t)c.rgb, 255}, false};
        return true;
      }
    }
    return false;
  }
  if (k.t == TK_HASH) {
    const std::string &h = k.s;
    for (char c : h)
      if (!isxdigit((unsigned char)c)) return false;
    auto hx = [&](int i, int len) { return (uint8_t)strtoul(h.substr(i, len).c_str(), nullptr, 16); };
    Color c{0, 0, 0, 255};
    if (h.size() == 3 || h.size() == 4) {
      c.r = hx(0, 1) * 17;
      c.g = hx(1, 1) * 17;
      c.b = hx(2, 1) * 17;
      if (h.size() == 4) c.a = hx(3, 1) * 17;
    } else if (h.size() == 6 || h.size() == 8) {
      c.r = hx(0, 2);
      c.g = hx(2, 2);
      c.b = hx(4, 2);
      if (h.size() == 8) c.a = hx(6, 2);
    } else {
      return false;
    }
    ts.i++;
    out = {c, false};
    return true;
  }
  if (k.t == TK_FUNC) {
    std::string f = k.s;
    if (f == "rgb" || f == "rgba" || f == "hsl" || f == "hsla") {
      size_t save = ts.i;
      ts.i++;
      float v[4] = {0, 0, 0, 1};
      int cnt = 0;
      while (!ts.end() && !ts.closeParen()) {
        const Tok &a = ts.next();
        if (a.t == TK_COMMA || (a.t == TK_DELIM && a.c == '/')) continue;
        if (cnt >= 4) {
          ts.i = save;
          return false;
        }
        bool isHue = (f[0] == 'h' && cnt == 0);
        if (isHue) {
          bool ok;
          v[cnt++] = angleDeg(a, ok);
          if (!ok) {
            ts.i = save;
            return false;
          }
        } else if (a.t == TK_NUM) {
          v[cnt++] = a.n;
        } else if (a.t == TK_PCT) {
          if (cnt == 3) v[cnt++] = a.n / 100;
          else if (f[0] == 'r') v[cnt++] = a.n * 2.55f;
          else v[cnt++] = a.n / 100;
        } else if (a.t == TK_IDENT && iequals(a.s, "none")) {
          v[cnt++] = 0;
        } else {
          ts.i = save;
          return false;
        }
      }
      if (cnt < 3) {
        ts.i = save;
        return false;
      }
      if (f[0] == 'r') {
        out = {{(uint8_t)std::lround(clamp255(v[0])), (uint8_t)std::lround(clamp255(v[1])),
                (uint8_t)std::lround(clamp255(v[2])), (uint8_t)std::lround(clampf(v[3], 0, 1) * 255)},
               false};
      } else {
        float s = v[1] > 1 ? v[1] / 100 : v[1];
        float l = v[2] > 1 ? v[2] / 100 : v[2];
        out = {hsl(v[0], clampf(s, 0, 1), clampf(l, 0, 1), v[3]), false};
      }
      return true;
    }
    if (f == "color-mix") {
      // color-mix(in srgb, A p%, B)
      size_t save = ts.i;
      ts.i++;
      while (!ts.end() && ts.peek().t != TK_COMMA) ts.i++;
      ts.comma();
      ColorSpec a, b;
      float pa = -1, pb = -1;
      if (!parseColorTS(ts, a)) {
        ts.i = save;
        return false;
      }
      if (ts.peek().t == TK_PCT) pa = ts.next().n / 100;
      ts.comma();
      if (!parseColorTS(ts, b)) {
        ts.i = save;
        return false;
      }
      if (ts.peek().t == TK_PCT) pb = ts.next().n / 100;
      ts.closeParen();
      if (pa < 0 && pb < 0) pa = 0.5f;
      if (pa < 0) pa = 1 - pb;
      out = {lerpColor(b.c, a.c, pa), false};
      return true;
    }
  }
  return false;
}

bool parseColorStr(const std::string &s, Color &out) {
  std::vector<Tok> t;
  cssTokenize(s.data(), s.size(), t);
  TS ts(t);
  ColorSpec c;
  if (!parseColorTS(ts, c) || !ts.end()) return false;
  out = c.c;
  return true;
}

// ============================================================ lengths / calc

enum LenFlags : uint32_t {
  LF_AUTO = 1, LF_NONE = 2, LF_NEG = 4, LF_PCT = 8, LF_CONTENT = 16, LF_NORMAL = 32, LF_UNITLESS = 64,
  LF_CONTENT_KW = 128,
};

static bool unitToSpec(float v, const std::string &u, LenSpec &l) {
  if (u == "px") l.px += v;
  else if (u == "em") l.em += v;
  else if (u == "rem") l.rem += v;
  else if (u == "vw") l.vw += v;
  else if (u == "vh" || u == "dvh" || u == "svh" || u == "lvh") l.vh += v;
  else if (u == "vmin") l.vh += v;  // landscape panel: vmin == vh
  else if (u == "vmax") l.vw += v;
  else if (u == "ch") l.ch += v;
  else if (u == "ex") l.em += v * 0.5f;
  else if (u == "pt") l.px += v * 4.f / 3.f;
  else if (u == "pc") l.px += v * 16;
  else if (u == "in") l.px += v * 96;
  else if (u == "cm") l.px += v * 37.795f;
  else if (u == "mm") l.px += v * 3.7795f;
  else if (u == "q") l.px += v * 0.9449f;
  else return false;
  return true;
}

static std::shared_ptr<CalcNode> parseCalcSum(TS &ts);

static std::shared_ptr<CalcNode> parseCalcValue(TS &ts) {
  const Tok &k = ts.peek();
  auto node = std::make_shared<CalcNode>();
  if (k.t == TK_NUM) {
    ts.i++;
    node->isNum = true;
    node->num = k.n;
    return node;
  }
  if (k.t == TK_DIM) {
    ts.i++;
    if (!unitToSpec(k.n, k.s, node->leaf)) return nullptr;
    return node;
  }
  if (k.t == TK_PCT) {
    ts.i++;
    node->leaf.pct = k.n;
    return node;
  }
  if (k.t == TK_LPAREN || (k.t == TK_FUNC && k.s == "calc")) {
    ts.i++;
    auto r = parseCalcSum(ts);
    if (!r || !ts.closeParen()) return nullptr;
    return r;
  }
  if (k.t == TK_FUNC && (k.s == "min" || k.s == "max" || k.s == "clamp")) {
    std::string f = k.s;
    ts.i++;
    node->op = f == "min" ? CalcNode::MIN : f == "max" ? CalcNode::MAX : CalcNode::CLAMP;
    while (true) {
      auto a = parseCalcSum(ts);
      if (!a) return nullptr;
      node->kids.push_back(a);
      if (ts.comma()) continue;
      if (ts.closeParen()) break;
      return nullptr;
    }
    if (node->op == CalcNode::CLAMP && node->kids.size() != 3) return nullptr;
    return node;
  }
  return nullptr;
}

static std::shared_ptr<CalcNode> parseCalcProduct(TS &ts) {
  auto a = parseCalcValue(ts);
  if (!a) return nullptr;
  while (true) {
    if (ts.delim('*')) {
      auto b = parseCalcValue(ts);
      if (!b) return nullptr;
      auto n = std::make_shared<CalcNode>();
      n->op = CalcNode::MUL;
      n->kids = {a, b};
      a = n;
    } else if (ts.delim('/')) {
      auto b = parseCalcValue(ts);
      if (!b) return nullptr;
      auto n = std::make_shared<CalcNode>();
      n->op = CalcNode::DIV;
      n->kids = {a, b};
      a = n;
    } else {
      return a;
    }
  }
}

static std::shared_ptr<CalcNode> parseCalcSum(TS &ts) {
  auto a = parseCalcProduct(ts);
  if (!a) return nullptr;
  while (true) {
    const Tok &k = ts.peek();
    bool plus = k.t == TK_DELIM && k.c == '+';
    bool minus = k.t == TK_DELIM && k.c == '-';
    // "a -2px" tokenizes as a negative dimension: treat as subtraction
    bool negNum = (k.t == TK_DIM || k.t == TK_NUM || k.t == TK_PCT) && k.n < 0;
    if (!plus && !minus && !negNum) return a;
    if (!negNum) ts.i++;
    auto b = parseCalcProduct(ts);
    if (!b) return nullptr;
    auto n = std::make_shared<CalcNode>();
    n->op = (minus) ? CalcNode::SUB : CalcNode::ADD;
    n->kids = {a, b};
    a = n;
  }
}

// Fold a calc tree into a linear LenSpec when it only adds/scales lengths.
static bool foldCalc(const CalcNode &n, LenSpec &out, float &num, bool &isNum) {
  switch (n.op) {
    case CalcNode::LEAF:
      if (n.isNum) {
        isNum = true;
        num = n.num;
      } else {
        isNum = false;
        out = n.leaf;
      }
      return true;
    case CalcNode::ADD:
    case CalcNode::SUB: {
      LenSpec a, b;
      float na, nb;
      bool ia, ib;
      if (!foldCalc(*n.kids[0], a, na, ia) || !foldCalc(*n.kids[1], b, nb, ib)) return false;
      if (ia && ib) {
        isNum = true;
        num = n.op == CalcNode::ADD ? na + nb : na - nb;
        return true;
      }
      if (ia || ib) return false;
      float s = n.op == CalcNode::ADD ? 1.f : -1.f;
      out = a;
      out.px += s * b.px; out.pct += s * b.pct; out.em += s * b.em; out.rem += s * b.rem;
      out.vw += s * b.vw; out.vh += s * b.vh; out.ch += s * b.ch;
      isNum = false;
      return true;
    }
    case CalcNode::MUL:
    case CalcNode::DIV: {
      LenSpec a, b;
      float na, nb;
      bool ia, ib;
      if (!foldCalc(*n.kids[0], a, na, ia) || !foldCalc(*n.kids[1], b, nb, ib)) return false;
      if (ia && ib) {
        isNum = true;
        num = n.op == CalcNode::MUL ? na * nb : (nb != 0 ? na / nb : 0);
        return true;
      }
      if (n.op == CalcNode::DIV && !ib) return false;
      LenSpec l = ia ? b : a;
      float k = ia ? na : nb;
      if (n.op == CalcNode::DIV) k = k != 0 ? 1 / k : 0;
      l.px *= k; l.pct *= k; l.em *= k; l.rem *= k; l.vw *= k; l.vh *= k; l.ch *= k;
      out = l;
      isNum = false;
      return true;
    }
    default:
      return false;
  }
}

static bool parseLenTS(TS &ts, uint32_t flags, LenSpec &out) {
  const Tok &k = ts.peek();
  if (k.t == TK_DIM) {
    LenSpec l;
    if (!unitToSpec(k.n, k.s, l)) return false;
    if (k.n < 0 && !(flags & LF_NEG)) return false;
    ts.i++;
    out = l;
    return true;
  }
  if (k.t == TK_PCT && (flags & LF_PCT)) {
    if (k.n < 0 && !(flags & LF_NEG)) return false;
    ts.i++;
    out = LenSpec::Pct(k.n);
    return true;
  }
  if (k.t == TK_NUM && (k.n == 0 || (flags & LF_UNITLESS))) {
    ts.i++;
    out = LenSpec::Px(k.n);
    return true;
  }
  if (k.t == TK_IDENT) {
    if ((flags & LF_AUTO) && iequals(k.s, "auto")) {
      ts.i++;
      out = LenSpec::Kind(LK_AUTO);
      return true;
    }
    if ((flags & LF_NONE) && iequals(k.s, "none")) {
      ts.i++;
      out = LenSpec::Kind(LK_NONE);
      return true;
    }
    if ((flags & LF_NORMAL) && iequals(k.s, "normal")) {
      ts.i++;
      out = LenSpec::Kind(LK_NORMAL);
      return true;
    }
    if (flags & LF_CONTENT_KW) {
      if (iequals(k.s, "min-content")) { ts.i++; out = LenSpec::Kind(LK_MIN_CONTENT); return true; }
      if (iequals(k.s, "max-content")) { ts.i++; out = LenSpec::Kind(LK_MAX_CONTENT); return true; }
      if (iequals(k.s, "fit-content") || iequals(k.s, "-webkit-fill-available") || iequals(k.s, "stretch")) {
        ts.i++;
        out = LenSpec::Kind(iequals(k.s, "fit-content") ? LK_FIT_CONTENT : LK_AUTO);
        return true;
      }
    }
    if ((flags & LF_CONTENT) && iequals(k.s, "content")) {
      ts.i++;
      out = LenSpec::Kind(LK_CONTENT);
      return true;
    }
    return false;
  }
  if (k.t == TK_FUNC && (k.s == "calc" || k.s == "min" || k.s == "max" || k.s == "clamp")) {
    size_t save = ts.i;
    std::shared_ptr<CalcNode> n;
    if (k.s == "calc") {
      ts.i++;
      n = parseCalcSum(ts);
      if (!n || !ts.closeParen()) {
        ts.i = save;
        return false;
      }
    } else {
      n = parseCalcValue(ts);
      if (!n) {
        ts.i = save;
        return false;
      }
    }
    LenSpec l;
    float num;
    bool isNum;
    if (foldCalc(*n, l, num, isNum)) {
      if (isNum) l = LenSpec::Px(num);  // unitless number in a length context
      out = l;
    } else {
      out = LenSpec();
      out.calc = n;
    }
    return true;
  }
  return false;
}

static float evalCalcNode(const CalcNode &n, const ResolveCtx *rc, float ref) {
  switch (n.op) {
    case CalcNode::LEAF:
      if (n.isNum) return n.num;
      if (rc) return n.leaf.px + n.leaf.em * rc->em + n.leaf.rem * rc->rem + n.leaf.vw * rc->vw * 0.01f +
                     n.leaf.vh * rc->vh * 0.01f + n.leaf.ch * rc->ch + n.leaf.pct * 0.01f * ref;
      return n.leaf.px + n.leaf.pct * 0.01f * ref;
    case CalcNode::ADD: return evalCalcNode(*n.kids[0], rc, ref) + evalCalcNode(*n.kids[1], rc, ref);
    case CalcNode::SUB: return evalCalcNode(*n.kids[0], rc, ref) - evalCalcNode(*n.kids[1], rc, ref);
    case CalcNode::MUL: return evalCalcNode(*n.kids[0], rc, ref) * evalCalcNode(*n.kids[1], rc, ref);
    case CalcNode::DIV: {
      float d = evalCalcNode(*n.kids[1], rc, ref);
      return d != 0 ? evalCalcNode(*n.kids[0], rc, ref) / d : 0;
    }
    case CalcNode::MIN: {
      float m = 1e30f;
      for (auto &k : n.kids) m = std::min(m, evalCalcNode(*k, rc, ref));
      return m;
    }
    case CalcNode::MAX: {
      float m = -1e30f;
      for (auto &k : n.kids) m = std::max(m, evalCalcNode(*k, rc, ref));
      return m;
    }
    case CalcNode::CLAMP: {
      float lo = evalCalcNode(*n.kids[0], rc, ref), v = evalCalcNode(*n.kids[1], rc, ref),
            hi = evalCalcNode(*n.kids[2], rc, ref);
      return std::max(lo, std::min(v, hi));
    }
  }
  return 0;
}

static bool calcHasPct(const CalcNode &n) {
  if (n.op == CalcNode::LEAF) return !n.isNum && n.leaf.pct != 0;
  for (auto &k : n.kids)
    if (calcHasPct(*k)) return true;
  return false;
}

// Replace font/viewport-relative units in a calc tree by px (keeping %).
static std::shared_ptr<CalcNode> resolveCalcUnits(const CalcNode &n, const ResolveCtx &rc) {
  auto r = std::make_shared<CalcNode>(n);
  if (n.op == CalcNode::LEAF) {
    if (!n.isNum) {
      LenSpec l;
      l.px = n.leaf.px + n.leaf.em * rc.em + n.leaf.rem * rc.rem + n.leaf.vw * rc.vw * 0.01f +
             n.leaf.vh * rc.vh * 0.01f + n.leaf.ch * rc.ch;
      l.pct = n.leaf.pct;
      r->leaf = l;
    }
    return r;
  }
  for (auto &k : r->kids) k = resolveCalcUnits(*k, rc);
  return r;
}

float Len::evalCalc(float ref) const { return calc ? evalCalcNode(*calc, nullptr, ref) : px + pct * 0.01f * ref; }

Len computeLen(const LenSpec &s, const ResolveCtx &c) {
  Len l;
  l.kind = s.kind;
  if (s.kind != LK_LEN) return l;
  if (s.calc) {
    if (calcHasPct(*s.calc)) {
      l.calc = resolveCalcUnits(*s.calc, c);
    } else {
      l.px = evalCalcNode(*s.calc, &c, 0);
    }
    return l;
  }
  l.px = s.px + s.em * c.em + s.rem * c.rem + s.vw * c.vw * 0.01f + s.vh * c.vh * 0.01f + s.ch * c.ch;
  l.pct = s.pct;
  return l;
}

// ============================================================ property table

struct PropDef {
  const char *name;
  bool inherited;
};

static const PropDef kProps[] = {
#define RT_PROP_DEF(n) {#n, false},
    RT_PROPS(RT_PROP_DEF)
#undef RT_PROP_DEF
};

static std::string cssName(const char *id) {
  std::string s = id;
  if (!s.empty() && s.back() == '_') s.pop_back();
  for (auto &c : s)
    if (c == '_') c = '-';
  return s;
}

static bool inheritedProp(Prop p) {
  switch (p) {
    case P_color: case P_font_family: case P_font_size: case P_font_weight: case P_font_style:
    case P_line_height: case P_letter_spacing: case P_word_spacing: case P_text_align:
    case P_text_transform: case P_white_space: case P_word_break: case P_overflow_wrap:
    case P_text_indent: case P_text_shadow: case P_visibility: case P_list_style_type:
    case P_list_style_position: case P_border_collapse: case P_border_spacing: case P_caret_color:
    case P_accent_color: case P_pointer_events: case P_cursor: case P_text_align_last:
    case P_font_variant_numeric: case P_user_select: case P_fill: case P_stroke: case P_stroke_width:
    case P_stroke_linecap: case P_stroke_linejoin:
      return true;
    default:
      return false;
  }
}

const char *propName(Prop p) {
  static std::vector<std::string> *names = nullptr;
  if (!names) {
    names = new std::vector<std::string>();
    for (auto &d : kProps) names->push_back(cssName(d.name));
  }
  return p < P__COUNT ? (*names)[p].c_str() : "";
}

int propByName(const std::string &name) {
  static std::unordered_map<std::string, int> *map = nullptr;
  if (!map) {
    map = new std::unordered_map<std::string, int>();
    for (int i = 0; i < P__COUNT; i++) (*map)[propName((Prop)i)] = i;
    (*map)["-webkit-line-clamp"] = P_line_clamp;
    (*map)["float"] = P_float_;
    (*map)["word-wrap"] = P_overflow_wrap;
    (*map)["grid-column-gap"] = P_column_gap;
    (*map)["grid-row-gap"] = P_row_gap;
    (*map)["text-decoration-line"] = P_text_decoration_line;
    (*map)["-webkit-appearance"] = P_appearance;
    (*map)["-moz-appearance"] = P_appearance;
    (*map)["-webkit-user-select"] = P_user_select;
    (*map)["-webkit-text-fill-color"] = -1;
  }
  auto it = map->find(name);
  return it == map->end() ? -1 : it->second;
}

bool propInherited(Prop p) { return inheritedProp(p); }

// ============================================================ keyword tables

struct KW {
  const char *n;
  int v;
};

static bool kwLookup(const Tok &k, std::initializer_list<KW> tbl, int &out) {
  if (k.t != TK_IDENT) return false;
  for (auto &e : tbl) {
    if (iequals(k.s, e.n)) {
      out = e.v;
      return true;
    }
  }
  return false;
}

static const std::initializer_list<KW> kDisplay = {
    {"none", D_NONE}, {"block", D_BLOCK}, {"inline", D_INLINE}, {"inline-block", D_INLINE_BLOCK},
    {"flex", D_FLEX}, {"inline-flex", D_INLINE_FLEX}, {"grid", D_GRID}, {"inline-grid", D_INLINE_GRID},
    {"table", D_TABLE}, {"inline-table", D_INLINE_TABLE}, {"table-row", D_TABLE_ROW},
    {"table-cell", D_TABLE_CELL}, {"table-row-group", D_TABLE_ROW_GROUP},
    {"table-header-group", D_TABLE_HEADER_GROUP}, {"table-footer-group", D_TABLE_FOOTER_GROUP},
    {"table-caption", D_TABLE_CAPTION}, {"table-column", D_TABLE_COLUMN},
    {"table-column-group", D_TABLE_COLUMN_GROUP}, {"list-item", D_LIST_ITEM}, {"contents", D_CONTENTS},
    {"flow-root", D_FLOW_ROOT}, {"-webkit-box", D_FLEX}, {"-webkit-flex", D_FLEX}, {"inline flex", D_INLINE_FLEX}};

static const std::initializer_list<KW> kPosition = {
    {"static", POS_STATIC}, {"relative", POS_RELATIVE}, {"absolute", POS_ABSOLUTE}, {"fixed", POS_FIXED},
    {"sticky", POS_STICKY}, {"-webkit-sticky", POS_STICKY}};
static const std::initializer_list<KW> kBorderStyle = {
    {"none", BS_NONE}, {"hidden", BS_HIDDEN}, {"solid", BS_SOLID}, {"dashed", BS_DASHED},
    {"dotted", BS_DOTTED}, {"double", BS_DOUBLE}, {"groove", BS_GROOVE}, {"ridge", BS_RIDGE},
    {"inset", BS_INSET}, {"outset", BS_OUTSET}};
static const std::initializer_list<KW> kOverflow = {
    {"visible", OV_VISIBLE}, {"hidden", OV_HIDDEN}, {"scroll", OV_SCROLL}, {"auto", OV_AUTO},
    {"clip", OV_CLIP}, {"overlay", OV_AUTO}};
static const std::initializer_list<KW> kTextAlign = {
    {"start", TA_START}, {"left", TA_LEFT}, {"right", TA_RIGHT}, {"center", TA_CENTER},
    {"justify", TA_JUSTIFY}, {"end", TA_END}, {"-webkit-center", TA_CENTER}, {"match-parent", TA_START}};
static const std::initializer_list<KW> kWhiteSpace = {
    {"normal", WS_NORMAL}, {"nowrap", WS_NOWRAP}, {"pre", WS_PRE}, {"pre-wrap", WS_PRE_WRAP},
    {"pre-line", WS_PRE_LINE}, {"break-spaces", WS_BREAK_SPACES}};
static const std::initializer_list<KW> kAlign = {
    {"auto", AL_AUTO}, {"normal", AL_NORMAL}, {"stretch", AL_STRETCH}, {"start", AL_START},
    {"flex-start", AL_START}, {"self-start", AL_START}, {"end", AL_END}, {"flex-end", AL_END},
    {"self-end", AL_END}, {"center", AL_CENTER}, {"baseline", AL_BASELINE},
    {"first", AL_BASELINE}, {"last", AL_BASELINE}, {"space-between", AL_SPACE_BETWEEN},
    {"space-around", AL_SPACE_AROUND}, {"space-evenly", AL_SPACE_EVENLY}, {"left", AL_LEFT},
    {"right", AL_RIGHT}, {"safe", AL_AUTO}, {"unsafe", AL_AUTO}};

// ============================================================ value helpers

static bool parseNumberTS(TS &ts, float &out, bool allowPct) {
  const Tok &k = ts.peek();
  if (k.t == TK_NUM) {
    ts.i++;
    out = k.n;
    return true;
  }
  if (allowPct && k.t == TK_PCT) {
    ts.i++;
    out = k.n / 100;
    return true;
  }
  if (k.t == TK_FUNC && k.s == "calc") {
    size_t save = ts.i;
    ts.i++;
    auto n = parseCalcSum(ts);
    if (n && ts.closeParen()) {
      LenSpec l;
      float num;
      bool isNum;
      if (foldCalc(*n, l, num, isNum) && isNum) {
        out = num;
        return true;
      }
    }
    ts.i = save;
  }
  return false;
}

static bool parseTime(const Tok &k, float &out) {
  if (k.t == TK_DIM && k.s == "s") {
    out = k.n;
    return true;
  }
  if (k.t == TK_DIM && k.s == "ms") {
    out = k.n / 1000;
    return true;
  }
  if (k.t == TK_NUM && k.n == 0) {
    out = 0;
    return true;
  }
  return false;
}

static bool parseTimingTS(TS &ts, TimingFn &out) {
  const Tok &k = ts.peek();
  if (k.t == TK_IDENT) {
    std::string n = toLower(k.s);
    TimingFn f;
    if (n == "ease") f = TimingFn{TimingFn::BEZIER, 0.25f, 0.1f, 0.25f, 1};
    else if (n == "linear") f = TimingFn{TimingFn::LINEAR};
    else if (n == "ease-in") f = TimingFn{TimingFn::BEZIER, 0.42f, 0, 1, 1};
    else if (n == "ease-out") f = TimingFn{TimingFn::BEZIER, 0, 0, 0.58f, 1};
    else if (n == "ease-in-out") f = TimingFn{TimingFn::BEZIER, 0.42f, 0, 0.58f, 1};
    else if (n == "step-start") { f.kind = TimingFn::STEPS; f.steps = 1; f.jumpStart = true; }
    else if (n == "step-end") { f.kind = TimingFn::STEPS; f.steps = 1; }
    else return false;
    ts.i++;
    out = f;
    return true;
  }
  if (k.t == TK_FUNC && k.s == "cubic-bezier") {
    size_t save = ts.i;
    ts.i++;
    float v[4];
    for (int j = 0; j < 4; j++) {
      if (!parseNumberTS(ts, v[j], false)) {
        ts.i = save;
        return false;
      }
      ts.comma();
    }
    ts.closeParen();
    out = TimingFn{TimingFn::BEZIER, v[0], v[1], v[2], v[3]};
    return true;
  }
  if (k.t == TK_FUNC && k.s == "steps") {
    size_t save = ts.i;
    ts.i++;
    float n;
    if (!parseNumberTS(ts, n, false)) {
      ts.i = save;
      return false;
    }
    TimingFn f;
    f.kind = TimingFn::STEPS;
    f.steps = std::max(1, (int)n);
    if (ts.comma()) {
      if (ts.ident("start") || ts.ident("jump-start")) f.jumpStart = true;
      else ts.next();
    }
    ts.closeParen();
    out = f;
    return true;
  }
  if (k.t == TK_FUNC && k.s == "linear") {  // linear(...) easing: approximate as linear
    ts.i++;
    ts.skipArgs();
    out = TimingFn{TimingFn::LINEAR};
    return true;
  }
  return false;
}

bool parseTimingFn(const std::vector<Tok> &t, size_t &i, TimingFn &out) {
  TS ts(t);
  ts.i = i;
  bool ok = parseTimingTS(ts, out);
  i = ts.i;
  return ok;
}

float TimingFn::eval(float t) const {
  if (t <= 0) return jumpStart && kind == STEPS ? 1.f / steps : 0;
  if (t >= 1) return 1;
  if (kind == LINEAR) return t;
  if (kind == STEPS) {
    float s = std::floor(t * steps + (jumpStart ? 1 : 0)) / steps;
    return std::min(1.f, s);
  }
  // Solve x(u) = t for the cubic bezier with Newton then bisection.
  auto bx = [&](float u) { return 3 * x1 * u * (1 - u) * (1 - u) + 3 * x2 * u * u * (1 - u) + u * u * u; };
  auto by = [&](float u) { return 3 * y1 * u * (1 - u) * (1 - u) + 3 * y2 * u * u * (1 - u) + u * u * u; };
  auto dx = [&](float u) {
    return 3 * x1 * (1 - u) * (1 - u) + 6 * (x2 - x1) * u * (1 - u) + 3 * (1 - x2) * u * u;
  };
  float u = t;
  for (int k = 0; k < 6; k++) {
    float e = bx(u) - t;
    if (std::fabs(e) < 1e-4f) return by(u);
    float d = dx(u);
    if (std::fabs(d) < 1e-5f) break;
    u -= e / d;
  }
  float lo = 0, hi = 1;
  u = t;
  for (int k = 0; k < 20; k++) {
    float x = bx(u);
    if (std::fabs(x - t) < 1e-4f) break;
    if (x < t) lo = u;
    else hi = u;
    u = (lo + hi) / 2;
  }
  return by(u);
}

// position keywords -> percentage
static bool parsePosComponent(TS &ts, LenSpec &out, bool &isX, bool &isY) {
  const Tok &k = ts.peek();
  isX = isY = false;
  if (k.t == TK_IDENT) {
    std::string n = toLower(k.s);
    if (n == "left") { out = LenSpec::Pct(0); isX = true; }
    else if (n == "right") { out = LenSpec::Pct(100); isX = true; }
    else if (n == "top") { out = LenSpec::Pct(0); isY = true; }
    else if (n == "bottom") { out = LenSpec::Pct(100); isY = true; }
    else if (n == "center") out = LenSpec::Pct(50);
    else return false;
    ts.i++;
    return true;
  }
  return parseLenTS(ts, LF_PCT | LF_NEG, out);
}

static bool parsePosition(TS &ts, LenSpec &x, LenSpec &y) {
  LenSpec a, b;
  bool ax, ay, bx, by;
  if (!parsePosComponent(ts, a, ax, ay)) return false;
  size_t save = ts.i;
  if (!parsePosComponent(ts, b, bx, by)) {
    ts.i = save;
    if (ay) {
      x = LenSpec::Pct(50);
      y = a;
    } else {
      x = a;
      y = LenSpec::Pct(50);
    }
    return true;
  }
  if (ay || bx) {
    x = b;
    y = a;
  } else {
    x = a;
    y = b;
  }
  // "right 10px" style offsets are approximated by ignoring the offset keyword form.
  return true;
}

static bool parseGradient(TS &ts, std::shared_ptr<Gradient> &out) {
  const Tok &f = ts.peek();
  if (f.t != TK_FUNC) return false;
  std::string fn = f.s;
  if (startsWith(fn, "-webkit-")) fn = fn.substr(8);
  auto g = std::make_shared<Gradient>();
  if (fn == "linear-gradient" || fn == "repeating-linear-gradient") g->kind = Gradient::LINEAR;
  else if (fn == "radial-gradient" || fn == "repeating-radial-gradient") g->kind = Gradient::RADIAL;
  else if (fn == "conic-gradient" || fn == "repeating-conic-gradient") g->kind = Gradient::CONIC;
  else return false;
  g->repeating = startsWith(fn, "repeating");
  size_t save = ts.i;
  ts.i++;
  // optional first argument: direction / shape
  if (g->kind == Gradient::LINEAR) {
    bool ok;
    if (ts.peek().t == TK_DIM || (ts.peek().t == TK_NUM && ts.peek().n == 0)) {
      float a = angleDeg(ts.peek(), ok);
      if (ok) {
        ts.i++;
        g->angleDeg = a;
        ts.comma();
      }
    } else if (ts.ident("to")) {
      int cx = 0, cy = 0;
      for (int k = 0; k < 2; k++) {
        if (ts.ident("left")) cx = -1;
        else if (ts.ident("right")) cx = 1;
        else if (ts.ident("top")) cy = -1;
        else if (ts.ident("bottom")) cy = 1;
      }
      if (cx && cy) {
        g->toCorner = true;
        g->cornerX = cx;
        g->cornerY = cy;
      } else if (cx) {
        g->angleDeg = cx > 0 ? 90 : 270;
      } else if (cy) {
        g->angleDeg = cy > 0 ? 180 : 0;
      }
      ts.comma();
    }
  } else if (g->kind == Gradient::RADIAL) {
    bool any = false;
    while (ts.peek().t == TK_IDENT && !ts.peek().s.empty()) {
      std::string n = toLower(ts.peek().s);
      if (n == "circle") { g->circle = true; ts.i++; any = true; }
      else if (n == "ellipse" || n == "closest-side" || n == "farthest-corner" || n == "closest-corner" || n == "farthest-side") { ts.i++; any = true; }
      else if (n == "at") {
        ts.i++;
        parsePosition(ts, g->cx, g->cy);
        any = true;
      } else break;
    }
    while (ts.peek().t == TK_DIM || ts.peek().t == TK_PCT) {  // explicit size: ignored
      ts.i++;
      any = true;
    }
    if (ts.ident("at")) {
      parsePosition(ts, g->cx, g->cy);
      any = true;
    }
    if (any) ts.comma();
  } else {  // conic
    bool any = false;
    if (ts.ident("from")) {
      bool ok;
      g->angleDeg = angleDeg(ts.next(), ok);
      any = true;
    } else {
      g->angleDeg = 0;
    }
    if (ts.ident("at")) {
      parsePosition(ts, g->cx, g->cy);
      any = true;
    }
    if (any) ts.comma();
  }
  // colour stops
  while (!ts.end() && !ts.closeParen()) {
    GradStop st;
    if (!parseColorTS(ts, st.color)) {
      // a bare position is a colour hint
      const Tok &k = ts.peek();
      if (k.t == TK_PCT || k.t == TK_DIM) {
        ts.i++;
        if (ts.comma()) continue;
        continue;
      }
      ts.i = save;
      return false;
    }
    for (int rep = 0; rep < 2; rep++) {
      const Tok &k = ts.peek();
      GradStop s2 = st;
      bool got = false;
      if (k.t == TK_PCT) {
        s2.pos = k.n / 100;
        got = true;
      } else if (k.t == TK_DIM && (k.s == "px" || k.s == "em" || k.s == "rem")) {
        s2.pos = k.n * (k.s == "px" ? 1 : 16);
        s2.isPx = true;
        got = true;
      } else if (k.t == TK_DIM && g->kind == Gradient::CONIC) {
        bool ok;
        float a = angleDeg(k, ok);
        if (ok) {
          s2.pos = a / 360;
          got = true;
        }
      } else if (k.t == TK_NUM && k.n == 0) {
        s2.pos = 0;
        got = true;
      }
      if (!got) break;
      ts.i++;
      if (rep == 0) st = s2;
      else {
        g->stops.push_back(st);
        st = s2;
      }
    }
    g->stops.push_back(st);
    ts.comma();
  }
  if (g->stops.size() < 1) {
    ts.i = save;
    return false;
  }
  if (g->stops.size() == 1) g->stops.push_back(g->stops[0]);
  out = g;
  return true;
}

static bool parseShadowList(TS &ts, std::vector<Shadow> &out, bool allowInset) {
  if (ts.ident("none")) return ts.end();
  while (!ts.end()) {
    Shadow s;
    LenSpec lens[4];
    int nl = 0;
    bool any = true;
    while (any && !ts.end() && ts.peek().t != TK_COMMA) {
      any = false;
      if (allowInset && ts.ident("inset")) {
        s.inset = true;
        any = true;
        continue;
      }
      ColorSpec c;
      if (parseColorTS(ts, c)) {
        s.color = c;
        s.hasColor = true;
        any = true;
        continue;
      }
      if (nl < 4 && parseLenTS(ts, LF_NEG, lens[nl])) {
        nl++;
        any = true;
        continue;
      }
    }
    if (!any && !ts.end() && ts.peek().t != TK_COMMA) return false;
    if (nl < 2) return false;
    s.x = lens[0];
    s.y = lens[1];
    if (nl > 2) s.blur = lens[2];
    if (nl > 3) s.spread = lens[3];
    if (!s.hasColor) s.color = {kBlack, true};
    out.push_back(s);
    if (!ts.comma()) break;
  }
  return ts.end();
}

static bool parseTransformList(TS &ts, std::vector<TransformFn> &out) {
  if (ts.ident("none")) return ts.end();
  while (!ts.end()) {
    const Tok &f = ts.peek();
    if (f.t != TK_FUNC) return false;
    std::string fn = f.s;
    ts.i++;
    std::vector<Tok> args;
    int depth = 1;
    std::vector<std::vector<Tok>> argv(1);
    while (!ts.end()) {
      const Tok &a = ts.next();
      if (a.t == TK_FUNC || a.t == TK_LPAREN) depth++;
      if (a.t == TK_RPAREN && --depth == 0) break;
      if (a.t == TK_COMMA && depth == 1) {
        argv.emplace_back();
        continue;
      }
      argv.back().push_back(a);
    }
    if (argv.size() == 1 && argv[0].size() > 1 && fn != "matrix") {
      // space separated arguments (e.g. "translate(10px 20px)")
      std::vector<std::vector<Tok>> split;
      for (auto &k : argv[0]) split.push_back({k});
      bool calcs = false;
      for (auto &k : argv[0])
        if (k.t == TK_FUNC) calcs = true;
      if (!calcs) argv = split;
    }
    auto lenArg = [&](size_t i, LenSpec &l) {
      if (i >= argv.size()) return false;
      TS a(argv[i]);
      return parseLenTS(a, LF_PCT | LF_NEG, l);
    };
    auto numArg = [&](size_t i, float &v) {
      if (i >= argv.size() || argv[i].empty()) return false;
      TS a(argv[i]);
      return parseNumberTS(a, v, true);
    };
    auto angArg = [&](size_t i, float &v) {
      if (i >= argv.size() || argv[i].empty()) return false;
      bool ok;
      v = angleDeg(argv[i][0], ok) * 0.0174532925f;
      return ok;
    };
    TransformFn t;
    if (fn == "translate" || fn == "translate3d") {
      t.kind = TransformFn::TRANSLATE;
      if (!lenArg(0, t.a)) return false;
      if (!lenArg(1, t.b)) t.b = LenSpec();
    } else if (fn == "translatex") {
      t.kind = TransformFn::TRANSLATE;
      if (!lenArg(0, t.a)) return false;
    } else if (fn == "translatey") {
      t.kind = TransformFn::TRANSLATE;
      if (!lenArg(0, t.b)) return false;
    } else if (fn == "translatez") {
      continue;
    } else if (fn == "scale" || fn == "scale3d") {
      t.kind = TransformFn::SCALE;
      if (!numArg(0, t.v[0])) return false;
      if (!numArg(1, t.v[1])) t.v[1] = t.v[0];
    } else if (fn == "scalex") {
      t.kind = TransformFn::SCALE;
      if (!numArg(0, t.v[0])) return false;
      t.v[1] = 1;
    } else if (fn == "scaley") {
      t.kind = TransformFn::SCALE;
      t.v[0] = 1;
      if (!numArg(0, t.v[1])) return false;
    } else if (fn == "rotate" || fn == "rotatez") {
      t.kind = TransformFn::ROTATE;
      if (!angArg(0, t.v[0])) return false;
    } else if (fn == "skew") {
      t.kind = TransformFn::SKEW;
      if (!angArg(0, t.v[0])) return false;
      if (!angArg(1, t.v[1])) t.v[1] = 0;
    } else if (fn == "skewx") {
      t.kind = TransformFn::SKEW;
      if (!angArg(0, t.v[0])) return false;
    } else if (fn == "skewy") {
      t.kind = TransformFn::SKEW;
      if (!angArg(0, t.v[1])) return false;
    } else if (fn == "matrix") {
      t.kind = TransformFn::MATRIX;
      for (int k = 0; k < 6; k++)
        if (!numArg(k, t.v[k])) return false;
    } else {
      continue;  // 3D functions (perspective, rotateX...) are ignored
    }
    out.push_back(t);
  }
  return true;
}

static bool parseTrackSize(TS &ts, TrackSize &out) {
  const Tok &k = ts.peek();
  if (k.t == TK_DIM && k.s == "fr") {
    ts.i++;
    out.kind = TrackSize::FR;
    out.fr = k.n;
    return true;
  }
  if (k.t == TK_IDENT) {
    if (ts.ident("auto")) { out.kind = TrackSize::AUTO; return true; }
    if (ts.ident("min-content")) { out.kind = TrackSize::MIN_CONTENT; return true; }
    if (ts.ident("max-content")) { out.kind = TrackSize::MAX_CONTENT; return true; }
    return false;
  }
  if (k.t == TK_FUNC && k.s == "minmax") {
    size_t save = ts.i;
    ts.i++;
    auto a = std::make_shared<TrackSize>(), b = std::make_shared<TrackSize>();
    if (!parseTrackSize(ts, *a) || !ts.comma() || !parseTrackSize(ts, *b) || !ts.closeParen()) {
      ts.i = save;
      return false;
    }
    out.kind = TrackSize::MINMAX;
    out.mn = a;
    out.mx = b;
    return true;
  }
  if (k.t == TK_FUNC && k.s == "fit-content") {
    ts.i++;
    LenSpec l;
    parseLenTS(ts, LF_PCT, l);
    ts.closeParen();
    out.kind = TrackSize::AUTO;
    return true;
  }
  LenSpec l;
  if (parseLenTS(ts, LF_PCT, l)) {
    out.kind = TrackSize::FIXED;
    out.len = l;
    return true;
  }
  return false;
}

static bool parseTrackList(TS &ts, TrackList &out) {
  if (ts.ident("none")) return ts.end();
  bool seenAuto = false;
  while (!ts.end()) {
    if (ts.peek().t == TK_LBRACK) {  // line names: skip
      while (!ts.end() && ts.next().t != TK_RBRACK) {}
      continue;
    }
    if (ts.peek().t == TK_FUNC && ts.peek().s == "repeat") {
      ts.i++;
      int count = 1, autoRep = 0;
      if (ts.ident("auto-fill")) autoRep = 1;
      else if (ts.ident("auto-fit")) autoRep = 2;
      else if (ts.peek().t == TK_NUM) count = std::max(1, (int)ts.next().n);
      else return false;
      if (!ts.comma()) return false;
      std::vector<TrackSize> pat;
      while (!ts.end() && !ts.closeParen()) {
        if (ts.peek().t == TK_LBRACK) {
          while (!ts.end() && ts.next().t != TK_RBRACK) {}
          continue;
        }
        TrackSize t;
        if (!parseTrackSize(ts, t)) return false;
        pat.push_back(t);
      }
      if (autoRep) {
        if (seenAuto) return false;
        seenAuto = true;
        out.autoRepeat = autoRep;
        out.before = out.tracks;
        out.tracks = pat;
      } else {
        for (int k = 0; k < count; k++)
          for (auto &t : pat) (seenAuto ? out.after : out.tracks).push_back(t);
      }
      continue;
    }
    TrackSize t;
    if (!parseTrackSize(ts, t)) return false;
    (seenAuto ? out.after : out.tracks).push_back(t);
  }
  return true;
}

static bool parseGridLine(TS &ts, GridLine &g) {
  g = GridLine();
  if (ts.ident("auto")) return true;
  if (ts.ident("span")) {
    if (ts.peek().t == TK_NUM) g.span = std::max(1, (int)ts.next().n);
    else g.span = 1;
    return true;
  }
  if (ts.peek().t == TK_NUM) {
    g.line = (int)ts.next().n;
    if (ts.ident("span")) {}
    return true;
  }
  if (ts.peek().t == TK_IDENT) {  // named lines unsupported: auto
    ts.i++;
    return true;
  }
  return false;
}

static bool parseContent(TS &ts, std::vector<ContentPart> &out, bool &none) {
  none = false;
  if (ts.ident("none") || ts.ident("normal")) {
    none = true;
    return ts.end();
  }
  while (!ts.end()) {
    const Tok &k = ts.peek();
    ContentPart p;
    if (k.t == TK_STRING) {
      p.kind = ContentPart::STR;
      p.s = k.s;
      ts.i++;
    } else if (k.t == TK_FUNC && k.s == "attr") {
      ts.i++;
      p.kind = ContentPart::ATTR;
      if (ts.peek().t == TK_IDENT) p.s = toLower(ts.next().s);
      ts.skipArgs();
    } else if (k.t == TK_FUNC && (k.s == "counter" || k.s == "counters")) {
      ts.i++;
      ts.skipArgs();
      continue;
    } else if (k.t == TK_URL) {
      ts.i++;
      continue;
    } else if (ts.ident("open-quote")) {
      p.kind = ContentPart::STR;
      p.s = "“";
    } else if (ts.ident("close-quote")) {
      p.kind = ContentPart::STR;
      p.s = "”";
    } else if (ts.ident("no-open-quote") || ts.ident("no-close-quote")) {
      continue;
    } else if (k.t == TK_DELIM && k.c == '/') {  // alt text: stop
      break;
    } else {
      return false;
    }
    out.push_back(p);
  }
  return true;
}

static bool parseFontFamily(TS &ts, std::vector<std::string> &out) {
  while (!ts.end()) {
    std::string name;
    if (ts.peek().t == TK_STRING) {
      name = ts.next().s;
    } else {
      while (ts.peek().t == TK_IDENT) {
        if (!name.empty()) name += ' ';
        name += ts.next().s;
      }
    }
    if (name.empty()) return false;
    out.push_back(name);
    if (!ts.comma()) break;
  }
  return ts.end() && !out.empty();
}

static bool parseFilter(TS &ts, std::vector<FilterFn> &out) {
  if (ts.ident("none")) return ts.end();
  while (!ts.end()) {
    const Tok &f = ts.peek();
    if (f.t != TK_FUNC) return false;
    std::string fn = f.s;
    ts.i++;
    FilterFn ff;
    float v = 1;
    if (fn == "blur") {
      LenSpec l;
      parseLenTS(ts, 0, l);
      ff.kind = FilterFn::BLUR;
      v = l.px;
    } else if (fn == "hue-rotate") {
      bool ok;
      v = ts.end() ? 0 : angleDeg(ts.next(), ok);
      ff.kind = FilterFn::HUE_ROTATE;
    } else if (fn == "drop-shadow") {
      ts.skipArgs();
      continue;
    } else {
      if (!parseNumberTS(ts, v, true)) v = 1;
      if (fn == "grayscale") ff.kind = FilterFn::GRAYSCALE;
      else if (fn == "brightness") ff.kind = FilterFn::BRIGHTNESS;
      else if (fn == "contrast") ff.kind = FilterFn::CONTRAST;
      else if (fn == "invert") ff.kind = FilterFn::INVERT;
      else if (fn == "opacity") ff.kind = FilterFn::OPACITY;
      else if (fn == "sepia") ff.kind = FilterFn::SEPIA;
      else if (fn == "saturate") ff.kind = FilterFn::SATURATE;
      else {
        ts.skipArgs();
        continue;
      }
    }
    ff.v = v;
    ts.closeParen();
    out.push_back(ff);
  }
  return true;
}

// ============================================================ longhand parsing

template <class T>
static std::shared_ptr<void> box(T &&v) {
  return std::make_shared<typename std::decay<T>::type>(std::forward<T>(v));
}

static bool parseLonghandTS(Prop p, TS &ts, Value &v) {
  auto kwv = [&](std::initializer_list<KW> tbl) {
    int k;
    if (!kwLookup(ts.peek(), tbl, k)) return false;
    ts.i++;
    v.type = VT_KW;
    v.kw = k;
    return true;
  };
  auto lenv = [&](uint32_t f) {
    if (!parseLenTS(ts, f, v.len)) return false;
    v.type = VT_LEN;
    return true;
  };
  auto colorv = [&]() {
    if (!parseColorTS(ts, v.color)) return false;
    v.type = VT_COLOR;
    return true;
  };
  switch (p) {
    case P_display: {
      if (ts.peek().t == TK_IDENT && ts.peek(1).t == TK_IDENT) {  // two-value syntax
        std::string a = toLower(ts.peek().s), b = toLower(ts.peek(1).s);
        ts.i += 2;
        v.type = VT_KW;
        bool inl = a == "inline";
        if (b == "flex") v.kw = inl ? D_INLINE_FLEX : D_FLEX;
        else if (b == "grid") v.kw = inl ? D_INLINE_GRID : D_GRID;
        else if (b == "flow-root") v.kw = inl ? D_INLINE_BLOCK : D_FLOW_ROOT;
        else if (b == "flow") v.kw = inl ? D_INLINE : D_BLOCK;
        else if (b == "list-item") v.kw = D_LIST_ITEM;
        else return false;
        return true;
      }
      return kwv(kDisplay);
    }
    case P_position: return kwv(kPosition);
    case P_float_: return kwv({{"none", 0}, {"left", 1}, {"right", 2}, {"inline-start", 1}, {"inline-end", 2}});
    case P_clear: return kwv({{"none", 0}, {"left", 1}, {"right", 2}, {"both", 3}});
    case P_top: case P_right: case P_bottom: case P_left:
      return lenv(LF_AUTO | LF_PCT | LF_NEG);
    case P_z_index:
      if (ts.ident("auto")) {
        v.type = VT_KW;
        v.kw = 0;
        return true;
      }
      if (ts.peek().t == TK_NUM) {
        v.type = VT_NUM;
        v.num = ts.next().n;
        return true;
      }
      return false;
    case P_width: case P_height: case P_min_width: case P_min_height:
      return lenv(LF_AUTO | LF_PCT | LF_CONTENT_KW);
    case P_max_width: case P_max_height:
      return lenv(LF_NONE | LF_PCT | LF_CONTENT_KW);
    case P_margin_top: case P_margin_right: case P_margin_bottom: case P_margin_left:
      return lenv(LF_AUTO | LF_PCT | LF_NEG);
    case P_padding_top: case P_padding_right: case P_padding_bottom: case P_padding_left:
      return lenv(LF_PCT);
    case P_border_top_width: case P_border_right_width: case P_border_bottom_width: case P_border_left_width:
    case P_outline_width:
      if (ts.ident("thin")) { v.type = VT_LEN; v.len = LenSpec::Px(1); return true; }
      if (ts.ident("medium")) { v.type = VT_LEN; v.len = LenSpec::Px(3); return true; }
      if (ts.ident("thick")) { v.type = VT_LEN; v.len = LenSpec::Px(5); return true; }
      return lenv(0);
    case P_border_top_style: case P_border_right_style: case P_border_bottom_style: case P_border_left_style:
    case P_outline_style:
      if (p == P_outline_style && ts.ident("auto")) { v.type = VT_KW; v.kw = BS_SOLID; return true; }
      return kwv(kBorderStyle);
    case P_border_top_color: case P_border_right_color: case P_border_bottom_color: case P_border_left_color:
    case P_outline_color: case P_text_decoration_color:
      return colorv();
    case P_border_top_left_radius: case P_border_top_right_radius: case P_border_bottom_right_radius:
    case P_border_bottom_left_radius:
      if (!lenv(LF_PCT)) return false;
      if (!ts.end()) {
        LenSpec l2;
        if (parseLenTS(ts, LF_PCT, l2)) { /* elliptical: use first radius */ }
      }
      return true;
    case P_box_sizing: return kwv({{"content-box", BOX_CONTENT}, {"border-box", BOX_BORDER}});
    case P_overflow_x: case P_overflow_y: return kwv(kOverflow);
    case P_visibility: return kwv({{"visible", VIS_VISIBLE}, {"hidden", VIS_HIDDEN}, {"collapse", VIS_COLLAPSE}});
    case P_opacity:
      if (!parseNumberTS(ts, v.num, true)) return false;
      v.type = VT_NUM;
      return true;
    case P_background_color: case P_color: return colorv();
    case P_caret_color: case P_accent_color:
      if (ts.ident("auto")) { v.type = VT_KW; v.kw = 0; return true; }
      return colorv();
    case P_background_image: {
      if (ts.ident("none")) {
        v.type = VT_KW;
        v.kw = 0;
        return true;
      }
      std::vector<BgLayer> layers;
      while (!ts.end()) {
        BgLayer L;
        if (ts.peek().t == TK_URL) {
          L.url = ts.next().s;
        } else if (ts.ident("none")) {
        } else if (!parseGradient(ts, L.grad)) {
          return false;
        }
        layers.push_back(L);
        if (!ts.comma()) break;
      }
      v.type = VT_OBJ;
      v.ext().obj = box(std::move(layers));
      return true;
    }
    case P_background_size: {
      if (ts.ident("cover")) { v.type = VT_KW; v.kw = BGS_COVER; return true; }
      if (ts.ident("contain")) { v.type = VT_KW; v.kw = BGS_CONTAIN; return true; }
      if (!parseLenTS(ts, LF_AUTO | LF_PCT, v.len)) return false;
      if (!parseLenTS(ts, LF_AUTO | LF_PCT, v.ext().len2)) v.ext().len2 = LenSpec::Kind(LK_AUTO);
      v.type = VT_LEN2;
      return true;
    }
    case P_background_position: case P_object_position: case P_transform_origin:
      if (!parsePosition(ts, v.len, v.ext().len2)) return false;
      if (!ts.end()) parseLenTS(ts, LF_NEG, v.num == 0 ? v.len : v.len);  // z offset ignored
      v.type = VT_LEN2;
      return true;
    case P_background_repeat:
      return kwv({{"repeat", BR_REPEAT}, {"no-repeat", BR_NO_REPEAT}, {"repeat-x", BR_REPEAT_X},
                  {"repeat-y", BR_REPEAT_Y}, {"space", BR_REPEAT}, {"round", BR_REPEAT}});
    case P_background_clip:
      return kwv({{"border-box", 0}, {"padding-box", 1}, {"content-box", 2}, {"text", 3}});
    case P_font_family: {
      std::vector<std::string> f;
      if (!parseFontFamily(ts, f)) return false;
      v.type = VT_OBJ;
      v.ext().obj = box(std::move(f));
      return true;
    }
    case P_font_size: {
      static const std::initializer_list<KW> sizes = {
          {"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16}, {"large", 18},
          {"x-large", 24}, {"xx-large", 32}, {"xxx-large", 48}};
      int k;
      if (kwLookup(ts.peek(), sizes, k)) {
        ts.i++;
        v.type = VT_LEN;
        v.len = LenSpec::Px((float)k);
        return true;
      }
      if (ts.ident("smaller")) { v.type = VT_LEN; v.len = LenSpec(); v.len.em = 0.8333f; return true; }
      if (ts.ident("larger")) { v.type = VT_LEN; v.len = LenSpec(); v.len.em = 1.2f; return true; }
      if (!parseLenTS(ts, LF_PCT, v.len)) return false;
      if (v.len.pct != 0) {  // % of parent font size == em
        v.len.em += v.len.pct / 100;
        v.len.pct = 0;
      }
      v.type = VT_LEN;
      return true;
    }
    case P_font_weight:
      if (ts.ident("normal")) { v.type = VT_NUM; v.num = 400; return true; }
      if (ts.ident("bold")) { v.type = VT_NUM; v.num = 700; return true; }
      if (ts.ident("bolder")) { v.type = VT_KW; v.kw = 1; return true; }
      if (ts.ident("lighter")) { v.type = VT_KW; v.kw = 2; return true; }
      if (ts.peek().t == TK_NUM) { v.type = VT_NUM; v.num = ts.next().n; return true; }
      return false;
    case P_font_style:
      return kwv({{"normal", 0}, {"italic", 1}, {"oblique", 1}});
    case P_line_height:
      if (ts.ident("normal")) { v.type = VT_KW; v.kw = 0; return true; }
      if (ts.peek().t == TK_NUM) { v.type = VT_NUM; v.num = ts.next().n; return true; }
      if (!parseLenTS(ts, LF_PCT, v.len)) return false;
      if (v.len.pct != 0) {
        v.len.em += v.len.pct / 100;
        v.len.pct = 0;
      }
      v.type = VT_LEN;
      return true;
    case P_letter_spacing: case P_word_spacing:
      if (ts.ident("normal")) { v.type = VT_LEN; v.len = LenSpec::Px(0); return true; }
      return lenv(LF_NEG);
    case P_text_align: case P_text_align_last: return kwv(kTextAlign);
    case P_text_decoration_line: {
      int flags = 0;
      if (ts.ident("none")) { v.type = VT_KW; v.kw = 0; return true; }
      while (!ts.end()) {
        if (ts.ident("underline")) flags |= TD_UNDERLINE;
        else if (ts.ident("overline")) flags |= TD_OVERLINE;
        else if (ts.ident("line-through")) flags |= TD_LINE_THROUGH;
        else return false;
      }
      v.type = VT_KW;
      v.kw = flags;
      return true;
    }
    case P_text_transform:
      return kwv({{"none", TT_NONE}, {"uppercase", TT_UPPER}, {"lowercase", TT_LOWER}, {"capitalize", TT_CAPITALIZE}});
    case P_text_overflow: return kwv({{"clip", TO_CLIP}, {"ellipsis", TO_ELLIPSIS}});
    case P_white_space: return kwv(kWhiteSpace);
    case P_word_break:
      return kwv({{"normal", WB_NORMAL}, {"break-all", WB_BREAK_ALL}, {"keep-all", WB_KEEP_ALL}, {"break-word", WB_BREAK_WORD}});
    case P_overflow_wrap:
      return kwv({{"normal", OW_NORMAL}, {"anywhere", OW_ANYWHERE}, {"break-word", OW_BREAK_WORD}});
    case P_vertical_align: {
      if (kwv({{"baseline", VA_BASELINE}, {"middle", VA_MIDDLE}, {"top", VA_TOP}, {"bottom", VA_BOTTOM},
               {"text-top", VA_TEXT_TOP}, {"text-bottom", VA_TEXT_BOTTOM}, {"sub", VA_SUB}, {"super", VA_SUPER}}))
        return true;
      return lenv(LF_PCT | LF_NEG);
    }
    case P_text_indent: return lenv(LF_PCT | LF_NEG);
    case P_text_shadow: case P_box_shadow: {
      std::vector<Shadow> sh;
      if (!parseShadowList(ts, sh, p == P_box_shadow)) return false;
      v.type = VT_OBJ;
      v.ext().obj = box(std::move(sh));
      return true;
    }
    case P_line_clamp:
      if (ts.ident("none")) { v.type = VT_NUM; v.num = 0; return true; }
      if (ts.peek().t == TK_NUM) { v.type = VT_NUM; v.num = ts.next().n; return true; }
      return false;
    case P_outline_offset: return lenv(LF_NEG);
    case P_transform: {
      std::vector<TransformFn> tf;
      if (!parseTransformList(ts, tf)) return false;
      v.type = VT_OBJ;
      v.ext().obj = box(std::move(tf));
      return true;
    }
    case P_filter: {
      std::vector<FilterFn> f;
      if (!parseFilter(ts, f)) return false;
      v.type = VT_OBJ;
      v.ext().obj = box(std::move(f));
      return true;
    }
    case P_flex_direction:
      return kwv({{"row", FD_ROW}, {"row-reverse", FD_ROW_REVERSE}, {"column", FD_COLUMN}, {"column-reverse", FD_COLUMN_REVERSE}});
    case P_flex_wrap: return kwv({{"nowrap", FW_NOWRAP}, {"wrap", FW_WRAP}, {"wrap-reverse", FW_WRAP_REVERSE}});
    case P_justify_content: case P_align_items: case P_align_self: case P_align_content:
    case P_justify_items: case P_justify_self: {
      int k;
      // "safe center" / "first baseline": take the meaningful keyword
      if (!kwLookup(ts.peek(), kAlign, k)) return false;
      ts.i++;
      if ((k == AL_AUTO || k == AL_BASELINE) && !ts.end()) {
        int k2;
        if (kwLookup(ts.peek(), kAlign, k2)) {
          ts.i++;
          k = k2;
        }
      }
      if (p == P_justify_items && ts.ident("legacy")) {}
      v.type = VT_KW;
      v.kw = k;
      return true;
    }
    case P_flex_grow: case P_flex_shrink:
      if (!parseNumberTS(ts, v.num, false)) return false;
      v.type = VT_NUM;
      return true;
    case P_flex_basis: return lenv(LF_AUTO | LF_PCT | LF_CONTENT | LF_CONTENT_KW);
    case P_order:
      if (ts.peek().t != TK_NUM) return false;
      v.type = VT_NUM;
      v.num = ts.next().n;
      return true;
    case P_row_gap: case P_column_gap: return lenv(LF_PCT | LF_NORMAL);
    case P_grid_template_columns: case P_grid_template_rows: case P_grid_auto_rows: case P_grid_auto_columns: {
      TrackList tl;
      if (!parseTrackList(ts, tl)) return false;
      v.type = VT_OBJ;
      v.ext().obj = box(std::move(tl));
      return true;
    }
    case P_grid_auto_flow: {
      int f = 0;
      while (!ts.end()) {
        if (ts.ident("row")) {}
        else if (ts.ident("column")) f |= GF_COLUMN;
        else if (ts.ident("dense")) f |= GF_DENSE;
        else return false;
      }
      v.type = VT_KW;
      v.kw = f;
      return true;
    }
    case P_grid_column_start: case P_grid_column_end: case P_grid_row_start: case P_grid_row_end: {
      GridLine g;
      if (!parseGridLine(ts, g)) return false;
      v.type = VT_LEN2;
      v.kw = g.line;
      v.num = (float)g.span;
      return true;
    }
    case P_transition_property: {
      std::vector<Atom> props;
      while (!ts.end()) {
        if (ts.peek().t != TK_IDENT) return false;
        std::string n = toLower(ts.next().s);
        props.push_back(n == "all" ? 0 : (n == "none" ? (Atom)0xFFFFFFFF : intern(n)));
        if (!ts.comma()) break;
      }
      v.type = VT_OBJ;
      v.ext().obj = box(std::move(props));
      return true;
    }
    case P_transition_duration: case P_transition_delay: case P_animation_duration: case P_animation_delay: {
      std::vector<float> ts_;
      while (!ts.end()) {
        float t;
        if (!parseTime(ts.next(), t)) return false;
        ts_.push_back(t);
        if (!ts.comma()) break;
      }
      v.type = VT_OBJ;
      v.ext().obj = box(std::move(ts_));
      return true;
    }
    case P_transition_timing_function: case P_animation_timing_function: {
      std::vector<TimingFn> fs;
      while (!ts.end()) {
        TimingFn f;
        if (!parseTimingTS(ts, f)) return false;
        fs.push_back(f);
        if (!ts.comma()) break;
      }
      v.type = VT_OBJ;
      v.ext().obj = box(std::move(fs));
      return true;
    }
    case P_animation_name: {
      std::vector<Atom> names;
      while (!ts.end()) {
        const Tok &k = ts.next();
        if (k.t == TK_IDENT) names.push_back(iequals(k.s, "none") ? 0 : intern(k.s));
        else if (k.t == TK_STRING) names.push_back(intern(k.s));
        else return false;
        if (!ts.comma()) break;
      }
      v.type = VT_OBJ;
      v.ext().obj = box(std::move(names));
      return true;
    }
    case P_animation_iteration_count: {
      std::vector<float> c;
      while (!ts.end()) {
        if (ts.ident("infinite")) c.push_back(INFINITY);
        else if (ts.peek().t == TK_NUM) c.push_back(ts.next().n);
        else return false;
        if (!ts.comma()) break;
      }
      v.type = VT_OBJ;
      v.ext().obj = box(std::move(c));
      return true;
    }
    case P_animation_direction: case P_animation_fill_mode: case P_animation_play_state: {
      std::vector<float> c;
      while (!ts.end()) {
        int k;
        if (p == P_animation_direction) {
          if (!kwLookup(ts.peek(), {{"normal", 0}, {"reverse", 1}, {"alternate", 2}, {"alternate-reverse", 3}}, k)) return false;
        } else if (p == P_animation_fill_mode) {
          if (!kwLookup(ts.peek(), {{"none", 0}, {"forwards", 1}, {"backwards", 2}, {"both", 3}}, k)) return false;
        } else {
          if (!kwLookup(ts.peek(), {{"running", 0}, {"paused", 1}}, k)) return false;
        }
        ts.i++;
        c.push_back((float)k);
        if (!ts.comma()) break;
      }
      v.type = VT_OBJ;
      v.ext().obj = box(std::move(c));
      return true;
    }
    case P_content: {
      std::vector<ContentPart> c;
      bool none;
      if (!parseContent(ts, c, none)) return false;
      v.type = none ? VT_KW : VT_OBJ;
      v.kw = 0;
      if (!none) v.ext().obj = box(std::move(c));
      return true;
    }
    case P_list_style_type:
      return kwv({{"disc", LS_DISC}, {"circle", LS_CIRCLE}, {"square", LS_SQUARE}, {"decimal", LS_DECIMAL},
                  {"none", LS_NONE}, {"lower-alpha", LS_LOWER_ALPHA}, {"lower-latin", LS_LOWER_ALPHA},
                  {"upper-alpha", LS_UPPER_ALPHA}, {"upper-latin", LS_UPPER_ALPHA},
                  {"lower-roman", LS_LOWER_ROMAN}, {"upper-roman", LS_UPPER_ROMAN}, {"thai", LS_THAI}});
    case P_list_style_position: return kwv({{"outside", 0}, {"inside", 1}});
    case P_border_collapse: return kwv({{"separate", 0}, {"collapse", 1}});
    case P_border_spacing:
      if (!parseLenTS(ts, 0, v.len)) return false;
      if (!parseLenTS(ts, 0, v.ext().len2)) v.ext().len2 = v.len;
      v.type = VT_LEN2;
      return true;
    case P_table_layout: return kwv({{"auto", TL_AUTO}, {"fixed", TL_FIXED}});
    case P_pointer_events:
      if (ts.ident("none")) { v.type = VT_KW; v.kw = PE_NONE_; return true; }
      ts.next();
      v.type = VT_KW;
      v.kw = PE_AUTO_;
      return true;
    case P_appearance:
      if (ts.ident("none")) { v.type = VT_KW; v.kw = AP_NONE; return true; }
      ts.next();
      v.type = VT_KW;
      v.kw = AP_AUTO;
      return true;
    case P_object_fit:
      return kwv({{"fill", OF_FILL}, {"contain", OF_CONTAIN}, {"cover", OF_COVER}, {"none", OF_NONE}, {"scale-down", OF_SCALE_DOWN}});
    case P_aspect_ratio: {
      if (ts.ident("auto")) {
        if (ts.end()) { v.type = VT_NUM; v.num = 0; return true; }
      }
      float a, b = 1;
      if (!parseNumberTS(ts, a, false)) return false;
      if (ts.delim('/')) {
        if (!parseNumberTS(ts, b, false)) return false;
      }
      v.type = VT_NUM;
      v.num = b != 0 ? a / b : 0;
      return true;
    }
    case P_image_rendering:
      v.type = VT_KW;
      v.kw = (ts.ident("pixelated") || ts.ident("crisp-edges")) ? 1 : 0;
      if (v.kw == 0) ts.next();
      return true;
    case P_scroll_snap_type: {
      int k = 0;
      if (ts.ident("x") || ts.ident("inline")) k = 1;
      else if (ts.ident("y") || ts.ident("block")) k = 2;
      else if (ts.ident("both")) k = 3;
      else if (ts.ident("none")) k = 0;
      else return false;
      ts.ident("mandatory") || ts.ident("proximity");
      v.type = VT_KW;
      v.kw = k;
      return true;
    }
    case P_scroll_snap_align:
      return kwv({{"none", 0}, {"start", 1}, {"center", 2}, {"end", 3}});
    case P_font_variant_numeric:
      v.type = VT_KW;
      v.kw = 0;
      while (!ts.end()) {
        if (ts.ident("tabular-nums")) v.kw = 1;
        else ts.next();
      }
      return true;
    // the values of raster.h's LineCap / LineJoin
    case P_stroke_linecap: return kwv({{"butt", 0}, {"round", 1}, {"square", 2}});
    case P_stroke_linejoin: return kwv({{"miter", 0}, {"round", 1}, {"bevel", 2}, {"miter-clip", 0}, {"arcs", 1}});
    case P_fill: case P_stroke:
      if (ts.ident("none")) { v.type = VT_KW; v.kw = 0; return true; }
      if (ts.peek().t == TK_URL) { ts.i++; ts.next(); v.type = VT_KW; v.kw = 0; return true; }
      return colorv();
    case P_stroke_width: return lenv(LF_PCT | LF_UNITLESS);
    case P_user_select: case P_cursor:
      while (!ts.end()) ts.next();
      v.type = VT_KW;
      v.kw = 0;
      return true;
    default:
      return false;
  }
}

static bool hasVar(const std::vector<Tok> &t) {
  for (auto &k : t)
    if (k.t == TK_FUNC && (k.s == "var" || k.s == "env")) return true;
  return false;
}

static bool globalKeyword(const std::vector<Tok> &t, Value &v) {
  TS ts(t);
  if (ts.t.size() != 1 || ts.t[0].t != TK_IDENT) return false;
  const std::string &s = ts.t[0].s;
  if (iequals(s, "inherit")) v.type = VT_INHERIT;
  else if (iequals(s, "initial")) v.type = VT_INITIAL;
  else if (iequals(s, "unset") || iequals(s, "revert") || iequals(s, "revert-layer")) v.type = VT_UNSET;
  else return false;
  return true;
}

bool parseLonghand(Prop p, const std::vector<Tok> &toks, Value &out) {
  if (globalKeyword(toks, out)) return true;
  TS ts(toks);
  Value v;
  if (!parseLonghandTS(p, ts, v)) return false;
  if (!ts.end()) return false;
  out = std::move(v);
  return true;
}

// ============================================================ shorthands

static const uint16_t P_SHORTHAND_RAW = 0xFFFD;

static void push(std::vector<Decl> &out, Prop p, const Value &v, bool imp) {
  Decl d;
  d.prop = p;
  d.important = imp;
  d.v = v;
  out.push_back(std::move(d));
}

static Value lenVal(const LenSpec &l) {
  Value v;
  v.type = VT_LEN;
  v.len = l;
  return v;
}

// Box shorthands: 1-4 values -> top right bottom left
static bool boxShorthand(TS &ts, uint32_t flags, LenSpec out[4]) {
  LenSpec v[4];
  int n = 0;
  while (!ts.end() && n < 4) {
    if (!parseLenTS(ts, flags, v[n])) return false;
    n++;
  }
  if (!ts.end() || n == 0) return false;
  out[0] = v[0];
  out[1] = n > 1 ? v[1] : v[0];
  out[2] = n > 2 ? v[2] : v[0];
  out[3] = n > 3 ? v[3] : out[1];
  return true;
}

static bool expandShorthand(const std::string &name, const std::vector<Tok> &toks, std::vector<Decl> &out, bool imp) {
  TS ts(toks);
  Value g;
  bool isGlobal = globalKeyword(toks, g);
  auto all = [&](std::initializer_list<Prop> props) {
    for (Prop p : props) push(out, p, g, imp);
    return true;
  };
  if (name == "margin" || name == "padding" || name == "inset") {
    Prop a, b, c, d;
    uint32_t flags;
    if (name == "margin") { a = P_margin_top; b = P_margin_right; c = P_margin_bottom; d = P_margin_left; flags = LF_AUTO | LF_PCT | LF_NEG; }
    else if (name == "padding") { a = P_padding_top; b = P_padding_right; c = P_padding_bottom; d = P_padding_left; flags = LF_PCT; }
    else { a = P_top; b = P_right; c = P_bottom; d = P_left; flags = LF_AUTO | LF_PCT | LF_NEG; }
    if (isGlobal) return all({a, b, c, d});
    LenSpec v[4];
    if (!boxShorthand(ts, flags, v)) return false;
    push(out, a, lenVal(v[0]), imp);
    push(out, b, lenVal(v[1]), imp);
    push(out, c, lenVal(v[2]), imp);
    push(out, d, lenVal(v[3]), imp);
    return true;
  }
  if (name == "margin-block" || name == "padding-block" || name == "margin-inline" || name == "padding-inline") {
    bool m = name[0] == 'm';
    bool block = name.find("block") != std::string::npos;
    Prop a = block ? (m ? P_margin_top : P_padding_top) : (m ? P_margin_left : P_padding_left);
    Prop b = block ? (m ? P_margin_bottom : P_padding_bottom) : (m ? P_margin_right : P_padding_right);
    if (isGlobal) return all({a, b});
    LenSpec x, y;
    uint32_t flags = m ? (LF_AUTO | LF_PCT | LF_NEG) : LF_PCT;
    if (!parseLenTS(ts, flags, x)) return false;
    if (!parseLenTS(ts, flags, y)) y = x;
    push(out, a, lenVal(x), imp);
    push(out, b, lenVal(y), imp);
    return ts.end();
  }
  if (name == "border-width" || name == "border-style" || name == "border-color") {
    Prop ps[4];
    if (name == "border-width") { ps[0] = P_border_top_width; ps[1] = P_border_right_width; ps[2] = P_border_bottom_width; ps[3] = P_border_left_width; }
    else if (name == "border-style") { ps[0] = P_border_top_style; ps[1] = P_border_right_style; ps[2] = P_border_bottom_style; ps[3] = P_border_left_style; }
    else { ps[0] = P_border_top_color; ps[1] = P_border_right_color; ps[2] = P_border_bottom_color; ps[3] = P_border_left_color; }
    if (isGlobal) return all({ps[0], ps[1], ps[2], ps[3]});
    Value v[4];
    int n = 0;
    while (!ts.end() && n < 4) {
      if (!parseLonghandTS(ps[0], ts, v[n])) return false;
      n++;
    }
    if (n == 0 || !ts.end()) return false;
    Value r[4] = {v[0], n > 1 ? v[1] : v[0], n > 2 ? v[2] : v[0], n > 3 ? v[3] : (n > 1 ? v[1] : v[0])};
    for (int k = 0; k < 4; k++) push(out, ps[k], r[k], imp);
    return true;
  }
  if (name == "border" || name == "border-top" || name == "border-right" || name == "border-bottom" ||
      name == "border-left" || name == "outline" || name == "border-block" || name == "border-inline" ||
      name == "border-block-end" || name == "border-block-start" || name == "border-inline-start" || name == "border-inline-end") {
    std::vector<int> sides;
    bool outline = name == "outline";
    if (name == "border") sides = {0, 1, 2, 3};
    else if (name == "border-top" || name == "border-block-start") sides = {0};
    else if (name == "border-right" || name == "border-inline-end") sides = {1};
    else if (name == "border-bottom" || name == "border-block-end") sides = {2};
    else if (name == "border-left" || name == "border-inline-start") sides = {3};
    else if (name == "border-block") sides = {0, 2};
    else if (name == "border-inline") sides = {1, 3};
    static const Prop W[4] = {P_border_top_width, P_border_right_width, P_border_bottom_width, P_border_left_width};
    static const Prop S[4] = {P_border_top_style, P_border_right_style, P_border_bottom_style, P_border_left_style};
    static const Prop C[4] = {P_border_top_color, P_border_right_color, P_border_bottom_color, P_border_left_color};
    Value w, s, c;
    if (isGlobal) {
      w = s = c = g;
    } else {
      w = lenVal(LenSpec::Px(3));
      s.type = VT_KW;
      s.kw = BS_NONE;
      c.type = VT_COLOR;
      c.color = {kBlack, true};
      bool gw = false, gs = false, gc = false;
      if (ts.ident("none") && ts.end()) {
        gs = true;
      } else {
        ts.i = 0;
        while (!ts.end()) {
          Value v;
          size_t save = ts.i;
          if (!gw && parseLonghandTS(P_border_top_width, ts, v)) { w = v; gw = true; continue; }
          ts.i = save;
          if (!gs && parseLonghandTS(outline ? P_outline_style : P_border_top_style, ts, v)) { s = v; gs = true; continue; }
          ts.i = save;
          if (!gc && parseLonghandTS(P_border_top_color, ts, v)) { c = v; gc = true; continue; }
          return false;
        }
      }
    }
    if (outline) {
      push(out, P_outline_width, w, imp);
      push(out, P_outline_style, s, imp);
      push(out, P_outline_color, c, imp);
      return true;
    }
    for (int k : sides) {
      push(out, W[k], w, imp);
      push(out, S[k], s, imp);
      push(out, C[k], c, imp);
    }
    return true;
  }
  if (name == "border-radius") {
    static const Prop R[4] = {P_border_top_left_radius, P_border_top_right_radius, P_border_bottom_right_radius, P_border_bottom_left_radius};
    if (isGlobal) return all({R[0], R[1], R[2], R[3]});
    LenSpec v[4];
    int n = 0;
    while (!ts.end() && n < 4 && !(ts.peek().t == TK_DELIM && ts.peek().c == '/')) {
      if (!parseLenTS(ts, LF_PCT, v[n])) return false;
      n++;
    }
    if (n == 0) return false;
    // ignore the elliptical "/ ..." part
    LenSpec r[4] = {v[0], n > 1 ? v[1] : v[0], n > 2 ? v[2] : v[0], n > 3 ? v[3] : (n > 1 ? v[1] : v[0])};
    for (int k = 0; k < 4; k++) push(out, R[k], lenVal(r[k]), imp);
    return true;
  }
  if (name == "background") {
    if (isGlobal) return all({P_background_color, P_background_image, P_background_size, P_background_position, P_background_repeat});
    Value color, image, size, pos, rep;
    color.type = VT_COLOR;
    color.color = {kTransparent, false};
    image.type = VT_KW;
    image.kw = 0;
    size.type = VT_KW;
    size.kw = BGS_AUTO;
    pos.type = VT_LEN2;
    pos.len = LenSpec::Pct(0);
    pos.ext().len2 = LenSpec::Pct(0);
    rep.type = VT_KW;
    rep.kw = BR_REPEAT;
    std::vector<BgLayer> layers;
    while (!ts.end()) {
      if (ts.comma()) continue;
      size_t save = ts.i;
      Value v;
      ColorSpec cs;
      if (ts.peek().t == TK_URL) {
        BgLayer L;
        L.url = ts.next().s;
        layers.push_back(L);
        continue;
      }
      std::shared_ptr<Gradient> gr;
      if (parseGradient(ts, gr)) {
        BgLayer L;
        L.grad = gr;
        layers.push_back(L);
        continue;
      }
      ts.i = save;
      if (ts.ident("none")) continue;
      if (parseColorTS(ts, cs)) {
        color.color = cs;
        continue;
      }
      if (parseLonghandTS(P_background_repeat, ts, v)) {
        rep = v;
        continue;
      }
      ts.i = save;
      if (parseLonghandTS(P_background_clip, ts, v)) continue;
      ts.i = save;
      if (ts.ident("fixed") || ts.ident("scroll") || ts.ident("local")) continue;
      LenSpec x, y;
      if (parsePosition(ts, x, y)) {
        pos.len = x;
        pos.ext().len2 = y;
        if (ts.delim('/')) {
          if (ts.ident("cover")) { size.type = VT_KW; size.kw = BGS_COVER; }
          else if (ts.ident("contain")) { size.type = VT_KW; size.kw = BGS_CONTAIN; }
          else {
            size.type = VT_LEN2;
            if (!parseLenTS(ts, LF_AUTO | LF_PCT, size.len)) return false;
            if (!parseLenTS(ts, LF_AUTO | LF_PCT, size.ext().len2)) size.ext().len2 = LenSpec::Kind(LK_AUTO);
          }
        }
        continue;
      }
      return false;
    }
    if (!layers.empty()) {
      image.type = VT_OBJ;
      image.ext().obj = box(std::move(layers));
    }
    push(out, P_background_color, color, imp);
    push(out, P_background_image, image, imp);
    push(out, P_background_size, size, imp);
    push(out, P_background_position, pos, imp);
    push(out, P_background_repeat, rep, imp);
    return true;
  }
  if (name == "font") {
    if (isGlobal) return all({P_font_style, P_font_weight, P_font_size, P_line_height, P_font_family});
    Value style, weight, size, lh, fam;
    style.type = VT_KW;
    style.kw = 0;
    weight.type = VT_NUM;
    weight.num = 400;
    lh.type = VT_KW;
    lh.kw = 0;
    // [style] [variant] [weight] size[/line-height] family
    while (!ts.end()) {
      if (ts.ident("normal")) continue;
      if (ts.ident("italic") || ts.ident("oblique")) { style.kw = 1; continue; }
      if (ts.ident("small-caps")) continue;
      if (ts.ident("bold")) { weight.num = 700; continue; }
      if (ts.ident("bolder")) { weight.type = VT_KW; weight.kw = 1; continue; }
      if (ts.ident("lighter")) { weight.type = VT_KW; weight.kw = 2; continue; }
      if (ts.peek().t == TK_NUM && ts.peek().n >= 1 && ts.peek().n <= 1000) { weight.num = ts.next().n; continue; }
      break;
    }
    if (!parseLonghandTS(P_font_size, ts, size)) return false;
    if (ts.delim('/')) {
      if (!parseLonghandTS(P_line_height, ts, lh)) return false;
    }
    if (!parseLonghandTS(P_font_family, ts, fam)) return false;
    push(out, P_font_style, style, imp);
    push(out, P_font_weight, weight, imp);
    push(out, P_font_size, size, imp);
    push(out, P_line_height, lh, imp);
    push(out, P_font_family, fam, imp);
    return true;
  }
  if (name == "flex") {
    if (isGlobal) return all({P_flex_grow, P_flex_shrink, P_flex_basis});
    Value grow, shrink, basis;
    grow.type = shrink.type = VT_NUM;
    if (ts.ident("none")) {
      grow.num = 0;
      shrink.num = 0;
      basis = lenVal(LenSpec::Kind(LK_AUTO));
    } else if (ts.ident("auto")) {
      grow.num = 1;
      shrink.num = 1;
      basis = lenVal(LenSpec::Kind(LK_AUTO));
    } else {
      grow.num = 1;
      shrink.num = 1;
      basis = lenVal(LenSpec::Px(0));
      bool gotGrow = false;
      while (!ts.end()) {
        if (!gotGrow && ts.peek().t == TK_NUM && !(ts.peek().n == 0 && ts.peek(1).t == TK_EOF && false)) {
          grow.num = ts.next().n;
          gotGrow = true;
          if (ts.peek().t == TK_NUM) shrink.num = ts.next().n;
          continue;
        }
        LenSpec l;
        if (parseLenTS(ts, LF_AUTO | LF_PCT | LF_CONTENT | LF_CONTENT_KW, l)) {
          basis = lenVal(l);
          continue;
        }
        return false;
      }
    }
    push(out, P_flex_grow, grow, imp);
    push(out, P_flex_shrink, shrink, imp);
    push(out, P_flex_basis, basis, imp);
    return true;
  }
  if (name == "flex-flow") {
    if (isGlobal) return all({P_flex_direction, P_flex_wrap});
    while (!ts.end()) {
      Value v;
      size_t save = ts.i;
      if (parseLonghandTS(P_flex_direction, ts, v)) { push(out, P_flex_direction, v, imp); continue; }
      ts.i = save;
      if (parseLonghandTS(P_flex_wrap, ts, v)) { push(out, P_flex_wrap, v, imp); continue; }
      return false;
    }
    return true;
  }
  if (name == "gap" || name == "grid-gap") {
    if (isGlobal) return all({P_row_gap, P_column_gap});
    LenSpec a, b;
    if (!parseLenTS(ts, LF_PCT | LF_NORMAL, a)) return false;
    if (!parseLenTS(ts, LF_PCT | LF_NORMAL, b)) b = a;
    push(out, P_row_gap, lenVal(a), imp);
    push(out, P_column_gap, lenVal(b), imp);
    return ts.end();
  }
  if (name == "place-items" || name == "place-content" || name == "place-self") {
    Prop a = name == "place-items" ? P_align_items : name == "place-content" ? P_align_content : P_align_self;
    Prop b = name == "place-items" ? P_justify_items : name == "place-content" ? P_justify_content : P_justify_self;
    if (isGlobal) return all({a, b});
    Value v1, v2;
    if (!parseLonghandTS(a, ts, v1)) return false;
    if (ts.end()) v2 = v1;
    else if (!parseLonghandTS(b, ts, v2)) return false;
    push(out, a, v1, imp);
    push(out, b, v2, imp);
    return true;
  }
  if (name == "overflow") {
    if (isGlobal) return all({P_overflow_x, P_overflow_y});
    Value a, b;
    if (!parseLonghandTS(P_overflow_x, ts, a)) return false;
    if (ts.end()) b = a;
    else if (!parseLonghandTS(P_overflow_y, ts, b)) return false;
    push(out, P_overflow_x, a, imp);
    push(out, P_overflow_y, b, imp);
    return true;
  }
  if (name == "transition" || name == "animation") {
    bool tr = name == "transition";
    if (isGlobal) {
      if (tr) return all({P_transition_property, P_transition_duration, P_transition_timing_function, P_transition_delay});
      return all({P_animation_name, P_animation_duration, P_animation_timing_function, P_animation_delay,
                  P_animation_iteration_count, P_animation_direction, P_animation_fill_mode, P_animation_play_state});
    }
    std::vector<Atom> props, names;
    std::vector<float> durs, delays, iters, dirs, fills, plays;
    std::vector<TimingFn> tims;
    while (!ts.end()) {
      Atom prop = 0, aname = 0;
      float dur = -1, delay = 0, iter = 1, dir = 0, fill = 0, play = 0;
      TimingFn tim;
      bool gotDur = false, gotProp = false;
      while (!ts.end() && ts.peek().t != TK_COMMA) {
        const Tok &k = ts.peek();
        float t;
        if (parseTime(k, t) && (k.t == TK_DIM || gotDur || k.n == 0)) {
          ts.i++;
          if (!gotDur) {
            dur = t;
            gotDur = true;
          } else {
            delay = t;
          }
          continue;
        }
        TimingFn f;
        if (parseTimingTS(ts, f)) {
          tim = f;
          continue;
        }
        if (!tr) {
          int kv;
          if (ts.ident("infinite")) { iter = INFINITY; continue; }
          if (k.t == TK_NUM) { iter = ts.next().n; continue; }
          if (kwLookup(k, {{"normal", 0}, {"reverse", 1}, {"alternate", 2}, {"alternate-reverse", 3}}, kv)) { ts.i++; dir = (float)kv; continue; }
          if (kwLookup(k, {{"none", 0}, {"forwards", 1}, {"backwards", 2}, {"both", 3}}, kv) && !(kv == 0 && !aname)) { ts.i++; fill = (float)kv; continue; }
          if (kwLookup(k, {{"running", 0}, {"paused", 1}}, kv)) { ts.i++; play = (float)kv; continue; }
          if (k.t == TK_IDENT || k.t == TK_STRING) { aname = iequals(k.s, "none") ? 0 : intern(k.s); ts.i++; continue; }
          return false;
        }
        if (k.t == TK_IDENT && !gotProp) {
          std::string n = toLower(k.s);
          prop = n == "all" ? 0 : (n == "none" ? (Atom)0xFFFFFFFF : intern(n));
          gotProp = true;
          ts.i++;
          continue;
        }
        if (k.t == TK_IDENT && iequals(k.s, "allow-discrete")) { ts.i++; continue; }
        return false;
      }
      if (dur < 0) dur = 0;
      if (tr) {
        props.push_back(prop);
      } else {
        names.push_back(aname);
        iters.push_back(iter);
        dirs.push_back(dir);
        fills.push_back(fill);
        plays.push_back(play);
      }
      durs.push_back(dur);
      delays.push_back(delay);
      tims.push_back(tim);
      ts.comma();
    }
    auto objv = [&](auto vec) {
      Value v;
      v.type = VT_OBJ;
      v.ext().obj = box(std::move(vec));
      return v;
    };
    if (tr) {
      push(out, P_transition_property, objv(props), imp);
      push(out, P_transition_duration, objv(durs), imp);
      push(out, P_transition_timing_function, objv(tims), imp);
      push(out, P_transition_delay, objv(delays), imp);
    } else {
      push(out, P_animation_name, objv(names), imp);
      push(out, P_animation_duration, objv(durs), imp);
      push(out, P_animation_timing_function, objv(tims), imp);
      push(out, P_animation_delay, objv(delays), imp);
      push(out, P_animation_iteration_count, objv(iters), imp);
      push(out, P_animation_direction, objv(dirs), imp);
      push(out, P_animation_fill_mode, objv(fills), imp);
      push(out, P_animation_play_state, objv(plays), imp);
    }
    return true;
  }
  if (name == "grid-column" || name == "grid-row") {
    Prop a = name == "grid-column" ? P_grid_column_start : P_grid_row_start;
    Prop b = name == "grid-column" ? P_grid_column_end : P_grid_row_end;
    if (isGlobal) return all({a, b});
    Value v1, v2;
    if (!parseLonghandTS(a, ts, v1)) return false;
    if (ts.delim('/')) {
      if (!parseLonghandTS(b, ts, v2)) return false;
    } else {
      v2.type = VT_LEN2;
      v2.kw = 0;
      v2.num = 0;
    }
    push(out, a, v1, imp);
    push(out, b, v2, imp);
    return ts.end();
  }
  if (name == "grid-area") {
    Prop ps[4] = {P_grid_row_start, P_grid_column_start, P_grid_row_end, P_grid_column_end};
    if (isGlobal) return all({ps[0], ps[1], ps[2], ps[3]});
    Value v[4];
    for (int k = 0; k < 4; k++) {
      v[k].type = VT_LEN2;
      v[k].kw = 0;
      v[k].num = 0;
    }
    for (int k = 0; k < 4 && !ts.end(); k++) {
      if (!parseLonghandTS(ps[k], ts, v[k])) return false;
      if (!ts.delim('/')) break;
    }
    for (int k = 0; k < 4; k++) push(out, ps[k], v[k], imp);
    return true;
  }
  if (name == "grid-template") {
    // rows / columns
    std::vector<Tok> rows, cols;
    bool slash = false;
    for (auto &k : toks) {
      if (k.t == TK_DELIM && k.c == '/') {
        slash = true;
        continue;
      }
      (slash ? cols : rows).push_back(k);
    }
    Value r, c;
    if (!parseLonghand(P_grid_template_rows, rows, r)) return false;
    push(out, P_grid_template_rows, r, imp);
    if (slash) {
      if (!parseLonghand(P_grid_template_columns, cols, c)) return false;
      push(out, P_grid_template_columns, c, imp);
    }
    return true;
  }
  if (name == "list-style") {
    if (isGlobal) return all({P_list_style_type, P_list_style_position});
    while (!ts.end()) {
      Value v;
      size_t save = ts.i;
      if (parseLonghandTS(P_list_style_type, ts, v)) { push(out, P_list_style_type, v, imp); continue; }
      ts.i = save;
      if (parseLonghandTS(P_list_style_position, ts, v)) { push(out, P_list_style_position, v, imp); continue; }
      ts.i = save;
      if (ts.peek().t == TK_URL) { ts.i++; continue; }
      return false;
    }
    return true;
  }
  if (name == "text-decoration") {
    if (isGlobal) return all({P_text_decoration_line, P_text_decoration_color});
    int flags = 0;
    bool any = false;
    while (!ts.end()) {
      if (ts.ident("none")) { any = true; continue; }
      if (ts.ident("underline")) { flags |= TD_UNDERLINE; any = true; continue; }
      if (ts.ident("overline")) { flags |= TD_OVERLINE; any = true; continue; }
      if (ts.ident("line-through")) { flags |= TD_LINE_THROUGH; any = true; continue; }
      if (ts.ident("solid") || ts.ident("double") || ts.ident("dotted") || ts.ident("dashed") || ts.ident("wavy")) continue;
      Value c;
      size_t save = ts.i;
      if (parseLonghandTS(P_text_decoration_color, ts, c)) { push(out, P_text_decoration_color, c, imp); continue; }
      ts.i = save;
      LenSpec l;
      if (parseLenTS(ts, 0, l)) continue;  // thickness
      return false;
    }
    Value v;
    v.type = VT_KW;
    v.kw = flags;
    if (any || flags) push(out, P_text_decoration_line, v, imp);
    return true;
  }
  if (name == "scroll-snap-type" || name == "border-spacing") return false;
  return false;
}

static bool isShorthand(const std::string &n) {
  static const char *names[] = {"margin", "padding", "inset", "border", "border-top", "border-right", "border-bottom",
                                "border-left", "border-width", "border-style", "border-color", "border-radius",
                                "background", "font", "flex", "flex-flow", "gap", "grid-gap", "place-items",
                                "place-content", "place-self", "overflow", "transition", "animation", "grid-column",
                                "grid-row", "grid-area", "grid-template", "list-style", "outline", "text-decoration",
                                "margin-block", "margin-inline", "padding-block", "padding-inline", "border-block",
                                "border-inline", "border-block-start", "border-block-end", "border-inline-start",
                                "border-inline-end"};
  for (auto s : names)
    if (n == s) return true;
  return false;
}

// Map logical longhands to physical (horizontal-tb, ltr).
static std::string physicalName(const std::string &n) {
  static const std::pair<const char *, const char *> map[] = {
      {"margin-block-start", "margin-top"}, {"margin-block-end", "margin-bottom"},
      {"margin-inline-start", "margin-left"}, {"margin-inline-end", "margin-right"},
      {"padding-block-start", "padding-top"}, {"padding-block-end", "padding-bottom"},
      {"padding-inline-start", "padding-left"}, {"padding-inline-end", "padding-right"},
      {"inline-size", "width"}, {"block-size", "height"}, {"min-inline-size", "min-width"},
      {"min-block-size", "min-height"}, {"max-inline-size", "max-width"}, {"max-block-size", "max-height"},
      {"inset-block-start", "top"}, {"inset-block-end", "bottom"}, {"inset-inline-start", "left"},
      {"inset-inline-end", "right"}, {"border-start-start-radius", "border-top-left-radius"},
      {"border-start-end-radius", "border-top-right-radius"}, {"border-end-start-radius", "border-bottom-left-radius"},
      {"border-end-end-radius", "border-bottom-right-radius"}, {"row-gap", "row-gap"},
      {"-webkit-box-orient", ""}, {"-webkit-box-shadow", "box-shadow"}, {"-webkit-transform", "transform"},
      {"-webkit-transition", "transition"}, {"-webkit-animation", "animation"},
      {"-webkit-border-radius", "border-radius"}, {"-webkit-flex", "flex"},
      {"-webkit-box-flex", "flex-grow"}, {"-webkit-font-smoothing", ""}, {"-moz-osx-font-smoothing", ""},
      {"-webkit-tap-highlight-color", ""}, {"text-rendering", ""}, {"will-change", ""}, {"contain", ""},
      {"touch-action", ""}, {"resize", ""}, {"isolation", ""}, {"mix-blend-mode", ""}, {"backdrop-filter", ""},
      {"-webkit-backdrop-filter", ""}, {"scroll-behavior", ""}, {"overscroll-behavior", ""},
      {"font-feature-settings", ""}, {"font-variant", ""}, {"text-size-adjust", ""},
      {"-webkit-text-size-adjust", ""}, {"speak", ""}, {"quotes", ""}, {"counter-reset", ""},
      {"counter-increment", ""}, {"print-color-adjust", ""}, {"color-scheme", ""}, {"forced-color-adjust", ""},
      {"text-wrap", ""}, {"text-wrap-style", ""}, {"hyphens", ""}, {"tab-size", ""}, {"orphans", ""},
      {"widows", ""}, {"page-break-inside", ""}, {"break-inside", ""}, {"content-visibility", ""},
      {"scrollbar-width", ""}, {"scrollbar-color", ""}, {"scrollbar-gutter", ""}, {"clip-path", ""},
      {"mask", ""}, {"-webkit-mask", ""}, {"perspective", ""}, {"backface-visibility", ""},
      {"transform-style", ""}, {"text-underline-offset", ""}, {"text-decoration-thickness", ""},
      {"text-decoration-style", ""}, {"font-optical-sizing", ""}, {"font-display", ""},
      {"unicode-bidi", ""}, {"direction", ""}, {"writing-mode", ""}, {"zoom", ""},
      {"outline-style", "outline-style"}};
  for (auto &p : map)
    if (n == p.first) return p.second;
  return n;
}

static bool tokensHaveVar(const std::vector<Tok> &t) { return hasVar(t); }

static void parseOneDecl(const std::string &nameIn, const std::vector<Tok> &valToks, bool imp, std::vector<Decl> &out,
                         bool keepText = true) {
  std::string name = toLower(nameIn);
  if (startsWith(name, "--")) {
    Decl d;
    d.prop = P_CUSTOM;
    d.customName = intern(nameIn);  // custom property names are case-sensitive
    d.important = imp;
    d.v.type = VT_RAW;
    d.v.ext().str = tokensToString(valToks, 0, valToks.size());
    out.push_back(std::move(d));
    return;
  }
  name = physicalName(name);
  if (name.empty()) return;
  if (tokensHaveVar(valToks)) {
    Decl d;
    d.important = imp;
    d.v.type = VT_RAW;
    d.v.ext().str = tokensToString(valToks, 0, valToks.size());
    if (isShorthand(name)) {
      d.prop = P_SHORTHAND_RAW;
      d.customName = intern(name);
    } else {
      int p = propByName(name);
      if (p < 0) return;
      d.prop = (uint16_t)p;
    }
    out.push_back(std::move(d));
    return;
  }
  if (isShorthand(name)) {
    expandShorthand(name, valToks, out, imp);
    return;
  }
  int p = propByName(name);
  if (p < 0) return;
  Value v;
  if (!parseLonghand((Prop)p, valToks, v)) return;
  if (keepText && (v.type == VT_KW || v.type == VT_OBJ)) v.ext().str = tokensToString(valToks, 0, valToks.size());
  Decl d;
  d.prop = (uint16_t)p;
  d.important = imp;
  d.v = std::move(v);
  out.push_back(std::move(d));
}

bool expandShorthandPublic(const std::string &name, const std::vector<Tok> &toks, std::vector<Decl> &out, bool imp) {
  if (isShorthand(name)) return expandShorthand(name, toks, out, imp);
  int p = propByName(name);
  if (p < 0) return false;
  Value v;
  if (!parseLonghand((Prop)p, toks, v)) return false;
  Decl d;
  d.prop = (uint16_t)p;
  d.important = imp;
  d.v = std::move(v);
  out.push_back(std::move(d));
  return true;
}

void DeclBlock::set(Decl d) {
  for (auto &e : decls) {
    if (e.prop == d.prop && e.customName == d.customName) {
      e = std::move(d);
      return;
    }
  }
  decls.push_back(std::move(d));
}

const Decl *DeclBlock::find(uint16_t prop, Atom custom) const {
  for (auto it = decls.rbegin(); it != decls.rend(); ++it)
    if (it->prop == prop && it->customName == custom) return &*it;
  return nullptr;
}

bool DeclBlock::remove(uint16_t prop, Atom custom) {
  bool any = false;
  for (size_t i = 0; i < decls.size();) {
    if (decls[i].prop == prop && decls[i].customName == custom) {
      decls.erase(decls.begin() + i);
      any = true;
    } else {
      i++;
    }
  }
  return any;
}

void parseDeclarations(const char *s, size_t n, DeclBlock &out, bool sheet) {
  std::vector<Tok> toks;
  cssTokenize(s, n, toks);
  size_t i = 0;
  while (i < toks.size()) {
    while (i < toks.size() && (toks[i].t == TK_WS || toks[i].t == TK_SEMI)) i++;
    if (i >= toks.size()) break;
    if (toks[i].t != TK_IDENT) {  // skip to next ';'
      int depth = 0;
      while (i < toks.size() && !(toks[i].t == TK_SEMI && depth == 0)) {
        if (toks[i].t == TK_LBRACE || toks[i].t == TK_FUNC || toks[i].t == TK_LPAREN) depth++;
        if (toks[i].t == TK_RBRACE || toks[i].t == TK_RPAREN) depth--;
        i++;
      }
      continue;
    }
    std::string name = toks[i].s;
    i++;
    while (i < toks.size() && toks[i].t == TK_WS) i++;
    if (i >= toks.size() || toks[i].t != TK_COLON) {
      while (i < toks.size() && toks[i].t != TK_SEMI) i++;
      continue;
    }
    i++;
    size_t vs = i;
    int depth = 0;
    while (i < toks.size()) {
      TokType t = toks[i].t;
      if (t == TK_FUNC || t == TK_LPAREN || t == TK_LBRACK || t == TK_LBRACE) depth++;
      else if (t == TK_RPAREN || t == TK_RBRACK || t == TK_RBRACE) depth--;
      else if (t == TK_SEMI && depth <= 0) break;
      i++;
    }
    std::vector<Tok> val(toks.begin() + vs, toks.begin() + i);
    // !important
    bool imp = false;
    while (!val.empty() && val.back().t == TK_WS) val.pop_back();
    if (val.size() >= 2 && val.back().t == TK_IDENT && iequals(val.back().s, "important")) {
      size_t k = val.size() - 2;
      while (k > 0 && val[k].t == TK_WS) k--;
      if (val[k].t == TK_DELIM && val[k].c == '!') {
        imp = true;
        val.resize(k);
      }
    }
    std::vector<Decl> decls;
    parseOneDecl(name, val, imp, decls, !sheet);
    for (auto &d : decls) out.set(std::move(d));
  }
  out.decls.shrink_to_fit();  // a sheet holds thousands of these: no room to grow
}

bool parsePropertyValue(const std::string &name, const std::string &value, std::vector<Decl> &out, bool important) {
  std::vector<Tok> toks;
  cssTokenize(value.data(), value.size(), toks);
  size_t before = out.size();
  parseOneDecl(name, toks, important, out);
  return out.size() > before;
}

// ============================================================ serialization (for getComputedStyle / style.x)

static std::string lenStr(const LenSpec &l) {
  switch (l.kind) {
    case LK_AUTO: return "auto";
    case LK_NONE: return "none";
    case LK_NORMAL: return "normal";
    case LK_MIN_CONTENT: return "min-content";
    case LK_MAX_CONTENT: return "max-content";
    case LK_FIT_CONTENT: return "fit-content";
    case LK_CONTENT: return "content";
    default: break;
  }
  if (l.calc) return "calc(...)";
  std::string parts;
  int n = 0;
  auto add = [&](float v, const char *u) {
    if (v == 0) return;
    if (n++) parts += v < 0 ? " - " : " + ";
    else if (v < 0) parts += "-";
    parts += numStr(std::fabs(v)) + u;
  };
  add(l.px, "px");
  add(l.pct, "%");
  add(l.em, "em");
  add(l.rem, "rem");
  add(l.vw, "vw");
  add(l.vh, "vh");
  add(l.ch, "ch");
  if (n == 0) return "0px";
  if (n > 1) return "calc(" + parts + ")";
  return parts;
}

static std::string colorStr(const ColorSpec &c) {
  if (c.current) return "currentcolor";
  char b[64];
  if (c.c.a == 255) snprintf(b, sizeof b, "rgb(%d, %d, %d)", c.c.r, c.c.g, c.c.b);
  else snprintf(b, sizeof b, "rgba(%d, %d, %d, %g)", c.c.r, c.c.g, c.c.b, c.c.a / 255.0);
  return b;
}

const ValueExt Value::kNoExt;

ValueExt &Value::ext() {
  if (!x_) x_ = std::make_shared<ValueExt>();
  else if (x_.use_count() > 1) x_ = std::make_shared<ValueExt>(*x_);  // shared with a copy: our own now
  return *x_;
}

std::string serializeValue(Prop p, const Value &v) {
  switch (v.type) {
    case VT_INHERIT: return "inherit";
    case VT_INITIAL: return "initial";
    case VT_UNSET: return "unset";
    case VT_RAW: return v.str();
    case VT_LEN: return lenStr(v.len);
    case VT_LEN2: return lenStr(v.len) + " " + lenStr(v.len2());
    case VT_NUM: return numStr(v.num);
    case VT_COLOR: return colorStr(v.color);
    case VT_STR: return v.str();
    default: break;
  }
  (void)p;
  return v.str();
}

}  // namespace rt
