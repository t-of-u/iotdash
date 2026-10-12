// iotdash OS firmware for the Waveshare ESP32-S3-Touch-LCD-7: the HTML/CSS/JS engine, the SD
// card, the registry and the flash slots programs are loaded into. It shows the boot screen
// (built in) and brings the system up from the card (see lib/rt/os.h).
//
// Core 0: panel scan-out interrupt (compositor), paint helper, WiFi/HTTP worker.
// Core 1: engine task - touch, scripts, style, layout, paint (through DMA into PSRAM).

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_memory_utils.h>
#include <esp_mmu_map.h>
#include <esp_partition.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <esp_cpu.h>

#include <dirent.h>

#include <deque>
#include <map>
#include <mutex>

#include "board.h"
#include "ch422g.h"
#include "devlink.h"
#include "dma_blit.h"
#include "engine.h"
#include "gt911.h"
#include "js.h"
#include "prof.h"
#include "microbench.h"
#include "rgb_display.h"
#include "imageslot.h"
#include "os.h"
#include "radio.h"
#include "sdcard.h"

using namespace rt;

static const uint32_t kBaud = 921600;

// ============================================================ memory: engine + JS live in PSRAM

void *operator new(size_t n) {
  void *p = heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = malloc(n ? n : 1);
  if (!p) {  // the panic that follows shows where; this says what was asked for and what was left
    esp_rom_printf("\nout of memory: new %u B | psram free %u largest %u | internal free %u largest %u\n", (unsigned)n,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    abort();
  }
  return p;
}
void *operator new[](size_t n) { return operator new(n); }
void *operator new(size_t n, const std::nothrow_t &) noexcept {
  void *p = heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n ? n : 1);
}
void *operator new[](size_t n, const std::nothrow_t &t) noexcept { return operator new(n, t); }
void operator delete(void *p) noexcept { heap_caps_free(p); }
void operator delete[](void *p) noexcept { heap_caps_free(p); }
void operator delete(void *p, size_t) noexcept { heap_caps_free(p); }
void operator delete[](void *p, size_t) noexcept { heap_caps_free(p); }

