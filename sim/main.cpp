// Desktop simulator for the iotdash runtime: same engine, same band/double-buffer pipeline as
// the panel. Window mode (SDL2, mouse = touch) or headless with a script and PNG screenshots.

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <zlib.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include "engine.h"
#include "prof.h"
#include "microbench.h"
#include "js.h"
#include "os.h"

#ifdef HAVE_SDL
#include <SDL.h>
#endif

#include "fake_radio.h"

#ifdef __APPLE__
#include <malloc/malloc.h>
#endif

using namespace rt;

static bool gVirtualTime = false;
static uint64_t gVirtualUs = 1000000;

namespace rt {
uint32_t cycles() {  // the host reports in "cycles" of a 240 MHz clock to share the device report
  using namespace std::chrono;
  return (uint32_t)(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() * 240 / 1000);
}
uint64_t nowUs() {
  if (gVirtualTime) return gVirtualUs;
  using namespace std::chrono;
  static auto t0 = steady_clock::now();
  return (uint64_t)duration_cast<microseconds>(steady_clock::now() - t0).count() + 1000000;
}
void logWrite(const char *s) { fprintf(stderr, "%s\n", s); }
}  // namespace rt

// ---------------------------------------------------------------- PNG

static void writePng(const std::string &path, const uint16_t *px, int w, int h) {
  std::vector<uint8_t> raw((size_t)(w * 3 + 1) * h);
  for (int y = 0; y < h; y++) {
    uint8_t *row = &raw[(size_t)y * (w * 3 + 1)];
    row[0] = 0;
    for (int x = 0; x < w; x++) {
      uint16_t c = px[y * w + x];
      uint8_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
      row[1 + x * 3] = (uint8_t)((r * 255 + 15) / 31);
      row[2 + x * 3] = (uint8_t)((g * 255 + 31) / 63);
      row[3 + x * 3] = (uint8_t)((b * 255 + 15) / 31);
    }
  }
  uLongf clen = compressBound(raw.size());
  std::vector<uint8_t> comp(clen);
  compress2(comp.data(), &clen, raw.data(), raw.size(), 6);
  comp.resize(clen);
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return;
  auto be32 = [](uint32_t v, uint8_t *o) {
    o[0] = v >> 24;
    o[1] = v >> 16;
    o[2] = v >> 8;
    o[3] = v;
  };
  auto chunk = [&](const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    be32(len, hdr);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, data, len);
    uint8_t c[4];
    be32((uint32_t)crc, c);
    fwrite(c, 1, 4, f);
  };
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  fwrite(sig, 1, 8, f);
  uint8_t ihdr[13];
  be32(w, ihdr);
  be32(h, ihdr + 4);
  ihdr[8] = 8;
  ihdr[9] = 2;
  ihdr[10] = ihdr[11] = ihdr[12] = 0;
  chunk("IHDR", ihdr, 13);
  chunk("IDAT", comp.data(), (uint32_t)comp.size());
  chunk("IEND", nullptr, 0);
  fclose(f);
}

// ---------------------------------------------------------------- platform

