#pragma once
// The runtime: loads an app bundle, keeps the document styled/laid out, renders only what
// changed, runs animations, routes touch input and hosts the script engine.

#include <map>
#include <unordered_set>

#include "anim.h"
#include "bundle.h"
#include "html.h"
#include "render.h"

namespace rt {

// ---- platform interfaces ----
struct PanelDisplay {
  virtual int width() const = 0;
  virtual int height() const = 0;
  virtual int alignX() const { return 1; }  // required x/w alignment of render rects
  // Draw rect r of the back buffer: draw() is called with bands covering r.
  virtual void render(const RectI &r, const std::function<void(const Band &)> &draw) = 0;
  // Make the back buffer visible at the next frame boundary.
  virtual void present() = 0;
  // Optional compositor page transition between the current front buffer and a freshly
  // rendered page: returns false if unsupported (the engine falls back to animating).
  virtual bool transition(int kind, int durationMs, const RectI &area) {
    (void)kind; (void)durationMs; (void)area;
    return false;
  }
  virtual float refreshHz() const { return 39; }
  // Page buffers in rotation: the one drawn into holds the frame from bufferCount() - 1
  // presents ago, so that many frames of damage are repainted along with the new one.
  virtual int bufferCount() const { return 2; }
  virtual ~PanelDisplay() {}
};

struct FetchRequest {
  std::string url, method = "GET", body;
  std::vector<std::pair<std::string, std::string>> headers;
  int timeoutMs = 15000;
};
struct FetchResponse {
  int status = 0;  // 0 = network error
  std::string statusText, body, error, url;
  std::vector<std::pair<std::string, std::string>> headers;
};

enum PointerPhase : uint8_t { PTR_DOWN, PTR_MOVE, PTR_UP, PTR_CANCEL };

// A compiled image (the OS or an app) being made available for in-place use. On the device
// that may mean copying it from the SD card into a flash slot first, a slice per step().
struct ImageLoad {
  virtual ~ImageLoad() {}
  virtual float step() = 0;  // do some work; progress 0..1 (1 = ready or failed)
  virtual bool failed() const = 0;
  virtual std::string error() const = 0;
  // the image once step() has returned 1 without failing: in RAM or read on demand
  virtual std::shared_ptr<const ImageSource> source() const = 0;
};

struct SdInfo {
  bool mounted = false;
  uint64_t totalBytes = 0, freeBytes = 0;
  std::string type, name;  // e.g. "SDHC", card product name
};

struct Platform {
  virtual uint64_t micros() = 0;
  virtual void log(const char *s) = 0;
  // Asynchronous HTTP: done() is invoked later from Engine::update() (main thread).
  virtual void fetch(const FetchRequest &req, std::function<void(FetchResponse)> done) = 0;
  virtual bool storageGet(const std::string &key, std::string &out) = 0;
  virtual void storageSet(const std::string &key, const std::string &value) = 0;
  virtual void storageRemove(const std::string &key) = 0;
  virtual std::vector<std::string> storageKeys() = 0;
  virtual void pollAsync() {}  // deliver completed async work

  // ---- OS services (SD card, registry, program images). A plain one-bundle player leaves
  // these at their defaults.
  virtual bool sdMount(std::string &err) {
    err = "no SD card support";
    return false;
  }
  virtual std::string sdRoot() const { return ""; }  // host path of the card's root ("" = not mounted)
  virtual SdInfo sdInfo() { return {}; }
  // Erase the card and create an empty FAT file system (then mounted again).
  virtual bool sdFormat(std::string &err) {
    err = "not supported";
    return false;
  }
  // The registry: a small JSON object kept in internal flash (fast, survives card changes).
  virtual std::string registryLoad() { return "{}"; }
  virtual void registrySave(const std::string &json) { (void)json; }
  // Map image `path` (a host path on the card) for in-place use. `key` names its cache slot
  // ("system", "app:calculator"); an image whose `hash` is already there is not copied again.
  virtual std::unique_ptr<ImageLoad> loadImage(const std::string &key, const std::string &path, const std::string &hash) {
    (void)key;
    (void)path;
    (void)hash;
    return nullptr;
  }
  virtual void reboot() {}