namespace rt {
void *jsMalloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void *jsCalloc(size_t c, size_t n) { return heap_caps_calloc(c, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void *jsRealloc(void *p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void jsFree(void *p) { heap_caps_free(p); }
}  // namespace rt

// ============================================================ hardware

static CH422G io;
static GT911 touch;
static RgbDisplay lcd;
static DmaBlitter dma;
static DevLink devlink;
static SdCard sd;

// the boot screen image (os/build/boot.idb, from: tools/iotdash.py os build)
extern const uint8_t bootImageStart[] asm("_binary_os_build_boot_idb_start");
extern const uint8_t bootImageEnd[] asm("_binary_os_build_boot_idb_end");

namespace rt {
uint32_t cycles() { return esp_cpu_get_cycle_count(); }
uint64_t nowUs() { return (uint64_t)esp_timer_get_time(); }
void logWrite(const char *s) { devlink.log(s); }
}  // namespace rt

// ============================================================ display adapter

// Paints on core 0 (otherwise idle apart from the scan-out interrupt and WiFi): each
// band is split in two, the engine core paints the left half while this task paints the
// right half. Painting only reads the document (lazy caches lock themselves).
class PaintHelper {
 public:
  bool begin() {
    done_ = xSemaphoreCreateBinary();
    return xTaskCreatePinnedToCore(task, "paint1", 9 * 1024, this, 5, &th_, 0) == pdPASS;  // ~5.6 KB used
  }
  void run(const std::function<void()> *job) {
    job_ = job;
    xTaskNotifyGive(th_);
  }
  void wait() {
    uint32_t t0 = esp_cpu_get_cycle_count();
    xSemaphoreTake(done_, portMAX_DELAY);
    waitCycles += esp_cpu_get_cycle_count() - t0;
  }
  bool ready() const { return th_ != nullptr; }
  volatile uint64_t busyCycles = 0, waitCycles = 0;  // helper painting / engine core waiting for it
  volatile uint32_t lastJobCycles = 0;

 private:
  static void task(void *arg) {
    auto *self = static_cast<PaintHelper *>(arg);
    rt::profMuted = true;
    for (;;) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      uint32_t t0 = esp_cpu_get_cycle_count();
      (*self->job_)();
      uint32_t dt = esp_cpu_get_cycle_count() - t0;
      self->lastJobCycles = dt;
      self->busyCycles += dt;
      xSemaphoreGive(self->done_);
    }
  }
  TaskHandle_t th_ = nullptr;

 public:
  TaskHandle_t handle() const { return th_; }
  SemaphoreHandle_t done_ = nullptr;
  const std::function<void()> *volatile job_ = nullptr;
};
static PaintHelper paintHelper;
static bool dualCore = true;

class DeviceDisplay : public PanelDisplay {
  static constexpr int kSplitBands = 48;
  float splitFrac_[kSplitBands];

 public:
  DeviceDisplay() {
    for (auto &f : splitFrac_) f = 0.5f;
  }
  int width() const override { return lcd.width(); }
  int height() const override { return lcd.height(); }
  int alignX() const override { return DmaBlitter::ALIGN_PX; }
  void render(const RectI &r, const std::function<void(const Band &)> &draw) override {
    Rect rr{(int16_t)r.x, (int16_t)r.y, (int16_t)r.w, (int16_t)r.h};
    lcd.waitPresented();  // the previous frame may still be latching
    dma.render(lcd.back(), rr, [&](uint16_t *band, int y, int rows) {
      if (dualCore && paintHelper.ready() && r.w >= 128 && r.w * rows >= 4096) {
        // left part here, right part on core 0: each core walks only the elements on its
        // side, over the full band height. Where the content is lopsided the split point
        // follows the measured cost (per screen band, carried over between frames).
        float &frac = splitFrac_[std::min(kSplitBands - 1, y / 10)];
        int w1 = ((int)(r.w * frac) + 4) & ~7;
        w1 = std::max(32, std::min(r.w - 32, w1));
        Band right{band + w1, r.w, r.x + w1, y, r.w - w1, rows};
        struct Job {
          const std::function<void(const Band &)> *draw;
          const Band *band;
          void operator()() const { (*draw)(*band); }
        };
        std::function<void()> job = Job{&draw, &right};  // fits std::function's inline storage
        paintHelper.run(&job);
        uint32_t t0 = esp_cpu_get_cycle_count();
        draw(Band{band, r.w, r.x, y, w1, rows});
        uint32_t tl = esp_cpu_get_cycle_count() - t0;
        paintHelper.wait();
        uint32_t tr = paintHelper.lastJobCycles;
        if (r.w >= lcd.width() / 2 && tl + tr > 24000) {
          // move the split by half the imbalance, assuming each side's cost is spread evenly
          float wl = (float)w1, wr = (float)(r.w - w1);
          float dl = tl / wl, dr = tr / wr;  // cycles per column
          float target = (tl + tr) / 2.f;
          float nw = tl > tr ? wl - (tl - target) / dl : wl + (tr - target) / dr;
          float f = (wl + (nw - wl) * 0.5f) / r.w;
          frac = std::max(0.1f, std::min(0.9f, f));
        }
        return;
      }
      draw(Band{band, r.w, r.x, y, r.w, rows});
    });
  }
  void present() override { lcd.present(); }
  bool transition(int kind, int ms, const RectI &a) override {
    RgbDisplay::Slide s;
    switch (kind) {
      case TR_SLIDE_LEFT: s = RgbDisplay::SLIDE_LEFT; break;
      case TR_SLIDE_RIGHT: s = RgbDisplay::SLIDE_RIGHT; break;
      case TR_SLIDE_UP: s = RgbDisplay::SLIDE_UP; break;
      case TR_SLIDE_DOWN: s = RgbDisplay::SLIDE_DOWN; break;
      default: return false;
    }
    lcd.slide(s, ms, Rect{(int16_t)a.x, (int16_t)a.y, (int16_t)a.w, (int16_t)a.h});
    return true;
  }
  float refreshHz() const override { return lcd.refreshHz(); }
  int bufferCount() const override { return lcd.bufferCount(); }
};

// ============================================================ platform

struct FetchJob {
  FetchRequest req;
  std::function<void(FetchResponse)> done;
  // a download (Platform::download): the body goes to the sink as it arrives, not into memory
  Platform::DownloadSink sink;
  Platform::DownloadDone dlDone;
};

class DevicePlatform : public Platform {
 public:
  void begin() {
    jobs_ = xQueueCreate(8, sizeof(FetchJob *));
    // stack in PSRAM: internal RAM is for the radios (TLS needs ~6 KB of it while connecting)
    xTaskCreatePinnedToCoreWithCaps(worker, "http", 10 * 1024, this, 4, &httpTask, 0, MALLOC_CAP_SPIRAM);
    prefs_.begin("iotdash", false);
    radio::begin(
        [this](std::function<void()> f) {
          std::lock_guard<std::mutex> lk(mu_);
          done_.push_back(std::move(f));
        },
        [this](const std::string &name, const std::string &json) {
          if (radioEvent) radioEvent(name, json);
        });
  }
  uint64_t micros() override { return (uint64_t)esp_timer_get_time(); }
  void log(const char *s) override { devlink.log(s); }

  void fetch(const FetchRequest &req, std::function<void(FetchResponse)> done) override {
    auto *job = new FetchJob{req, std::move(done)};
    if (xQueueSend(jobs_, &job, 0) != pdTRUE) {
      FetchResponse r;
      r.error = "too many requests";
      complete(job, std::move(r));
    }
  }
  void download(const std::string &url, DownloadSink sink, DownloadDone done) override {
    auto *job = new FetchJob{};
    job->req.url = url;
    job->req.timeoutMs = 20000;
    job->sink = std::move(sink);
    job->dlDone = std::move(done);
    if (xQueueSend(jobs_, &job, 0) != pdTRUE) finishDownload(job, "too many requests", 0);
  }
  void radio(const std::string &op, const std::string &args, RadioDone done) override { radio::request(op, args, done); }
  TaskHandle_t httpTask = nullptr;
  void pollAsync() override {
    radio::tick();
    std::vector<std::function<void()>> q;
    {
      std::lock_guard<std::mutex> lk(mu_);
      q.swap(done_);
    }
    for (auto &f : q) f();
  }
  // plain localStorage is only used without the OS (apps keep theirs in memory.json)
  bool storageGet(const std::string &k, std::string &out) override {
    auto it = store_.find(k);
    if (it == store_.end()) return false;
    out = it->second;
    return true;
  }
  void storageSet(const std::string &k, const std::string &v) override { store_[k] = v; }
  void storageRemove(const std::string &k) override { store_.erase(k); }
  std::vector<std::string> storageKeys() override {
    std::vector<std::string> v;
    for (auto &kv : store_) v.push_back(kv.first);
    return v;
  }
  Preferences &prefs() { return prefs_; }

  // ---- OS services
  // Storage: the SD card, or, when there is none at boot (or "storage flash" was chosen), the
  // LittleFS partition "fs" in flash standing in for it with the same layout.
  static constexpr const char *kFlashRoot = "/flash";
  bool sdMount(std::string &err) override {
    if (flash_) return true;
    bool flashOnly = prefs_.isKey("storage") && prefs_.getString("storage") == "flash";
    if (!flashOnly && sd.mount(err)) return true;
    if (!LittleFS.begin(true, kFlashRoot, 10, "fs")) {
      err += err.empty() ? "flash file system unavailable" : "; flash file system unavailable";
      return false;
    }
    flash_ = true;
    devlink.log(("storage: internal flash" + (err.empty() ? std::string() : " (SD: " + err + ")")).c_str());
    return true;
  }
  std::string sdRoot() const override { return flash_ ? kFlashRoot : sd.mounted() ? SdCard::kRoot : ""; }
  SdInfo sdInfo() override {
    SdInfo i;
    if (flash_) {
      i.mounted = true;
      i.totalBytes = LittleFS.totalBytes();
      i.freeBytes = i.totalBytes - LittleFS.usedBytes();
      i.type = "FLASH";
      i.name = "internal";
      return i;
    }
    i.mounted = sd.info(i.totalBytes, i.freeBytes, i.type, i.name);
    return i;
  }
  bool sdFormat(std::string &err) override {
    if (!flash_) return sd.format(err);
    if (LittleFS.format()) return true;
    err = "flash file system format failed";
    return false;
  }
  bool onFlash() const { return flash_; }
  // the registry: one JSON blob in the "reg" NVS partition
  std::string registryLoad() override {
    Preferences r;
    if (!r.begin("registry", false, "reg")) return "{}";  // read-write: creates the namespace
    size_t n = r.isKey("json") ? r.getBytesLength("json") : 0;
    std::string s(n, '\0');
    if (n) r.getBytes("json", &s[0], n);
    r.end();
    return n ? s : "{}";
  }
  void registrySave(const std::string &json) override {
    Preferences r;
    if (!r.begin("registry", false, "reg")) return;
    r.putBytes("json", json.data(), json.size());
    r.end();
  }
  std::unique_ptr<ImageLoad> loadImage(const std::string &key, const std::string &path, const std::string &hash) override {
    return loadImageToFlash(key, path, hash);
  }
  void reboot() override { esp_restart(); }

 private:
  // the response moves on to the page (no copy: a body can be a large share of the free memory)
  void complete(FetchJob *job, FetchResponse &&r) {
    std::lock_guard<std::mutex> lk(mu_);
    auto done = job->done;
    done_.push_back([done, r = std::move(r)]() mutable { done(std::move(r)); });
    delete job;
  }

  // Room for `extra` more body bytes. A growing string needs its old and new buffers at once, and
  // the page then needs as much again for its script string (the script engine fails gracefully
  // past that, the rest of the engine does not): a body that does not fit fails its fetch
  // ("ESP_ERR_NO_MEM") instead of running the board out of memory.
  static bool bodyRoom(std::string &body, size_t extra) {
    static constexpr size_t kMaxBody = 1024 * 1024, kLeave = 64 * 1024;
    size_t need = body.size() + extra;
    if (need > kMaxBody) return false;
    if (need <= body.capacity()) return true;
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t cap = std::min(kMaxBody, std::max(need, body.capacity() + body.capacity() / 2));
    if (largest < cap + kLeave) cap = need;  // tight: exactly what is needed
    if (largest < cap + kLeave || heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < cap + need + kLeave) return false;
    body.reserve(cap);
    return true;
  }

  void finishDownload(FetchJob *job, const std::string &err, int status) {
    std::lock_guard<std::mutex> lk(mu_);
    auto done = std::move(job->dlDone);
    done_.push_back([done, err, status]() { done(err, status); });
    delete job;
  }

  // A download on the worker: GET (redirects followed), the body in 4 KB pieces to the sink, which
  // may stop it (returns false).
  void runDownload(FetchJob *job) {
    if (WiFi.status() != WL_CONNECTED) return finishDownload(job, "WiFi not connected", 0);
    esp_http_client_config_t cfg = {};
    cfg.url = job->req.url.c_str();
    cfg.timeout_ms = job->req.timeoutMs;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 3072;  // the request line: presigned storage URLs run to ~1.5 KB
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    std::string err;
    int status = 0;
    for (int hop = 0;; hop++) {
      if (esp_http_client_open(c, 0) != ESP_OK) {
        err = "cannot connect";
        break;
      }
      int64_t len = esp_http_client_fetch_headers(c);
      if (len < 0) {
        err = "no answer";
        break;
      }
      status = esp_http_client_get_status_code(c);
      bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
      if (redirect && hop < 5 && esp_http_client_set_redirection(c) == ESP_OK) {
        esp_http_client_close(c);
        continue;
      }
      if (status < 200 || status >= 300) break;
      static constexpr size_t kPiece = 4096;
      std::unique_ptr<char, void (*)(void *)> buf((char *)heap_caps_malloc(kPiece, MALLOC_CAP_SPIRAM), heap_caps_free);
      if (!buf) {
        err = "no memory";
        break;
      }
      int n;
      while ((n = esp_http_client_read(c, buf.get(), kPiece)) > 0)
        if (!job->sink((const uint8_t *)buf.get(), (size_t)n, len > 0 ? (uint64_t)len : 0)) {
          err = "stopped";
          break;
        }
      if (err.empty() && n < 0) err = "the connection broke";
      break;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    finishDownload(job, err, status);
  }

  static esp_err_t onEvent(esp_http_client_event_t *e) {
    auto *r = (FetchResponse *)e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER) {
      std::string k = e->header_key;
      for (auto &c : k) c = (char)tolower((unsigned char)c);
      r->headers.push_back({k, e->header_value});
    }
    return ESP_OK;  // the body: esp_http_client_read in worker()
  }

  static void worker(void *arg) {
    auto *self = (DevicePlatform *)arg;
    while (true) {
      FetchJob *job;
      if (xQueueReceive(self->jobs_, &job, portMAX_DELAY) != pdTRUE) continue;
      if (job->sink) {
        self->runDownload(job);
        continue;
      }
      FetchResponse r;
      r.url = job->req.url;
      if (WiFi.status() != WL_CONNECTED) {
        r.error = "WiFi not connected";
        self->complete(job, std::move(r));
        continue;
      }
      esp_http_client_config_t cfg = {};
      cfg.url = job->req.url.c_str();
      cfg.timeout_ms = job->req.timeoutMs;
      cfg.event_handler = onEvent;
      cfg.user_data = &r;
      cfg.crt_bundle_attach = esp_crt_bundle_attach;
      cfg.buffer_size = 2048;
      cfg.buffer_size_tx = 1024;
      std::string m = job->req.method;
      cfg.method = m == "POST" ? HTTP_METHOD_POST : m == "PUT" ? HTTP_METHOD_PUT : m == "DELETE" ? HTTP_METHOD_DELETE
                 : m == "PATCH" ? HTTP_METHOD_PATCH : m == "HEAD" ? HTTP_METHOD_HEAD : HTTP_METHOD_GET;
      esp_http_client_handle_t c = esp_http_client_init(&cfg);
      for (auto &h : job->req.headers) esp_http_client_set_header(c, h.first.c_str(), h.second.c_str());
      // The streaming calls, not esp_http_client_perform: that one fails a request answered 401
      // without an auth challenge (a wrong password is an answer for the page, body and all).
      // Redirects are followed as fetch() does, five at most.
      std::string body = job->req.body;
      esp_err_t err = ESP_OK;
      for (int hop = 0;; hop++) {
        r.headers.clear();
        r.body.clear();
        err = esp_http_client_open(c, (int)body.size());
        if (err != ESP_OK) break;
        if (!body.empty() && esp_http_client_write(c, body.data(), (int)body.size()) != (int)body.size()) {
          err = ESP_FAIL;
          break;
        }
        int64_t len = esp_http_client_fetch_headers(c);
        if (len < 0) {
          err = ESP_FAIL;
          break;
        }
        int status = esp_http_client_get_status_code(c);
        bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        if (redirect && hop < 5 && esp_http_client_set_redirection(c) == ESP_OK) {
          esp_http_client_close(c);
          if (status <= 303 && m != "GET" && m != "HEAD") {  // POST and the like go on as GET
            esp_http_client_set_method(c, HTTP_METHOD_GET);
            esp_http_client_delete_header(c, "Content-Type");
            body.clear();
          }
          continue;
        }
        if (len > 0 && !bodyRoom(r.body, (size_t)len)) {  // the whole body at once when its size is known
          err = ESP_ERR_NO_MEM;
          break;
        }
        char buf[1024];
        int n;
        while ((n = esp_http_client_read(c, buf, sizeof buf)) > 0) {
          if (!bodyRoom(r.body, n)) {
            err = ESP_ERR_NO_MEM;
            break;
          }
          r.body.append(buf, n);
        }
        if (err != ESP_OK) break;
        if (n < 0) {
          err = ESP_FAIL;
          break;
        }
        r.status = status;
        r.statusText = status == 200 ? "OK" : "";
        char u[512];
        if (hop && esp_http_client_get_url(c, u, sizeof u) == ESP_OK) r.url = u;
        break;
      }
      if (err != ESP_OK) {
        r.error = esp_err_to_name(err);
        r.body.clear();
        r.body.shrink_to_fit();
        if (err == ESP_ERR_NO_MEM) devlink.log(("http: no memory for the answer of " + job->req.url).c_str());
      }
      esp_http_client_close(c);
      esp_http_client_cleanup(c);
      self->complete(job, std::move(r));
    }
  }

  QueueHandle_t jobs_ = nullptr;
  std::mutex mu_;
  std::vector<std::function<void()>> done_;
  std::map<std::string, std::string> store_;
  Preferences prefs_;
  bool flash_ = false;  // storage is the flash file system, not the SD card
};

static DevicePlatform plat;

static void ioPause() {
  if (plat.onFlash()) esp_rom_delay_us(150);  // a file read from the flash file system
}
static DeviceDisplay disp;
static Engine *eng = nullptr;
static JsHost *js = nullptr;
static Os *os = nullptr;

// ============================================================ dev link

static void sendShot(bool scanned) {
  // RLE-compressed RGB565 front buffer: (count:u8, pixel:u16) runs. Streamed: a dithered
  // screen can need over a megabyte, more than the free PSRAM. scanned: the page the panel is
  // sending (what the eye sees), not the newest presented one
  lcd.invalidatePages();
  const uint16_t *fb = scanned && lcd.scanPage() ? lcd.scanPage() : lcd.front();
  int W = lcd.width(), H = lcd.height();
  size_t n = (size_t)W * H;
  auto runs = [&](const std::function<void(uint8_t, uint16_t)> &out) {
    size_t i = 0;
    while (i < n) {
      uint16_t c = fb[i];
      size_t run = 1;
      while (i + run < n && run < 255 && fb[i + run] == c) run++;
      out((uint8_t)run, c);
      i += run;
    }
  };
  size_t count = 0;
  runs([&](uint8_t, uint16_t) { count++; });
  devlink.begin(DevLink::R_SHOT, 4 + count * 3);
  uint8_t hdr[4] = {(uint8_t)(W & 255), (uint8_t)(W >> 8), (uint8_t)(H & 255), (uint8_t)(H >> 8)};
  devlink.write(hdr, 4);
  static uint8_t buf[3 * 1024];
  size_t k = 0, sent = 0, seen = 0;
  auto put = [&](uint8_t r, uint16_t c) {
    buf[k++] = r;
    buf[k++] = c & 255;
    buf[k++] = c >> 8;
    if (k == sizeof buf) {
      devlink.write(buf, k);
      k = 0;
    }
  };
  // exactly `count` runs, whatever happens to the pixels meanwhile: the frame length is sent
  runs([&](uint8_t r, uint16_t c) {
    if (seen++ < count) {
      put(r, c);
      sent++;
    }
  });
  while (sent < count) put(0, 0), sent++;  // empty runs
  if (k) devlink.write(buf, k);
  devlink.end();
  if (seen != count) {
    char b[80];
    snprintf(b, sizeof b, "shot: frame changed while sending (%u runs, then %u)", (unsigned)count, (unsigned)seen);
    devlink.log(b);
  }
}

static std::string tearPhases() {  // " (load 2, paint 1)"
  std::string s;
  for (int p = 0; p < rt::PH_COUNT; p++)
    if (uint32_t n = lcd.tearsIn(p)) s += std::string(s.empty() ? " (" : ", ") + rt::kPhaseNames[p] + " " + std::to_string(n);
  if (lcd.tears()) {
    char b[64];
    snprintf(b, sizeof b, "%s with blit %u, oldest refill %u us", s.empty() ? " (" : ", ", (unsigned)lcd.tearsWithBlit(),
             (unsigned)lcd.maxTearAgeUs());
    s += b;
  }
  char c[120];
  snprintf(c, sizeof c, " | isr core %d late max %u us, >200us %u, back-to-back %u, dma isr max %u us", lcd.isrCore(),
           (unsigned)lcd.isrLateMaxUs(), (unsigned)lcd.isrLate200(), (unsigned)lcd.isrBackToBack(), (unsigned)lcd.maxDmaIsrUs());
  return (s.empty() ? s : s + ")") + c;
}

static TaskHandle_t engineTaskH = nullptr, touchTaskH = nullptr;

// Internal RAM: what is free, the largest block, the low-water mark, and how much of each
// task's stack has never been used ("heap" text command)
static std::string heapText() {
  char b[320];
  auto hwm = [](TaskHandle_t t) { return t ? (unsigned)uxTaskGetStackHighWaterMark(t) : 0u; };
  snprintf(b, sizeof b,
           "heap: internal free %u KB largest %u KB min %u KB | psram free %u KB largest %u KB | "
           "stack unused: engine %u B paint1 %u B http %u B touch %u B",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024), hwm(engineTaskH),
           hwm(paintHelper.handle()), hwm(plat.httpTask), hwm(touchTaskH));
  return b;
}

static std::string statsText() {
  EngineStats &s = eng->stats();
  char buf[512];
  snprintf(buf, sizeof buf,
           "fps %.1f | frame %.1f ms (style %.1f [%u el] layout %.1f [%s] paint %.1f) damage %u px | lcd %.1f Hz late %u fill avg %u max %u / %u us dmafb %u | "
           "sram %u KB psram %u KB js %u KB fonts-in-ram %u KB | tears %u%s",
           s.fps, s.lastFrameUs / 1000.0, s.lastStyleUs / 1000.0, (unsigned)s.lastRestyled, s.lastLayoutUs / 1000.0,
           s.lastLayoutFull ? "full" : "part", s.lastPaintUs / 1000.0,
           s.lastDamagePx, lcd.refreshHz(), lcd.lateFills(), lcd.avgFillUs(), lcd.maxFillUs(), lcd.budgetUs(), lcd.dmaFallbacks(),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), (unsigned)(js ? js->memoryUsed() / 1024 : 0),
           (unsigned)((rt::residentBytes() + rt::fontsResidentBytes()) / 1024), (unsigned)lcd.tears(), tearPhases().c_str());
  return buf;
}