class SimPlatform : public Platform {
 public:
  uint64_t micros() override { return nowUs(); }
  void log(const char *s) override { fprintf(stderr, "%s\n", s); }
  void fetch(const FetchRequest &req, std::function<void(FetchResponse)> done) override {
    // run curl in a worker thread; deliver on the main thread in pollAsync()
    std::thread([this, req, done]() {
      FetchResponse r;
      r.url = req.url;
      std::string cmd = "curl -s -i -m " + std::to_string(req.timeoutMs / 1000 + 1) + " -X " + req.method;
      for (auto &h : req.headers) cmd += " -H '" + h.first + ": " + h.second + "'";
      std::string bodyFile;
      if (!req.body.empty()) {
        bodyFile = "/tmp/iotdash-sim-body-" + std::to_string((uintptr_t)this) + std::to_string(rand());
        std::ofstream(bodyFile) << req.body;
        cmd += " --data-binary @" + bodyFile;
      }
      cmd += " '" + req.url + "'";
      FILE *p = popen(cmd.c_str(), "r");
      std::string out;
      if (p) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
        pclose(p);
      }
      if (!bodyFile.empty()) remove(bodyFile.c_str());
      // parse "HTTP/1.1 200 OK\r\nheaders\r\n\r\nbody" (skip 100-continue blocks)
      size_t pos = 0;
      while (true) {
        size_t he = out.find("\r\n\r\n", pos);
        if (he == std::string::npos || out.compare(pos, 5, "HTTP/") != 0) break;
        std::string head = out.substr(pos, he - pos);
        pos = he + 4;
        int code = 0;
        sscanf(head.c_str(), "HTTP/%*s %d", &code);
        if (code == 100) continue;
        r.status = code;
        std::istringstream hs(head);
        std::string line;
        std::getline(hs, line);
        size_t sp = line.find(' ', line.find(' ') + 1);
        if (sp != std::string::npos) r.statusText = trim(line.substr(sp + 1));
        while (std::getline(hs, line)) {
          size_t c = line.find(':');
          if (c != std::string::npos) r.headers.push_back({toLower(trim(line.substr(0, c))), trim(line.substr(c + 1))});
        }
        break;
      }
      r.body = out.substr(std::min(pos, out.size()));
      if (!r.status) r.error = "network error";
      std::lock_guard<std::mutex> lk(mu_);
      done_.push_back([done, r]() { done(r); });
    }).detach();
  }
  void download(const std::string &url, DownloadSink sink, DownloadDone done) override {
    // curl streams the body to the sink; its headers go to a file (status, Content-Length)
    std::thread([this, url, sink, done]() {
      std::string headFile = "/tmp/iotdash-sim-head-" + std::to_string((uintptr_t)this) + std::to_string(rand());
      std::string cmd = "curl -s -L -m 600 -D '" + headFile + "' '" + url + "'";
      // the last response's status and length (redirects write several blocks)
      auto head = [&](uint64_t &len) {
        std::ifstream f(headFile);
        std::string line;
        int status = 0;
        while (std::getline(f, line)) {
          if (line.compare(0, 5, "HTTP/") == 0) sscanf(line.c_str(), "HTTP/%*s %d", &status), len = 0;
          else if (toLower(line).compare(0, 15, "content-length:") == 0) len = strtoull(line.c_str() + 15, nullptr, 10);
        }
        return status;
      };
      std::string err;
      int status = 0;
      FILE *p = popen(cmd.c_str(), "r");
      if (!p) err = "cannot run curl";
      else {
        char buf[16384];
        size_t n;
        uint64_t total = 0;
        bool first = true, stopped = false;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0) {
          if (first) status = head(total), first = false;
          if (status >= 200 && status < 300 && !sink((const uint8_t *)buf, n, total)) {
            stopped = true;
            break;
          }
        }
        pclose(p);
        uint64_t len = 0;
        if (!status) status = head(len);
        if (stopped) err = "stopped";
        else if (!status) err = "network error";
      }
      remove(headFile.c_str());
      std::lock_guard<std::mutex> lk(mu_);
      done_.push_back([done, err, status]() { done(err, status); });
    }).detach();
  }
  void pollAsync() override {
    std::vector<std::function<void()>> q;
    {
      std::lock_guard<std::mutex> lk(mu_);
      q.swap(done_);
    }
    for (auto &f : q) f();
    radio_.poll();
  }
  // ---- radios: fake WiFi networks and Bluetooth LE devices (fake_radio.h)
  FakeRadio radio_;
  SimPlatform() {
    radio_.nowUs = [] { return nowUs(); };
    radio_.event = [this](const std::string &n, const std::string &j) {
      if (radioEvent) radioEvent(n, j);
    };
  }
  void radio(const std::string &op, const std::string &args, RadioDone done) override { radio_.request(op, args, done); }
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
  bool pending() {
    std::lock_guard<std::mutex> lk(mu_);
    return !done_.empty();
  }

  // ---- OS: the SD card is a directory, the registry a JSON file next to it (flash)
  std::string card;
  bool mounted = false;
  bool sdMount(std::string &err) override {
    struct stat st;
    if (card.empty() || stat(card.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
      err = "no card (" + card + ")";
      return false;
    }
    mounted = true;
    return true;
  }
  std::string sdRoot() const override { return mounted ? card : ""; }
  SdInfo sdInfo() override {
    SdInfo i;
    struct statvfs v;
    if (mounted && statvfs(card.c_str(), &v) == 0) {
      i.mounted = true;
      i.totalBytes = (uint64_t)v.f_blocks * v.f_frsize;
      i.freeBytes = (uint64_t)v.f_bavail * v.f_frsize;
      i.type = "SIM";
      i.name = "sdcard";
    }
    return i;
  }
  bool sdFormat(std::string &err) override {
    if (!mounted) {
      err = "no card";
      return false;
    }
    if (DIR *d = opendir(card.c_str())) {
      std::vector<std::string> names;
      while (dirent *e = readdir(d))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
      closedir(d);
      for (auto &n : names) Os::removeTree(card + "/" + n);
    }
    return true;
  }
  std::string registryPath() const { return card + ".registry.json"; }
  std::string registryLoad() override {
    std::string s;
    return Os::readFile(registryPath(), s) ? s : "{}";
  }
  void registrySave(const std::string &json) override { Os::writeFile(registryPath(), json); }
  // images are read from the card in slices (like the device copying into a flash slot)
  struct FileImage : ImageLoad {
    FILE *f = nullptr;
    std::shared_ptr<std::vector<uint8_t>> keep = std::make_shared<std::vector<uint8_t>>();
    std::vector<uint8_t> &buf = *keep;
    size_t total = 0, got = 0;
    std::string err;
    ~FileImage() override {
      if (f) fclose(f);
    }
    float step() override {
      if (!f) return 1;
      size_t n = fread(buf.data() + got, 1, std::min<size_t>(256 * 1024, total - got), f);
      got += n;
      if (n == 0 && got < total) err = "read error";
      if (got >= total || !err.empty()) {
        fclose(f);
        f = nullptr;
        return 1;
      }
      return (float)got / total;
    }
    bool failed() const override { return !err.empty(); }
    std::string error() const override { return err; }
    std::shared_ptr<const ImageSource> source() const override {
      // IOTDASH_SLOWMEM: read on demand like the device's flash slots (not in place)
      static const bool slow = getenv("IOTDASH_SLOWMEM") != nullptr;
      return std::make_shared<RamSource>(buf.data(), total, keep, !slow);
    }
  };
  std::unique_ptr<ImageLoad> loadImage(const std::string &, const std::string &path, const std::string &) override {
    auto img = std::make_unique<FileImage>();
    img->f = fopen(path.c_str(), "rb");
    if (!img->f) {
      img->err = "cannot open " + path;
      return img;
    }
    fseek(img->f, 0, SEEK_END);
    img->total = (size_t)ftell(img->f);
    fseek(img->f, 0, SEEK_SET);
    img->buf.resize(img->total);
    return img;
  }

 private:
  std::mutex mu_;
  std::vector<std::function<void()>> done_;
  std::map<std::string, std::string> store_;
};

