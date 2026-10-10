#include "rt_base.h"

#include <mutex>

namespace rt {

int gDebugFlags = 0;
volatile uint8_t gPhase = PH_IDLE;
uint32_t gRestyleCount = 0;
bool gSiblingSelectors = true;  // until the style index says otherwise
bool gStateDescendants = true;
const char *const kPhaseNames[PH_COUNT] = {"idle", "load", "script", "os", "layout", "paint", "io", "input"};

// ---- slow memory

static bool fastMemory(const void *) { return false; }
static void plainCopy(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
static size_t unlimited() { return SIZE_MAX; }
bool (*gIsSlowMemory)(const void *p) = fastMemory;
void (*gBulkCopy)(void *dst, const void *src, size_t n) = plainCopy;
size_t (*gLargeFree)() = unlimited;
static void noPause() {}
void (*gIoPause)() = noPause;
void (*gSlowPause)() = noPause;
static void *plainAlloc(size_t n) { return malloc(n); }
void *(*gAllocBulk)(size_t n) = plainAlloc;

namespace {
struct Resident {
  const uint8_t *src;
  size_t len;
  uint8_t *copy;
  bool dropped;
};
std::mutex gResidentMu;
std::vector<Resident> gResident;
size_t gResidentBytes = 0;
constexpr size_t kResidentReserve = 384 * 1024;  // left free for documents, images, scripts
}  // namespace

const uint8_t *residentCopy(const uint8_t *src, size_t len) {
  if (!src || !len || !gIsSlowMemory(src)) return src;
  std::lock_guard<std::mutex> lock(gResidentMu);
  for (auto &r : gResident)
    if (r.src == src && r.len == len && !r.dropped) return r.copy;
  if (gLargeFree() < len + kResidentReserve) return src;  // short of memory: read in place
  auto *copy = (uint8_t *)malloc(len);
  if (!copy) return src;
  gBulkCopy(copy, src, len);
  gResident.push_back({src, len, copy, false});
  gResidentBytes += len;
  return copy;
}

void dropResident(const uint8_t *begin, size_t len) {
  std::lock_guard<std::mutex> lock(gResidentMu);
  for (auto &r : gResident)
    if (r.src >= begin && r.src < begin + len) r.dropped = true;
}

void collectResident() {
  std::lock_guard<std::mutex> lock(gResidentMu);
  for (auto it = gResident.begin(); it != gResident.end();) {
    if (it->dropped) {
      free(it->copy);
      gResidentBytes -= it->len;
      it = gResident.erase(it);
    } else {
      ++it;
    }
  }
}

size_t residentBytes() {
  std::lock_guard<std::mutex> lock(gResidentMu);
  return gResidentBytes;
}

static std::recursive_mutex &paintMutex() {
  static auto *m = new std::recursive_mutex();
  return *m;
}
PaintLock::PaintLock() { paintMutex().lock(); }
PaintLock::~PaintLock() { paintMutex().unlock(); }

void logf(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  logWrite(buf);
}

std::string toLower(const std::string &s) {
  std::string r = s;
  for (auto &c : r) c = lowerAscii(c);
  return r;
}

std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && isSpace(s[a])) a++;
  while (b > a && isSpace(s[b - 1])) b--;
  return s.substr(a, b - a);
}

bool startsWith(const std::string &s, const char *p) {
  size_t n = strlen(p);
  return s.size() >= n && memcmp(s.data(), p, n) == 0;
}

bool iequals(const std::string &a, const char *b) {
  size_t n = strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; i++)
    if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
  return true;
}