  // Stream a GET of `url` into `sink`, called on a worker thread with each piece as it arrives
  // (total = Content-Length, 0 when unknown); returning false stops the transfer. Then done(err,
  // status) on the engine thread. For app packages: nothing is held in memory but the piece.
  using DownloadSink = std::function<bool(const uint8_t *data, size_t len, uint64_t total)>;
  using DownloadDone = std::function<void(const std::string &err, int status)>;
  virtual void download(const std::string &url, DownloadSink sink, DownloadDone done) {
    (void)url;
    (void)sink;
    done("downloads are not supported on this platform", 0);
  }

  // ---- radios: WiFi station and Bluetooth LE central. The OS (os.h) keeps the policy, the
  // saved networks and the JS API on top. A request names an operation ("wifi.scan",
  // "bt.connect", ...) with JSON arguments; done(err, json) comes back on the engine thread
  // (through pollAsync), err empty on success. The default: no radios.
  using RadioDone = std::function<void(const std::string &err, const std::string &json)>;
  virtual void radio(const std::string &op, const std::string &argsJson, RadioDone done) {
    (void)op;
    (void)argsJson;
    done("no radio on this platform", "");
  }
  // Unsolicited radio events ("wifi" state changes, "bt" notifications / disconnects / scan
  // results), JSON payloads, delivered on the engine thread. Set by the OS.
  std::function<void(const std::string &name, const std::string &json)> radioEvent;
  virtual ~Platform() {}
};

// Hooks for an OS layer on top of the engine (system bar, gestures, in-app navigation).
struct OsHooks {
  virtual void documentReady(Document *doc) = 0;  // parsed, before scripts run
  virtual bool pointer(int id, PointerPhase phase, float x, float y) = 0;  // true: taken by the system UI
  virtual bool linkActivated(const std::string &url) = 0;  // true: handled as an in-app navigation
  virtual void tick(double nowMs) = 0;
  virtual std::shared_ptr<Image> image(const std::string &url) { (void)url; return nullptr; }  // "sd:/..." URLs
  virtual ~OsHooks() {}
};

// ---- script engine hooks (implemented by the QuickJS binding) ----
// App code that runs longer than this without returning is interrupted (ScriptHost::armWatchdog).
constexpr uint32_t kScriptWatchdogMs = 3000;

struct ScriptHost {
  virtual void reset(Document *doc) = 0;  // new document: fresh global scope
  virtual void armWatchdog(uint32_t ms) { (void)ms; }  // scripts from now on: at most ms
  virtual void runScript(const std::string &src, const std::string &name, bool module) = 0;
  virtual bool dispatch(Element *target, const std::string &type, const std::string &detailJson, bool bubbles,
                        bool cancelable, float x = 0, float y = 0, int pointerId = 0) = 0;  // returns !defaultPrevented
  virtual void dispatchWindow(const std::string &type, const std::string &detailJson) = 0;
  virtual void tick(uint64_t nowMs) = 0;     // timers + promise jobs
  virtual void animationFrame(double ts) = 0;  // requestAnimationFrame callbacks
  virtual bool wantsFrame() const = 0;
  virtual uint64_t nextTimerMs() const = 0;  // UINT64_MAX = none
  virtual void nodeConnected(Node *n, bool connected) = 0;
  virtual void nodeDestroyed(Node *n) = 0;
  virtual std::string eval(const std::string &src) = 0;  // dev console
  virtual void attributeChanged(Element *e, Atom name) { (void)e; (void)name; }
  virtual void animationFinished(int id) { (void)id; }
  virtual ~ScriptHost() {}
};

enum TransitionKind : uint8_t { TR_NONE, TR_SLIDE_LEFT, TR_SLIDE_RIGHT, TR_SLIDE_UP, TR_SLIDE_DOWN, TR_FADE };

struct EngineStats {
  uint32_t frames = 0;
  uint32_t lastStyleUs = 0, lastLayoutUs = 0, lastPaintUs = 0, lastFrameUs = 0, lastScriptUs = 0;
  uint32_t lastDamagePx = 0;
  uint32_t partialLayouts = 0;  // layouts limited to a changed subtree
  uint32_t lastRestyled = 0;    // elements cascaded by the last style recalc
  bool lastLayoutFull = false;
  float fps = 0;
};

class Widgets;
class InputController;

class Engine : public DomObserver, public LayoutHost, public PaintHost {
 public:
  Engine(Platform &p, PanelDisplay &d);
  ~Engine();