// ---------------------------------------------------------------- display

// Runs one job at a time on a second thread (the device's second core).
class Helper {
 public:
  Helper() {
    th_ = std::thread([this] {
      rt::profMuted = true;
      std::unique_lock<std::mutex> l(m_);
      for (;;) {
        cv_.wait(l, [this] { return job_ != nullptr || quit_; });
        if (quit_) return;
        l.unlock();
        (*job_)();
        l.lock();
        job_ = nullptr;
        done_.notify_all();
      }
    });
  }
  ~Helper() {
    {
      std::lock_guard<std::mutex> l(m_);
      quit_ = true;
    }
    cv_.notify_all();
    th_.join();
  }
  void run(const std::function<void()> &f) {
    std::lock_guard<std::mutex> l(m_);
    job_ = &f;
    cv_.notify_all();
  }
  void wait() {
    std::unique_lock<std::mutex> l(m_);
    done_.wait(l, [this] { return job_ == nullptr; });
  }

 private:
  std::thread th_;
  std::mutex m_;
  std::condition_variable cv_, done_;
  const std::function<void()> *job_ = nullptr;
  bool quit_ = false;
};

class SimDisplay : public PanelDisplay {
 public:
  static const int W = 800, H = 480;
  int split = getenv("IOTDASH_SPLIT") ? atoi(getenv("IOTDASH_SPLIT")) : 0;  // 1: two threads, 2: same split on one
  Helper helper;
  // page buffers in rotation like the device (IOTDASH_BUFFERS=3: triple buffering)
  int nbuf = getenv("IOTDASH_BUFFERS") ? std::max(2, std::min(3, atoi(getenv("IOTDASH_BUFFERS")))) : 2;
  std::vector<uint16_t> buf[3];
  int front = 0;  // newest presented
  uint32_t presents = 0;
  std::function<void()> onPresent;
  std::function<void(int kind, int ms, const RectI &area)> onTransition;
  // IOTDASH_DAMAGECHECK=1: every presented frame is compared with a from-scratch render of the
  // whole screen; a difference is a frame the panel would show wrong (stale or missing pixels)
  std::function<void(uint16_t *out)> paintAll;
  uint32_t checkFrames = 0, badFrames = 0;
  void damageCheck(const char *how) {
    static const bool on = getenv("IOTDASH_DAMAGECHECK") != nullptr;
    if (!on || !paintAll) return;
    std::vector<uint16_t> ref((size_t)W * H, 0xF81F);
    paintAll(ref.data());
    const uint16_t *f = buf[front].data();
    int n = 0, x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
        if (f[y * W + x] != ref[y * W + x]) {
          n++;
          x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
        }
    checkFrames++;
    if (!n) return;
    badFrames++;
    fprintf(stderr, "damagecheck: %s #%u (buf %d): %d px differ in %d,%d %dx%d\n", how, presents, front, n, x0, y0,
            x1 - x0 + 1, y1 - y0 + 1);
    if (badFrames <= 6) {
      char name[64];
      snprintf(name, sizeof name, "/tmp/damage_%u_shown.png", presents);
      writePngTo(name, f);
      snprintf(name, sizeof name, "/tmp/damage_%u_ref.png", presents);
      writePngTo(name, ref.data());
    }
  }
  std::function<void(const char *, const uint16_t *)> writePngHook;
  void writePngTo(const char *name, const uint16_t *px) {
    if (writePngHook) writePngHook(name, px);
  }
  SimDisplay() {
    for (int i = 0; i < nbuf; i++) buf[i].assign(W * H, 0);
  }
  int backIndex() const { return (front + 1) % nbuf; }
  int bufferCount() const override { return nbuf; }
  int width() const override { return W; }
  int height() const override { return H; }
  int alignX() const override { return 32; }
  void render(const RectI &r, const std::function<void(const Band &)> &draw) override {
    // same banding as the panel's DMA blitter: 16 KB of SRAM per band
    int rowsPerBand = std::max(1, std::min(64, 8192 / std::max(1, r.w)));
    if (const char *br = getenv("IOTDASH_BAND_ROWS")) rowsPerBand = std::max(1, atoi(br));  // culling checks
    std::vector<uint16_t> band((size_t)r.w * rowsPerBand);
    uint16_t *back = buf[backIndex()].data();
    static const bool dbg = getenv("RT_DAMAGE_DEBUG") != nullptr;
    if (dbg) fprintf(stderr, "  sim: draw %d,%d %dx%d into buf %d\n", r.x, r.y, r.w, r.h, backIndex());
    for (int y = r.y; y < r.bottom(); y += rowsPerBand) {
      int rows = std::min(rowsPerBand, r.bottom() - y);
      std::fill(band.begin(), band.end(), 0xF81F);  // magenta: unpainted pixels show up
      if (split && r.w >= 128) {
        // like the device: the right half of the band is painted on a second thread
        int w1 = r.w / 2;
        Band left{band.data(), r.w, r.x, y, w1, rows};
        Band right{band.data() + w1, r.w, r.x + w1, y, r.w - w1, rows};
        if (split == 2) {  // same split, one thread (clip-dependence check)
          draw(left);
          draw(right);
        } else {
          std::function<void()> job = [&] { draw(right); };
          helper.run(job);
          draw(left);
          helper.wait();
        }
      } else {
        draw(Band{band.data(), r.w, r.x, y, r.w, rows});
      }
      for (int k = 0; k < rows; k++) memcpy(back + (size_t)(y + k) * W + r.x, band.data() + (size_t)k * r.w, r.w * 2);
    }
  }
  void present() override {
    front = backIndex();
    static const bool dbg = getenv("RT_DAMAGE_DEBUG") != nullptr;
    if (dbg) fprintf(stderr, "  sim: present buf %d\n", front);
    presents++;
    damageCheck("present");
    if (onPresent) onPresent();
  }
  bool transition(int kind, int ms, const RectI &area) override {
    if (onTransition) onTransition(kind, ms, area);
    front = backIndex();
    presents++;
    damageCheck("transition");
    if (onPresent) onPresent();
    return true;
  }
  const uint16_t *frontBuf() const { return buf[front].data(); }
};

