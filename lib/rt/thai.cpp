#include "thai.h"

#include <unordered_set>

namespace rt {

namespace {
struct Dict {
  std::unordered_set<std::string> words;
  size_t maxLen = 0;  // bytes
};
Dict &dict() {
  static Dict *d = new Dict();
  return *d;
}
inline bool thaiCp(uint32_t c) { return c >= 0x0E01 && c <= 0x0E5B; }
// Code points that never start a word (combining marks, SARA AM, ...)
inline bool nonStarter(uint32_t c) {
  return c == 0x0E31 || (c >= 0x0E33 && c <= 0x0E3A) || (c >= 0x0E45 && c <= 0x0E4E);
}
}  // namespace

void thaiLoadDict(const char *data, size_t len) {
  Dict &d = dict();
  d.words.clear();
  d.maxLen = 0;
  size_t s = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i == len || data[i] == '\n') {
      if (i > s) {
        d.words.emplace(data + s, i - s);
        d.maxLen = std::max(d.maxLen, i - s);
      }
      s = i + 1;
    }
  }
}

bool thaiHasDict() { return !dict().words.empty(); }

void thaiSegment(std::string &s) {
  Dict &d = dict();
  if (d.words.empty()) return;
  // quick reject: any Thai at all?
  bool any = false;
  for (size_t i = 0; i + 1 < s.size(); i++)
    if ((uint8_t)s[i] == 0xE0 && ((uint8_t)s[i + 1] == 0xB8 || (uint8_t)s[i + 1] == 0xB9)) {
      any = true;
      break;
    }
  if (!any) return;
  std::string out;
  out.reserve(s.size() + 16);
  size_t i = 0;
  while (i < s.size()) {
    size_t j = i;
    uint32_t c = utf8Next(s.data(), s.size(), j);
    if (!thaiCp(c)) {
      out.append(s, i, j - i);
      i = j;
      continue;
    }
    // Thai run [i, end)
    size_t end = i;
    while (end < s.size()) {
      size_t k = end;
      uint32_t cc = utf8Next(s.data(), s.size(), k);
      if (!thaiCp(cc)) break;
      end = k;
    }
    size_t pos = i;
    bool pendingUnknown = false;
    while (pos < end) {
      // longest dictionary word starting at pos
      size_t best = 0;
      size_t maxL = std::min(d.maxLen, end - pos);
      for (size_t L = maxL; L >= 3; L--) {
        // must end on a code point boundary and not before a combining mark
        if (pos + L < end) {
          uint8_t b = (uint8_t)s[pos + L];
          if ((b & 0xC0) == 0x80) continue;
          size_t k = pos + L;
          uint32_t nx = utf8Next(s.data(), s.size(), k);
          if (nonStarter(nx)) continue;
        }
        if (d.words.count(std::string(s, pos, L))) {
          best = L;
          break;
        }
      }
      if (best) {
        if (pendingUnknown) out += "\xE2\x80\x8B";
        pendingUnknown = false;
        out.append(s, pos, best);
        pos += best;
        if (pos < end) out += "\xE2\x80\x8B";
      } else {
        size_t k = pos;
        utf8Next(s.data(), s.size(), k);
        out.append(s, pos, k - pos);
        pos = k;
        pendingUnknown = true;
      }
    }
    // avoid a dangling ZWSP at the end of the run
    if (out.size() >= 3 && out.compare(out.size() - 3, 3, "\xE2\x80\x8B") == 0) out.resize(out.size() - 3);
    i = end;
  }
  s.swap(out);
}

}  // namespace rt
