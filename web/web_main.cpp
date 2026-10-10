// iotdash in a web browser (Emscripten): the board's OS, apps and engine, drawn into a canvas by
// web/iotdash-web.js. The card is an in-memory directory (/card) the page fills with the system
// image and app packages (.idbz); the radios are the desktop simulator's fake ones
// (sim/fake_radio.h), driven by the page: access points in reach, the internet up or down, a
// weighing scale whose reading the page moves.
//
// Built by web/build.sh. The page calls the iot_* functions below (ccall/cwrap); the engine calls
// the page back through Module.iot* hooks (EM_JS).

#include <emscripten.h>
#include <sys/stat.h>

#include <cstdio>
#include <deque>
#include <map>

#include "engine.h"
#include "js.h"
#include "os.h"
#include "../sim/fake_radio.h"

using namespace rt;

// ---------------------------------------------------------------- calls into the page

EM_JS(void, js_log, (const char *s), {
  const t = UTF8ToString(s);
  if (Module.iotLog) Module.iotLog(t); else console.log(t);
});
EM_JS(void, js_fetch, (int id, const char *method, const char *url, const char *headers, const uint8_t *body, int len,
                       int timeoutMs), {
  Module.iotFetch(id, UTF8ToString(method), UTF8ToString(url), JSON.parse(UTF8ToString(headers)),
                  len ? HEAPU8.slice(body, body + len) : null, timeoutMs);
});
EM_JS(void, js_download, (int id, const char *url), {
  Module.iotDownload(id, UTF8ToString(url));
});
EM_JS(void, js_present, (int kind, int ms, int ax, int ay, int aw, int ah), {
  if (Module.iotPresent) Module.iotPresent(kind, ms, ax, ay, aw, ah);
});
EM_JS(void, js_radio, (const char *name, const char *json), {
  if (Module.iotRadio) Module.iotRadio(UTF8ToString(name), UTF8ToString(json));
});
EM_JS(void, js_tare, (), {
  if (Module.iotTare) Module.iotTare();
});

namespace rt {
uint32_t cycles() { return (uint32_t)(emscripten_get_now() * 240000.0); }  // a 240 MHz clock, like the board
uint64_t nowUs() { return (uint64_t)(emscripten_get_now() * 1000.0) + 1000000; }
void logWrite(const char *s) { js_log(s); }
}  // namespace rt

// ---------------------------------------------------------------- platform

static const char *kCard = "/card";

class WebPlatform : public Platform {
 public:
  FakeRadio radio_;
  bool internet = true;  // what the OS's internet probe ("sim:probe") finds

  WebPlatform() {
    radio_.nowUs = [] { return nowUs(); };
    radio_.event = [this](const std::string &n, const std::string &j) {
      if (radioEvent) radioEvent(n, j);
      js_radio(n.c_str(), j.c_str());
    };
    radio_.onTare = [] { js_tare(); };
  }

  uint64_t micros() override { return nowUs(); }
  void log(const char *s) override { js_log(s); }

  // HTTP through the browser's fetch(); "sim:" URLs are the simulated network's own
  void fetch(const FetchRequest &req, std::function<void(FetchResponse)> done) override {
    if (req.url.rfind("sim:", 0) == 0) {
      FetchResponse r;
      r.url = req.url;
      if (internet) r.status = 204;
      else r.error = "no internet (simulated)";
      ready_.push_back([done, r]() { done(r); });
      return;
    }
    int id = nextFetch_++;
    pending_[id] = std::move(done);
    std::string h = "{";
    for (auto &kv : req.headers) h += (h.size() > 1 ? "," : "") + Json::quote(kv.first) + ":" + Json::quote(kv.second);
    h += "}";
    js_fetch(id, req.method.c_str(), req.url.c_str(), h.c_str(), (const uint8_t *)req.body.data(), (int)req.body.size(),
             req.timeoutMs);
  }
  void fetchDone(int id, FetchResponse r) {
    auto it = pending_.find(id);
    if (it == pending_.end()) return;
    auto done = std::move(it->second);
    pending_.erase(it);
    ready_.push_back([done, r]() { done(r); });
  }
  // a download streams through the page (iot_download_data / iot_download_done)
  void download(const std::string &url, DownloadSink sink, DownloadDone done) override {
    int id = nextFetch_++;
    downloads_[id] = {std::move(sink), std::move(done)};
    js_download(id, url.c_str());
  }
  bool downloadData(int id, const uint8_t *p, size_t n, uint64_t total) {
    auto it = downloads_.find(id);
    return it != downloads_.end() && it->second.first(p, n, total);
  }
  void downloadDone(int id, const std::string &err, int status) {
    auto it = downloads_.find(id);
    if (it == downloads_.end()) return;
    auto done = std::move(it->second.second);
    downloads_.erase(it);
    ready_.push_back([done, err, status]() { done(err, status); });
  }
  void pollAsync() override {
    std::deque<std::function<void()>> q;
    q.swap(ready_);
    for (auto &f : q) f();
    radio_.poll();
  }
  void radio(const std::string &op, const std::string &args, RadioDone done) override { radio_.request(op, args, done); }