// ---------------------------------------------------------------- app

static std::vector<uint8_t> readFile(const std::string &p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static time_t mtimeOf(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 ? st.st_mtime : 0;
}

struct App {
  SimPlatform plat;
  SimDisplay disp;
  Engine *eng = nullptr;
  JsHost *js = nullptr;
  std::vector<uint8_t> bundle;
  std::string bundlePath;

  bool load(const std::string &path, const std::string &page) {
    bundlePath = path;
    bundle = readFile(path);
    if (bundle.empty()) {
      fprintf(stderr, "cannot read %s\n", path.c_str());
      return false;
    }
    if (!eng) {
      eng = new Engine(plat, disp);
      js = new JsHost(*eng);
      eng->setScriptHost(js);
      hookDamageCheck();
    }
    if (!eng->loadBundle(bundle.data(), bundle.size())) {
      fprintf(stderr, "bad bundle\n");
      return false;
    }
    std::string entry = page;
    if (entry.empty()) {
      std::string m = eng->bundle().text("manifest.json");
      Json j;
      if (Json::parse(m.data(), m.size(), j)) entry = j.strOr("entry", "index.html");
    }
    eng->open(entry.empty() ? "index.html" : entry);
    return true;
  }

  // OS mode: boot from a card directory with the firmware's boot image
  Os *os = nullptr;
  void hookDamageCheck() {
    disp.paintAll = [this](uint16_t *out) {
      const int W = SimDisplay::W, H = SimDisplay::H;
      eng->renderer().paint(Band{out, W, 0, 0, W, H}, RectI{0, 0, W, H});
    };
    disp.writePngHook = [](const char *name, const uint16_t *px) { writePng(name, px, SimDisplay::W, SimDisplay::H); };
  }

  bool bootOs(const std::string &cardDir, const std::string &bootPath) {
    bundlePath.clear();
    bundle = readFile(bootPath);
    if (bundle.empty()) {
      fprintf(stderr, "cannot read %s (run: tools/iotdash.py os build)\n", bootPath.c_str());
      return false;
    }
    plat.card = cardDir;
    eng = new Engine(plat, disp);
    js = new JsHost(*eng);
    eng->setScriptHost(js);
    hookDamageCheck();
    os = new Os(*eng, *js);
    os->boot(bundle.data(), bundle.size());
    return true;
  }

  // advance virtual time, running frames like the panel (39 Hz)
  void runFor(double ms) {
    double frame = 1000.0 / 39.0;
    double t = 0;
    while (t < ms) {
      eng->update();
      double step = std::min(frame, ms - t);
      gVirtualUs += (uint64_t)(step * 1000);
      t += step;
      // let real network threads finish
      if (plat.pending()) eng->update();
    }
    eng->update();
  }
};

static std::vector<std::string> splitCmds(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ';' || c == '\n') {
      if (!trim(cur).empty()) out.push_back(trim(cur));
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!trim(cur).empty()) out.push_back(trim(cur));
  return out;
}

