#pragma once
// Thai line breaking: Thai has no spaces between words, so break opportunities come from a
// word list (built by the compiler with Chromium's segmenter over the app's text) using
// longest matching. Unknown text falls back to breaking between clusters when a line overflows.

#include "rt_base.h"

namespace rt {

void thaiLoadDict(const char *data, size_t len);
bool thaiHasDict();
// Insert U+200B (zero-width space) at word boundaries inside Thai runs of s.
void thaiSegment(std::string &s);

}  // namespace rt
