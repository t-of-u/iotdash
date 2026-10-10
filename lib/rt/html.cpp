#include "html.h"

namespace rt {

bool isVoidTag(Atom t) {
  switch (tagOf(t)) {
    case TAG_area: case TAG_base: case TAG_br: case TAG_col: case TAG_embed: case TAG_hr:
    case TAG_img: case TAG_input: case TAG_link: case TAG_meta: case TAG_param: case TAG_source:
    case TAG_track: case TAG_wbr:
      return true;
    default:
      return false;
  }
}

static bool closesP(Tag t) {
  switch (t) {
    case TAG_address: case TAG_article: case TAG_aside: case TAG_blockquote: case TAG_details:
    case TAG_dialog: case TAG_div: case TAG_dl: case TAG_fieldset: case TAG_figcaption:
    case TAG_figure: case TAG_footer: case TAG_form: case TAG_h1: case TAG_h2: case TAG_h3:
    case TAG_h4: case TAG_h5: case TAG_h6: case TAG_header: case TAG_hr: case TAG_main:
    case TAG_menu: case TAG_nav: case TAG_ol: case TAG_p: case TAG_pre: case TAG_section:
    case TAG_table: case TAG_ul: case TAG_search: case TAG_summary:
      return true;
    default:
      return false;
  }
}

static bool isHeadTag(Tag t) {
  switch (t) {
    case TAG_title: case TAG_meta: case TAG_link: case TAG_style: case TAG_script: case TAG_base:
    case TAG_noscript: case TAG_template_:
      return true;
    default:
      return false;
  }
}

struct Entity {
  const char *name;
  uint32_t cp;
};
static const Entity kEntities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
    {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"deg", 0xB0}, {"plusmn", 0xB1},
    {"times", 0xD7}, {"divide", 0xF7}, {"middot", 0xB7}, {"bull", 0x2022}, {"hellip", 0x2026},
    {"mdash", 0x2014}, {"ndash", 0x2013}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C},
    {"rdquo", 0x201D}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"larr", 0x2190}, {"uarr", 0x2191},
    {"rarr", 0x2192}, {"darr", 0x2193}, {"harr", 0x2194}, {"lArr", 0x21D0}, {"rArr", 0x21D2},
    {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5}, {"cent", 0xA2}, {"sect", 0xA7},
    {"para", 0xB6}, {"micro", 0xB5}, {"frac12", 0xBD}, {"frac14", 0xBC}, {"frac34", 0xBE},
    {"sup2", 0xB2}, {"sup3", 0xB3}, {"le", 0x2264}, {"ge", 0x2265}, {"ne", 0x2260},
    {"asymp", 0x2248}, {"infin", 0x221E}, {"minus", 0x2212}, {"check", 0x2713}, {"cross", 0x2717},
    {"star", 0x2606}, {"hearts", 0x2665}, {"thinsp", 0x2009}, {"ensp", 0x2002}, {"emsp", 0x2003},
    {"zwsp", 0x200B}, {"shy", 0xAD}, {"iexcl", 0xA1}, {"iquest", 0xBF}, {"ordm", 0xBA},
    {"ordf", 0xAA}, {"prime", 0x2032}, {"Prime", 0x2033}, {"permil", 0x2030}, {"loz", 0x25CA},
    {"spades", 0x2660}, {"clubs", 0x2663}, {"diams", 0x2666}, {"alpha", 0x3B1}, {"beta", 0x3B2},
    {"gamma", 0x3B3}, {"delta", 0x3B4}, {"pi", 0x3C0}, {"sigma", 0x3C3}, {"mu", 0x3BC},
    {"omega", 0x3C9}, {"Omega", 0x3A9}, {"Delta", 0x394}, {"theta", 0x3B8}, {"lambda", 0x3BB},
    {"uarr", 0x2191}, {"crarr", 0x21B5}, {"sbquo", 0x201A}, {"bdquo", 0x201E}, {"dagger", 0x2020},
};

