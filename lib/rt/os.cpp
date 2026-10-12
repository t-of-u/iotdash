#include "os.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

#include "image.h"
#include "js.h"
#include "render.h"

namespace rt {

static constexpr float kEdge = 18;          // bottom strip that starts the system-bar swipe
static constexpr double kNavShowMs = 3500;  // the bar hides again after this

// ============================================================ file helpers

bool Os::readFile(const std::string &path, std::string &out) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
    out.append(buf, n);
    if (n == sizeof buf) gIoPause();
  }
  fclose(f);
  return true;
}

bool Os::writeFile(const std::string &path, const std::string &data) {
  size_t slash = path.rfind('/');
  if (slash != std::string::npos && slash > 0) makeDirs(path.substr(0, slash));
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  return fclose(f) == 0 && ok;
}

bool Os::makeDirs(const std::string &path) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
  size_t slash = path.rfind('/');
  if (slash != std::string::npos && slash > 0) makeDirs(path.substr(0, slash));
  return mkdir(path.c_str(), 0775) == 0;
}

bool Os::removeTree(const std::string &path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return true;
  if (!S_ISDIR(st.st_mode)) return unlink(path.c_str()) == 0;
  if (DIR *d = opendir(path.c_str())) {
    std::vector<std::string> names;
    while (dirent *e = readdir(d)) {
      std::string n = e->d_name;
      if (n != "." && n != "..") names.push_back(n);
    }
    closedir(d);
    for (auto &n : names) removeTree(path + "/" + n);
  }
  return rmdir(path.c_str()) == 0;
}

static std::string jsonStr(const std::string &s) { return Json::quote(s); }

// ============================================================ lifecycle

Os::Os(Engine &e, JsHost &js) : eng_(e), js_(js), plat_(e.platform()) {
  eng_.setOsHooks(this);
  js_.setOs(this);
  plat_.radioEvent = [this](const std::string &name, const std::string &json) { onRadio(name, json); };
}

Os::~Os() {
  storeStop();
  plat_.radioEvent = nullptr;
  flush();
  eng_.setOsHooks(nullptr);
  js_.setOs(nullptr);
}

void Os::boot(const uint8_t *bootImage, size_t len) {
  bootImage_ = bootImage;
  bootLen_ = len;
  state_ = BOOT_CARD;
  nextTry_ = 0;
  app_ = OsApp{};
  app_.id = "boot";
  app_.root = "boot/";
  app_.system = true;
  stack_.clear();
  if (!eng_.mount("boot/", bootImage, len)) {
    plat_.log("os: bad boot image");
    return;
  }
  eng_.open("boot/boot.html");
}

void Os::reloadSystem() {
  flush();
  js_.endSession();
  appImage_.reset();
  eng_.unmount("app/");
  eng_.unmount("system/");
  sysImage_.reset();
  if (bootImage_) boot(bootImage_, bootLen_);
}

void Os::bootStatus(const std::string &text, float progress, const std::string &error, bool canFormat) {
  char buf[64];
  snprintf(buf, sizeof buf, "%.3f", progress);
  std::string json = "{\"text\":" + jsonStr(text) + ",\"progress\":" + buf + ",\"error\":" + jsonStr(error) +
                     ",\"canFormat\":" + (canFormat ? "true" : "false") + ",\"version\":" + jsonStr(kVersion) + "}";
  js_.eval("typeof bootStatus === 'function' && bootStatus(" + json + ")");
}

