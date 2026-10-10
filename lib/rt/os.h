#pragma once
// iotdash OS: a small operating layer on the engine.
//
//  - boots from the SD card: the boot screen (an image built into the firmware) shows progress
//    while the system image /system/os.idb is mapped (copied into a flash slot when it changed)
//  - the system image holds the home screen, the system UI (bottom bar, dialogs), the OS script
//    API (os.js) and the core apps (settings); installed apps live in /applications/<id>/ with
//    their compiled image app.idb, icon.img, memory.json and data/
//  - one app runs at a time: its pages are opened one after another (a navigation stack) in
//    fresh script contexts; background processes with APP scope live as long as the app
//  - the registry (small JSON values in internal flash) is read by everyone, written by the
//    system only
//
// Card layout:  /system/{os.idb, os.json, memory.json, data/}   /applications/<id>/{app.json,
// app.idb, icon.img, memory.json, data/}

#include <map>

#include "engine.h"

namespace rt {

class JsHost;

struct OsApp {
  std::string id, title, version, hash;
  std::string dir;    // card path: /applications/<id>, or /system for system apps
  std::string image;  // card path of app.idb ("" = inside the system image)
  std::string root;   // file prefix of its pages once mounted: "app/" or "system/apps/<id>/"
  std::string icon;   // URL for <img src>
  bool system = false;  // part of the OS: may write the registry, format the card
  // install.json: where the app came from ("store", "usb"; "" = unknown) and the store's version
  std::string source, storeVersion, storeVersionId;
  double installed = 0;  // unix seconds
};

class Os : public OsHooks {
 public:
  static constexpr const char *kVersion = "0.1.0";

  Os(Engine &e, JsHost &js);
  ~Os() override;

  // Show the boot image (built into the firmware) and bring the system up from the card.
  void boot(const uint8_t *bootImage, size_t len);
  bool running() const { return state_ == RUNNING; }

  // ---- apps and pages
  std::vector<OsApp> apps(bool withSystem = true);
  bool launch(const std::string &id, std::string &err);
  void home();          // close the running app (no question) and show the home screen
  void requestHome();   // asks first when an app is running
  void back();          // previous page, or offer to close the app on its first page
  // Open pages/<page> of the running app (pushes onto the navigation stack).
  bool navigate(const std::string &page, std::string &err);
  bool navigateBack();  // false on the app's first page
  const OsApp &current() const { return app_; }
  bool inApp() const { return !app_.id.empty() && app_.id != "home"; }
  std::string page() const;  // current page, relative to the app's root
  int depth() const { return (int)stack_.size(); }
  bool privileged() const { return app_.system || state_ != RUNNING; }
  bool uninstall(const std::string &id, std::string &err);
  void reloadSystem();  // after a new system image was deployed: boot again
  // Apps were installed or removed from outside (USB): the home screen lists them again; the
  // running app, when it is the one that changed, closes.
  void appsChanged(const std::string &id);
  // System dialog; done(true) for the OK button.
  void confirm(const std::string &title, const std::string &message, const std::string &ok, const std::string &cancel,
               std::function<void(bool)> done);

  // ---- memory.json: localStorage of the running app (the system's for system pages)
  bool memGet(const std::string &k, std::string &out) const;
  void memSet(const std::string &k, const std::string &v);
  void memRemove(const std::string &k);
  std::vector<std::string> memKeys() const;
  void memClear();
  void flush();  // write pending memory.json changes now

  // ---- files of the running app: a path inside its data/ folder -> host path ("" = refused)
  std::string dataPath(const std::string &rel, std::string &err) const;
  std::string hostPath(const std::string &cardPath) const;  // "/system/x" -> host path

  // ---- registry: values are JSON text
  std::string regGet(const std::string &key) const;  // "" when missing
  std::vector<std::string> regKeys() const;
  std::string regDump() const;  // the whole registry as a JSON object
  bool regSet(const std::string &key, const std::string &json, std::string &err);
  bool regRemove(const std::string &key, std::string &err);
  bool regClear(std::string &err);

  // ---- connectivity (os_net.cpp): WiFi, Bluetooth LE and whether the internet answers.
  // Registry: wifi.enabled, bt.enabled, bt.devices (remembered), secure.wifi (saved networks
  // with their passwords: system pages only). Defaults for the first boot come from the system
  // image's private/secrets.json.
  // A request from script (os.wifi / os.bt / os.net): checked against the caller's rights,
  // answered later on the engine thread.
  // asSystem: a request from the kernel itself (the USB link), not from the running page.
  void net(const std::string &op, const std::string &argsJson, Platform::RadioDone done, bool asSystem = false);
  std::string netState() const;  // {"wifi": {...}, "internet": true|false|null, "bt": {...}}

  // ---- card
  SdInfo sdInfo() { return plat_.sdInfo(); }
  // Factory reset: formats the card (apps, their data, the system memory), puts the running
  // system back on it and clears the registry. Only the empty file system is left.
  bool sdFormat(std::string &err);

