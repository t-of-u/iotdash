#pragma once
// QuickJS host. One JS runtime per app session, a fresh context (global scope) per page: a
// page starts clean and the previous page's code stops. Timers, animation frames, fetches,
// background processes, OS event listeners and open files carry the scope that created them -
// the page (dropped when it is left) or the app (kScopeApp, dropped with the session).
// DOM nodes are exposed through wrapper objects that hold a reference on the native node;
// wrappers of connected nodes are pinned so expando properties and listeners survive. Most of
// the Web API surface lives in bootstrap.js on top of a small set of native functions (js.cpp).

#include <cstdio>
#include <map>

#include "engine.h"

extern "C" {
#include "quickjs.h"
}

namespace rt {

class Os;

class JsHost : public ScriptHost {
 public:
  explicit JsHost(Engine &e);
  ~JsHost() override;

  void reset(Document *doc) override;
  void runScript(const std::string &src, const std::string &name, bool module) override;
  bool dispatch(Element *target, const std::string &type, const std::string &detailJson, bool bubbles, bool cancelable,
                float x, float y, int pointerId) override;
  void dispatchWindow(const std::string &type, const std::string &detailJson) override;
  void tick(uint64_t nowMs) override;
  // Kernel guards for app code: a script that runs longer than `ms` from now is interrupted
  // (an error the app cannot catch), so a stuck app cannot freeze drawing or input.
  void armWatchdog(uint32_t ms) override;
  uint32_t watchdogStops() const { return interrupts_; }
  void animationFrame(double ts) override;
  bool wantsFrame() const override { return !raf_.empty(); }
  uint64_t nextTimerMs() const override;
  void nodeConnected(Node *n, bool connected) override;
  void nodeDestroyed(Node *n) override;
  std::string eval(const std::string &src) override;

  // ---- OS: sessions, scopes, background processes, event bus, files
  static constexpr uint32_t kScopeApp = 0;
  void setOs(Os *os) { os_ = os; }
  Os *os() const { return os_; }
  void endSession();  // the app closes: free the runtime with everything in it
  uint32_t scope() const { return curScope_; }
  // Background process `fn`, called every intervalMs (first call right away). It may return
  // a number (ms until the next call), false (stop) or a promise of either.
  int setProcess(const std::string &name, bool appScope, JSValueConst fn, uint32_t intervalMs);
  bool removeProcess(const std::string &name);
  int clearProcesses(int which);  // 0 all, 1 app scope, 2 page scope; returns how many
  JSValue processList(JSContext *ctx);
  void processSettled(JSContext *ctx, int id, JSValueConst value, bool rejected);
  int busOn(const std::string &name, JSValueConst fn);
  void busOff(int id);
  void busEmit(const std::string &name, const std::string &json);  // delivered on the next tick
  int fileOpen(FILE *f, bool binary);
  FILE *file(int id, bool *binary = nullptr);
  void fileClose(int id);

  // ---- used by the native functions
  Engine &engine() { return eng_; }
  JSContext *ctx() const { return ctx_; }
  void listen(const std::string &type) { listened_[type]++; }
  int addTimer(JSValueConst fn, uint32_t delay, bool repeat, std::vector<JSValue> args);
  void clearTimer(int id);
  int addRaf(JSValueConst fn);
  void cancelRaf(int id);
  void runJobs();
  void reportException(const char *where);
  JSClassID nodeClass() const { return nodeClass_; }
  JSClassID canvasClass() const { return canvasClass_; }
  void setProto(const std::string &key, JSValueConst proto);
  JSValue wrap(Node *n);             // new reference (JS_NULL for nullptr)
  Node *unwrap(JSValueConst v) const;  // nullptr if not a node
  void attributeChanged(Element *e, Atom name) override;
  void animationFinished(int id) override;
  size_t memoryUsed() const;
  // fetch completions etc. queued by the platform
  void completeFetch(int id, const FetchResponse &r);
  int newFetch(JSValue resolve, JSValue reject);
  void imageLoaded(Element *img, bool ok);

 private:
  void createRuntime();
  void retirePage();
  void dropScope(uint32_t scope);
  void runProcesses(uint64_t now);
  // app work (timers, processes) per tick: what does not fit waits for the next tick, so a
  // busy app delays itself, not the frame or the input
  static constexpr uint32_t kTickBudgetUs = 10000;

  uint64_t budgetEndUs_ = 0;
  bool overBudget() const;
  uint64_t deadlineUs_ = 0;
  uint32_t interrupts_ = 0;
  bool interrupted_ = false;  // the last exception came from the watchdog
  bool takeInterrupted();
  static int onInterrupt(JSContext *ctx, void *opaque);
  void deliverBus();
  void pin(Node *n);
  void unpin(Node *n);
  bool interested(const std::string &type, Element *target) const;
  void fireTimers(uint64_t now);

  Engine &eng_;
  JSRuntime *rt_ = nullptr;
  JSContext *ctx_ = nullptr;
  JSClassID nodeClass_ = 0, canvasClass_ = 0;
  std::map<std::string, JSValue> protos_;
  struct Timer {
    int id;
    uint64_t when;
    uint32_t interval;  // 0 = one shot
    JSValue fn;
    std::vector<JSValue> args;
    uint32_t scope;
  };
  std::vector<Timer> timers_;
  int nextTimer_ = 1;
  struct Raf {
    int id;
    JSValue fn;
    uint32_t scope;
  };
  std::vector<Raf> raf_;
  int nextRaf_ = 1;
  std::unordered_map<std::string, int> listened_;
  JSValue dispatchFn_, dispatchWinFn_, connectedFn_, attrFn_, responseFn_;
  std::vector<Node *> pinned_;
  uint32_t detached_ = 0;    // nodes taken out of the document under wrappers since the last GC
  uint64_t lastGcMs_ = 0, lastDetachUs_ = 0;
  struct PendingFetch {
    int id;
    JSValue resolve, reject;
    uint32_t scope;
  };
  std::vector<PendingFetch> fetches_;
  int nextFetch_ = 1;
  bool alive_ = false;

  Os *os_ = nullptr;
  uint32_t pageGen_ = 0, nextGen_ = 1, curScope_ = 0;
  struct Proc {
    int id;
    std::string name;
    uint32_t scope;
    JSValue fn;
    uint32_t interval;
    uint64_t next;
    bool busy = false, dead = false;
    uint32_t runs = 0;
  };
  std::vector<Proc> procs_;
  int nextProc_ = 1;
  struct BusListener {
    int id;
    std::string name;
    uint32_t scope;
    JSValue fn;
  };
  std::vector<BusListener> bus_;
  std::vector<std::pair<std::string, std::string>> busQueue_;
  int nextBus_ = 1;
  struct OpenFile {
    int id;
    FILE *f;
    bool binary;
    uint32_t scope;
  };
  std::vector<OpenFile> files_;
  int nextFile_ = 1;
};

}  // namespace rt