void Os::bootStep(double now) {
  if (state_ == BOOT_CARD) {
    if (now < nextTry_) return;
    std::string err;
    if (!plat_.sdMount(err)) {
      bool unformatted = err.find("format") != std::string::npos;
      bootStatus(unformatted ? "The SD card has no file system" : "Insert an SD card", 0, err, unformatted);
      nextTry_ = now + 2000;
      return;
    }
    std::string root = plat_.sdRoot(), meta;
    if (!readFile(root + "/system/os.json", meta)) {
      bootStatus("The system is not installed on this card", 0, "Install it from a computer: tools/iotdash.py os deploy",
                 false);
      nextTry_ = now + 3000;
      return;
    }
    Json j;
    Json::parse(meta.data(), meta.size(), j);
    sysHash_ = j.strOr("hash", "");
    sysImage_ = plat_.loadImage("system", root + "/system/os.idb", sysHash_);
    if (!sysImage_) {
      bootStatus("Cannot load the system", 0, "no image loader", false);
      nextTry_ = now + 3000;
      return;
    }
    state_ = BOOT_IMAGE;
    bootStatus("Loading the system", 0.1f);
    return;
  }
  if (state_ == BOOT_IMAGE) {
    float p = sysImage_->step();
    if (sysImage_->failed()) {
      bootStatus("Cannot load the system", 0, sysImage_->error());
      sysImage_.reset();
      state_ = BOOT_CARD;
      nextTry_ = now + 3000;
      return;
    }
    bootStatus("Loading the system", 0.1f + 0.85f * p);
    if (p >= 1) startSystem();
  }
}

void Os::startSystem() {
  if (!eng_.mount("system/", sysImage_->source())) {
    bootStatus("The system image is damaged", 0, "/system/os.idb");
    sysImage_.reset();
    state_ = BOOT_CARD;
    nextTry_ = now_ + 5000;
    return;
  }
  eng_.setSystemCss(eng_.bundle().text("system/shell/shell.css"));
  // registry
  reg_.clear();
  std::string r = plat_.registryLoad();
  Json j;
  if (Json::parse(r.data(), r.size(), j) && j.type == Json::Obj)
    for (auto &kv : j.obj) reg_[kv.first] = kv.second.dump();
  applyRegistry();
  netStart();
  std::string root = plat_.sdRoot();
  makeDirs(root + "/system/data");
  makeDirs(root + "/applications");
  state_ = RUNNING;
  plat_.log(("os: system " + sysHash_ + " up").c_str());
  enter(homeApp(), TR_FADE);
  if (!kioskApp_.empty()) kioskResume();  // kiosk mode: the app is the way in
}

// ============================================================ apps

OsApp Os::homeApp() const {
  OsApp h;
  h.id = "home";
  h.title = "Home";
  h.dir = "/system";
  h.root = "system/home/";
  h.system = true;
  return h;
}

bool Os::readApp(const std::string &dir, OsApp &out) {
  std::string meta;
  std::string host = hostPath(dir);
  if (!readFile(host + "/app.json", meta)) return false;
  Json j;
  if (!Json::parse(meta.data(), meta.size(), j)) return false;
  size_t slash = dir.rfind('/');
  out.id = dir.substr(slash + 1);
  out.title = j.strOr("title", out.id.c_str());
  out.version = j.strOr("version", "");
  out.hash = j.strOr("hash", "");
  out.dir = dir;
  out.image = dir + "/app.idb";
  out.root = "app/";
  struct stat st;
  out.icon = stat((host + "/icon.img").c_str(), &st) == 0 ? "sd:" + dir + "/icon.img" : "/system/assets/default-icon.img";
  out.system = false;
  std::string inst;
  Json ij;
  if (readFile(host + "/install.json", inst) && Json::parse(inst.data(), inst.size(), ij)) {
    out.source = ij.strOr("source", "");
    out.storeVersion = ij.strOr("version", "");
    out.storeVersionId = ij.strOr("version_id", "");
    out.installed = ij.numOr("installed", 0);
  }
  return true;
}

