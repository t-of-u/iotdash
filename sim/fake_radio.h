#pragma once
// The simulator's radios: a few WiFi networks to scan and join, and Bluetooth LE devices, one
// of them a weighing scale that streams ASCII frames ("ST,GS,+0012.34kg") over a notify
// characteristic like the BLE-UART modules real scales use. Script command "scale <kg|OL>"
// puts a load on it (it settles there with a little noise).
//
// Passwords of the fake networks: "Home-WiFi" home1234, "TopuTofu" tofu1234, "IoT-Lab"
// iotlab99; "Cafe Free WiFi" is open.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "rt_base.h"

class FakeRadio {
 public:
  using Done = std::function<void(const std::string &err, const std::string &json)>;
  std::function<void(const std::string &, const std::string &)> event;  // Platform::radioEvent
  std::function<uint64_t()> nowUs;
  double scaleKg = 0;      // the load on the fake scale
  bool scaleOver = false;  // shows "OL"
  // The web simulator's scale game moves the reading itself (a display that settles like a real
  // one): frames then carry scaleShownKg and scaleStable as they are, every scaleIntervalMs.
  bool scaleExternal = false;
  double scaleShownKg = 0;
  bool scaleStable = true;
  int scaleIntervalMs = 250;
  // a scale that is switched off: not found by scans, cannot be connected, drops its link
  bool scalePresent = true;
  std::function<void()> onTare;  // an app sent "TARE" to the scale
  // the access points in reach (web simulator): out of reach, scans find nothing and a link drops
  bool wifiReach = true;
  void setWifiReach(bool on) {
    if (wifiReach == on) return;
    wifiReach = on;
    if (on || wifi_ != "connected") return;
    wifi_ = "connecting";  // the driver keeps trying a while
    wifiEvent();
    int gen = ++connGen_;
    later(4000, [this, gen] {
      if (gen != connGen_) return;
      wifi_ = "failed";
      err_ = "network not found";
      wifiEvent();
    });
  }
  void setScalePresent(bool on) {
    if (scalePresent == on) return;
    scalePresent = on;
    if (!on) dropScale();
  }