// Measuring probe on the Sensor AD header (J8 pin 3 = GPIO6, free otherwise): while on, ~10
// lines/s with the average, min and max voltage over the last 100 ms (min/max catch a line
// that toggles, like SPI clock bursts).
static constexpr int kProbePin = 6;
static volatile bool probeOn = false;

static void probeTask(void *) {
  analogReadMilliVolts(kProbePin);  // attaches the pin to the ADC (default 11 dB: 0..~3.1 V)
  while (true) {
    if (!probeOn) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    int mn = 1 << 30, mx = 0, n = 0;
    long sum = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < 100) {
      int mv = analogReadMilliVolts(kProbePin);
      mn = std::min(mn, mv);
      mx = std::max(mx, mv);
      sum += mv;
      n++;
      if ((n & 15) == 0) vTaskDelay(1);
    }
    int avg = (int)(sum / std::max(1, n));
    const char *lv = mx - mn > 800 ? "TOGGLING" : avg > 2000 ? "HIGH" : avg < 800 ? "LOW" : "MID";
    char b[140];
    snprintf(b, sizeof b, "probe GPIO6: %4d mV  min %4d  max %4d  %-8s | %s", avg, mn, mx, lv, sd.lineState().c_str());
    devlink.log(b);
  }
}

// input events (sampled by touchTask, or sent over the dev link), drained by the engine loop
struct TouchEvent {
  uint8_t phase;
  int16_t x, y;
  int64_t us;
};
static QueueHandle_t touchQ = nullptr;

