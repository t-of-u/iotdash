// Connectivity for the OS on top of Platform::radio: WiFi (saved networks in the registry,
// auto-connect to the best one in range), Bluetooth LE (powered on first use, devices that
// were connected are remembered) and an internet check while WiFi is up.
//
// Registry keys:  wifi.enabled (bool, default true)   bt.enabled (bool, default true)
//                 secure.wifi  [{ssid, pass}] most recent first (system pages only)
//                 bt.devices   [{id, name, type}] most recent first
//                 net.probeUrl the internet check (default: Google's generate_204)

#include "js.h"
#include "os.h"

namespace rt {

static constexpr double kProbeEveryMs = 30000;  // internet check while WiFi is up
static constexpr double kProbeRetryMs = 8000;   // sooner after a failed one
static constexpr double kAutoEveryMs = 30000;   // look for a saved network while not connected
static constexpr size_t kMaxSaved = 8;
static const char *kProbeUrl = "http://connectivitycheck.gstatic.com/generate_204";

static Json parseJson(const std::string &s) {
  Json j;
  if (!s.empty()) Json::parse(s.data(), s.size(), j);
  return j;
}
static std::string q(const std::string &s) { return Json::quote(s); }
static bool boolOr(const Json &j, bool d) { return j.type == Json::Bool ? j.b : d; }

bool Os::visible(const std::string &key) const { return privileged() || !startsWith(key, "secure."); }

Json Os::regJson(const std::string &key) const {
  auto it = reg_.find(key);
  return it == reg_.end() ? Json() : parseJson(it->second);
}

void Os::regPut(const std::string &key, const Json &v) {
  reg_[key] = v.dump();
  plat_.registrySave(regDumpAll());
  js_.busEmit("registry", "{\"key\":" + q(key) + ",\"value\":" + (visible(key) ? reg_[key] : "null") + "}");
}

void Os::rememberNetwork(const std::string &ssid, const std::string &pass) {
  Json list = regJson("secure.wifi"), out;
  out.type = Json::Arr;
  Json e;
  e.type = Json::Obj;
  Json s, p;
  s.type = p.type = Json::Str;
  s.str = ssid;
  p.str = pass;
  e.obj = {{"ssid", s}, {"pass", p}};
  out.arr.push_back(e);
  if (list.type == Json::Arr)
    for (auto &n : list.arr)
      if (n.strOr("ssid", "") != ssid && out.arr.size() < kMaxSaved) out.arr.push_back(n);
  regPut("secure.wifi", out);
}

void Os::rememberDevice(const std::string &id, const std::string &name, int addrType) {
  Json list = regJson("bt.devices"), out;
  out.type = Json::Arr;
  Json e = parseJson("{\"id\":" + q(id) + ",\"name\":" + q(name) + ",\"type\":" + std::to_string(addrType) + "}");
  out.arr.push_back(e);
  if (list.type == Json::Arr)
    for (auto &d : list.arr)
      if (d.strOr("id", "") != id && out.arr.size() < kMaxSaved) out.arr.push_back(d);
  regPut("bt.devices", out);
}

// ---------------------------------------------------------------- start-up and events

void Os::netStart() {
  // what the radios are doing now (they may have been switched before the system came up)
  plat_.radio("wifi.state", "{}", [this](const std::string &err, const std::string &json) {
    if (err.empty() && !json.empty()) onRadio("wifi", json);
  });
  plat_.radio("bt.state", "{}", [this](const std::string &err, const std::string &json) {
    if (err.empty() && !json.empty()) onRadio("bt", json);
  });
  // the networks the system image was built with (os/core/private/secrets.json): added to the
  // saved ones whenever that file changes (a network forgotten in Settings stays forgotten
  // until the file changes again)
  std::string secText = eng_.bundle().text("system/private/secrets.json");
  uint32_t h = 2166136261u;
  for (unsigned char c : secText) h = (h ^ c) * 16777619u;
  std::string hash = std::to_string(h);
  if (!secText.empty() && regJson("secure.secretsHash").str != hash) {
    Json hv;
    hv.type = Json::Str;
    hv.str = hash;
    regPut("secure.secretsHash", hv);
    Json sec = parseJson(secText);
    const Json *w = sec.get("wifi");
    std::vector<std::pair<std::string, std::string>> nets;
    if (w && w->type == Json::Arr)
      for (auto &n : w->arr) nets.push_back({n.strOr("ssid", ""), n.strOr("pass", "")});
    else if (sec.type == Json::Obj)
      nets.push_back({sec.strOr("ssid", ""), sec.strOr("pass", "")});
    for (auto it = nets.rbegin(); it != nets.rend(); ++it)
      if (!it->first.empty()) rememberNetwork(it->first, it->second);
  }
  if (boolOr(regJson("wifi.enabled"), true))
    plat_.radio("wifi.power", "{\"on\":true}", [this](const std::string &err, const std::string &) {
      if (err.empty()) wifiAutoConnect();
    });
  // Bluetooth stays off until something uses it (it costs internal RAM)
}

void Os::wifiAutoConnect() {
  nextAuto_ = now_ + kAutoEveryMs;
  Json saved = regJson("secure.wifi");
  if (saved.type != Json::Arr || saved.arr.empty() || wifiUp_ || !pendingSsid_.empty()) return;
  plat_.radio("wifi.scan", "{}", [this](const std::string &err, const std::string &json) {
    if (!err.empty() || wifiUp_ || !pendingSsid_.empty()) return;
    Json found = parseJson(json), saved = regJson("secure.wifi");
    if (found.type != Json::Arr || saved.type != Json::Arr) return;
    // the most recently used saved network that is in range
    for (auto &n : saved.arr) {
      std::string ssid = n.strOr("ssid", "");
      for (auto &ap : found.arr)
        if (ap.strOr("ssid", "") == ssid) {
          pendingSsid_ = ssid;
          pendingPass_ = n.strOr("pass", "");
          plat_.radio("wifi.connect", "{\"ssid\":" + q(ssid) + ",\"pass\":" + q(pendingPass_) + "}",
                      [](const std::string &, const std::string &) {});
          return;
        }
    }
  });
}

void Os::onRadio(const std::string &name, const std::string &json) {
  if (name == "wifi") {
    wifiJson_ = json;
    Json j = parseJson(json);
    std::string state = j.strOr("state", "off"), ssid = j.strOr("ssid", "");
    bool up = state == "connected";
    if (up && !wifiUp_) {
      if (!pendingSsid_.empty() && pendingSsid_ == ssid) rememberNetwork(pendingSsid_, pendingPass_);
      nextProbe_ = 0;  // check the internet now
    }
    if (up || state == "failed" || state == "off" || state == "idle") {
      pendingSsid_.clear();
      pendingPass_.clear();
    }
    wifiUp_ = up;
    if (!up) {
      setInternet(state == "connecting" ? internet_ : 0);
      if (state == "idle" || state == "failed") nextAuto_ = now_ + kAutoEveryMs;
    }
    emitNet();
  } else if (name == "bt") {
    Json j = parseJson(json);
    std::string type = j.strOr("type", "");
    if (type == "state") {
      btJson_ = json;
      const Json *p = j.get("power");
      btPower_ = p && p->type == Json::Bool && p->b;
      emitNet();
    }
    js_.busEmit("bt", json);
  }
}

void Os::emitNet() { js_.busEmit("net", netState()); }

std::string Os::netState() const {
  return "{\"wifi\":" + wifiJson_ + ",\"internet\":" + (internet_ < 0 ? "null" : internet_ ? "true" : "false") +
         ",\"bt\":" + btJson_ + "}";
}

void Os::setInternet(int v) {
  if (v == internet_) return;
  internet_ = v;
  emitNet();
}

void Os::probeInternet() {
  if (probing_) return;
  probing_ = true;
  FetchRequest r;
  Json custom = regJson("net.probeUrl");
  r.url = custom.type == Json::Str && !custom.str.empty() ? custom.str : kProbeUrl;
  r.timeoutMs = 6000;
  bool google = custom.type != Json::Str || custom.str.empty();
  plat_.fetch(r, [this, google](FetchResponse resp) {
    probing_ = false;
    // generate_204 answers 204; anything else there is a captive portal or no route
    bool ok = google ? resp.status == 204 : resp.status >= 200 && resp.status < 300;
    if (!wifiUp_) ok = false;
    nextProbe_ = now_ + (ok ? kProbeEveryMs : kProbeRetryMs);
    setInternet(ok ? 1 : 0);
    auto waiters = std::move(probeWaiters_);
    probeWaiters_.clear();
    for (auto &w : waiters) w("", ok ? "true" : "false");
  });
}

void Os::netTick() {
  if (wifiUp_ && !probing_ && now_ >= nextProbe_) probeInternet();
  if (!wifiUp_ && pendingSsid_.empty() && nextAuto_ > 0 && now_ >= nextAuto_ &&
      boolOr(regJson("wifi.enabled"), true))
    wifiAutoConnect();
}

void Os::btEnsure(std::function<void(const std::string &err)> then) {
  if (btPower_) return then("");
  if (!boolOr(regJson("bt.enabled"), true)) return then("Bluetooth is off");
  btWaiters_.push_back(std::move(then));
  if (btPowering_) return;
  btPowering_ = true;
  plat_.radio("bt.power", "{\"on\":true}", [this](const std::string &err, const std::string &) {
    btPowering_ = false;
    if (err.empty()) btPower_ = true;
    auto waiters = std::move(btWaiters_);
    btWaiters_.clear();
    for (auto &w : waiters) w(err);
  });
}

// ---------------------------------------------------------------- requests from script

void Os::net(const std::string &op, const std::string &argsJson, Platform::RadioDone done, bool asSystem) {
  Json args = parseJson(argsJson);
  static const char *kSystemOnly[] = {"wifi.power", "wifi.connect", "wifi.disconnect", "wifi.forget",
                                      "bt.power", "bt.forget"};
  for (const char *s : kSystemOnly)
    if (op == s && !asSystem && !privileged()) return done("only the system can change connections", "");
  if (startsWith(op, "store.")) {
    if (!asSystem && !privileged()) return done("only the system can install apps", "");
    return storeOp(op, args, std::move(done));
  }

  if (op == "net.check") {
    if (!wifiUp_) return done("", "false");
    probeWaiters_.push_back(std::move(done));
    nextProbe_ = 0;
    probeInternet();
    return;
  }
  // ---- WiFi
  if (op == "wifi.saved") {
    std::string out = "[";
    Json saved = regJson("secure.wifi");
    if (saved.type == Json::Arr)
      for (auto &n : saved.arr) out += (out.size() > 1 ? "," : "") + std::string("{\"ssid\":") + q(n.strOr("ssid", "")) + "}";
    return done("", out + "]");
  }
  if (op == "wifi.forget") {
    std::string ssid = args.strOr("ssid", "");
    Json saved = regJson("secure.wifi"), out;
    out.type = Json::Arr;
    if (saved.type == Json::Arr)
      for (auto &n : saved.arr)
        if (n.strOr("ssid", "") != ssid) out.arr.push_back(n);
    regPut("secure.wifi", out);
    if (parseJson(wifiJson_).strOr("ssid", "") == ssid) return plat_.radio("wifi.disconnect", "{}", done);
    return done("", "");
  }
  if (op == "wifi.power") {
    bool on = boolOr(args.get("on") ? *args.get("on") : Json(), true);
    Json v;
    v.type = Json::Bool;
    v.b = on;
    regPut("wifi.enabled", v);
    return plat_.radio("wifi.power", on ? "{\"on\":true}" : "{\"on\":false}",
                       [this, on, done](const std::string &err, const std::string &json) {
                         if (err.empty() && on) wifiAutoConnect();
                         done(err, json);
                       });
  }
  if (op == "wifi.connect") {
    std::string ssid = args.strOr("ssid", "");
    if (ssid.empty()) return done("no network name", "");
    std::string pass;
    if (const Json *p = args.get("pass"); p && p->type == Json::Str) pass = p->str;
    else {  // a saved network: its password from the registry
      Json saved = regJson("secure.wifi");
      if (saved.type == Json::Arr)
        for (auto &n : saved.arr)
          if (n.strOr("ssid", "") == ssid) pass = n.strOr("pass", "");
    }
    pendingSsid_ = ssid;
    pendingPass_ = pass;
    return plat_.radio("wifi.connect", "{\"ssid\":" + q(ssid) + ",\"pass\":" + q(pass) + "}", done);
  }
  if (startsWith(op, "wifi.")) {
    if (op == "wifi.scan" && !boolOr(regJson("wifi.enabled"), true)) return done("Wi-Fi is off", "");
    return plat_.radio(op, argsJson, done);
  }
  // ---- Bluetooth LE
  if (op == "bt.devices") {
    Json d = regJson("bt.devices");
    return done("", d.type == Json::Arr ? d.dump() : "[]");
  }
  if (op == "bt.power") {
    bool on = boolOr(args.get("on") ? *args.get("on") : Json(), true);
    Json v;
    v.type = Json::Bool;
    v.b = on;
    regPut("bt.enabled", v);
    if (on) return btEnsure([done](const std::string &err) { done(err, ""); });
    btPower_ = false;
    return plat_.radio("bt.power", "{\"on\":false}", done);
  }
  if (op == "bt.forget") {
    std::string id = args.strOr("id", "");
    Json list = regJson("bt.devices"), out;
    out.type = Json::Arr;
    if (list.type == Json::Arr)
      for (auto &d : list.arr)
        if (d.strOr("id", "") != id) out.arr.push_back(d);
    regPut("bt.devices", out);
    if (!btPower_) return done("", "");
    return plat_.radio("bt.disconnect", argsJson, done);
  }
  if (startsWith(op, "bt.")) {
    std::string a = argsJson;
    if (op == "bt.connect" && !args.get("type")) {  // the address type of a remembered device
      Json list = regJson("bt.devices");
      std::string id = args.strOr("id", "");
      if (list.type == Json::Arr)
        for (auto &d : list.arr)
          if (d.strOr("id", "") == id && d.get("type"))
            a = "{\"id\":" + q(id) + ",\"type\":" + std::to_string((int)d.numOr("type", 0)) + "}";
    }
    return btEnsure([this, op, a, done](const std::string &err) {
      if (!err.empty()) return done(err, "");
      plat_.radio(op, a, [this, op, done](const std::string &e, const std::string &json) {
        if (e.empty() && op == "bt.connect") {
          Json d = parseJson(json);
          rememberDevice(d.strOr("id", ""), d.strOr("name", ""), (int)d.numOr("type", 0));
        }
        done(e, json);
      });
    });
  }
  done("unknown operation " + op, "");
}

}  // namespace rt
