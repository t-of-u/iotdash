#pragma once
// App bundle: a flat archive produced by the host compiler.
//
//   "IDB1" u32 count  { u16 pathLen, path, u32 offset, u32 size }*  data (4-byte aligned)
//
// The archive is an ImageSource: in RAM its files are used in place (File::data); in slow
// storage (the device's flash) File::data is null and the bytes come through File::read --
// text() and bytes() for whole files, fonts read their tables and glyphs. Hot reload replaces
// individual files (overlay) without rebuilding it.

#include "rt_base.h"

namespace rt {

class Bundle {
 public:
  using File = ImageSlice;
  bool load(const uint8_t *data, size_t len);  // data must outlive the bundle
  // Several archives side by side (OS: "system/" and the running app's "app/"): files of
  // data appear under prefix. unmount drops every file under prefix (overlays included).
  bool mount(const std::string &prefix, const uint8_t *data, size_t len);  // in RAM, outliving it
  bool mount(const std::string &prefix, std::shared_ptr<const ImageSource> src);
  void unmount(const std::string &prefix);
  const ImageSource *sourceOf(const std::string &prefix) const;
  void clear();
  const File *find(const std::string &path) const;
  std::string text(const std::string &path) const;
  std::shared_ptr<std::vector<uint8_t>> bytes(const std::string &path) const;  // a RAM copy
  bool has(const std::string &path) const { return find(path) != nullptr; }
  // Overlay a file (hot reload); the bundle owns the copy.
  void put(const std::string &path, const uint8_t *data, size_t len);
  bool remove(const std::string &path);
  std::vector<std::string> list() const;
  static std::string resolve(const std::string &base, const std::string &rel);  // URL-ish path join

 private:
  std::unordered_map<std::string, File> files_;
  std::unordered_map<std::string, std::vector<uint8_t>> owned_;
  std::unordered_map<std::string, std::shared_ptr<const ImageSource>> sources_;  // by prefix
};

}  // namespace rt