static void onLink(uint8_t type, const uint8_t *d, size_t n) {
  switch (type) {
    case DevLink::T_PING: {
      char b[96];
      snprintf(b, sizeof b, "iotdash OS %s %ux%u card %s", Os::kVersion, lcd.width(), lcd.height(),
               plat.onFlash() ? "flash" : sd.mounted() ? "mounted" : "none");
      devlink.ack(type, true, b);
      break;
    }
    case DevLink::T_FS_WRITE: {  // u8 flags (1 first, 2 last), u16 pathLen, path, u32 offset, data
      static FILE *wf = nullptr;
      if (n < 7) break;
      uint8_t flags = d[0];
      uint16_t pl = d[1] | (d[2] << 8);
      if (3u + pl + 4 > n) break;
      std::string path((const char *)d + 3, pl);
      uint32_t off;
      memcpy(&off, d + 3 + pl, 4);
      const uint8_t *data = d + 7 + pl;
      size_t len = n - 7 - pl;
      std::string host = os ? os->hostPath(path) : "";
      if (host.empty() || path.find("..") != std::string::npos) {
        devlink.ack(type, false, "no card");
        break;
      }
      if (flags & 1) {
        if (wf) fclose(wf);
        size_t slash = host.rfind('/');
        if (slash != std::string::npos) Os::makeDirs(host.substr(0, slash));
        wf = fopen(host.c_str(), "wb");
      }
      bool ok = wf && fseek(wf, off, SEEK_SET) == 0 && fwrite(data, 1, len, wf) == len;
      if ((flags & 2) || !ok) {
        if (wf && fclose(wf) != 0) ok = false;
        wf = nullptr;
      }
      devlink.ack(type, ok, ok ? "" : "write failed: " + path);
      break;
    }
    case DevLink::T_FS_DELETE:
    case DevLink::T_FS_MKDIR:
    case DevLink::T_FS_LIST: {
      std::string path((const char *)d, n), host = os ? os->hostPath(path) : "";
      if (host.empty() || path.find("..") != std::string::npos) {
        devlink.ack(type, false, "no card");
        break;
      }
      if (type == DevLink::T_FS_DELETE) {
        devlink.ack(type, Os::removeTree(host), "");
      } else if (type == DevLink::T_FS_MKDIR) {
        devlink.ack(type, Os::makeDirs(host), "");
      } else {
        std::string out = "[";
        if (DIR *dir = opendir(host.c_str())) {
          while (dirent *e = readdir(dir))
            if (e->d_name[0] != '.') out += (out.size() > 1 ? "," : "") + Json::quote(e->d_name);
          closedir(dir);
        }
        devlink.ack(type, true, out + "]");
      }
      break;
    }
    case DevLink::T_FS_FORMAT: {
      std::string err;
      bool ok = plat.sdFormat(err);
      if (ok && !plat.sdRoot().empty()) {
        Os::makeDirs(plat.sdRoot() + "/system/data");
        Os::makeDirs(plat.sdRoot() + "/applications");
      }
      devlink.ack(type, ok, ok ? "formatted" : err);
      break;
    }
    case DevLink::T_OS_RELOAD:
      devlink.ack(type, true, "reloading");
      if (os) os->reloadSystem();
      break;
    case DevLink::T_APPS_CHANGED:
      if (os) os->appsChanged(std::string((const char *)d, n));
      devlink.ack(type, true, "");
      break;
    case DevLink::T_KIOSK_OFF: {  // a forgotten kiosk password: USB is physical access
      std::string was = os ? os->kioskApp() : "";
      if (os) os->kioskReset();
      devlink.ack(type, os != nullptr, was.empty() ? "kiosk mode was off" : "kiosk mode off (was " + was + ")");
      break;
    }
    case DevLink::T_FILE:
    case DevLink::T_BUNDLE:
    case DevLink::T_COMMIT:
      devlink.ack(type, false, "single bundles are gone: install apps (tools/iotdash.py app install NAME --to device)");
      break;
    case DevLink::T_EVAL:
      devlink.ack(type, true, js ? js->eval(std::string((const char *)d, n)) : "no js");
      break;
    case DevLink::T_SHOT:
      sendShot(n >= 1 && d[0] == 's');
      if (n >= 1 && d[0] == 's') {
        char b[96];
        snprintf(b, sizeof b, "shot: scanned page %s the newest presented one", lcd.scanPage() == lcd.front() ? "IS" : "is NOT");
        devlink.log(b);
      }
      break;
    case DevLink::T_STATS:
      devlink.ack(type, true, statsText());
      break;
    case DevLink::T_OPEN: {  // launch an app by id
      std::string err;
      bool ok = os && os->launch(std::string((const char *)d, n), err);
      devlink.ack(type, ok, err);
      break;
    }
    case DevLink::T_WIFI: {  // ssid \0 pass: joined and saved like Settings does (registry)
      std::string s((const char *)d, n);
      size_t z = s.find('\0');
      std::string ssid = s.substr(0, z), pass = z == std::string::npos ? "" : s.substr(z + 1);
      if (!os) {
        devlink.ack(type, false, "the system is not up");
        break;
      }
      os->net("wifi.connect", "{\"ssid\":" + Json::quote(ssid) + ",\"pass\":" + Json::quote(pass) + "}",
              [type](const std::string &err, const std::string &) {
                devlink.ack(type, err.empty(), err.empty() ? "connecting (saved once it is up)" : err);
              }, true);
      break;
    }
    case DevLink::T_TOUCH: {  // remote touch: phase:u8 x:u16 y:u16 -- through the input queue, like a finger
      if (n >= 5 && touchQ) {
        TouchEvent ev{d[0], (int16_t)(d[1] | (d[2] << 8)), (int16_t)(d[3] | (d[4] << 8)), esp_timer_get_time()};
        xQueueSend(touchQ, &ev, 0);
      }
      break;
    }
    case DevLink::T_RESET:
      esp_restart();
      break;
    case DevLink::R_TEXT: {
      std::string cmd((const char *)d, n);
      if (cmd == "s") devlink.log(statsText().c_str());
      else if (cmd == "heap") devlink.log(heapText().c_str());
      else if (cmd == "sd") devlink.log(("sd probe: " + sd.probe()).c_str());
      else if (cmd == "ilog") {
        rt::gDebugFlags ^= rt::DBG_INPUT;
        devlink.log(rt::gDebugFlags & rt::DBG_INPUT ? "input log on" : "input log off");
      }
      else if (cmd == "Tarm") {
        lcd.armTimeline();
        devlink.log("timeline armed: frozen at the next tear");
      } else if (cmd == "T") {  // the frozen timeline: time since previous event, pos, flags
        if (lcd.frozenHead_ < 0) {
          devlink.log("timeline: no tear since armed");
        } else {
          int h = lcd.frozenHead_;
          uint32_t prev = 0;
          for (int i = 0; i < RgbDisplay::kEv; i++) {
            const RgbDisplay::Ev &e = lcd.frozen_[(h + i) & (RgbDisplay::kEv - 1)];
            char b[96];
            snprintf(b, sizeof b, "T %+6d us  %s pos %6d %s%s %s", i ? (int)(e.us - prev) : 0, e.flags & 8 ? "VSYNC" : "fill ",
                     (int)e.pos, e.flags & 1 ? "busy " : "", e.flags & 4 ? "TEAR" : "", rt::kPhaseNames[e.phase & 7]);
            prev = e.us;
            devlink.log(b);
          }
        }
      }
      else if (cmd == "storage flash" || cmd == "storage auto") {  // takes effect at the next boot
        plat.prefs().putString("storage", cmd.substr(8).c_str());
        devlink.log(cmd == "storage flash" ? "storage: internal flash from the next boot"
                                           : "storage: SD card when present, else flash (next boot)");
      }
      else if (cmd == "probe" || cmd == "probe on" || cmd == "probe off") {
        probeOn = cmd == "probe" ? !probeOn : cmd == "probe on";
        static bool probeTaskUp = false;  // made on first use: its stack is internal RAM
        if (probeOn && !probeTaskUp) probeTaskUp = xTaskCreatePinnedToCore(probeTask, "probe", 3072, nullptr, 1, nullptr, 0) == pdPASS;
        devlink.log(probeOn ? "probe: streaming GPIO6 (Sensor AD header, pin 3)" : "probe: off");
      } else if (cmd == "cs0" || cmd == "cs1") {
        sd.holdCs(cmd == "cs0" ? 0 : 1);
        devlink.log(cmd == "cs0" ? "sd: card select held LOW" : "sd: card select held HIGH");
      } else if (cmd == "csauto" || cmd == "sdauto") {
        sd.autoMode();
        devlink.log("sd: card lines back to normal");
      } else if ((cmd.rfind("sck", 0) == 0 || cmd.rfind("mosi", 0) == 0 || cmd.rfind("miso", 0) == 0) &&
                 (cmd.back() == '0' || cmd.back() == '1' || cmd.back() == 'z')) {
        int gpio = cmd[1] == 'c' ? 12 : cmd[2] == 's' ? 11 : 13;  // sck / mosi / miso
        int level = cmd.back() == 'z' ? -1 : cmd.back() - '0';
        sd.drivePin(gpio, level);
        devlink.log(("sd: " + cmd + " -> " + sd.lineState()).c_str());
      }
      else if (cmd == "r") eng->reload();
      else if (cmd == "p") {
        lcd.preload = !lcd.preload;
        lcd.resetStats();
        devlink.log(lcd.preload ? "preload on" : "preload off");
      } else if (cmd == "d") {
        lcd.useDma = !lcd.useDma;
        lcd.resetStats();
        devlink.log(lcd.useDma ? "dma fill on" : "dma fill off");
      } else if (cmd == "P") {  // profile 3 s of full repaints
        rt::profEnabled = true;
        rt::profReport(true);
        paintHelper.busyCycles = paintHelper.waitCycles = 0;
        uint32_t t0 = millis();
        int n = 0;
        while (millis() - t0 < 3000) {
          RT_PROF(PF_FRAME);
          eng->invalidateAll();
          eng->update();
          n++;
        }
        rt::profEnabled = false;
        devlink.log(("profile over " + std::to_string(n) + " frames (ms/calls):").c_str());
        char hb[96];
        snprintf(hb, sizeof hb, "  helper busy %.1f ms, engine waited %.1f ms", paintHelper.busyCycles / 240000.0,
                 paintHelper.waitCycles / 240000.0);
        devlink.log(hb);
        for (auto &line : rt::split(rt::profReport(true), '\n'))
          if (!line.empty()) {
            devlink.log(("  " + line).c_str());
            delay(2);
          }
      } else if (cmd == "M") {  // raster micro benchmarks on an SRAM band
        auto *buf = (uint16_t *)heap_caps_malloc(800 * 10 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!buf) {
          devlink.log("micro: no SRAM");
          break;
        }
        std::string r = rt::microBench(buf, 800, 10);
        free(buf);
        for (auto &line : rt::split(r, '\n'))
          if (!line.empty()) {
            devlink.log(line.c_str());
            delay(2);
          }
      } else if (cmd[0] == 'R') {  // R<MB/s>: band DMA write rate cap (0 = none)
        int mb = atoi(cmd.c_str() + 1);
        dma.setMaxRate((uint32_t)mb * 1000000u);
        lcd.resetStats();
        devlink.log(("dma rate cap " + std::to_string(mb) + " MB/s").c_str());
      } else if (cmd == "m") {
        dualCore = !dualCore;
        devlink.log(dualCore ? "dual-core paint on" : "dual-core paint off");
      } else if (cmd == "z") {
        lcd.resetStats();
      } else if (cmd == "F" || cmd == "Q") {  // bus stress: one core reads flash (F) or PSRAM (Q) for 2 s
        const uint8_t *src = nullptr;
        size_t len = 0;
        esp_partition_mmap_handle_t h = 0;
        if (cmd == "F") {
          const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "sys");
          const void *ptr = nullptr;
          if (p && esp_partition_mmap(p, 0, 0x200000, ESP_PARTITION_MMAP_DATA, &ptr, &h) == ESP_OK) {
            src = (const uint8_t *)ptr;
            len = 0x200000;
          }
        } else {
          src = (const uint8_t *)lcd.front();
          len = (size_t)lcd.width() * lcd.height() * 2;
        }
        if (src) {
          static uint8_t tmp[2048];
          uint32_t fb0 = lcd.dmaFallbacks(), late0 = lcd.lateFills(), t0 = millis();
          size_t off = 0, bytes = 0;
          while (millis() - t0 < 2000) {
            memcpy(tmp, src + off, sizeof tmp);
            off = (off + 2048 * 37) % (len - 2048);  // strided: always missing the cache
            bytes += sizeof tmp;
          }
          char b[120];
          snprintf(b, sizeof b, "stress %s: %.1f MB/s read | dmafb +%u late +%u", cmd == "F" ? "flash" : "psram",
                   bytes / 2e6, (unsigned)(lcd.dmaFallbacks() - fb0), (unsigned)(lcd.lateFills() - late0));
          devlink.log(b);
        }
        if (h) esp_partition_munmap(h);
      } else if (cmd == "f") {  // full repaint stress: redraw everything for 3 s
        uint32_t t0 = millis();
        while (millis() - t0 < 3000) {
          eng->invalidateAll();
          eng->update();
        }
        devlink.log(statsText().c_str());
      }
      break;
    }
  }
}

