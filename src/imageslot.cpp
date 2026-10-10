#include "imageslot.h"

#include <esp_heap_caps.h>
#include <esp_rom_sys.h>

#include <cstdio>
#include <cstring>

static constexpr uint32_t kHeader = 4096;             // header sector before the image
static constexpr uint32_t kAppSlot = 0x180000;        // 1.5 MB per app slot
static constexpr uint32_t kChunk = 64 * 1024;         // erased + written per step

struct SlotHeader {
  char magic[4];  // "ISL1"
  uint32_t size;
  uint32_t stamp;  // copy order (the oldest app slot is reused first)
  char key[48];
  char hash[24];
};

static bool readHeader(const esp_partition_t *p, uint32_t off, SlotHeader &h) {
  return esp_partition_read(p, off, &h, sizeof h) == ESP_OK && memcmp(h.magic, "ISL1", 4) == 0;
}

// An image in a flash slot, read on demand through the flash driver -- never mapped into the
// address space, so nothing can read it by accident while drawing. Long reads go in 4 KB
// pieces with a pause between them: the panel's bounce-buffer refills share the bus.
class PartitionSource : public rt::ImageSource {
 public:
  PartitionSource(const esp_partition_t *p, uint32_t off, uint32_t size) : part_(p), off_(off), size_(size) {}
  size_t size() const override { return size_; }
  bool read(size_t off, void *dst, size_t n) const override {
    if (off > size_ || n > size_ - off) return false;
    auto *d = (uint8_t *)dst;
    while (n) {
      size_t k = std::min<size_t>(n, 4096);
      if (esp_partition_read(part_, off_ + off, d, k) != ESP_OK) return false;
      d += k;
      off += k;
      n -= k;
      if (n) esp_rom_delay_us(150);
    }
    return true;
  }

 private:
  const esp_partition_t *part_;
  uint32_t off_, size_;
};

class FlashImage : public rt::ImageLoad {
 public:
  FlashImage(const esp_partition_t *p, uint32_t off, uint32_t slotSize, const std::string &key, const std::string &path,
             const std::string &hash, uint32_t stamp)
      : part_(p), off_(off), slotSize_(slotSize), stamp_(stamp) {
    strncpy(key_, key.c_str(), sizeof key_ - 1);
    strncpy(hash_, hash.c_str(), sizeof hash_ - 1);
    SlotHeader h;
    if (!hash.empty() && readHeader(p, off, h) && strncmp(h.key, key_, sizeof h.key) == 0 &&
        strncmp(h.hash, hash_, sizeof h.hash) == 0) {
      size_ = h.size;
      ready();  // already there
      return;
    }
    f_ = fopen(path.c_str(), "rb");
    if (!f_) {
      err_ = "cannot open " + path;
      return;
    }
    fseek(f_, 0, SEEK_END);
    size_ = (uint32_t)ftell(f_);
    fseek(f_, 0, SEEK_SET);
    if (size_ == 0 || size_ > slotSize - kHeader) {
      err_ = "image of " + std::to_string(size_ / 1024) + " KB does not fit its " + std::to_string(slotSize / 1024) +
             " KB flash slot";
      return;
    }
    buf_ = (uint8_t *)heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM);
    // the header goes first: a copy cut short is never taken for a valid image
    if (!buf_ || esp_partition_erase_range(p, off, kHeader) != ESP_OK) err_ = "flash erase failed";
  }
  ~FlashImage() override {
    if (f_) fclose(f_);
    heap_caps_free(buf_);
  }

  float step() override {
    if (!err_.empty() || src_) return 1;
    uint32_t n = std::min(kChunk, size_ - done_);
    uint32_t erase = (n + 4095) & ~4095u;
    // in 4 KB reads with pauses: from the flash file system they share the bus the panel
    // refills its bounce buffers over (a 64 KB burst would glitch the screen)
    for (uint32_t got = 0; got < n;) {
      size_t k = std::min<uint32_t>(4096, n - got);
      if (fread(buf_ + got, 1, k, f_) != k) {
        err_ = "card read failed";
        return 1;
      }
      got += k;
      if (got < n) esp_rom_delay_us(150);
    }
    if (esp_partition_erase_range(part_, off_ + kHeader + done_, erase) != ESP_OK ||
        esp_partition_write(part_, off_ + kHeader + done_, buf_, n) != ESP_OK) {
      err_ = "flash write failed";
      return 1;
    }
    done_ += n;
    if (done_ < size_) return (float)done_ / size_;
    SlotHeader h = {};
    memcpy(h.magic, "ISL1", 4);
    h.size = size_;
    h.stamp = stamp_;
    memcpy(h.key, key_, sizeof h.key);
    memcpy(h.hash, hash_, sizeof h.hash);
    if (esp_partition_write(part_, off_, &h, sizeof h) != ESP_OK) {
      err_ = "flash write failed";
      return 1;
    }
    fclose(f_);
    f_ = nullptr;
    heap_caps_free(buf_);
    buf_ = nullptr;
    ready();
    return 1;
  }
  bool failed() const override { return !err_.empty(); }
  std::string error() const override { return err_; }
  std::shared_ptr<const rt::ImageSource> source() const override { return src_; }

 private:
  void ready() { src_ = std::make_shared<PartitionSource>(part_, off_ + kHeader, size_); }

  const esp_partition_t *part_;
  uint32_t off_, slotSize_, stamp_;
  char key_[48] = {}, hash_[24] = {};
  FILE *f_ = nullptr;
  uint8_t *buf_ = nullptr;
  uint32_t size_ = 0, done_ = 0;
  std::string err_;
  std::shared_ptr<const rt::ImageSource> src_;
};

namespace {
// failed() from the start (no partition, ...)
class FailedImage : public rt::ImageLoad {
 public:
  explicit FailedImage(std::string e) : err_(std::move(e)) {}
  float step() override { return 1; }
  bool failed() const override { return true; }
  std::string error() const override { return err_; }
  std::shared_ptr<const rt::ImageSource> source() const override { return nullptr; }

 private:
  std::string err_;
};
}  // namespace

std::unique_ptr<rt::ImageLoad> loadImageToFlash(const std::string &key, const std::string &path, const std::string &hash) {
  if (key == "system") {
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "sys");
    if (!p) return std::make_unique<FailedImage>("no 'sys' flash partition");
    return std::make_unique<FlashImage>(p, 0, p->size, key, path, hash, 0);
  }
  const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x41, "slots");
  if (!p) return std::make_unique<FailedImage>("no 'slots' flash partition");
  // the slot that has this image, else one that had this app, else an empty one, else the oldest
  int n = (int)(p->size / kAppSlot), pick = -1, same = -1, empty = -1, oldest = -1;
  uint32_t oldestStamp = UINT32_MAX, maxStamp = 0;
  for (int i = 0; i < n; i++) {
    SlotHeader h;
    if (!readHeader(p, i * kAppSlot, h)) {
      if (empty < 0) empty = i;
      continue;
    }
    maxStamp = std::max(maxStamp, h.stamp);
    if (strncmp(h.key, key.c_str(), sizeof h.key) == 0) {
      if (strncmp(h.hash, hash.c_str(), sizeof h.hash) == 0) pick = i;
      else same = i;
    }
    if (h.stamp < oldestStamp) {
      oldestStamp = h.stamp;
      oldest = i;
    }
  }
  if (pick < 0) pick = same >= 0 ? same : empty >= 0 ? empty : oldest >= 0 ? oldest : 0;
  return std::make_unique<FlashImage>(p, pick * kAppSlot, kAppSlot, key, path, hash, maxStamp + 1);
}