std::vector<OsApp> Os::apps(bool withSystem) {
  std::vector<OsApp> out;
  if (withSystem) {
    // core apps inside the system image: system/apps/<id>/app.json
    for (auto &f : eng_.bundle().list()) {
      if (!startsWith(f, "system/apps/") || f.size() < 9 || f.compare(f.size() - 9, 9, "/app.json") != 0) continue;
      std::string id = f.substr(12, f.size() - 12 - 9);
      if (id.find('/') != std::string::npos) continue;
      Json j;
      std::string t = eng_.bundle().text(f);
      Json::parse(t.data(), t.size(), j);
      OsApp a;
      a.id = id;
      a.title = j.strOr("title", id.c_str());
      a.version = j.strOr("version", kVersion);
      a.dir = "/system";
      a.root = "system/apps/" + id + "/";
      a.icon = eng_.bundle().has(a.root + "icon.img") ? "/" + a.root + "icon.img" : "/system/assets/default-icon.img";
      a.system = true;
      out.push_back(a);
    }
  }
  std::string root = plat_.sdRoot();
  if (root.empty()) return out;
  std::vector<std::string> ids;
  if (DIR *d = opendir((root + "/applications").c_str())) {
    while (dirent *e = readdir(d))
      if (e->d_name[0] != '.') ids.push_back(e->d_name);
    closedir(d);
  }
  std::sort(ids.begin(), ids.end());
  for (auto &id : ids) {
    OsApp a;
    if (readApp("/applications/" + id, a)) out.push_back(a);
  }
  return out;
}

bool Os::launch(const std::string &id, std::string &err) {
  if (state_ != RUNNING) {
    err = "the system is starting";
    return false;
  }
  if (loading_) {
    err = "another app is opening";
    return false;
  }
  for (auto &a : apps(true)) {
    if (a.id != id) continue;
    if (a.system) {
      OsApp app = a;
      deferred_.push_back([this, app]() {
        appImage_.reset();
        eng_.unmount("app/");
        enter(app, TR_SLIDE_UP);
      });
      return true;
    }
    loading_ = plat_.loadImage("app:" + id, hostPath(a.image), a.hash);
    if (!loading_) {
      err = "cannot read " + a.image;
      return false;
    }
    loadingApp_ = a;
    launching_ = true;
    if (launchLabel_) {
      launchLabel_->removeAllChildren();
      launchLabel_->appendChild(new Text(a.title));
    }
    setShellClass();
    return true;
  }
  err = "no app " + id;
  return false;
}

void Os::home() {
  deferred_.push_back([this]() {
    appImage_.reset();
    enter(homeApp(), TR_SLIDE_DOWN);
    eng_.unmount("app/");
  });
}

void Os::requestHome() {
  if (!inApp()) return;
  if (!kioskApp_.empty()) return kioskUnlock();  // kiosk mode: the password, not a question
  confirm(app_.title, "Close the app and go to the home screen?", "Close", "Cancel", [this](bool ok) {
    if (ok) home();
  });
}

void Os::back() {
  if (navigateBack()) return;
  requestHome();
}

bool Os::uninstall(const std::string &id, std::string &err) {
  if (!privileged()) {
    err = "only the system can remove apps";
    return false;
  }
  if (id.empty() || id.find('/') != std::string::npos || id[0] == '.') {
    err = "bad app id";
    return false;
  }
  if (app_.id == id) {
    err = "the app is running";
    return false;
  }
  std::string path = hostPath("/applications/" + id);
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    err = "no app " + id;
    return false;
  }
  if (!removeTree(path)) {
    err = "cannot remove " + path;
    return false;
  }
  return true;
}

void Os::appsChanged(const std::string &id) {
  if (state_ != RUNNING) return;
  if (!id.empty() && inApp() && app_.id == id) {
    home();  // replaced or removed under it
    if (id == kioskApp_) deferred_.push_back([this]() { kioskResume(); });  // kiosk: back into the new one
    return;
  }
  js_.busEmit("apps", "{\"id\":" + jsonStr(id) + "}");
}

// ============================================================ pages

void Os::enter(const OsApp &app, TransitionKind tr) {
  if (modalOpen_) {  // a question of the app being left stays unanswered (cancel)
    modalOpen_ = false;
    if (modalPass_) passClose();
    if (modalDone_) {
      auto done = std::move(modalDone_);
      modalDone_ = nullptr;
      done(false);  // before its scripts end: a JS confirm() settles while its runtime lives
    }
  }
  flush();
  js_.endSession();  // the previous app's scripts and background processes end here
  app_ = app;
  stack_.clear();
  memPath_ = plat_.sdRoot().empty() ? "" : hostPath(app.dir + "/memory.json");
  loadMemory();
  if (!plat_.sdRoot().empty()) makeDirs(hostPath(app.dir + "/data"));
  navOpen_ = false;
  openPage(app.root + "main.html", tr);
}