static void runScript(App &app, const std::string &script, const std::string &defaultShot) {
  bool shot = false;
  app.runFor(400);
  for (auto &c : splitCmds(script)) {
    std::istringstream is(c);
    std::string cmd;
    is >> cmd;
    if (cmd == "wait") {
      double ms;
      is >> ms;
      app.runFor(ms);
    } else if (cmd == "sleep") {  // real time: lets another process change the card meanwhile
      double ms;
      is >> ms;
      std::this_thread::sleep_for(std::chrono::milliseconds((int)ms));
      app.runFor(30);
    } else if (cmd == "tap") {
      float x, y;
      is >> x >> y;
      app.eng->pointer(0, PTR_DOWN, x, y);
      app.runFor(60);
      app.eng->pointer(0, PTR_UP, x, y);
      app.runFor(60);
    } else if (cmd == "tapel" || cmd == "dragel") {
      // tap (or drag horizontally by dx) the centre of the first element matching a selector
      std::string rest;
      std::getline(is, rest);
      rest = trim(rest);
      float dx = 0;
      if (cmd == "dragel") {
        size_t sp = rest.rfind(' ');
        dx = (float)atof(rest.substr(sp + 1).c_str());
        rest = trim(rest.substr(0, sp));
      }
      // native lookup (also finds the runtime's internal overlays)
      float x = 0, y = 0;
      bool found = false;
      SelectorList sl;
      if (parseSelectorList(rest, sl) && app.eng->document() && app.eng->document()->html) {
        app.eng->flushLayout();
        app.eng->renderer().build(app.eng->document());
        app.eng->document()->html->forEachElement([&](Element *e) {
          if (found || !e->box || !e->cs || e->cs->display == D_NONE || !matchesAny(sl, e)) return;
          RectF b = app.eng->renderer().screenRect(e);
          x = b.x + b.w / 2;
          y = b.y + b.h / 2;
          found = true;
        });
      }
      if (!found) {
        fprintf(stderr, "%s: no element %s\n", cmd.c_str(), rest.c_str());
        continue;
      }
      app.eng->pointer(0, PTR_DOWN, x, y);
      if (cmd == "dragel") {
        for (int i = 1; i <= 12; i++) {
          app.runFor(16);
          app.eng->pointer(0, PTR_MOVE, x + dx * i / 12, y);
        }
      } else {
        app.runFor(60);
      }
      app.eng->pointer(0, PTR_UP, x + dx, y);
      app.runFor(60);
    } else if (cmd == "down" || cmd == "move" || cmd == "up") {
      float x, y;
      is >> x >> y;
      app.eng->pointer(0, cmd == "down" ? PTR_DOWN : cmd == "move" ? PTR_MOVE : PTR_UP, x, y);
      app.runFor(16);
    } else if (cmd == "drag") {
      float x1, y1, x2, y2, ms = 300;
      is >> x1 >> y1 >> x2 >> y2;
      if (!(is >> ms)) ms = 300;
      app.eng->pointer(0, PTR_DOWN, x1, y1);
      int steps = std::max(2, (int)(ms / 16));
      for (int i = 1; i <= steps; i++) {
        app.runFor(ms / steps);
        app.eng->pointer(0, PTR_MOVE, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
      }
      app.eng->pointer(0, PTR_UP, x2, y2);
      app.runFor(16);
    } else if (cmd == "type") {
      std::string rest;
      std::getline(is, rest);
      app.eng->typeText(trim(rest));
      app.runFor(30);
    } else if (cmd == "key") {
      std::string k;
      is >> k;
      app.eng->keyDown(k);
      app.runFor(30);
    } else if (cmd == "open") {
      std::string u;
      is >> u;
      app.eng->open(u);
      app.runFor(100);
    } else if (cmd == "eval") {
      std::string rest;
      std::getline(is, rest);
      std::string r = app.js->eval(trim(rest));
      fprintf(stderr, "eval> %s\n", r.c_str());
      app.runFor(30);
    } else if (cmd == "shot") {
      std::string f;
      is >> f;
      app.runFor(30);
      writePng(f, app.disp.frontBuf(), SimDisplay::W, SimDisplay::H);
      fprintf(stderr, "shot %s\n", f.c_str());
      shot = true;
    } else if (cmd == "bench") {
      // full-screen repaints (like the device's "f" command)
      int n = 30;
      is >> n;
      auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < n; i++) {
        app.eng->invalidateAll();
        app.eng->update();
      }
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      fprintf(stderr, "bench: %.2f ms per full repaint\n", ms / n);
    } else if (cmd == "prof") {
      // "prof" starts collecting, a second "prof" prints and resets
      if (!rt::profEnabled) {
        rt::profReport(true);
        rt::profEnabled = true;
      } else {
        fprintf(stderr, "%s", rt::profReport(true).c_str());
        rt::profEnabled = false;
      }
    } else if (cmd == "cliptest") {
      fprintf(stderr, "%s", rt::clipTest().c_str());
    } else if (cmd == "micro") {
      std::vector<uint16_t> buf(800 * 10);
      fprintf(stderr, "%s", rt::microBench(buf.data(), 800, 10).c_str());
    } else if (cmd == "relayout") {
      int n = 30;
      is >> n;
      auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < n; i++) {
        app.eng->markLayoutDirty();
        app.eng->flushLayout();
      }
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      fprintf(stderr, "relayout: %.3f ms per layout\n", ms / n);
    } else if (cmd == "launch" && app.os) {
      std::string id, err;
      is >> id;
      if (!app.os->launch(id, err)) fprintf(stderr, "launch: %s\n", err.c_str());
      app.runFor(400);
    } else if (cmd == "home" && app.os) {
      app.os->home();
      app.runFor(400);
    } else if (cmd == "back" && app.os) {
      app.os->back();
      app.runFor(400);
    } else if (cmd == "swipeup") {  // from the bottom edge: shows the system bar
      app.eng->pointer(0, PTR_DOWN, 400, 476);
      for (int y = 470; y >= 380; y -= 15) {
        app.runFor(16);
        app.eng->pointer(0, PTR_MOVE, 400, (float)y);
      }
      app.eng->pointer(0, PTR_UP, 400, 380);
      app.runFor(300);
    } else if (cmd == "scale") {  // scale <kg> | scale OL: the load on the fake BLE scale
      std::string v;
      is >> v;
      app.plat.radio_.scaleOver = v == "OL" || v == "ol";
      if (!app.plat.radio_.scaleOver) app.plat.radio_.scaleKg = atof(v.c_str());
      app.runFor(30);
    } else if (cmd == "mem") {  // heap bytes in use (what PSRAM holds on the device, roughly)
#ifdef __APPLE__
      malloc_statistics_t st;
      malloc_zone_statistics(nullptr, &st);
      std::string tag;
      std::getline(is, tag);
      fprintf(stderr, "mem%s: %zu KB in use (js %u KB, fonts %u KB, resident %u KB)\n", tag.c_str(), st.size_in_use / 1024,
              (unsigned)(app.js->memoryUsed() / 1024), (unsigned)(rt::fontsResidentBytes() / 1024), (unsigned)(rt::residentBytes() / 1024));
#endif
    } else if (cmd == "scaleoff") {  // the fake scale drops its link
      app.plat.radio_.dropScale();
      app.runFor(30);
    } else if (cmd == "regset" && app.os) {
      std::string key, val, err;
      is >> key;
      std::getline(is, val);
      bool sys = true;
      (void)sys;
      if (!app.os->regSet(key, trim(val), err)) fprintf(stderr, "regset: %s\n", err.c_str());
      app.runFor(200);
    } else if (cmd == "appschanged" && app.os) {  // what the board does on APPS_CHANGED
      std::string id;
      is >> id;
      app.os->appsChanged(id);
      app.runFor(400);
    } else if (cmd == "kioskoff" && app.os) {  // like `iotdash device kiosk-off` (a forgotten password)
      app.os->kioskReset();
      app.runFor(400);
    } else if (cmd == "reboot" && app.os) {
      app.os->reloadSystem();
      app.runFor(400);
    } else if (cmd == "ospage" && app.os) {
      fprintf(stderr, "ospage: app %s page %s depth %d\n", app.os->current().id.c_str(), app.os->page().c_str(),
              app.os->depth());
    } else if (cmd == "dbg") {  // dbg N: rt::gDebugFlags, then a full repaint
      is >> rt::gDebugFlags;
      app.eng->invalidateAll();
      app.runFor(30);
    } else if (cmd == "split") {  // split 0|1|2, then a full repaint
      is >> app.disp.split;
      app.eng->invalidateAll();
      app.runFor(30);
    } else if (cmd == "spincheck") {
      // without time passing, update() must settle (else the device would spin)
      int n = 0;
      while (n < 50 && app.eng->update()) n++;
      fprintf(stderr, "spincheck: %d frames without time passing\n", n);
    } else if (cmd == "layoutcheck") {
      fprintf(stderr, "layoutcheck: %u partial layouts verified, %u mismatched (partial total %u)\n", app.eng->checkCount_,
              app.eng->checkBad_, app.eng->stats().partialLayouts);
    } else if (cmd == "stats") {
      auto &s = app.eng->stats();
      fprintf(stderr, "stats: fps %.1f frames %u style %.2f ms layout %.2f ms paint %.2f ms frame %.2f ms damage %u px\n", s.fps, s.frames,
              s.lastStyleUs / 1000.0, s.lastLayoutUs / 1000.0, s.lastPaintUs / 1000.0, s.lastFrameUs / 1000.0,
              s.lastDamagePx);
    }
  }
  if (!shot && !defaultShot.empty()) {
    app.runFor(50);
    writePng(defaultShot, app.disp.frontBuf(), SimDisplay::W, SimDisplay::H);
    fprintf(stderr, "shot %s\n", defaultShot.c_str());
  }
}