  bool loadBundle(const uint8_t *data, size_t len);
  // OS: images side by side in the file namespace ("boot/", "system/", "app/"); fonts and the
  // Thai dictionary of every mounted image are available. data must stay valid until unmount.
  bool mount(const std::string &prefix, std::shared_ptr<const ImageSource> src);
  bool mount(const std::string &prefix, const uint8_t *data, size_t len);
  void unmount(const std::string &prefix);
  // Extra user-agent CSS (the OS system UI); kept across pages and theme changes.
  void setSystemCss(const std::string &css);
  void setDarkMode(bool dark);
  bool darkMode() const { return style_.env().dark; }
  void setOsHooks(OsHooks *h) { os_ = h; }
  OsHooks *osHooks() const { return os_; }
  Bundle &bundle() { return bundle_; }
  void setScriptHost(ScriptHost *s) { js_ = s; }
  ScriptHost *script() const { return js_; }

  // Load a page of the bundle (e.g. "index.html"), optionally with a transition.
  void open(const std::string &url, TransitionKind tr = TR_NONE);
  void reload();
  const std::string &url() const { return url_; }

  // One iteration of the main loop. Returns true if a frame was rendered.
  bool update();
  // Milliseconds until the engine needs to run again (0 = immediately).
  uint32_t idleMs() const;

  // input
  void pointer(int id, PointerPhase phase, float x, float y);
  // A pointer event sampled earlier (input task queue): handled as of its sample time.
  void pointerAt(int id, PointerPhase phase, float x, float y, uint64_t sampleUs) {
    eventUs_ = sampleUs;
    pointer(id, phase, x, y);
    eventUs_ = 0;
  }
  void typeText(const std::string &utf8);  // keyboard text into the focused control
  void keyDown(const std::string &key);    // "Backspace", "Enter", "ArrowLeft", ...

  // hot reload / partial updates
  void hotUpdate(const std::string &path, const uint8_t *data, size_t len);
  // Replace the contents of every element matching selector with new HTML.
  int replaceComponent(const std::string &selector, const std::string &html, bool outer);

  // DOM helpers for script bindings
  Document *document() const { return doc_; }
  StyleEngine &style() { return style_; }
  LayoutEngine &layout() { return layout_; }
  Renderer &renderer() { return renderer_; }
  FontManager &fonts() { return fonts_; }
  Animator &animator() { return anim_; }
  Platform &platform() { return plat_; }
  PanelDisplay &display() { return disp_; }
  Widgets &widgets() { return *widgets_; }
  InputController &input() { return *input_; }
  // the time of the input event being handled (sampled by the input layer), else now
  uint64_t nowMs() const { return (eventUs_ ? eventUs_ : plat_.micros()) / 1000; }