// ============================================================ touch

// ---- input layer: the touch controller is sampled by its own task (core 0, above the paint
// helper) at a steady ~120 Hz, whatever the engine is doing; each event carries its sample
// time. The engine drains the queue at the start of every cycle, before app code runs, and
// handles each event as of its sample time (scroll velocity, gestures).

static void touchTask(void *) {
  bool down = false;
  int lastX = 0, lastY = 0;
  TickType_t wake = xTaskGetTickCount();
  while (true) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(8));
    GT911::Point pts[5];
    int n = touch.read(pts, 5);
    if (n < 0) continue;  // bus error / no new sample
    TouchEvent ev{0, 0, 0, esp_timer_get_time()};
    if (n > 0) {
      int x = pts[0].x, y = pts[0].y;
      if (down && x == lastX && y == lastY) continue;
      ev.phase = down ? PTR_MOVE : PTR_DOWN;
      ev.x = (int16_t)x;
      ev.y = (int16_t)y;
      down = true;
      lastX = x;
      lastY = y;
    } else if (down) {
      ev.phase = PTR_UP;
      ev.x = (int16_t)lastX;
      ev.y = (int16_t)lastY;
      down = false;
    } else {
      continue;
    }
    if (xQueueSend(touchQ, &ev, 0) != pdTRUE && ev.phase != PTR_MOVE) {
      // full (the engine is stuck): an up/down must not get lost, drop the oldest instead
      TouchEvent old;
      xQueueReceive(touchQ, &old, 0);
      xQueueSend(touchQ, &ev, 0);
    }
  }
}