void Os::openPage(const std::string &url, TransitionKind tr) {
  stack_.push_back(url);
  eng_.openLater("/" + url, tr);  // absolute: not relative to the page being left
}

bool Os::navigate(const std::string &pageIn, std::string &err) {
  if (state_ != RUNNING) {
    err = "the system is starting";
    return false;
  }
  std::string page = pageIn;
  while (!page.empty() && page[0] == '/') page = page.substr(1);
  std::string base = app_.root + "pages/";
  std::string url = Bundle::resolve(base, page);
  if (!startsWith(url, base.c_str())) {
    err = "pages are under pages/: " + pageIn;
    return false;
  }
  if (!eng_.bundle().has(url)) {
    err = "no page " + pageIn;
    return false;
  }
  openPage(url, TR_SLIDE_LEFT);
  return true;
}

bool Os::navigateBack() {
  if (stack_.size() <= 1) return false;
  stack_.pop_back();
  eng_.openLater("/" + stack_.back(), TR_SLIDE_RIGHT);
  return true;
}

std::string Os::page() const {
  if (stack_.empty()) return "";
  const std::string &u = stack_.back();
  return startsWith(u, app_.root.c_str()) ? u.substr(app_.root.size()) : u;
}

bool Os::linkActivated(const std::string &url) {
  if (state_ != RUNNING) return false;
  // links between pages of the running app go through the navigation stack
  std::string u = url;
  size_t q = u.find_first_of("?#");
  if (q != std::string::npos) u = u.substr(0, q);
  if (!startsWith(u, app_.root.c_str()) || !eng_.bundle().has(u)) return false;
  openPage(u, TR_SLIDE_LEFT);
  return true;
}

// ============================================================ memory.json

void Os::loadMemory() {
  mem_.clear();
  memDirty_ = false;
  std::string text;
  if (memPath_.empty() || !readFile(memPath_, text)) return;
  Json j;
  if (!Json::parse(text.data(), text.size(), j) || j.type != Json::Obj) return;
  for (auto &kv : j.obj) mem_[kv.first] = kv.second.type == Json::Str ? kv.second.str : kv.second.dump();
}

bool Os::memGet(const std::string &k, std::string &out) const {
  auto it = mem_.find(k);
  if (it == mem_.end()) return false;
  out = it->second;
  return true;
}

void Os::memSet(const std::string &k, const std::string &v) {
  auto it = mem_.find(k);
  if (it != mem_.end() && it->second == v) return;
  mem_[k] = v;
  memDirty_ = true;
  memFlushAt_ = now_ + 500;  // coalesce bursts of writes
}

void Os::memRemove(const std::string &k) {
  if (mem_.erase(k)) {
    memDirty_ = true;
    memFlushAt_ = now_ + 500;
  }
}

std::vector<std::string> Os::memKeys() const {
  std::vector<std::string> v;
  for (auto &kv : mem_) v.push_back(kv.first);
  return v;
}

void Os::memClear() {
  if (mem_.empty()) return;
  mem_.clear();
  memDirty_ = true;
  memFlushAt_ = now_ + 500;
}

void Os::flush() {
  if (!memDirty_ || memPath_.empty()) return;
  std::string o = "{";
  for (auto &kv : mem_) o += (o.size() > 1 ? "," : "") + jsonStr(kv.first) + ":" + jsonStr(kv.second);
  o += "}";
  if (!writeFile(memPath_, o)) plat_.log(("os: cannot write " + memPath_).c_str());
  memDirty_ = false;
}

// ============================================================ files

std::string Os::hostPath(const std::string &cardPath) const {
  std::string root = plat_.sdRoot();
  if (root.empty()) return "";
  return root + (cardPath.empty() || cardPath[0] != '/' ? "/" : "") + cardPath;
}