  // localStorage of the pages (apps keep theirs in memory.json through the OS)
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

  // the card: an in-memory directory the page filled
  bool mounted = false;
  bool sdMount(std::string &err) override {
    struct stat st;
    if (stat(kCard, &st) != 0 || !S_ISDIR(st.st_mode)) {
      err = "no card";
      return false;
    }
    mounted = true;
    return true;
  }
  std::string sdRoot() const override { return mounted ? kCard : ""; }
  SdInfo sdInfo() override {
    SdInfo i;
    i.mounted = mounted;
    i.totalBytes = 8ull << 30;
    i.freeBytes = 7ull << 30;
    i.type = "WEB";
    i.name = "simulator";
    return i;
  }
  std::string registryLoad() override {
    std::string s;
    return Os::readFile(std::string(kCard) + ".registry.json", s) ? s : "{}";
  }
  void registrySave(const std::string &json) override { Os::writeFile(std::string(kCard) + ".registry.json", json); }

  // program images: read whole from the card (the browser has the memory)
  struct FileImage : ImageLoad {
    std::shared_ptr<std::vector<uint8_t>> keep = std::make_shared<std::vector<uint8_t>>();
    std::string err;
    float step() override { return 1; }
    bool failed() const override { return !err.empty(); }
    std::string error() const override { return err; }
    std::shared_ptr<const ImageSource> source() const override {
      return std::make_shared<RamSource>(keep->data(), keep->size(), keep, true);
    }
  };
  std::unique_ptr<ImageLoad> loadImage(const std::string &, const std::string &path, const std::string &) override {
    auto img = std::make_unique<FileImage>();
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
      img->err = "cannot open " + path;
      return img;
    }
    fseek(f, 0, SEEK_END);
    img->keep->resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(img->keep->data(), 1, img->keep->size(), f) != img->keep->size()) img->err = "read error";
    fclose(f);
    return img;
  }
  void reboot() override { js_log("reboot asked: reload the page"); }

 private:
  int nextFetch_ = 1;
  std::map<int, std::function<void(FetchResponse)>> pending_;
  std::map<int, std::pair<DownloadSink, DownloadDone>> downloads_;
  std::deque<std::function<void()>> ready_;
  std::map<std::string, std::string> store_;
};

// ---------------------------------------------------------------- display: two page buffers

class WebDisplay : public PanelDisplay {
 public:
  static const int W = 800, H = 480;
  std::vector<uint16_t> buf[2];
  int front = 0;
  uint32_t presents = 0;
  WebDisplay() {
    buf[0].assign(W * H, 0);
    buf[1].assign(W * H, 0);
  }
  int width() const override { return W; }
  int height() const override { return H; }
  int alignX() const override { return 32; }
  int bufferCount() const override { return 2; }
  void render(const RectI &r, const std::function<void(const Band &)> &draw) override {
    int rows = std::max(1, std::min(64, 8192 / std::max(1, r.w)));  // the board's band size
    std::vector<uint16_t> band((size_t)r.w * rows);
    uint16_t *back = buf[1 - front].data();
    for (int y = r.y; y < r.bottom(); y += rows) {
      int n = std::min(rows, r.bottom() - y);
      draw(Band{band.data(), r.w, r.x, y, r.w, n});
      for (int k = 0; k < n; k++) memcpy(back + (size_t)(y + k) * W + r.x, band.data() + (size_t)k * r.w, r.w * 2);
    }
  }
  void present() override {
    front = 1 - front;
    presents++;
    js_present(0, 0, 0, 0, W, H);
  }
  // the page animates the change from the frame before like the board (a push inside `area`)
  bool transition(int kind, int ms, const RectI &area) override {
    front = 1 - front;
    presents++;
    js_present(kind, ms, area.x, area.y, area.w, area.h);
    return true;
  }
};

// ---------------------------------------------------------------- the page's API

static WebPlatform *gPlat;
static WebDisplay *gDisp;
static Engine *gEng;
static JsHost *gJs;
static Os *gOs;
static std::vector<uint8_t> gBoot;
static std::string gOut;