static void drainTouch() {
  TouchEvent ev;
  while (touchQ && xQueueReceive(touchQ, &ev, 0) == pdTRUE) {
    if ((rt::gDebugFlags & rt::DBG_INPUT) && ev.phase != PTR_MOVE) {
      char b[64];
      snprintf(b, sizeof b, "touch: %s %d,%d (sampled %d ms ago)", ev.phase == PTR_DOWN ? "DOWN" : "UP", ev.x, ev.y,
               (int)((esp_timer_get_time() - ev.us) / 1000));
      devlink.log(b);
    }
    eng->pointerAt(0, (PointerPhase)ev.phase, (float)ev.x, (float)ev.y, (uint64_t)ev.us);
  }
}

// ============================================================ flash reads
// Program images are mapped from flash, and flash shares its bus with the PSRAM the panel
// scans out from: a stream of flash reads starves the bounce-buffer refills and the screen
// glitches (tools: "F" stress). The engine keeps RAM copies of what it draws from (fonts,
// images) and copies page sources through pacedCopy: short bursts with the bus left free
// in between.

static bool IRAM_ATTR flashMapped(const void *p) {
  if (esp_ptr_internal(p)) return false;
  esp_paddr_t pa;
  mmu_target_t target;
  return esp_mmu_vaddr_to_paddr((void *)p, &pa, &target) == ESP_OK && target == MMU_TARGET_FLASH0;
}