#ifdef HAVE_SDL
static void runWindow(App &app) {
  SDL_Init(SDL_INIT_VIDEO);
  SDL_Window *win = SDL_CreateWindow("iotdash sim  800x480", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 480,
                                     SDL_WINDOW_ALLOW_HIGHDPI);
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  SDL_RenderSetLogicalSize(ren, 800, 480);
  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, 800, 480);
  bool dirty = true;
  app.disp.onPresent = [&]() { dirty = true; };
  // compositor slide emulation
  app.disp.onTransition = [&](int kind, int ms, const RectI &area) {
    const uint16_t *oldF = app.disp.buf[app.disp.front].data();
    const uint16_t *newF = app.disp.buf[app.disp.backIndex()].data();
    std::vector<uint16_t> tmp(800 * 480);
    auto t0 = SDL_GetTicks();
    while (true) {
      float t = std::min(1.f, (SDL_GetTicks() - t0) / (float)ms);
      float e = 1 - (1 - t) * (1 - t) * (1 - t);
      memcpy(tmp.data(), newF, tmp.size() * 2);
      int off = (int)(e * (kind == TR_SLIDE_UP || kind == TR_SLIDE_DOWN ? area.h : area.w));
      for (int y = area.y; y < area.bottom(); y++) {
        for (int x = area.x; x < area.right(); x++) {
          int sx = x, sy = y;
          bool fromOld = false;
          if (kind == TR_SLIDE_LEFT) { sx = x + off; fromOld = sx < area.right(); if (!fromOld) sx -= area.w; }
          else if (kind == TR_SLIDE_RIGHT) { sx = x - off; fromOld = sx >= area.x; if (!fromOld) sx += area.w; }
          else if (kind == TR_SLIDE_UP) { sy = y + off; fromOld = sy < area.bottom(); if (!fromOld) sy -= area.h; }
          else { sy = y - off; fromOld = sy >= area.y; if (!fromOld) sy += area.h; }
          tmp[y * 800 + x] = (fromOld ? oldF : newF)[sy * 800 + sx];
        }
      }
      SDL_UpdateTexture(tex, nullptr, tmp.data(), 800 * 2);
      SDL_RenderClear(ren);
      SDL_RenderCopy(ren, tex, nullptr, nullptr);
      SDL_RenderPresent(ren);
      if (t >= 1) break;
    }
  };
  SDL_StartTextInput();
  bool quit = false, mouseDown = false;
  time_t mt = mtimeOf(app.bundlePath);
  uint32_t lastCheck = 0;
  while (!quit) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      switch (ev.type) {
        case SDL_QUIT: quit = true; break;
        case SDL_MOUSEBUTTONDOWN:
          mouseDown = true;
          app.eng->pointer(0, PTR_DOWN, (float)ev.button.x, (float)ev.button.y);
          break;
        case SDL_MOUSEMOTION:
          if (mouseDown) app.eng->pointer(0, PTR_MOVE, (float)ev.motion.x, (float)ev.motion.y);
          break;
        case SDL_MOUSEBUTTONUP:
          mouseDown = false;
          app.eng->pointer(0, PTR_UP, (float)ev.button.x, (float)ev.button.y);
          break;
        case SDL_MOUSEWHEEL: {
          int mx, my;
          SDL_GetMouseState(&mx, &my);
          app.eng->pointer(1, PTR_DOWN, (float)mx, (float)my);
          app.eng->pointer(1, PTR_MOVE, (float)mx, (float)my + ev.wheel.y * 30.f);
          app.eng->pointer(1, PTR_MOVE, (float)mx, (float)my + ev.wheel.y * 60.f);
          app.eng->pointer(1, PTR_CANCEL, (float)mx, (float)my + ev.wheel.y * 60.f);
          break;
        }
        case SDL_TEXTINPUT: app.eng->typeText(ev.text.text); break;
        case SDL_KEYDOWN: {
          SDL_Keycode k = ev.key.keysym.sym;
          if (k == SDLK_BACKSPACE) app.eng->keyDown("Backspace");
          else if (k == SDLK_RETURN) app.eng->keyDown("Enter");
          else if (k == SDLK_LEFT) app.eng->keyDown("ArrowLeft");
          else if (k == SDLK_RIGHT) app.eng->keyDown("ArrowRight");
          else if (k == SDLK_ESCAPE) app.eng->keyDown("Escape");
          else if (k == SDLK_F5 || (k == SDLK_r && (ev.key.keysym.mod & KMOD_GUI))) app.eng->reload();
          else if (k == SDLK_F2) {
            writePng("sim-shot.png", app.disp.frontBuf(), 800, 480);
            fprintf(stderr, "saved sim-shot.png\n");
          }
          break;
        }
      }
    }
    // hot reload when the bundle changes on disk
    if (SDL_GetTicks() - lastCheck > 300) {
      lastCheck = SDL_GetTicks();
      time_t m = mtimeOf(app.bundlePath);
      if (m != mt) {
        mt = m;
        SDL_Delay(100);
        std::string url = app.eng->url() + (app.eng->hash().empty() ? "" : "#" + app.eng->hash());
        app.load(app.bundlePath, url);
        fprintf(stderr, "reloaded bundle\n");
      }
    }
    app.eng->update();
    if (dirty) {
      SDL_UpdateTexture(tex, nullptr, app.disp.frontBuf(), 800 * 2);
      SDL_RenderClear(ren);
      SDL_RenderCopy(ren, tex, nullptr, nullptr);
      SDL_RenderPresent(ren);
      dirty = false;
    } else {
      SDL_Delay(std::min<uint32_t>(app.eng->idleMs(), 10));
    }
  }
  SDL_Quit();
}
#endif