std::string decodeEntities(const char *s, size_t n) {
  std::string out;
  out.reserve(n);
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c != '&') {
      out += c;
      continue;
    }
    size_t j = i + 1;
    if (j < n && s[j] == '#') {
      j++;
      bool hex = j < n && (s[j] == 'x' || s[j] == 'X');
      if (hex) j++;
      size_t st = j;
      while (j < n && (hex ? isxdigit((unsigned char)s[j]) : isdigit((unsigned char)s[j]))) j++;
      if (j > st) {
        uint32_t cp = (uint32_t)strtoul(std::string(s + st, j - st).c_str(), nullptr, hex ? 16 : 10);
        if (j < n && s[j] == ';') j++;
        utf8Append(out, cp ? cp : 0xFFFD);
        i = j - 1;
        continue;
      }
    } else {
      size_t st = j;
      while (j < n && j - st < 10 && isalnum((unsigned char)s[j])) j++;
      if (j > st) {
        std::string name(s + st, j - st);
        bool found = false;
        for (auto &e : kEntities) {
          if (name == e.name) {
            utf8Append(out, e.cp);
            found = true;
            break;
          }
        }
        if (found) {
          if (j < n && s[j] == ';') j++;
          i = j - 1;
          continue;
        }
      }
    }
    out += '&';
  }
  return out;
}

std::string escapeText(const std::string &s, bool attr) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += attr ? "<" : "&lt;"; break;
      case '>': o += attr ? ">" : "&gt;"; break;
      case '"': o += attr ? "&quot;" : "\""; break;
      default: o += c;
    }
  }
  return o;
}

namespace {

struct TreeBuilder {
  Document *doc;
  Element *root;  // document or fragment
  bool docMode;
  std::vector<Element *> stack;
  Element *html = nullptr, *head = nullptr, *body = nullptr;
  bool headClosed = false;
  int svgDepth = 0;

  Element *top() { return stack.empty() ? root : stack.back(); }

  void ensureHtml() {
    if (!docMode || html) return;
    html = doc->createElement(TAG_html);
    root->appendChild(html);
    stack.push_back(html);
  }
  void ensureHead() {
    ensureHtml();
    if (!docMode || head) return;
    head = doc->createElement(TAG_head);
    html->appendChild(head);
    stack.push_back(head);
  }
  void ensureBody() {
    if (!docMode || body) return;
    ensureHead();
    // close head
    while (!stack.empty() && stack.back() != html) stack.pop_back();
    headClosed = true;
    body = doc->createElement(TAG_body);
    html->appendChild(body);
    stack.push_back(body);
  }

