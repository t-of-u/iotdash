#pragma once
// Tolerant HTML parser (enough of HTML5 tree building for app markup) and serializer.

#include "dom.h"

namespace rt {

// Parse a full document into doc (creates html/head/body as browsers do).
void parseDocument(Document *doc, const std::string &html);

// Parse a fragment as children of a context element (innerHTML); returns a DocumentFragment
// node with refs == 0 (insert it or delete it).
Element *parseFragment(Document *doc, const std::string &html, Atom contextTag);

// innerHTML (outer = false) / outerHTML (outer = true)
std::string serializeNode(const Node *n, bool outer);

bool isVoidTag(Atom tag);
std::string decodeEntities(const char *s, size_t n);
std::string escapeText(const std::string &s, bool attr);

}  // namespace rt