std::vector<std::string> split(const std::string &s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); i++) {
    if (i == s.size() || s[i] == sep) {
      out.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

uint32_t utf8Next(const char *s, size_t len, size_t &i) {
  uint8_t c = (uint8_t)s[i];
  if (c < 0x80) {
    i++;
    return c;
  }
  int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
  if (n == 0 || i + n >= len) {  // stray continuation byte or truncated sequence
    i++;
    return 0xFFFD;
  }
  uint32_t cp = c & (0x3F >> n);
  for (int k = 1; k <= n; k++) {
    uint8_t cc = (uint8_t)s[i + k];
    if ((cc & 0xC0) != 0x80) {
      i += k;
      return 0xFFFD;
    }
    cp = (cp << 6) | (cc & 0x3F);
  }
  i += n + 1;
  return cp;
}

void utf8Append(std::string &out, uint32_t cp) {
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

size_t utf8PrevBoundary(const std::string &s, size_t i) {
  if (i == 0) return 0;
  i--;
  while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80) i--;
  return i;
}

// ---- JSON ----
const Json *Json::get(const char *key) const {
  for (auto &kv : obj)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

namespace {
struct JP {
  const char *s;
  size_t n, i = 0;
  void ws() {
    while (i < n && isSpace(s[i])) i++;
  }
  bool str(std::string &out) {
    if (s[i] != '"') return false;
    i++;
    while (i < n && s[i] != '"') {
      char c = s[i++];
      if (c == '\\' && i < n) {
        char e = s[i++];
        switch (e) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            if (i + 4 > n) return false;
            uint32_t cp = (uint32_t)strtoul(std::string(s + i, 4).c_str(), nullptr, 16);
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= n && s[i] == '\\' && s[i + 1] == 'u') {
              uint32_t lo = (uint32_t)strtoul(std::string(s + i + 2, 4).c_str(), nullptr, 16);
              i += 6;
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            utf8Append(out, cp);
            break;
          }
          default: out += e;
        }
      } else {
        out += c;
      }
    }
    if (i >= n) return false;
    i++;
    return true;
  }
  bool val(Json &j) {
    ws();
    if (i >= n) return false;
    char c = s[i];
    if (c == '{') {
      j.type = Json::Obj;
      i++;
      ws();
      if (i < n && s[i] == '}') {
        i++;
        return true;
      }
      while (i < n) {
        ws();
        std::string k;
        if (!str(k)) return false;
        ws();
        if (i >= n || s[i] != ':') return false;
        i++;
        Json v;
        if (!val(v)) return false;
        j.obj.emplace_back(std::move(k), std::move(v));
        ws();
        if (i < n && s[i] == ',') {
          i++;
          continue;
        }
        if (i < n && s[i] == '}') {
          i++;
          return true;
        }
        return false;
      }
      return false;
    }
    if (c == '[') {
      j.type = Json::Arr;
      i++;
      ws();
      if (i < n && s[i] == ']') {
        i++;
        return true;
      }
      while (i < n) {
        Json v;
        if (!val(v)) return false;
        j.arr.push_back(std::move(v));
        ws();
        if (i < n && s[i] == ',') {
          i++;
          continue;
        }
        if (i < n && s[i] == ']') {
          i++;
          return true;
        }
        return false;
      }
      return false;
    }
    if (c == '"') {
      j.type = Json::Str;
      return str(j.str);
    }
    if (!strncmp(s + i, "true", 4)) {
      j.type = Json::Bool;
      j.b = true;
      i += 4;
      return true;
    }
    if (!strncmp(s + i, "false", 5)) {
      j.type = Json::Bool;
      i += 5;
      return true;
    }
    if (!strncmp(s + i, "null", 4)) {
      i += 4;
      return true;
    }
    char *end;
    std::string tmp(s + i, std::min<size_t>(n - i, 40));
    j.num = strtod(tmp.c_str(), &end);
    if (end == tmp.c_str()) return false;
    j.type = Json::Num;
    i += end - tmp.c_str();
    return true;
  }
};
}  // namespace

std::string Json::quote(const std::string &s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else {
          o += (char)c;
        }
    }
  }
  return o + "\"";
}

std::string Json::dump() const {
  switch (type) {
    case Null: return "null";
    case Bool: return b ? "true" : "false";
    case Num: {
      if (std::isfinite(num) && num == std::floor(num) && std::fabs(num) < 1e15) return std::to_string((long long)num);
      char buf[32];
      snprintf(buf, sizeof buf, "%.17g", num);
      return buf;
    }
    case Str: return quote(str);
    case Arr: {
      std::string o = "[";
      for (size_t i = 0; i < arr.size(); i++) o += (i ? "," : "") + arr[i].dump();
      return o + "]";
    }
    case Obj: {
      std::string o = "{";
      for (size_t i = 0; i < obj.size(); i++) o += (i ? "," : "") + quote(obj[i].first) + ":" + obj[i].second.dump();
      return o + "}";
    }
  }
  return "null";
}

bool Json::parse(const char *s, size_t n, Json &out) {
  JP p{s, n};
  return p.val(out);
}

}  // namespace rt