static void pacedCopy(void *dst, const void *src, size_t n) {
  if (!flashMapped(src)) {
    memcpy(dst, src, n);
    return;
  }
  auto *d = (uint8_t *)dst;
  auto *s = (const uint8_t *)src;
  while (n) {
    size_t k = std::min<size_t>(n, 4096);  // ~0.2 ms of flash reads: within a refill's slack
    memcpy(d, s, k);
    d += k;
    s += k;
    n -= k;
    if (n) esp_rom_delay_us(150);
  }
}

static size_t psramFree() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
static void *psramAlloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void slowPause() { esp_rom_delay_us(150); }

static void ioPause();  // after the platform (it knows where the storage is)

// ============================================================ engine task

static void engineTask(void *) {
  devlink.log("engine: start");
  rt::gIsSlowMemory = flashMapped;
  rt::gBulkCopy = pacedCopy;
  rt::gLargeFree = psramFree;
  rt::gIoPause = ioPause;
  rt::gSlowPause = slowPause;
  rt::gAllocBulk = psramAlloc;
  eng = new Engine(plat, disp);
  js = new JsHost(*eng);
  eng->setScriptHost(js);
  os = new Os(*eng, *js);
  os->boot(bootImageStart, bootImageEnd - bootImageStart);  // the boot screen brings the system up
  uint32_t lastStats = millis();
  while (true) {
    rt::gPhase = rt::PH_IO;
    devlink.poll();
    rt::gPhase = rt::PH_INPUT;
    drainTouch();
    bool drew = eng->update();
    if (millis() - lastStats > 10000 && eng->stats().frames) {
      lastStats = millis();
      devlink.log(statsText().c_str());
    }
    if (!drew) {
      // nothing presented: sleep until the next timer or animation step (at least a tick, so
      // a step that changed nothing does not spin the core) -- a touch wakes us at once
      uint32_t idle = std::max<uint32_t>(1, std::min<uint32_t>(eng->idleMs(), 16));
      TouchEvent ev;
      xQueuePeek(touchQ, &ev, pdMS_TO_TICKS(idle));
    }
  }
}