extern "C" {

// Start the OS from the boot image at `bootPath` (in the in-memory file system) and the card in
// /card (system/os.idb, applications/...). 1 on success.
EMSCRIPTEN_KEEPALIVE int iot_boot(const char *bootPath) {
  if (gOs) return 1;
  FILE *f = fopen(bootPath, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  gBoot.resize((size_t)ftell(f));
  fseek(f, 0, SEEK_SET);
  size_t n = fread(gBoot.data(), 1, gBoot.size(), f);
  fclose(f);
  if (n != gBoot.size()) return 0;
  gPlat = new WebPlatform();
  gDisp = new WebDisplay();
  gEng = new Engine(*gPlat, *gDisp);
  gJs = new JsHost(*gEng);
  gEng->setScriptHost(gJs);
  gOs = new Os(*gEng, *gJs);
  gOs->boot(gBoot.data(), gBoot.size());
  return 1;
}

// One step of the engine (the page calls it every animation frame). Returns the milliseconds
// until it wants to run again.
EMSCRIPTEN_KEEPALIVE int iot_update() {
  if (!gEng) return 100;
  gEng->update();
  return (int)gEng->idleMs();
}

EMSCRIPTEN_KEEPALIVE const uint16_t *iot_frame() { return gDisp ? gDisp->buf[gDisp->front].data() : nullptr; }
EMSCRIPTEN_KEEPALIVE uint32_t iot_presents() { return gDisp ? gDisp->presents : 0; }

// input: phase 0 down, 1 move, 2 up, 3 cancel; display pixels
EMSCRIPTEN_KEEPALIVE void iot_pointer(int phase, float x, float y) {
  if (gEng) gEng->pointer(0, (PointerPhase)phase, x, y);
}
EMSCRIPTEN_KEEPALIVE void iot_text(const char *utf8) {
  if (gEng) gEng->typeText(utf8);
}
EMSCRIPTEN_KEEPALIVE void iot_key(const char *key) {
  if (gEng) gEng->keyDown(key);
}

// apps
EMSCRIPTEN_KEEPALIVE const char *iot_launch(const char *id) {
  gOut.clear();
  if (gOs && !gOs->launch(id, gOut) && gOut.empty()) gOut = "cannot open";
  return gOut.c_str();
}
EMSCRIPTEN_KEEPALIVE void iot_home() {
  if (gOs) gOs->home();
}
EMSCRIPTEN_KEEPALIVE void iot_apps_changed(const char *id) {
  if (gOs) gOs->appsChanged(id);
}

// a fetch the page finished: status 0 = failed (error says why)
EMSCRIPTEN_KEEPALIVE void iot_fetch_done(int id, int status, const char *statusText, const char *headersJson,
                                         const uint8_t *body, int len, const char *url, const char *error) {
  if (!gPlat) return;
  FetchResponse r;
  r.status = status;
  r.statusText = statusText ? statusText : "";
  r.url = url ? url : "";
  r.error = error ? error : "";
  if (len > 0) r.body.assign((const char *)body, (size_t)len);
  Json h;
  if (headersJson && Json::parse(headersJson, strlen(headersJson), h) && h.type == Json::Obj)
    for (auto &kv : h.obj)
      if (kv.second.type == Json::Str) r.headers.push_back({kv.first, kv.second.str});
  gPlat->fetchDone(id, std::move(r));
}

// a download's next piece (0 = stop it) and its end
EMSCRIPTEN_KEEPALIVE int iot_download_data(int id, const uint8_t *p, int len, double total) {
  return gPlat && gPlat->downloadData(id, p, (size_t)len, (uint64_t)total) ? 1 : 0;
}
EMSCRIPTEN_KEEPALIVE void iot_download_done(int id, const char *error, int status) {
  if (gPlat) gPlat->downloadDone(id, error ? error : "", status);
}

// the simulated world
EMSCRIPTEN_KEEPALIVE void iot_internet(int on) {
  if (gPlat) gPlat->internet = on != 0;
}
EMSCRIPTEN_KEEPALIVE void iot_wifi_reach(int on) {
  if (gPlat) gPlat->radio_.setWifiReach(on != 0);
}
// the scale's display as the page moves it; frames go out every intervalMs while an app listens
EMSCRIPTEN_KEEPALIVE void iot_scale(double kg, int stable, int overload, int intervalMs) {
  if (!gPlat) return;
  FakeRadio &r = gPlat->radio_;
  r.scaleExternal = true;
  r.scaleShownKg = kg;
  r.scaleKg = kg;
  r.scaleStable = stable != 0;
  r.scaleOver = overload != 0;
  if (intervalMs > 0) r.scaleIntervalMs = intervalMs;
}
EMSCRIPTEN_KEEPALIVE void iot_scale_power(int on) {
  if (gPlat) gPlat->radio_.setScalePresent(on != 0);
}

// run JavaScript in the current page (tests, the page's tools); the result as a string
EMSCRIPTEN_KEEPALIVE const char *iot_eval(const char *js) {
  gOut = gJs ? gJs->eval(js) : "";
  return gOut.c_str();
}

}  // extern "C"