  bool inStack(Atom t) {
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
      if ((*it)->tag == t) return true;
      Tag tt = tagOf((*it)->tag);
      if (t == TAG_p && (tt == TAG_button || tt == TAG_table || tt == TAG_td || tt == TAG_th)) return false;
    }
    return false;
  }
  void popUntil(Atom t) {
    while (!stack.empty()) {
      Element *e = stack.back();
      stack.pop_back();
      if (e == html || e == body) {  // never pop the document skeleton implicitly
        if (e->tag != t) {
          stack.push_back(e);
          return;
        }
      }
      if (e->tag == TAG_svg) svgDepth--;
      if (e->tag == t) return;
    }
  }
  // Close an element of tag t if open above any of the "stop" tags.
  void closeIfOpen(Atom t, std::initializer_list<Tag> stops) {
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
      Tag tt = tagOf((*it)->tag);
      if ((*it)->tag == t) {
        popUntil(t);
        return;
      }
      for (Tag s : stops)
        if (tt == s) return;
    }
  }

  void text(const std::string &s) {
    if (s.empty()) return;
    if (docMode && !body) {
      bool allWs = true;
      for (char c : s)
        if (!isSpace(c)) {
          allWs = false;
          break;
        }
      if (allWs) return;
      ensureBody();
    }
    Element *p = top();
    Tag pt = p->tagId();
    // whitespace directly inside table structure is dropped
    if (pt == TAG_table || pt == TAG_tbody || pt == TAG_thead || pt == TAG_tfoot || pt == TAG_tr ||
        pt == TAG_select || pt == TAG_ul || pt == TAG_ol) {
      bool allWs = true;
      for (char c : s)
        if (!isSpace(c)) {
          allWs = false;
          break;
        }
      if (allWs && pt != TAG_ul && pt != TAG_ol) return;
    }
    if (p->last && p->last->isText()) {
      ((Text *)p->last)->data += s;
      return;
    }
    p->appendChild(doc->createText(s));
  }

  Element *start(Atom tagA, std::vector<Attr> &attrs, bool selfClosing) {
    Tag t = tagOf(tagA);
    if (docMode) {
      if (t == TAG_html) {
        ensureHtml();
        for (auto &a : attrs)
          if (!html->hasAttr(a.name)) html->setAttr(a.name, a.value);
        return nullptr;
      }
      if (t == TAG_head) {
        ensureHead();
        return nullptr;
      }
      if (t == TAG_body) {
        if (!body) {
          ensureHead();
          while (!stack.empty() && stack.back() != html) stack.pop_back();
          body = doc->createElement(TAG_body);
          html->appendChild(body);
          stack.push_back(body);
        }
        for (auto &a : attrs)
          if (!body->hasAttr(a.name)) body->setAttr(a.name, a.value);
        return nullptr;
      }
      if (!body && !headClosed && isHeadTag(t)) {
        ensureHead();
      } else {
        ensureBody();
      }
    }
    if (svgDepth == 0) {
      // implied end tags
      if (closesP(t) && inStack(TAG_p)) popUntil(TAG_p);
      if (t == TAG_li) closeIfOpen(TAG_li, {TAG_ul, TAG_ol, TAG_menu});
      if (t == TAG_dt || t == TAG_dd) {
        closeIfOpen(TAG_dt, {TAG_dl});
        closeIfOpen(TAG_dd, {TAG_dl});
      }
      if (t == TAG_option) closeIfOpen(TAG_option, {TAG_select, TAG_datalist, TAG_optgroup});
      if (t == TAG_optgroup) {
        closeIfOpen(TAG_option, {TAG_select});
        closeIfOpen(TAG_optgroup, {TAG_select});
      }
      if (t == TAG_td || t == TAG_th) {
        closeIfOpen(TAG_td, {TAG_table, TAG_tr});
        closeIfOpen(TAG_th, {TAG_table, TAG_tr});
        Tag pt = top()->tagId();
        if (pt == TAG_table || pt == TAG_tbody || pt == TAG_thead || pt == TAG_tfoot) {
          std::vector<Attr> none;
          start(TAG_tr, none, false);
        }
      }
      if (t == TAG_tr) {
        closeIfOpen(TAG_td, {TAG_table});
        closeIfOpen(TAG_th, {TAG_table});
        closeIfOpen(TAG_tr, {TAG_table});
        if (top()->tagId() == TAG_table) {
          std::vector<Attr> none;
          start(TAG_tbody, none, false);
        }
      }
      if (t == TAG_thead || t == TAG_tbody || t == TAG_tfoot) {
        closeIfOpen(TAG_td, {TAG_table});
        closeIfOpen(TAG_th, {TAG_table});
        closeIfOpen(TAG_tr, {TAG_table});
        closeIfOpen(TAG_thead, {TAG_table});
        closeIfOpen(TAG_tbody, {TAG_table});
        closeIfOpen(TAG_tfoot, {TAG_table});
      }
      if ((t == TAG_button) && inStack(TAG_button)) popUntil(TAG_button);
      if (t == TAG_a && inStack(TAG_a)) popUntil(TAG_a);
    }
    Element *e = doc->createElement(tagA);
    for (auto &a : attrs) e->setAttr(a.name, a.value);
    top()->appendChild(e);
    bool isVoid = isVoidTag(tagA) || (selfClosing && (svgDepth > 0 || t == TAG_svg || t == TAG_NONE));
    if (!isVoid) {
      stack.push_back(e);
      if (t == TAG_svg) svgDepth++;
    }
    return e;
  }

  void end(Atom tagA) {
    Tag t = tagOf(tagA);
    if (docMode && (t == TAG_html || t == TAG_body)) return;  // keep the skeleton open
    if (docMode && t == TAG_head) {
      if (head && !body) {
        while (!stack.empty() && stack.back() != html) stack.pop_back();
        headClosed = true;
      }
      return;
    }
    if (t == TAG_br) {  // </br> acts like <br>
      std::vector<Attr> none;
      start(TAG_br, none, false);
      return;
    }
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
      if ((*it)->tag == tagA) {
        popUntil(tagA);
        return;
      }
    }
  }
};