void setup() {
  devlink.begin(kBaud, onLink);
  delay(50);
  devlink.log("\niotdash player");
  Wire.begin(board::I2C_SDA, board::I2C_SCL, 400000);
  io.begin(Wire, board::EXIO_LCD_RST | board::EXIO_SD_CS);
  if (!touch.begin(Wire, io, board::EXIO_TP_RST, board::TOUCH_INT)) devlink.log("touch: GT911 not found");
  // before the panel: memory-to-memory GDMA (scan-out fill, band blitter) borrows the trigger
  // id of an idle peripheral, and SPI2 must have its own first
  if (!sd.begin(io)) devlink.log("sd: SPI bus init failed");
  if (!lcd.begin(board::LCD_PCLK_HZ)) {
    devlink.log("lcd init failed");
    return;
  }
  dma.begin(lcd.width());
  dma.setScanOut(&lcd);
  if (!paintHelper.begin()) devlink.log("paint helper: out of internal RAM");
  touchQ = xQueueCreate(64, sizeof(TouchEvent));
  xTaskCreatePinnedToCore(touchTask, "touch", 3072, nullptr, 6, &touchTaskH, 0);
  io.set(board::EXIO_BL, true);
  plat.begin();
  char msg[160];
  snprintf(msg, sizeof msg, "iotdash OS %s | boot image %u KB | sram %u KB psram %u KB", Os::kVersion,
           (unsigned)((bootImageEnd - bootImageStart) / 1024), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  devlink.log(msg);
  // big stack in internal RAM: QuickJS (up to 32 KB, js.cpp) + layout recursion
  if (xTaskCreatePinnedToCore(engineTask, "engine", 48 * 1024, nullptr, 3, &engineTaskH, 1) != pdPASS)
    devlink.log("engine task: out of internal RAM");
  vTaskDelete(nullptr);  // the Arduino loop task is not used: give its stack back
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