  void request(const std::string &op, const std::string &argsJson, Done done) {
    rt::Json a;
    rt::Json::parse(argsJson.data(), argsJson.size(), a);
    if (op == "wifi.power") {
      wifiOn_ = boolArg(a, "on", true);
      wifi_ = wifiOn_ ? "idle" : "off";
      ssid_.clear();
      later(80, [this, done] {
        done("", "");
        wifiEvent();
      });
    } else if (op == "wifi.scan") {
      if (!wifiOn_) return done("Wi-Fi is off", "");
      if (!wifiReach) return later(1500, [done] { done("", "[]"); });
      later(1500, [done] {
        done("", "[{\"ssid\":\"Home-WiFi\",\"rssi\":-48,\"secure\":true,\"channel\":6},"
                 "{\"ssid\":\"TopuTofu\",\"rssi\":-57,\"secure\":true,\"channel\":1},"
                 "{\"ssid\":\"IoT-Lab\",\"rssi\":-66,\"secure\":true,\"channel\":11},"
                 "{\"ssid\":\"Cafe Free WiFi\",\"rssi\":-71,\"secure\":false,\"channel\":3},"
                 "{\"ssid\":\"Neighbor_5G\",\"rssi\":-83,\"secure\":true,\"channel\":44}]");
      });
    } else if (op == "wifi.connect") {
      if (!wifiOn_) return done("Wi-Fi is off", "");
      std::string ssid = a.strOr("ssid", ""), pass = a.strOr("pass", "");
      ssid_ = ssid;
      wifi_ = "connecting";
      err_.clear();
      wifiEvent();
      done("", "");
      int gen = ++connGen_;
      bool known = wifiReach && (ssid == "Home-WiFi" || ssid == "TopuTofu" || ssid == "IoT-Lab" || ssid == "Cafe Free WiFi");
      bool ok = known && ((ssid == "Home-WiFi" && pass == "home1234") || (ssid == "TopuTofu" && pass == "tofu1234") ||
                          (ssid == "IoT-Lab" && pass == "iotlab99") || ssid == "Cafe Free WiFi");
      later(ok ? 1200 : 2500, [this, gen, ok, known] {
        if (gen != connGen_) return;
        wifi_ = ok ? "connected" : "failed";
        err_ = ok ? "" : known ? "wrong password" : "network not found";
        wifiEvent();
      });
    } else if (op == "wifi.disconnect") {
      ++connGen_;
      wifi_ = wifiOn_ ? "idle" : "off";
      ssid_.clear();
      done("", "");
      wifiEvent();
    } else if (op == "wifi.state") {
      done("", wifiJson());
    } else if (op == "bt.power") {
      btOn_ = boolArg(a, "on", true);
      if (!btOn_) conns_.clear();
      later(300, [this, done] {
        done("", "");
        btState();
      });
    } else if (op == "bt.scan") {
      if (!btOn_) return done("Bluetooth is off", "");
      scanning_ = true;
      btState();
      int ms = (int)a.numOr("ms", 8000), gen = ++scanGen_;
      for (size_t i = 0; i < kDevices().size(); i++)
        later(250 + 450 * i, [this, i, gen] {
          if (gen == scanGen_ && scanning_ && (i || scalePresent))
            event("bt", "{\"type\":\"found\",\"device\":" + devJson(i) + "}");
        });
      later(ms, [this, done, gen] {
        if (gen != scanGen_) return done("", "[]");
        scanning_ = false;
        std::string out = "[";
        for (size_t i = 0; i < kDevices().size(); i++)
          if (i || scalePresent) out += (out.size() > 1 ? "," : "") + devJson(i);
        done("", out + "]");
        btState();
      });
    } else if (op == "bt.stop") {
      ++scanGen_;
      scanning_ = false;
      done("", "");
      btState();
    } else if (op == "bt.connect") {
      std::string id = a.strOr("id", "");
      int i = find(id);
      if (i < 0 || (i == 0 && !scalePresent)) return later(4000, [done] { done("device not found", ""); });
      later(700, [this, i, id, done] {
        if (!connected(id)) conns_.push_back(id);
        done("", "{\"id\":\"" + id + "\",\"name\":" + rt::Json::quote(kDevices()[i].name) +
                     ",\"type\":0,\"mtu\":247,\"services\":" + kDevices()[i].services + "}");
        btState();
      });
    } else if (op == "bt.disconnect") {
      std::string id = a.strOr("id", "");
      bool was = connected(id);
      drop(id);
      done("", "");
      if (was) {
        event("bt", "{\"type\":\"disconnected\",\"id\":\"" + id + "\",\"reason\":\"local\"}");
        btState();
      }
    } else if (op == "bt.notify") {
      std::string id = a.strOr("id", "");
      if (!connected(id)) return done("not connected", "");
      if (find(id) == 0) {
        streaming_ = boolArg(a, "on", true);
        if (streaming_) tick();
      }
      done("", "");
    } else if (op == "bt.write") {
      std::string id = a.strOr("id", "");
      if (!connected(id)) return done("not connected", "");
      if (a.strOr("data", "").rfind("54415245", 0) == 0) {  // "TARE"
        scaleKg = 0;
        if (onTare) onTare();
      }
      done("", "");
    } else if (op == "bt.read") {
      if (!connected(a.strOr("id", ""))) return done("not connected", "");
      done("", "{\"data\":\"\"}");
    } else if (op == "bt.state") {
      done("", btJson());
    } else {
      done("unknown radio operation " + op, "");
    }
  }

  // run due timers (engine thread)
  void poll() {
    uint64_t now = nowUs();
    for (size_t i = 0; i < timers_.size();) {
      if (timers_[i].first <= now) {
        auto f = std::move(timers_[i].second);
        timers_.erase(timers_.begin() + i);
        f();
        i = 0;
        now = nowUs();
      } else {
        i++;
      }
    }
  }
  bool busy() const { return !timers_.empty(); }
  // a dropped link (script command "scaleoff")
  void dropScale() {
    std::string id = kDevices()[0].id;
    if (!connected(id)) return;
    drop(id);
    event("bt", "{\"type\":\"disconnected\",\"id\":\"" + id + "\",\"reason\":\"timeout\"}");
    btState();
  }