  // Flush style + layout so geometry queries are current (getBoundingClientRect etc.)
  void flushLayout();
  void requestFrame() { frameRequested_ = true; }
  void invalidateAll() { fullRepaint_ = true; }
  void invalidateElement(Element *e);  // repaint (no layout)
  // Layout must be redone; hint = the node whose content/style changed (nullptr: everything).
  void markLayoutDirty(Node *hint = nullptr);
  void markListDirty() { listDirty_ = true; }
  void scrolled(Element *e);
  void focus(Element *e);
  Element *focused() const { return focus_; }
  void setHash(const std::string &hash, bool push, TransitionKind tr);
  const std::string &hash() const { return hash_; }
  void navigateHistory(int delta);
  void openLater(const std::string &url, TransitionKind tr) {
    pendingOpen_ = url;
    pendingTr_ = tr;
    frameRequested_ = true;
  }
  // Default action of a click on target (links, form controls, labels, summary).
  void activate(Element *target, float x = 0, float y = 0);
  void setModal(Element *dialog, bool open);
  // Run fn on the main loop (next update).
  void post(std::function<void()> fn) {
    posted_.push_back(std::move(fn));
    frameRequested_ = true;
  }
  std::shared_ptr<Image> loadImage(const std::string &src);
  void refreshFormState(Element *e);
  EngineStats &stats() { return stats_; }
  std::string currentValue(Element *e);   // form control value
  void setValue(Element *e, const std::string &v, bool fromUser);
  bool checked(Element *e) const { return e->state & ST_CHECKED; }
  void setChecked(Element *e, bool on, bool fromUser);
  void log(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

  // DomObserver
  void nodeInserted(Node *n) override;
  void nodeRemoved(Node *n, Element *oldParent) override;
  void attrChanged(Element *e, Atom name, const std::string *oldValue) override;
  void textChanged(Text *t) override;
  void nodeDestroyed(Node *n) override;
  // LayoutHost
  bool intrinsicSize(Element *e, float &w, float &h) override;
  std::string controlText(Element *e) override;
  // PaintHost
  bool paintReplaced(Element *e, const PaintCtx &pc) override;
  std::shared_ptr<Image> bgImage(const std::string &url) override;

 private:
  friend class Widgets;
  friend class InputController;
  void loadFonts();
  void buildDocument(const std::string &html);
  void collectStyles();
  void runScripts();
  void initElement(Element *e);  // inline style, form state, images
  void forgetSubtree(Node *n);
  void styleChanged(Element *e, uint8_t diff);
  bool render();  // true if a frame was presented
  void applyHashTarget();
  void dumpLayout();
  void svgDirty(Element *e);

  Platform &plat_;
  PanelDisplay &disp_;
  Bundle bundle_;
  uint64_t eventUs_ = 0;
  std::vector<std::string> mounts_;  // prefixes, in mount order
  std::string systemCss_;
  OsHooks *os_ = nullptr;
  Document *doc_ = nullptr;
  StyleEngine style_;
  LayoutEngine layout_;
  Renderer renderer_;
  FontManager fonts_;
  Animator anim_;
  Widgets *widgets_ = nullptr;
  InputController *input_ = nullptr;
  ScriptHost *js_ = nullptr;
  std::string url_, hash_;
  std::vector<std::string> history_;
  int historyPos_ = -1;
  std::string pendingOpen_;
  TransitionKind pendingTr_ = TR_NONE;

  bool styleDirty_ = true, layoutDirty_ = true, listDirty_ = true, fullRepaint_ = true;
  bool building_ = false;  // parsing a new document: its sheets are collected once, at the end
  bool fullLayout_ = true;                 // layout everything (else only around layoutHints_)
  bool checkLayout_ = getenv("RT_CHECK_LAYOUT") != nullptr;
  void verifyPartialLayout();

 public:
  uint32_t checkCount_ = 0, checkBad_ = 0;  // RT_CHECK_LAYOUT results

 private:
  std::vector<Element *> layoutHints_;     // changed elements (referenced until the next layout)
  bool frameRequested_ = true;
  std::unordered_set<Element *> paintDirty_;
  std::vector<RectI> damage_, prevDamage_;  // prevDamage_: union of damageHistory_
  std::vector<std::vector<RectI>> damageHistory_;  // newest first, bufferCount() - 1 frames
  Element *focus_ = nullptr;
  std::unordered_map<std::string, std::shared_ptr<Image>> images_;
  EngineStats stats_;
  uint64_t lastFrameUs_ = 0;
  uint64_t fpsT0_ = 0;
  uint32_t fpsFrames_ = 0;
  TransitionKind runningTr_ = TR_NONE;
  std::vector<std::function<void()>> posted_;
  std::unordered_map<Element *, Element *> backdrops_;
};

}  // namespace rt
