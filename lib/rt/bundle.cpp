#include "bundle.h"

#include "image.h"

namespace rt {

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool Bundle::load(const uint8_t *data, size_t len) {
  files_.clear();
  return mount("", data, len);
}

void Bundle::unmount(const std::string &prefix) {
  for (auto it = files_.begin(); it != files_.end();) it = startsWith(it->first, prefix.c_str()) ? files_.erase(it) : std::next(it);
  for (auto it = owned_.begin(); it != owned_.end();) it = startsWith(it->first, prefix.c_str()) ? owned_.erase(it) : std::next(it);
  sources_.erase(prefix);
}

const ImageSource *Bundle::sourceOf(const std::string &prefix) const {
  auto it = sources_.find(prefix);
  return it == sources_.end() ? nullptr : it->second.get();
}

bool Bundle::mount(const std::string &prefix, const uint8_t *data, size_t len) {
  return mount(prefix, std::make_shared<RamSource>(data, len));
}

bool Bundle::mount(const std::string &prefix, std::shared_ptr<const ImageSource> src) {
  if (!src) return false;
  const size_t len = src->size();
  const uint8_t *base = src->direct();
  // the index, in place or read in (growing while it is longer)
  std::vector<uint8_t> idx;
  const uint8_t *data = base;
  size_t have = len;
  if (!base) {
    have = std::min<size_t>(len, 8192);
    idx.resize(have);
    if (!src->read(0, idx.data(), have)) return false;
    data = idx.data();
  }
  auto need = [&](size_t upto) {
    if (upto <= have) return true;
    if (base || upto > len) return false;
    size_t nh = std::min(len, std::max(upto, have * 2));
    idx.resize(nh);
    if (!src->read(have, idx.data() + have, nh - have)) return false;
    have = nh;
    data = idx.data();
    return true;
  };
  if (!need(8) || memcmp(data, "IDB1", 4) != 0) return false;
  uint32_t n = rd32(data + 4);
  size_t p = 8;
  for (uint32_t i = 0; i < n; i++) {
    if (!need(p + 2)) return false;
    uint16_t pl = rd16(data + p);
    p += 2;
    if (!need(p + pl + 8)) return false;
    std::string path((const char *)data + p, pl);
    p += pl;
    uint32_t off = rd32(data + p), size = rd32(data + p + 4);
    p += 8;
    if ((size_t)off + size > len) return false;
    File f;
    f.data = base ? base + off : nullptr;
    f.size = size;
    f.src = src;
    f.off = off;
    files_[prefix + path] = f;
  }
  sources_[prefix] = src;
  // overlays survive a reload of the base bundle
  for (auto &o : owned_)
    if (startsWith(o.first, prefix.c_str())) {
      File f;
      f.data = o.second.data();
      f.size = o.second.size();
      files_[o.first] = f;
    }
  return true;
}

void Bundle::clear() {
  files_.clear();
  owned_.clear();
  sources_.clear();
}

const Bundle::File *Bundle::find(const std::string &path) const {
  std::string p = path;
  while (startsWith(p, "./")) p = p.substr(2);
  while (!p.empty() && p[0] == '/') p = p.substr(1);
  size_t q = p.find_first_of("?#");
  if (q != std::string::npos) p = p.substr(0, q);
  auto it = files_.find(p);
  return it == files_.end() ? nullptr : &it->second;
}

std::string Bundle::text(const std::string &path) const {
  const File *f = find(path);
  if (!f) return std::string();
  std::string s(f->size, '\0');
  if (f->size && !f->read(0, &s[0], f->size)) return std::string();
  return s;
}

std::shared_ptr<std::vector<uint8_t>> Bundle::bytes(const std::string &path) const {
  const File *f = find(path);
  if (!f) return nullptr;
  auto v = std::make_shared<std::vector<uint8_t>>(f->size);
  if (f->size && !f->read(0, v->data(), f->size)) return nullptr;
  return v;
}

void Bundle::put(const std::string &path, const uint8_t *data, size_t len) {
  // keep 4-byte alignment for binary assets (fonts/images are read in place)
  std::vector<uint8_t> v(data, data + len);
  v.reserve(len + 4);
  owned_[path] = std::move(v);
  File f;
  f.data = owned_[path].data();
  f.size = len;
  files_[path] = f;
}

bool Bundle::remove(const std::string &path) {
  owned_.erase(path);
  return files_.erase(path) > 0;
}

std::vector<std::string> Bundle::list() const {
  std::vector<std::string> v;
  for (auto &f : files_) v.push_back(f.first);
  std::sort(v.begin(), v.end());
  return v;
}

std::string Bundle::resolve(const std::string &base, const std::string &rel) {
  if (rel.empty()) return base;
  if (rel.find("://") != std::string::npos || startsWith(rel, "data:")) return rel;
  std::string r = rel;
  size_t q = r.find_first_of("?#");
  std::string suffix;
  if (q != std::string::npos) {
    suffix = r.substr(q);
    r = r.substr(0, q);
  }
  std::vector<std::string> parts;
  if (!r.empty() && r[0] == '/') {
    r = r.substr(1);
  } else {
    size_t slash = base.rfind('/');
    if (slash != std::string::npos)
      for (auto &s : split(base.substr(0, slash), '/'))
        if (!s.empty()) parts.push_back(s);
  }
  for (auto &s : split(r, '/')) {
    if (s.empty() || s == ".") continue;
    if (s == "..") {
      if (!parts.empty()) parts.pop_back();
      continue;
    }
    parts.push_back(s);
  }
  std::string out;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i) out += '/';
    out += parts[i];
  }
  return out + suffix;
}

std::shared_ptr<Image> Image::fromImg1(const uint8_t *data, size_t len) {
  if (len < sizeof(ImgHeader) || memcmp(data, "IMG1", 4) != 0) return nullptr;
  const ImgHeader *h = (const ImgHeader *)data;
  auto img = std::make_shared<Image>();
  img->w = h->w;
  img->h = h->h;
  size_t n = (size_t)h->w * h->h;
  const uint8_t *p = data + sizeof(ImgHeader);
  if (h->flags & IMGF_MASK) {
    if (len < sizeof(ImgHeader) + n) return nullptr;
    img->mask = true;
    img->alpha = p;
    // masks still need a colour plane for generic drawing: use a tiny white plane
    img->owned.resize(n * 2, 0xFF);
    img->px = (const uint16_t *)img->owned.data();
    return img;
  }
  if (len < sizeof(ImgHeader) + n * 2) return nullptr;
  img->px = (const uint16_t *)p;
  if (h->flags & IMGF_ALPHA) {
    if (len < sizeof(ImgHeader) + n * 3) return nullptr;
    img->alpha = p + n * 2;
  }
  return img;
}

}  // namespace rt