std::string Os::dataPath(const std::string &rel, std::string &err) const {
  if (plat_.sdRoot().empty()) {
    err = "no SD card";
    return "";
  }
  // normalise inside data/: "..", empty and "." segments never leave it
  std::vector<std::string> parts;
  for (auto &s : split(rel, '/')) {
    if (s.empty() || s == ".") continue;
    if (s == "..") {
      if (parts.empty()) {
        err = "path outside the app: " + rel;
        return "";
      }
      parts.pop_back();
      continue;
    }
    parts.push_back(s);
  }
  std::string p = hostPath(app_.dir + "/data");
  for (auto &s : parts) p += "/" + s;
  return p;
}

std::shared_ptr<Image> Os::image(const std::string &url) {
  // "sd:/applications/x/icon.img": read-only access to images on the card (icons)
  std::string path = url.substr(3);
  if (path.find("..") != std::string::npos) return nullptr;
  auto data = std::make_shared<std::string>();
  if (!readFile(hostPath(path), *data)) return nullptr;
  auto img = Image::fromImg1((const uint8_t *)data->data(), data->size());
  if (img) img->keep = data;
  return img;
}

// ============================================================ registry

std::string Os::regGet(const std::string &key) const {
  auto it = reg_.find(key);
  return it == reg_.end() || !visible(key) ? "" : it->second;
}

std::vector<std::string> Os::regKeys() const {
  std::vector<std::string> v;
  for (auto &kv : reg_)
    if (visible(kv.first)) v.push_back(kv.first);
  return v;
}

std::string Os::regDump() const {
  std::string o = "{";
  for (auto &kv : reg_)
    if (visible(kv.first)) o += (o.size() > 1 ? "," : "") + jsonStr(kv.first) + ":" + kv.second;
  return o + "}";
}

std::string Os::regDumpAll() const {
  std::string o = "{";
  for (auto &kv : reg_) o += (o.size() > 1 ? "," : "") + jsonStr(kv.first) + ":" + kv.second;
  return o + "}";
}

bool Os::regSet(const std::string &key, const std::string &json, std::string &err) {
  if (!privileged()) {
    err = "the registry is read-only for apps";
    return false;
  }
  Json v;
  if (key.empty() || !Json::parse(json.data(), json.size(), v)) {
    err = "bad registry value";
    return false;
  }
  reg_[key] = v.dump();
  plat_.registrySave(regDumpAll());
  applyRegistry();
  js_.busEmit("registry", "{\"key\":" + jsonStr(key) + ",\"value\":" + reg_[key] + "}");
  return true;
}

bool Os::regRemove(const std::string &key, std::string &err) {
  if (!privileged()) {
    err = "the registry is read-only for apps";
    return false;
  }
  if (reg_.erase(key)) {
    plat_.registrySave(regDumpAll());
    applyRegistry();
    js_.busEmit("registry", "{\"key\":" + jsonStr(key) + ",\"value\":null}");
  }
  return true;
}

bool Os::regClear(std::string &err) {
  if (!privileged()) {
    err = "the registry is read-only for apps";
    return false;
  }
  reg_.clear();
  plat_.registrySave("{}");
  applyRegistry();
  js_.busEmit("registry", "{\"key\":null,\"value\":null}");
  return true;
}

void Os::applyRegistry() {
  eng_.setDarkMode(regGet("darkMode") == "true");
  kioskLoad();
}

// ============================================================ card

bool Os::sdFormat(std::string &err) {
  if (!privileged()) {
    err = "only the system can format the card";
    return false;
  }
  flush();
  memPath_.clear();  // nothing of the old card is written back
  if (!plat_.sdFormat(err)) return false;
  // the running system was mapped from flash: put it back on the fresh card so it boots again
  std::string root = plat_.sdRoot();
  makeDirs(root + "/applications");
  makeDirs(root + "/system/data");
  if (sysImage_) {
    if (auto src = sysImage_->source()) {  // streamed back from flash, 4 KB at a time
      if (FILE *f = fopen((root + "/system/os.idb").c_str(), "wb")) {
        char buf[4096];
        for (size_t at = 0, n; at < src->size(); at += n) {
          n = std::min(sizeof buf, src->size() - at);
          if (!src->read(at, buf, n) || fwrite(buf, 1, n, f) != n) break;
        }
        fclose(f);
      }
    }
    writeFile(root + "/system/os.json", "{\"version\":" + jsonStr(kVersion) + ",\"hash\":" + jsonStr(sysHash_) + "}");
    if (const Bundle::File *f = eng_.bundle().find("system/assets/default-icon.img"))
      writeFile(root + "/system/assets/default-icon.img", eng_.bundle().text("system/assets/default-icon.img"));
  }
  memPath_ = hostPath(app_.dir + "/memory.json");
  mem_.clear();
  return regClear(err);  // the settings go back to their defaults too
}