int main(int argc, char **argv) {
  // IOTDASH_SLOWMEM=1: program images are read on demand like the device's flash slots (see
  // FileImage::source): exercises that path under the sanitizers
  if (argc < 2) {
    fprintf(stderr, "usage: iotdash-sim bundle.bin [--shot out.png] [--script \"tap 10 10; wait 300; shot a.png\"] [--page p.html]\n"
                    "       iotdash-sim --os CARD_DIR --boot boot.idb [--script ...] [--shot ...]\n");
    return 1;
  }
  std::string bundle = argv[1], shot, script, page, card, boot;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--shot" && i + 1 < argc) shot = argv[++i];
    else if (a == "--script" && i + 1 < argc) script = argv[++i];
    else if (a == "--page" && i + 1 < argc) page = argv[++i];
    else if (a == "--os" && i + 1 < argc) card = argv[++i];
    else if (a == "--boot" && i + 1 < argc) boot = argv[++i];
  }
  App app;
  bool headless = !shot.empty() || !script.empty();
  gVirtualTime = headless;
  if (!card.empty()) {
    if (!app.bootOs(card, boot)) return 1;
  } else if (!app.load(bundle, page)) {
    return 1;
  }
  if (headless) {
    runScript(app, script, shot);
    return 0;
  }
#ifdef HAVE_SDL
  runWindow(app);
#else
  fprintf(stderr, "built without SDL2: use --shot\n");
#endif
  return 0;
}
