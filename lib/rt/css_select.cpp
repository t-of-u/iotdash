// Selector parsing/matching and stylesheet parsing.
#include "css.h"

namespace rt {

// ============================================================ selector parsing

namespace {

struct SP {
  const std::string &s;
  size_t i = 0;
  bool end() const { return i >= s.size(); }
  char peek(size_t k = 0) const { return i + k < s.size() ? s[i + k] : 0; }
  void ws() {
    while (!end() && isSpace(s[i])) i++;
  }
  static bool identChar(char c) { return isalnum((unsigned char)c) || c == '-' || c == '_' || (unsigned char)c >= 0x80; }
  std::string ident() {
    std::string r;
    while (!end()) {
      char c = s[i];
      if (identChar(c)) {
        r += c;
        i++;
      } else if (c == '\\' && i + 1 < s.size()) {
        r += s[i + 1];
        i += 2;
      } else {
        break;
      }
    }
    return r;
  }
  // Text inside balanced parentheses (cursor after '(').
  std::string parenArg() {
    int d = 1;
    size_t st = i;
    char q = 0;
    while (!end()) {
      char c = s[i];
      if (q) {
        if (c == q) q = 0;
      } else if (c == '"' || c == '\'') {
        q = c;
      } else if (c == '(') {
        d++;
      } else if (c == ')') {
        if (--d == 0) break;
      }
      i++;
    }
    std::string r = s.substr(st, i - st);
    if (!end()) i++;
    return r;
  }
};

bool parseNth(std::string a, int &A, int &B) {
  a = trim(toLower(a));
  std::string t;
  for (char c : a)
    if (!isSpace(c)) t += c;
  if (t == "odd") { A = 2; B = 1; return true; }
  if (t == "even") { A = 2; B = 0; return true; }
  size_t n = t.find('n');
  if (n == std::string::npos) {
    char *e;
    long v = strtol(t.c_str(), &e, 10);
    if (*e) return false;
    A = 0;
    B = (int)v;
    return true;
  }
  std::string as = t.substr(0, n), bs = t.substr(n + 1);
  if (as.empty() || as == "+") A = 1;
  else if (as == "-") A = -1;
  else {
    char *e;
    A = (int)strtol(as.c_str(), &e, 10);
    if (*e) return false;
  }
  if (bs.empty()) B = 0;
  else {
    char *e;
    B = (int)strtol(bs.c_str(), &e, 10);
    if (*e) return false;
  }
  return true;
}

bool parseSelector(const std::string &text, Selector &out, bool relative);

uint32_t maxSpec(const SelectorList &l) {
  uint32_t m = 0;
  for (auto &s : l.sels) m = std::max(m, s.specificity);
  return m;
}

bool parseSubList(const std::string &arg, std::shared_ptr<SelectorList> &out, bool relative) {
  out = std::make_shared<SelectorList>();
  // split on top-level commas
  int d = 0;
  size_t st = 0;
  for (size_t i = 0; i <= arg.size(); i++) {
    char c = i < arg.size() ? arg[i] : ',';
    if (c == '(' || c == '[') d++;
    else if (c == ')' || c == ']') d--;
    else if (c == ',' && d == 0) {
      Selector s;
      std::string part = trim(arg.substr(st, i - st));
      if (!part.empty() && parseSelector(part, s, relative)) out->sels.push_back(std::move(s));
      st = i + 1;
    }
  }
  return !out->sels.empty();
}

bool parseCompound(SP &p, Compound &c, uint32_t &spec, PseudoElem &pe) {
  bool any = false;
  if (p.peek() == '*') {
    p.i++;
    c.simple.push_back({SM_UNIVERSAL});
    any = true;
  } else if (SP::identChar(p.peek()) && !isdigit((unsigned char)p.peek())) {
    std::string t = p.ident();
    SimpleSel s{SM_TAG};
    s.a = internLower(t);
    c.simple.push_back(s);
    spec += 1;
    any = true;
  }
  while (!p.end()) {
    char ch = p.peek();
    if (ch == '#') {
      p.i++;
      SimpleSel s{SM_ID};
      s.a = intern(p.ident());
      c.simple.push_back(s);
      spec += 1 << 20;
    } else if (ch == '.') {
      p.i++;
      SimpleSel s{SM_CLASS};
      s.a = intern(p.ident());
      c.simple.push_back(s);
      spec += 1 << 10;
    } else if (ch == '[') {
      p.i++;
      p.ws();
      std::string name = p.ident();
      p.ws();
      SimpleSel s{SM_ATTR_EXISTS};
      s.a = internLower(name);
      char op = p.peek();
      if (op == ']') {
        p.i++;
      } else {
        if (op == '=') {
          s.m = SM_ATTR_EQ;
          p.i++;
        } else if (p.peek(1) == '=') {
          switch (op) {
            case '~': s.m = SM_ATTR_INCLUDES; break;
            case '|': s.m = SM_ATTR_DASH; break;
            case '^': s.m = SM_ATTR_PREFIX; break;
            case '$': s.m = SM_ATTR_SUFFIX; break;
            case '*': s.m = SM_ATTR_SUBSTR; break;
            default: return false;
          }
          p.i += 2;
        } else {
          return false;
        }
        p.ws();
        if (p.peek() == '"' || p.peek() == '\'') {
          char q = p.peek();
          p.i++;
          while (!p.end() && p.peek() != q) s.val += p.s[p.i++];
          p.i++;
        } else {
          s.val = p.ident();
        }
        p.ws();
        if (p.peek() == 'i' || p.peek() == 'I') {
          s.icase = true;
          p.i++;
          p.ws();
        } else if (p.peek() == 's' || p.peek() == 'S') {
          p.i++;
          p.ws();
        }
        if (p.peek() != ']') return false;
        p.i++;
      }
      c.simple.push_back(s);
      spec += 1 << 10;
    } else if (ch == ':') {
      p.i++;
      bool isElem = false;
      if (p.peek() == ':') {
        isElem = true;
        p.i++;
      }
      std::string name = toLower(p.ident());
      std::string arg;
      bool fn = false;
      if (p.peek() == '(') {
        p.i++;
        arg = p.parenArg();
        fn = true;
      }
      // legacy single-colon pseudo-elements
      if (!isElem && (name == "before" || name == "after" || name == "first-line" || name == "first-letter")) isElem = true;
      if (isElem) {
        if (name == "before") pe = PE_BEFORE;
        else if (name == "after") pe = PE_AFTER;
        else if (name == "placeholder" || name == "-webkit-input-placeholder" || name == "-moz-placeholder") pe = PE_PLACEHOLDER;
        else if (name == "selection" || name == "-moz-selection") pe = PE_SELECTION;
        else if (name == "marker") pe = PE_MARKER;
        else if (name == "backdrop") pe = PE_BACKDROP;
        else if (name == "-webkit-slider-thumb" || name == "-moz-range-thumb") pe = PE_THUMB;
        else if (name == "-webkit-slider-runnable-track" || name == "-moz-range-track") pe = PE_TRACK;
        else pe = PE_UNKNOWN;
        spec += 1;
        continue;
      }
      SimpleSel s{SM_PSEUDO};
      if (fn) {
        if (name == "not" || name == "is" || name == "where" || name == "matches" || name == "-webkit-any" || name == "any") {
          s.m = name == "not" ? SM_NOT : SM_IS;
          if (!parseSubList(arg, s.sub, false)) {
            if (name == "not") return false;
            s.pc = PC_NEVER;
            s.m = SM_PSEUDO;
          }
          if (name != "where" && s.sub) spec += maxSpec(*s.sub);
        } else if (name == "has") {
          s.m = SM_HAS;
          if (!parseSubList(arg, s.sub, true)) return false;
          spec += maxSpec(*s.sub);
        } else if (name == "nth-child" || name == "nth-last-child" || name == "nth-of-type" || name == "nth-last-of-type") {
          s.m = name == "nth-child" ? SM_NTH_CHILD : name == "nth-last-child" ? SM_NTH_LAST_CHILD
              : name == "nth-of-type" ? SM_NTH_OF_TYPE : SM_NTH_LAST_OF_TYPE;
          std::string a = arg;
          size_t of = toLower(a).find(" of ");
          if (of != std::string::npos) {
            parseSubList(a.substr(of + 4), s.sub, false);
            a = a.substr(0, of);
          }
          if (!parseNth(a, s.nthA, s.nthB)) return false;
          spec += 1 << 10;
        } else if (name == "lang" || name == "dir") {
          s.pc = PC_SCOPE;  // always true-ish: matched specially below
          s.m = SM_IS;
          s.sub = nullptr;
          spec += 1 << 10;
        } else {
          s.pc = PC_NEVER;
        }
        c.simple.push_back(s);
        continue;
      }
      static const struct {
        const char *n;
        PseudoClass pc;
      } pcs[] = {{"active", PC_ACTIVE}, {"hover", PC_HOVER}, {"focus", PC_FOCUS}, {"focus-within", PC_FOCUS_WITHIN},
                 {"focus-visible", PC_FOCUS_VISIBLE}, {"checked", PC_CHECKED}, {"disabled", PC_DISABLED},
                 {"enabled", PC_ENABLED}, {"first-child", PC_FIRST_CHILD}, {"last-child", PC_LAST_CHILD},
                 {"only-child", PC_ONLY_CHILD}, {"first-of-type", PC_FIRST_OF_TYPE}, {"last-of-type", PC_LAST_OF_TYPE},
                 {"only-of-type", PC_ONLY_OF_TYPE}, {"root", PC_ROOT}, {"empty", PC_EMPTY}, {"target", PC_TARGET},
                 {"placeholder-shown", PC_PLACEHOLDER_SHOWN}, {"invalid", PC_INVALID}, {"valid", PC_VALID},
                 {"user-invalid", PC_INVALID}, {"user-valid", PC_VALID},
                 {"required", PC_REQUIRED}, {"optional", PC_OPTIONAL}, {"read-only", PC_READ_ONLY},
                 {"read-write", PC_READ_WRITE}, {"link", PC_LINK}, {"any-link", PC_LINK}, {"visited", PC_VISITED},
                 {"open", PC_OPEN}, {"indeterminate", PC_INDETERMINATE}, {"default", PC_DEFAULT}, {"scope", PC_SCOPE},
                 {"defined", PC_SCOPE}, {"in-range", PC_SCOPE}, {"out-of-range", PC_NEVER}, {"modal", PC_OPEN},
                 {"popover-open", PC_OPEN}, {"autofill", PC_NEVER}, {"-webkit-autofill", PC_NEVER}};
      bool found = false;
      for (auto &e : pcs) {
        if (name == e.n) {
          s.pc = e.pc;
          found = true;
          break;
        }
      }
      if (!found) s.pc = PC_NEVER;
      if (name == "defined" || name == "in-range") {  // always-true pseudo classes
        s.m = SM_IS;
        s.sub = nullptr;
      }
      c.simple.push_back(s);
      spec += 1 << 10;
    } else {
      break;
    }
    any = true;
  }
  return any || !c.simple.empty() || pe != PE_NONE;
}

bool parseSelector(const std::string &text, Selector &out, bool relative) {
  SP p{text};
  p.ws();
  Combinator comb = CB_NONE;
  uint32_t spec = 0;
  PseudoElem pe = PE_NONE;
  if (relative) {
    Compound scope;
    SimpleSel s{SM_PSEUDO};
    s.pc = PC_SCOPE;
    scope.simple.push_back(s);
    out.parts.push_back(scope);
    comb = CB_DESC;
    char c = p.peek();
    if (c == '>' || c == '+' || c == '~') {
      comb = c == '>' ? CB_CHILD : c == '+' ? CB_ADJ : CB_SIB;
      p.i++;
      p.ws();
    }
  }
  while (!p.end()) {
    Compound c;
    if (pe != PE_NONE) return false;  // nothing may follow a pseudo-element
    if (!parseCompound(p, c, spec, pe)) return false;
    c.comb = comb;
    out.parts.push_back(std::move(c));
    bool sawWs = false;
    while (!p.end() && isSpace(p.peek())) {
      p.i++;
      sawWs = true;
    }
    if (p.end()) break;
    char ch = p.peek();
    if (ch == '>' || ch == '+' || ch == '~') {
      comb = ch == '>' ? CB_CHILD : ch == '+' ? CB_ADJ : CB_SIB;
      p.i++;
      p.ws();
    } else if (sawWs) {
      comb = CB_DESC;
    } else {
      return false;
    }
  }
  if (out.parts.empty()) return false;
  out.specificity = spec;
  out.pseudo = pe;
  return pe != PE_UNKNOWN;
}

}  // namespace

bool parseSelectorList(const std::string &s, SelectorList &out) {
  int d = 0;
  size_t st = 0;
  char q = 0;
  for (size_t i = 0; i <= s.size(); i++) {
    char c = i < s.size() ? s[i] : ',';
    if (q) {
      if (c == q) q = 0;
      continue;
    }
    if (c == '"' || c == '\'') q = c;
    else if (c == '(' || c == '[') d++;
    else if (c == ')' || c == ']') d--;
    else if (c == ',' && d == 0) {
      Selector sel;
      std::string part = trim(s.substr(st, i - st));
      if (part.empty() || !parseSelector(part, sel, false)) return false;
      out.sels.push_back(std::move(sel));
      st = i + 1;
    }
  }
  return !out.sels.empty();
}

// ============================================================ matching

static bool isFormControl(Element *e) {
  Tag t = e->tagId();
  return t == TAG_input || t == TAG_button || t == TAG_select || t == TAG_textarea || t == TAG_option ||
         t == TAG_fieldset || t == TAG_optgroup;
}

static bool nthMatch(int a, int b, int idx) {  // idx is 1-based
  if (a == 0) return idx == b;
  int d = idx - b;
  if (d % a != 0) return false;
  return d / a >= 0;
}

static bool matchCompound(const Compound &c, Element *e, Element *scope);

static bool matchFrom(const Selector &s, int idx, Element *e, Element *scope) {
  if (!matchCompound(s.parts[idx], e, scope)) return false;
  if (idx == 0) return true;
  switch (s.parts[idx].comb) {
    case CB_CHILD: {
      Element *p = e->parentElement();
      return p && matchFrom(s, idx - 1, p, scope);
    }
    case CB_DESC:
    case CB_NONE: {
      for (Element *p = e->parentElement(); p; p = p->parentElement())
        if (matchFrom(s, idx - 1, p, scope)) return true;
      return false;
    }
    case CB_ADJ: {
      Element *p = e->prevElementSibling();
      return p && matchFrom(s, idx - 1, p, scope);
    }
    case CB_SIB: {
      for (Element *p = e->prevElementSibling(); p; p = p->prevElementSibling())
        if (matchFrom(s, idx - 1, p, scope)) return true;
      return false;
    }
  }
  return false;
}

static bool attrMatch(const SimpleSel &s, Element *e) {
  const std::string *v = e->getAttr(s.a);
  if (!v) return false;
  if (s.m == SM_ATTR_EXISTS) return true;
  std::string val = *v, want = s.val;
  if (s.icase) {
    val = toLower(val);
    want = toLower(want);
  }
  switch (s.m) {
    case SM_ATTR_EQ: return val == want;
    case SM_ATTR_INCLUDES: {
      for (auto &w : split(val, ' '))
        if (w == want) return true;
      return false;
    }
    case SM_ATTR_DASH: return val == want || startsWith(val, (want + "-").c_str());
    case SM_ATTR_PREFIX: return !want.empty() && startsWith(val, want.c_str());
    case SM_ATTR_SUFFIX: return !want.empty() && val.size() >= want.size() && val.compare(val.size() - want.size(), want.size(), want) == 0;
    case SM_ATTR_SUBSTR: return !want.empty() && val.find(want) != std::string::npos;
    default: return false;
  }
}

static bool isEmptyEl(Element *e) {
  for (Node *c = e->first; c; c = c->next) {
    if (c->isElement()) return false;
    if (c->isText() && !((Text *)c)->data.empty()) return false;
  }
  return true;
}

static bool pseudoMatch(uint8_t pc, Element *e, Element *scope) {
  switch (pc) {
    case PC_ACTIVE: return e->state & ST_ACTIVE;
    case PC_HOVER: return e->state & (ST_HOVER | ST_ACTIVE);
    case PC_FOCUS: return e->state & ST_FOCUS;
    case PC_FOCUS_WITHIN: return e->state & (ST_FOCUS | ST_FOCUS_WITHIN);
    case PC_FOCUS_VISIBLE: return e->state & ST_FOCUS_VISIBLE;
    case PC_CHECKED: return e->state & ST_CHECKED;
    case PC_DISABLED: return e->state & ST_DISABLED;
    case PC_ENABLED: return isFormControl(e) && !(e->state & ST_DISABLED);
    case PC_FIRST_CHILD: return !e->prevElementSibling();
    case PC_LAST_CHILD: return !e->nextElementSibling();
    case PC_ONLY_CHILD: return !e->prevElementSibling() && !e->nextElementSibling();
    case PC_FIRST_OF_TYPE:
      for (Element *s = e->prevElementSibling(); s; s = s->prevElementSibling())
        if (s->tag == e->tag) return false;
      return true;
    case PC_LAST_OF_TYPE:
      for (Element *s = e->nextElementSibling(); s; s = s->nextElementSibling())
        if (s->tag == e->tag) return false;
      return true;
    case PC_ONLY_OF_TYPE:
      return pseudoMatch(PC_FIRST_OF_TYPE, e, scope) && pseudoMatch(PC_LAST_OF_TYPE, e, scope);
    case PC_ROOT: return e->doc && e == e->doc->html;
    case PC_EMPTY: return isEmptyEl(e);
    case PC_TARGET: return e->state & ST_TARGET;
    case PC_PLACEHOLDER_SHOWN: return e->state & ST_PLACEHOLDER;
    case PC_INVALID: return e->state & ST_INVALID;
    case PC_VALID: return isFormControl(e) && !(e->state & ST_INVALID);
    case PC_REQUIRED: return isFormControl(e) && e->hasAttr(A.required);
    case PC_OPTIONAL: return isFormControl(e) && !e->hasAttr(A.required);
    case PC_READ_WRITE: {
      Tag t = e->tagId();
      if ((t == TAG_input || t == TAG_textarea) && !e->hasAttr(A.readonly) && !(e->state & ST_DISABLED)) return true;
      return e->hasAttr(A.contenteditable);
    }
    case PC_READ_ONLY: return !pseudoMatch(PC_READ_WRITE, e, scope);
    case PC_LINK: return e->tagId() == TAG_a && e->hasAttr(A.href);
    case PC_VISITED: return false;
    case PC_OPEN: return e->state & ST_OPEN;
    case PC_INDETERMINATE: return e->state & ST_INDETERMINATE;
    case PC_DEFAULT: return false;
    case PC_SCOPE: return scope ? e == scope : (e->doc && e == e->doc->html);
    case PC_NEVER: return false;
  }
  return false;
}

static bool matchCompound(const Compound &c, Element *e, Element *scope) {
  for (const SimpleSel &s : c.simple) {
    switch (s.m) {
      case SM_UNIVERSAL: break;
      case SM_TAG:
        if (e->tag != s.a) return false;
        break;
      case SM_ID:
        if (e->idAtom != s.a) return false;
        break;
      case SM_CLASS:
        if (!e->hasClass(s.a)) return false;
        break;
      case SM_ATTR_EXISTS: case SM_ATTR_EQ: case SM_ATTR_INCLUDES: case SM_ATTR_DASH:
      case SM_ATTR_PREFIX: case SM_ATTR_SUFFIX: case SM_ATTR_SUBSTR:
        if (!attrMatch(s, e)) return false;
        break;
      case SM_PSEUDO:
        if (!pseudoMatch(s.pc, e, scope)) return false;
        break;
      case SM_NOT:
        if (s.sub && matchesAny(*s.sub, e, scope)) return false;
        break;
      case SM_IS:
        if (s.sub && !matchesAny(*s.sub, e, scope)) return false;
        break;
      case SM_HAS: {
        if (!s.sub) return false;
        bool found = false;
        for (auto &sel : s.sub->sels) {
          Combinator first = sel.parts.size() > 1 ? sel.parts[1].comb : CB_DESC;
          if (first == CB_ADJ || first == CB_SIB) {
            for (Element *n = e->nextElementSibling(); n && !found; n = n->nextElementSibling())
              if (matchSelector(sel, n, e)) found = true;
          } else {
            e->forEachElement([&](Element *d) {
              if (!found && d != e && matchSelector(sel, d, e)) found = true;
            });
          }
          if (found) break;
        }
        if (!found) return false;
        break;
      }
      case SM_NTH_CHILD: case SM_NTH_LAST_CHILD: case SM_NTH_OF_TYPE: case SM_NTH_LAST_OF_TYPE: {
        if (!e->parent) return false;
        int idx = 1;
        bool last = s.m == SM_NTH_LAST_CHILD || s.m == SM_NTH_LAST_OF_TYPE;
        bool type = s.m == SM_NTH_OF_TYPE || s.m == SM_NTH_LAST_OF_TYPE;
        for (Element *o = last ? e->nextElementSibling() : e->prevElementSibling(); o;
             o = last ? o->nextElementSibling() : o->prevElementSibling()) {
          if (type && o->tag != e->tag) continue;
          if (s.sub && !matchesAny(*s.sub, o, scope)) continue;
          idx++;
        }
        if (s.sub && !matchesAny(*s.sub, e, scope)) return false;
        if (!nthMatch(s.nthA, s.nthB, idx)) return false;
        break;
      }
    }
  }
  return true;
}

bool matchSelector(const Selector &s, Element *e, Element *scope) {
  if (s.parts.empty()) return false;
  return matchFrom(s, (int)s.parts.size() - 1, e, scope);
}

bool matchesAny(const SelectorList &l, Element *e, Element *scope) {
  for (auto &s : l.sels)
    if (matchSelector(s, e, scope)) return true;
  return false;
}

// ============================================================ stylesheet parsing

static std::string stripComments(const std::string &css) {
  std::string out;
  out.reserve(css.size());
  char q = 0;
  for (size_t i = 0; i < css.size(); i++) {
    char c = css[i];
    if (q) {
      out += c;
      if (c == '\\' && i + 1 < css.size()) {
        out += css[++i];
      } else if (c == q) {
        q = 0;
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      q = c;
      out += c;
      continue;
    }
    if (c == '/' && i + 1 < css.size() && css[i + 1] == '*') {
      size_t e = css.find("*/", i + 2);
      i = e == std::string::npos ? css.size() : e + 1;
      out += ' ';
      continue;
    }
    out += c;
  }
  return out;
}

// Find the index of the matching '}' for a '{' at position open.
static size_t matchBrace(const std::string &s, size_t open) {
  int d = 0;
  char q = 0;
  for (size_t i = open; i < s.size(); i++) {
    char c = s[i];
    if (q) {
      if (c == '\\') i++;
      else if (c == q) q = 0;
      continue;
    }
    if (c == '"' || c == '\'') q = c;
    else if (c == '{') d++;
    else if (c == '}') {
      if (--d == 0) return i;
    }
  }
  return s.size();
}

static float mediaLen(const std::string &v) {
  float f = strtof(v.c_str(), nullptr);
  if (v.find("em") != std::string::npos) f *= 16;
  return f;
}

static bool evalMediaFeature(std::string f, const MediaEnv &env) {
  f = trim(toLower(f));
  if (f.empty()) return true;
  size_t colon = f.find(':');
  std::string name = trim(colon == std::string::npos ? f : f.substr(0, colon));
  std::string val = colon == std::string::npos ? "" : trim(f.substr(colon + 1));
  // range syntax: (width >= 600px), (400px <= width <= 700px)
  for (const char *op : {">=", "<=", ">", "<"}) {
    size_t p = f.find(op);
    if (p != std::string::npos && colon == std::string::npos) {
      std::string a = trim(f.substr(0, p)), b = trim(f.substr(p + strlen(op)));
      bool aIsName = isalpha((unsigned char)a[0]);
      std::string feat = aIsName ? a : b;
      std::string num = aIsName ? b : a;
      size_t p2 = num.find_first_of("<>");
      if (p2 != std::string::npos) {  // double range
        std::string rest = num.substr(p2);
        num = trim(num.substr(0, p2));
        if (!evalMediaFeature(feat + " " + rest, env)) return false;
      }
      float dim = feat.find("height") != std::string::npos ? env.height : env.width;
      float v = mediaLen(num);
      std::string o = op;
      if (!aIsName) {  // "600px <= width" -> width >= 600px
        if (o == ">=") o = "<=";
        else if (o == "<=") o = ">=";
        else if (o == ">") o = "<";
        else o = ">";
      }
      if (o == ">=") return dim >= v;
      if (o == "<=") return dim <= v;
      if (o == ">") return dim > v;
      return dim < v;
    }
  }
  if (name == "min-width") return env.width >= mediaLen(val);
  if (name == "max-width") return env.width <= mediaLen(val);
  if (name == "min-height") return env.height >= mediaLen(val);
  if (name == "max-height") return env.height <= mediaLen(val);
  if (name == "width") return std::fabs(env.width - mediaLen(val)) < 0.5f;
  if (name == "height") return std::fabs(env.height - mediaLen(val)) < 0.5f;
  if (name == "orientation") return val == (env.width >= env.height ? "landscape" : "portrait");
  if (name == "prefers-color-scheme") return val == (env.dark ? "dark" : "light");
  if (name == "prefers-reduced-motion") return val == "no-preference";
  if (name == "hover") return val == "none";
  if (name == "any-hover") return val == "none";
  if (name == "pointer" || name == "any-pointer") return val == "coarse";
  if (name == "min-resolution" || name == "max-resolution" || name == "-webkit-min-device-pixel-ratio") return name != "min-resolution";
  if (name == "color") return true;
  return false;
}

static bool evalMediaQuery(std::string q, const MediaEnv &env) {
  q = trim(toLower(q));
  if (q.empty()) return true;
  // comma = or
  if (q.find(',') != std::string::npos) {
    for (auto &part : split(q, ','))
      if (evalMediaQuery(part, env)) return true;
    return false;
  }
  bool neg = false;
  if (startsWith(q, "not ")) {
    neg = true;
    q = q.substr(4);
  }
  if (startsWith(q, "only ")) q = q.substr(5);
  bool result = true;
  size_t i = 0;
  while (i < q.size()) {
    while (i < q.size() && isSpace(q[i])) i++;
    if (i >= q.size()) break;
    if (q[i] == '(') {
      int d = 1;
      size_t st = ++i;
      while (i < q.size() && d > 0) {
        if (q[i] == '(') d++;
        else if (q[i] == ')') d--;
        i++;
      }
      std::string inner = q.substr(st, i - st - 1);
      if (!evalMediaFeature(inner, env)) result = false;
      continue;
    }
    size_t st = i;
    while (i < q.size() && !isSpace(q[i]) && q[i] != '(') i++;
    std::string w = q.substr(st, i - st);
    if (w == "and") continue;
    if (w == "screen" || w == "all") continue;
    if (w == "print" || w == "speech") result = false;
    if (w == "or") {  // treat "a or b" approximately
      continue;
    }
  }
  return neg ? !result : result;
}

static void parseRules(const std::string &css, size_t a, size_t b, StyleSheet &out, uint8_t layer, uint32_t &order,
                       const MediaEnv &env);

static void parseKeyframes(const std::string &css, size_t a, size_t b, Keyframes &kf) {
  size_t i = a;
  while (i < b) {
    size_t open = css.find('{', i);
    if (open == std::string::npos || open >= b) break;
    size_t close = matchBrace(css, open);
    std::string sel = trim(css.substr(i, open - i));
    auto decls = std::make_shared<DeclBlock>();
    parseDeclarations(css.data() + open + 1, close - open - 1, *decls, true);
    std::shared_ptr<TimingFn> timing;
    // per-keyframe animation-timing-function
    for (auto it = decls->decls.begin(); it != decls->decls.end();) {
      if (it->prop == P_animation_timing_function) {
        auto v = std::static_pointer_cast<std::vector<TimingFn>>(it->v.obj());
        if (v && !v->empty()) timing = std::make_shared<TimingFn>((*v)[0]);
        it = decls->decls.erase(it);
      } else {
        ++it;
      }
    }
    for (auto &part : split(sel, ',')) {
      std::string p = trim(toLower(part));
      float off;
      if (p == "from") off = 0;
      else if (p == "to") off = 1;
      else off = strtof(p.c_str(), nullptr) / 100;
      kf.frames.push_back({off, decls, timing});
    }
    i = close + 1;
  }
  std::stable_sort(kf.frames.begin(), kf.frames.end(), [](const Keyframes::Frame &x, const Keyframes::Frame &y) { return x.offset < y.offset; });
}

static void parseRules(const std::string &css, size_t a, size_t b, StyleSheet &out, uint8_t layer, uint32_t &order,
                       const MediaEnv &env) {
  size_t i = a;
  while (i < b) {
    while (i < b && (isSpace(css[i]) || css[i] == ';')) i++;
    if (i >= b) break;
    if (css[i] == '}') {
      i++;
      continue;
    }
    if (css[i] == '@') {
      size_t st = i + 1;
      size_t j = st;
      while (j < b && (isalnum((unsigned char)css[j]) || css[j] == '-')) j++;
      std::string name = toLower(css.substr(st, j - st));
      // statement or block?
      size_t semi = css.find(';', j), brace = css.find('{', j);
      if (brace == std::string::npos || brace >= b || (semi != std::string::npos && semi < brace)) {
        i = semi == std::string::npos ? b : semi + 1;  // @import / @charset / @layer a, b;
        continue;
      }
      size_t close = matchBrace(css, brace);
      std::string prelude = trim(css.substr(j, brace - j));
      if (name == "media") {
        if (evalMediaQuery(prelude, env)) parseRules(css, brace + 1, close, out, layer, order, env);
      } else if (name == "supports" || name == "layer" || name == "container" || name == "scope" || name == "document") {
        bool ok = true;
        if (name == "supports") {
          std::string p = toLower(prelude);
          if (p.find("not ") == 0) ok = false;
          if (p.find("backdrop-filter") != std::string::npos || p.find("-webkit-") != std::string::npos ||
              p.find("display: contents") != std::string::npos)
            ok = !ok ? true : (p.find("not") == std::string::npos ? false : true);
        }
        if (ok) parseRules(css, brace + 1, close, out, layer, order, env);
      } else if (name == "keyframes" || name == "-webkit-keyframes" || name == "-moz-keyframes") {
        Keyframes kf;
        std::string n = trim(prelude);
        if (!n.empty() && (n[0] == '"' || n[0] == '\'')) n = n.substr(1, n.size() - 2);
        kf.name = intern(n);
        parseKeyframes(css, brace + 1, close, kf);
        // later definitions win
        for (auto it = out.keyframes.begin(); it != out.keyframes.end(); ++it) {
          if (it->name == kf.name) {
            out.keyframes.erase(it);
            break;
          }
        }
        out.keyframes.push_back(std::move(kf));
      } else if (name == "font-face") {
        DeclBlock d;
        std::string body = css.substr(brace + 1, close - brace - 1);
        // family / src are not regular properties: scan the text
        std::string fam, src;
        for (auto &decl : split(body, ';')) {
          size_t c = decl.find(':');
          if (c == std::string::npos) continue;
          std::string k = trim(toLower(decl.substr(0, c))), v = trim(decl.substr(c + 1));
          if (k == "font-family") {
            if (!v.empty() && (v[0] == '"' || v[0] == '\'')) v = v.substr(1, v.size() - 2);
            fam = v;
          } else if (k == "src") {
            size_t u = v.find("url(");
            if (u != std::string::npos) {
              size_t e = v.find(')', u);
              src = trim(v.substr(u + 4, e - u - 4));
              if (!src.empty() && (src[0] == '"' || src[0] == '\'')) src = src.substr(1, src.size() - 2);
            }
          }
        }
        if (!fam.empty()) out.fontFaces.push_back({fam, src});
      }
      i = close + 1;
      continue;
    }
    // qualified rule
    size_t brace = i;
    {
      char q = 0;
      int d = 0;
      while (brace < b) {
        char c = css[brace];
        if (q) {
          if (c == q) q = 0;
        } else if (c == '"' || c == '\'') {
          q = c;
        } else if (c == '(' || c == '[') {
          d++;
        } else if (c == ')' || c == ']') {
          d--;
        } else if (c == '{' && d <= 0) {
          break;
        }
        brace++;
      }
    }
    if (brace >= b) break;
    size_t close = matchBrace(css, brace);
    std::string prelude = trim(css.substr(i, brace - i));
    std::string body = css.substr(brace + 1, close - brace - 1);
    // CSS nesting: split nested rules out of the body
    std::string flat;
    std::vector<std::pair<std::string, std::string>> nested;
    {
      size_t k = 0;
      while (k < body.size()) {
        size_t nb = body.find('{', k);
        size_t semi = body.find(';', k);
        if (nb == std::string::npos) {
          flat += body.substr(k);
          break;
        }
        if (semi != std::string::npos && semi < nb) {
          flat += body.substr(k, semi - k + 1);
          k = semi + 1;
          continue;
        }
        size_t nc = matchBrace(body, nb);
        std::string nsel = trim(body.substr(k, nb - k));
        nested.push_back({nsel, body.substr(nb + 1, nc - nb - 1)});
        k = nc + 1;
      }
    }
    SelectorList list;
    if (parseSelectorList(prelude, list)) {
      auto decls = std::make_shared<DeclBlock>();
      parseDeclarations(flat.data(), flat.size(), *decls, true);
      if (!decls->decls.empty()) {
        for (auto &sel : list.sels) {
          StyleRule r;
          r.sel = sel;
          r.decls = decls;
          r.order = order++;
          r.layer = layer;
          out.rules.push_back(std::move(r));
        }
      }
      // nested rules: "&" refers to the parent selector; bare selectors are descendants
      for (auto &n : nested) {
        std::string combined;
        for (auto &parentSel : split(prelude, ',')) {
          for (auto &childSel : split(n.first, ',')) {
            std::string c = trim(childSel), p = trim(parentSel);
            std::string s;
            size_t amp = c.find('&');
            if (amp != std::string::npos) {
              s = c;
              size_t pos;
              while ((pos = s.find('&')) != std::string::npos) s.replace(pos, 1, p);
            } else {
              s = p + " " + c;
            }
            if (!combined.empty()) combined += ", ";
            combined += s;
          }
        }
        std::string sub = combined + "{" + n.second + "}";
        parseRules(sub, 0, sub.size(), out, layer, order, env);
      }
    }
    i = close + 1;
  }
}

void parseStyleSheet(const std::string &cssIn, StyleSheet &out, uint8_t layer, uint32_t &order, const MediaEnv &env) {
  std::string css = stripComments(cssIn);
  parseRules(css, 0, css.size(), out, layer, order, env);
  out.rules.shrink_to_fit();
}

}  // namespace rt