 private:
  struct Dev {
    std::string id, name;
    int rssi;
    std::string services;
  };
  static const std::vector<Dev> &kDevices() {
    static const std::vector<Dev> d = {
        {"c8:2e:18:a4:01:5d", "BLE-Scale-01", -55,
         "[{\"uuid\":\"ffe0\",\"chars\":[{\"uuid\":\"ffe1\",\"props\":[\"read\",\"writeNoResponse\",\"notify\"]}]}]"},
        {"e4:a1:22:07:9b:30", "Mi Smart Band 7", -68,
         "[{\"uuid\":\"180f\",\"chars\":[{\"uuid\":\"2a19\",\"props\":[\"read\",\"notify\"]}]}]"},
        {"f0:13:c3:55:2a:11", "JBL Flip 6", -79, "[]"},
        {"d3:44:9e:10:6c:02", "", -88, "[]"},
    };
    return d;
  }
  static bool boolArg(const rt::Json &a, const char *k, bool d) {
    const rt::Json *v = a.get(k);
    return v && v->type == rt::Json::Bool ? v->b : d;
  }
  std::string devJson(size_t i) const {
    const Dev &d = kDevices()[i];
    return "{\"id\":\"" + d.id + "\",\"name\":" + rt::Json::quote(d.name) + ",\"rssi\":" + std::to_string(d.rssi) +
           ",\"type\":0}";
  }
  int find(const std::string &id) const {
    for (size_t i = 0; i < kDevices().size(); i++)
      if (kDevices()[i].id == id) return (int)i;
    return -1;
  }
  bool connected(const std::string &id) const {
    for (auto &c : conns_)
      if (c == id) return true;
    return false;
  }
  void drop(const std::string &id) {
    for (size_t i = 0; i < conns_.size(); i++)
      if (conns_[i] == id) conns_.erase(conns_.begin() + i--);
    if (id == kDevices()[0].id) streaming_ = false;
  }
  void later(int ms, std::function<void()> f) { timers_.push_back({nowUs() + (uint64_t)ms * 1000, std::move(f)}); }
  std::string wifiJson() const {
    bool up = wifi_ == "connected";
    int rssi = ssid_ == "Home-WiFi" ? -48 : ssid_ == "TopuTofu" ? -57 : ssid_ == "IoT-Lab" ? -66 : -71;
    return "{\"power\":" + std::string(wifiOn_ ? "true" : "false") + ",\"state\":\"" + wifi_ + "\",\"ssid\":" +
           rt::Json::quote(ssid_) + ",\"ip\":\"" + (up ? "192.168.1.57" : "") + "\",\"rssi\":" +
           std::to_string(up ? rssi : 0) + ",\"error\":" + rt::Json::quote(err_) + "}";
  }
  void wifiEvent() { event("wifi", wifiJson()); }
  std::string btJson() const {
    std::string c = "[";
    for (auto &id : conns_) {
      int i = find(id);
      c += (c.size() > 1 ? "," : "") + std::string("{\"id\":\"") + id + "\",\"name\":" +
           rt::Json::quote(i >= 0 ? kDevices()[i].name : "") + "}";
    }
    return "{\"type\":\"state\",\"power\":" + std::string(btOn_ ? "true" : "false") +
           ",\"scanning\":" + (scanning_ ? "true" : "false") + ",\"connected\":" + c + "]}";
  }
  void btState() { event("bt", btJson()); }
  // the scale: 4 frames a second, settling on scaleKg
  void tick() {
    if (!streaming_) return;
    bool stable;
    double noise = 0;
    if (scaleExternal) {
      shown_ = scaleShownKg;
      stable = scaleStable;
    } else {
      shown_ += (scaleKg - shown_) * 0.45;
      if (std::fabs(scaleKg - shown_) < 0.004) shown_ = scaleKg;
      stable = std::fabs(scaleKg - shown_) < 0.01;
      noise = stable || scaleKg == 0 ? 0 : 0.01 * std::sin((double)nowUs() / 3e5);
    }
    char frame[48];
    if (scaleOver) snprintf(frame, sizeof frame, "OL,GS,+9999999kg\r\n");
    else
      snprintf(frame, sizeof frame, "%s,GS,%+08.2fkg\r\n", stable ? "ST" : "US", shown_ + noise);
    std::string hex;
    for (const char *p = frame; *p; p++) {
      char b[3];
      snprintf(b, sizeof b, "%02x", (unsigned char)*p);
      hex += b;
    }
    event("bt", "{\"type\":\"notify\",\"id\":\"" + kDevices()[0].id + "\",\"service\":\"ffe0\",\"char\":\"ffe1\",\"data\":\"" +
                    hex + "\"}");
    later(scaleIntervalMs, [this] { tick(); });
  }

  std::vector<std::pair<uint64_t, std::function<void()>>> timers_;
  bool wifiOn_ = false, btOn_ = false, scanning_ = false, streaming_ = false;
  std::string wifi_ = "off", ssid_, err_;
  int connGen_ = 0, scanGen_ = 0;
  std::vector<std::string> conns_;
  double shown_ = 0;
};
