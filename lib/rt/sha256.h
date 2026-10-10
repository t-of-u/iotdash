#pragma once
// SHA-256 (FIPS 180-4): checks the files of an app package (.idbz) as they are installed.
#include <cstddef>
#include <cstdint>
#include <string>

namespace rt {

class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const void *data, size_t len);
  void final(uint8_t out[32]);
  static std::string hex(const uint8_t h[32]);

 private:
  void block(const uint8_t *p);
  uint32_t h_[8];
  uint8_t buf_[64];
  uint64_t bytes_ = 0;
  size_t fill_ = 0;
};

}  // namespace rt