// ============================================================ system UI

void Os::documentReady(Document *doc) {
  shell_ = title_ = message_ = okBtn_ = cancelBtn_ = launchLabel_ = launchBar_ = passInput_ = passError_ = nullptr;
  if (state_ != RUNNING || !doc || !doc->html) return;
  std::string html = eng_.bundle().text("system/shell/shell.html");
  if (html.empty()) return;
  Element *frag = parseFragment(doc, html, TAG_div);
  frag->ref();
  Element *root = frag->firstElementChild();
  if (root) {
    root->forEachElement([this](Element *e) {
      e->flags |= NF_INTERNAL;
      if (e->hasClass(intern("os-title"))) title_ = e;
      else if (e->hasClass(intern("os-message"))) message_ = e;
      else if (e->hasClass(intern("os-ok"))) okBtn_ = e;
      else if (e->hasClass(intern("os-cancel"))) cancelBtn_ = e;
      else if (e->hasClass(intern("os-launch-label"))) launchLabel_ = e;
      else if (e->hasClass(intern("os-launch-bar"))) launchBar_ = e;
      else if (e->hasClass(intern("os-pass-input"))) passInput_ = e;
      else if (e->hasClass(intern("os-pass-error"))) passError_ = e;
    });
    doc->html->appendChild(root);
    shell_ = root;
  }
  frag->unref();
  // dialog text survives a page change underneath it
  auto setText = [](Element *e, const std::string &t) {
    if (!e) return;
    e->removeAllChildren();
    e->appendChild(new Text(t));
  };
  setText(title_, modalTitle_);
  setText(message_, modalMessage_);
  setText(okBtn_, modalOk_);
  setText(cancelBtn_, modalCancel_);
  if (launching_) setText(launchLabel_, loadingApp_.title);
  if (modalOpen_ && modalPass_) passShow();  // the password form survives too (empty)
  setShellClass();
}

void Os::setShellClass() {
  if (!shell_) return;
  std::string c = "os-shell";
  if (navOpen_) c += " nav-open";
  if (modalOpen_) c += " modal-open";
  if (launching_) c += " launching";
  if (modalCancel_.empty()) c += " no-cancel";
  if (modalOpen_ && modalPass_) c += " ask-pass";
  shell_->setAttr(A.class_, c);
}

void Os::showNav(bool on) {
  navOpen_ = on;
  navHideAt_ = now_ + kNavShowMs;
  setShellClass();
}

void Os::confirm(const std::string &title, const std::string &message, const std::string &ok,
                 const std::string &cancel, std::function<void(bool)> done) {
  if (modalOpen_ && modalDone_) {
    auto prev = std::move(modalDone_);
    prev(false);
  }
  if (modalPass_) passClose();
  modalTitle_ = title;
  modalMessage_ = message;
  modalOk_ = ok.empty() ? "OK" : ok;
  modalCancel_ = cancel;
  modalDone_ = std::move(done);
  modalOpen_ = true;
  navOpen_ = false;
  auto setText = [](Element *e, const std::string &t) {
    if (!e) return;
    e->removeAllChildren();
    e->appendChild(new Text(t));
  };
  setText(title_, modalTitle_);
  setText(message_, modalMessage_);
  setText(okBtn_, modalOk_);
  setText(cancelBtn_, modalCancel_);
  setShellClass();
  eng_.requestFrame();
}

Element *Os::shellHit(float x, float y, std::string &action) const {
  action.clear();
  if (!shell_) return nullptr;
  Element *e = eng_.renderer().hitTest(x, y, true);
  static Atom aOs = intern("data-os");
  for (Element *p = e; p; p = p->parent) {
    if (const std::string *v = p->getAttr(aOs)) {
      action = *v;
      return p;
    }
    if (p == shell_) break;
  }
  return nullptr;
}