struct Tokenizer {
  const char *s;
  size_t n, i = 0;
  TreeBuilder &tb;

  static bool nameChar(char c) { return !isSpace(c) && c != '/' && c != '>' && c != '=' && c != '<'; }

  void run() {
    std::string textBuf;
    auto flush = [&]() {
      if (!textBuf.empty()) {
        tb.text(decodeEntities(textBuf.data(), textBuf.size()));
        textBuf.clear();
      }
    };
    while (i < n) {
      char c = s[i];
      if (c != '<') {
        textBuf += c;
        i++;
        continue;
      }
      if (i + 1 >= n) {
        textBuf += c;
        i++;
        break;
      }
      char d = s[i + 1];
      if (d == '!') {
        flush();
        if (i + 3 < n && s[i + 2] == '-' && s[i + 3] == '-') {
          const char *e = strstr(s + i + 4, "-->");
          i = e ? (size_t)(e - s) + 3 : n;
        } else {
          while (i < n && s[i] != '>') i++;
          i++;
        }
        continue;
      }
      if (d == '?') {
        flush();
        while (i < n && s[i] != '>') i++;
        i++;
        continue;
      }
      if (d == '/') {
        size_t j = i + 2;
        size_t st = j;
        while (j < n && nameChar(s[j])) j++;
        if (j == st) {
          textBuf += c;
          i++;
          continue;
        }
        flush();
        Atom t = internLower(s + st, j - st);
        while (j < n && s[j] != '>') j++;
        i = j + 1;
        tb.end(t);
        continue;
      }
      if (!isalpha((unsigned char)d)) {
        textBuf += c;
        i++;
        continue;
      }
      flush();
      size_t j = i + 1, st = j;
      while (j < n && nameChar(s[j])) j++;
      Atom tag = internLower(s + st, j - st);
      std::vector<Attr> attrs;
      bool selfClosing = false;
      while (j < n) {
        while (j < n && isSpace(s[j])) j++;
        if (j >= n) break;
        if (s[j] == '>') {
          j++;
          break;
        }
        if (s[j] == '/') {
          if (j + 1 < n && s[j + 1] == '>') {
            selfClosing = true;
            j += 2;
            break;
          }
          j++;
          continue;
        }
        size_t as = j;
        while (j < n && nameChar(s[j])) j++;
        if (j == as) {
          j++;
          continue;
        }
        Atom an = internLower(s + as, j - as);
        while (j < n && isSpace(s[j])) j++;
        std::string val;
        if (j < n && s[j] == '=') {
          j++;
          while (j < n && isSpace(s[j])) j++;
          if (j < n && (s[j] == '"' || s[j] == '\'')) {
            char q = s[j++];
            size_t vs = j;
            while (j < n && s[j] != q) j++;
            val = decodeEntities(s + vs, j - vs);
            if (j < n) j++;
          } else {
            size_t vs = j;
            while (j < n && !isSpace(s[j]) && s[j] != '>') j++;
            val = decodeEntities(s + vs, j - vs);
          }
        }
        bool dup = false;
        for (auto &a : attrs)
          if (a.name == an) dup = true;
        if (!dup) attrs.push_back({an, val});
      }
      i = j;
      Element *e = tb.start(tag, attrs, selfClosing);
      Tag t = tagOf(tag);
      if (e && (t == TAG_script || t == TAG_style || t == TAG_textarea || t == TAG_title) && !selfClosing) {
        // raw text until the matching end tag
        const std::string &name = atomStr(tag);
        size_t k = i;
        size_t endPos = n, after = n;
        while (k + 1 < n) {
          if (s[k] == '<' && s[k + 1] == '/' && k + 2 + name.size() <= n &&
              strncasecmp(s + k + 2, name.c_str(), name.size()) == 0) {
            endPos = k;
            size_t m = k + 2 + name.size();
            while (m < n && s[m] != '>') m++;
            after = m < n ? m + 1 : n;
            break;
          }
          k++;
        }
        std::string body(s + i, endPos - i);
        if (t == TAG_textarea || t == TAG_title) {
          body = decodeEntities(body.data(), body.size());
          if (t == TAG_textarea && !body.empty() && body[0] == '\n') body.erase(0, 1);
        }
        if (!body.empty()) e->appendChild(tb.doc->createText(body));
        tb.end(tag);
        i = after;
      }
    }
    flush();
  }
};

}  // namespace