  // OsHooks
  void documentReady(Document *doc) override;
  bool pointer(int id, PointerPhase phase, float x, float y) override;
  bool linkActivated(const std::string &url) override;
  void tick(double nowMs) override;
  std::shared_ptr<Image> image(const std::string &url) override;

  // file helpers on host paths (POSIX; the card is mounted into the VFS on the device)
  static bool readFile(const std::string &path, std::string &out);
  static bool writeFile(const std::string &path, const std::string &data);
  static bool makeDirs(const std::string &path);  // like mkdir -p
  static bool removeTree(const std::string &path);  // file or directory, recursively

 private:
  enum State { BOOT_CARD, BOOT_IMAGE, RUNNING };
  void bootStatus(const std::string &text, float progress, const std::string &error = "", bool canFormat = false);
  void bootStep(double now);
  void startSystem();
  void enter(const OsApp &app, TransitionKind tr);  // make app current and open its first page
  void openPage(const std::string &url, TransitionKind tr);
  void loadMemory();
  void applyRegistry();
  OsApp homeApp() const;
  bool readApp(const std::string &dir, OsApp &out);
  // connectivity (os_net.cpp)
  void netStart();  // the registry is loaded: radios on and auto-connect, as it says
  void netTick();
  void onRadio(const std::string &name, const std::string &json);
  void probeInternet();
  void setInternet(int v);
  void emitNet();
  void wifiAutoConnect();
  Json regJson(const std::string &key) const;  // registry value parsed (Null when missing)
  void regPut(const std::string &key, const Json &v);  // system write, no rights check
  void rememberNetwork(const std::string &ssid, const std::string &pass);
  void rememberDevice(const std::string &id, const std::string &name, int addrType);
  void btEnsure(std::function<void(const std::string &err)> then);  // powered when enabled
  bool visible(const std::string &key) const;  // secure.* keys: system pages only
  // apps from the store (os_store.cpp)
  struct StoreJob;
  void storeOp(const std::string &op, const Json &args, Platform::RadioDone done);
  bool commitInstall(StoreJob &job, std::string &err);
  void storeTick();
  void storeStop();  // the OS goes away: downloads cancelled, their answers dropped
  std::vector<std::shared_ptr<StoreJob>> storeJobs_;
  // system UI
  void setShellClass();
  void showNav(bool on);
  void action(const std::string &what);
  Element *shellHit(float x, float y, std::string &action) const;

  Engine &eng_;
  JsHost &js_;
  Platform &plat_;
  const uint8_t *bootImage_ = nullptr;
  size_t bootLen_ = 0;
  State state_ = BOOT_CARD;
  std::vector<std::function<void()>> deferred_;  // work that must not run inside a script call
  double now_ = 0, nextTry_ = 0;
  std::string cardError_;

  std::unique_ptr<ImageLoad> sysImage_, appImage_, loading_;
  OsApp loadingApp_;
  std::string sysHash_;

  OsApp app_;                       // running app (or the home screen)
  std::vector<std::string> stack_;  // page URLs of the running app, last = shown

  std::map<std::string, std::string> mem_;
  std::string memPath_;
  bool memDirty_ = false;
  double memFlushAt_ = 0;

  std::map<std::string, std::string> reg_;  // key -> JSON text
  std::string regDumpAll() const;            // every key (what is saved)

  // connectivity state, as the radios last reported it
  std::string wifiJson_ = "{\"power\":false,\"state\":\"off\"}";
  std::string btJson_ = "{\"power\":false}";
  int internet_ = -1;  // -1 unknown, 0 no, 1 yes
  double nextProbe_ = 0, nextAuto_ = 0;
  std::vector<Platform::RadioDone> probeWaiters_;
  bool probing_ = false, wifiUp_ = false, btPower_ = false, btPowering_ = false;
  std::vector<std::function<void(const std::string &)>> btWaiters_;
  std::string pendingSsid_, pendingPass_;  // a connect in progress: remembered once it is up
  std::vector<std::string> autoQueue_;     // saved networks still to try at start-up

  // system UI elements in the current document (internal nodes)
  Element *shell_ = nullptr, *title_ = nullptr, *message_ = nullptr, *okBtn_ = nullptr, *cancelBtn_ = nullptr,
          *launchLabel_ = nullptr, *launchBar_ = nullptr;
  bool navOpen_ = false, modalOpen_ = false, launching_ = false;
  double navHideAt_ = 0;
  std::function<void(bool)> modalDone_;
  std::string modalTitle_, modalMessage_, modalOk_, modalCancel_;
  // bottom-edge swipe
  int gestId_ = -1;
  float gestY0_ = 0;
  bool gestUsed_ = false;
  int pressId_ = -1;  // pointer pressed on a system UI element
  std::string pressAction_;
};

}  // namespace rt