void Os::action(const std::string &what) {
  if (modalPass_ && (what == "ok" || what == "cancel")) {
    if (what == "ok") return passSubmit();
    passClose();  // Cancel: stay where we are
    modalOpen_ = false;
    setShellClass();
    return;
  }
  if (what == "ok" || what == "cancel" || what == "dismiss") {
    if (what == "dismiss" && !modalCancel_.empty()) return;  // a tap outside does not answer a question
    modalOpen_ = false;
    setShellClass();
    if (modalDone_) {
      auto done = std::move(modalDone_);
      modalDone_ = nullptr;
      done(what == "ok");
    }
    return;
  }
  if (what == "home") {
    showNav(true);
    requestHome();
  } else if (what == "back") {
    showNav(true);
    back();
  }
}

bool Os::pointer(int id, PointerPhase phase, float x, float y) {
  if (state_ != RUNNING || !shell_) return false;
  // a dialog or an app being opened takes every touch
  bool exclusive = modalOpen_ || launching_;
  bool onBar = false;
  if (phase == PTR_DOWN) {
    std::string a;
    shellHit(x, y, a);
    if (!a.empty() && (exclusive || (navOpen_ && (a == "home" || a == "back")))) {
      pressId_ = id;
      pressAction_ = a;
      navHideAt_ = now_ + kNavShowMs;
      return true;
    }
    onBar = false;
  }
  if (id == pressId_) {
    if (phase == PTR_UP) {
      std::string a;
      shellHit(x, y, a);
      std::string pressed = pressAction_;
      pressId_ = -1;
      if (a == pressed) action(pressed);
    } else if (phase == PTR_CANCEL) {
      pressId_ = -1;
    }
    return true;
  }
  if (exclusive) return !modalPass_;  // the password form types with the engine's keyboard (above the dialog)
  (void)onBar;
  // swipe up from the bottom edge shows the system bar (inside apps)
  if (phase == PTR_DOWN && inApp() && y >= eng_.display().height() - kEdge) {
    gestId_ = id;
    gestY0_ = y;
    gestUsed_ = false;
    return true;
  }
  if (id == gestId_) {
    if (phase == PTR_MOVE && !gestUsed_ && gestY0_ - y > 24) {
      gestUsed_ = true;
      showNav(true);
    }
    if (phase == PTR_UP || phase == PTR_CANCEL) gestId_ = -1;
    return true;
  }
  return false;
}

void Os::tick(double now) {
  now_ = now;
  if (state_ != RUNNING) {
    bootStep(now);
    return;
  }
  if (!deferred_.empty()) {
    std::vector<std::function<void()>> q;
    q.swap(deferred_);
    for (auto &f : q) f();
  }
  if (loading_) {
    float p = loading_->step();
    if (launchBar_) {
      char buf[48];
      snprintf(buf, sizeof buf, "width:%.1f%%", p * 100);
      launchBar_->setAttr(A.style, buf);
    }
    if (loading_->failed()) {
      std::string err = loading_->error();
      loading_.reset();
      launching_ = false;
      setShellClass();
      confirm(loadingApp_.title, "Cannot open the app: " + err, "OK", "", nullptr);
    } else if (p >= 1) {
      appImage_.reset();  // the previous app's image (if any) is released first
      eng_.unmount("app/");
      appImage_ = std::move(loading_);
      launching_ = false;
      if (!eng_.mount("app/", appImage_->source())) {
        appImage_.reset();
        setShellClass();
        confirm(loadingApp_.title, "The app image is damaged; install it again.", "OK", "", nullptr);
      } else {
        enter(loadingApp_, TR_SLIDE_UP);
      }
    }
  }
  if (navOpen_ && !modalOpen_ && pressId_ < 0 && gestId_ < 0 && now >= navHideAt_) showNav(false);
  if (memDirty_ && now >= memFlushAt_) flush();
  netTick();
  storeTick();
}

}  // namespace rt