void parseDocument(Document *doc, const std::string &html) {
  TreeBuilder tb{doc, doc, true};
  Tokenizer tk{html.data(), html.size(), 0, tb};
  tk.run();
  tb.ensureBody();
  doc->findStructure();
}

Element *parseFragment(Document *doc, const std::string &html, Atom contextTag) {
  Element *frag = new Element(0, NT_FRAGMENT);
  frag->doc = doc;
  TreeBuilder tb{doc, frag, false};
  // Table context: rows/cells parse into the right structure.
  Element *ctx = nullptr;
  Tag ct = tagOf(contextTag);
  if (ct == TAG_table || ct == TAG_tbody || ct == TAG_thead || ct == TAG_tfoot || ct == TAG_tr ||
      ct == TAG_select || ct == TAG_ul || ct == TAG_ol || ct == TAG_svg || ct == TAG_g) {
    ctx = doc->createElement(contextTag);
    ctx->ref();
    tb.root = ctx;
    if (ct == TAG_svg || ct == TAG_g) tb.svgDepth = 1;
  }
  Tokenizer tk{html.data(), html.size(), 0, tb};
  tk.run();
  if (ctx) {
    while (ctx->first) frag->appendChild(ctx->first);
    ctx->unref();
  }
  return frag;
}

static void serializeInto(const Node *n, std::string &out) {
  if (n->nodeType == NT_TEXT) {
    const Element *p = n->parent;
    Tag pt = p ? p->tagId() : TAG_NONE;
    if (pt == TAG_script || pt == TAG_style) out += ((const Text *)n)->data;
    else out += escapeText(((const Text *)n)->data, false);
    return;
  }
  if (n->nodeType == NT_COMMENT) {
    out += "<!--" + ((const Comment *)n)->data + "-->";
    return;
  }
  const Element *e = (const Element *)n;
  if (n->nodeType != NT_ELEMENT) {
    for (Node *c = e->first; c; c = c->next) serializeInto(c, out);
    return;
  }
  if (e->flags & (NF_PSEUDO | NF_INTERNAL)) return;
  out += '<';
  out += e->tagName();
  for (auto &a : e->attrs) {
    out += ' ';
    out += atomStr(a.name);
    out += "=\"";
    out += escapeText(a.value, true);
    out += '"';
  }
  out += '>';
  if (isVoidTag(e->tag)) return;
  for (Node *c = e->first; c; c = c->next) serializeInto(c, out);
  out += "</";
  out += e->tagName();
  out += '>';
}

std::string serializeNode(const Node *n, bool outer) {
  std::string out;
  if (outer) {
    serializeInto(n, out);
  } else if (n->isContainer()) {
    for (Node *c = ((const Element *)n)->first; c; c = c->next) serializeInto(c, out);
  } else if (n->isText()) {
    out = escapeText(((const Text *)n)->data, false);
  }
  return out;
}

}  // namespace rt
