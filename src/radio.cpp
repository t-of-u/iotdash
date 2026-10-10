#include "radio.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_wifi.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "rt_base.h"

// Keep the Bluetooth LE controller's memory: by default the Arduino core gives it back to the
// heap at start-up when no Arduino BLE library is linked, and BLE could never start.
extern "C" bool bleInUse(void) { return true; }

namespace radio {

static std::function<void(std::function<void()>)> gPost;
static std::function<void(const std::string &, const std::string &)> gEvent;
static std::recursive_mutex mu;

static std::string q(const std::string &s) { return rt::Json::quote(s); }

// Internal RAM a radio needs to come up (measured on the board). Starting one without it
// fails half-way and leaks (WiFi), or crashes inside the stack (NimBLE): refuse instead.
static constexpr size_t kWifiNeeds = 34 * 1024, kBleNeeds = 30 * 1024;
static std::string memoryShort(size_t need) {
  size_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (freeInt >= need) return "";
  return "not enough memory (" + std::to_string(freeInt / 1024) + " KB free, " + std::to_string(need / 1024) + " KB needed)";
}
static void reply(Done done, std::string err, std::string json) {
  gPost([done, err, json]() { done(err, json); });
}
static void emit(const std::string &name, std::string json) {
  gPost([name, json]() {
    if (gEvent) gEvent(name, json);
  });
}
static bool boolArg(const rt::Json &a, const char *k, bool d) {
  const rt::Json *v = a.get(k);
  return v && v->type == rt::Json::Bool ? v->b : d;
}

// ============================================================ WiFi

enum WifiState { W_OFF, W_IDLE, W_CONNECTING, W_CONNECTED, W_FAILED };
static const char *kWifiNames[] = {"off", "idle", "connecting", "connected", "failed"};
static WifiState wState = W_OFF;
static std::string wSsid, wPass, wError;
static uint32_t wConnectStart = 0;
static int wFailures = 0;  // failed handshakes (or "not found") of the current attempt
static int wLastRssi = 0;
static uint32_t wLastRssiCheck = 0;
static std::vector<Done> wScanWaiters;
static bool wHandlersSet = false;

static std::string macText() {  // from eFuse: right even before the driver starts
  uint8_t m[6] = {};
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  char b[18];
  snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  return b;
}

std::string wifiStateJson() {
  std::lock_guard<std::recursive_mutex> lk(mu);
  bool up = wState == W_CONNECTED;
  return "{\"power\":" + std::string(wState == W_OFF ? "false" : "true") + ",\"state\":\"" + kWifiNames[wState] +
         "\",\"ssid\":" + q(wSsid) + ",\"ip\":\"" + (up ? WiFi.localIP().toString().c_str() : "") +
         "\",\"rssi\":" + std::to_string(up ? wLastRssi : 0) + ",\"mac\":\"" + macText() +
         "\",\"error\":" + q(wError) + "}";
}
static void wifiEvent() { emit("wifi", wifiStateJson()); }
static void setWifi(WifiState s, const std::string &err = "") {
  {
    std::lock_guard<std::recursive_mutex> lk(mu);
    wState = s;
    wError = err;
  }
  wifiEvent();
}

static void scanDone() {
  int n = WiFi.scanComplete();
  std::vector<std::pair<int, std::string>> aps;  // rssi, json
  std::map<std::string, int> best;
  for (int i = 0; i < n; i++) {
    std::string ssid = WiFi.SSID(i).c_str();
    if (ssid.empty()) continue;  // hidden
    int rssi = WiFi.RSSI(i);
    auto it = best.find(ssid);
    if (it != best.end() && it->second >= rssi) continue;
    best[ssid] = rssi;
  }
  for (int i = 0; i < n; i++) {
    std::string ssid = WiFi.SSID(i).c_str();
    if (ssid.empty() || best[ssid] != WiFi.RSSI(i)) continue;
    best[ssid] = 1;  // listed once
    aps.push_back({WiFi.RSSI(i), "{\"ssid\":" + q(ssid) + ",\"rssi\":" + std::to_string(WiFi.RSSI(i)) +
                                     ",\"secure\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") +
                                     ",\"channel\":" + std::to_string(WiFi.channel(i)) + "}"});
  }
  WiFi.scanDelete();
  std::sort(aps.begin(), aps.end(), [](auto &a, auto &b) { return a.first > b.first; });
  std::string out = "[";
  for (auto &a : aps) out += (out.size() > 1 ? "," : "") + a.second;
  out += "]";
  std::vector<Done> waiters;
  {
    std::lock_guard<std::recursive_mutex> lk(mu);
    waiters.swap(wScanWaiters);
  }
  for (auto &w : waiters) reply(w, n < 0 ? "scan failed" : "", n < 0 ? "" : out);
}

static void onWifi(arduino_event_id_t ev, arduino_event_info_t info) {
  switch (ev) {
    case ARDUINO_EVENT_WIFI_SCAN_DONE:
      scanDone();
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
      wLastRssi = WiFi.RSSI();
      // wall-clock time from the network (records are stamped with it), Thai time zone
      static bool sntp = false;
      if (!sntp) {
        sntp = true;
        configTzTime("ICT-7", "pool.ntp.org", "time.google.com", "time.cloudflare.com");
      }
      {
        std::lock_guard<std::recursive_mutex> lk(mu);
        wFailures = 0;
      }
      setWifi(W_CONNECTED);
      break;
    }
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      int reason = info.wifi_sta_disconnected.reason;
      WifiState s;
      std::string err;
      {
        std::lock_guard<std::recursive_mutex> lk(mu);
        s = wState;
      }
      if (s == W_OFF || s == W_IDLE || s == W_FAILED) break;
      bool badPass = reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                     reason == WIFI_REASON_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_MIC_FAILURE ||
                     reason == WIFI_REASON_802_1X_AUTH_FAILED || reason == WIFI_REASON_AUTH_EXPIRE;
      if (s == W_CONNECTING && (badPass || reason == WIFI_REASON_NO_AP_FOUND)) {
        // One failed handshake is no proof of a wrong password: right after a boot, or with
        // Bluetooth busy, a right one times out too (the saved network then "had the wrong
        // password" until it was typed again). Three in a row are; until then, again.
        std::string ssid, pass;
        int fails;
        {
          std::lock_guard<std::recursive_mutex> lk(mu);
          fails = ++wFailures;
          ssid = wSsid;
          pass = wPass;
        }
        if (fails < 3 && !ssid.empty()) {
          WiFi.disconnect(false, false);
          WiFi.begin(ssid.c_str(), pass.empty() ? nullptr : pass.c_str());
          break;
        }
        WiFi.disconnect(false, false);  // no more retries with a wrong password
        setWifi(W_FAILED, badPass ? "wrong password" : "network not found");
      } else if (s == W_CONNECTED) {
        setWifi(W_CONNECTING);  // dropped: the driver reconnects by itself
        std::lock_guard<std::recursive_mutex> lk(mu);
        wConnectStart = millis();
      }
      break;
    }
    default:
      break;
  }
}

static void wifiPower(bool on, Done done) {
  if (!wHandlersSet) {
    WiFi.onEvent(onWifi);
    wHandlersSet = true;
  }
  if (on) {
    if (wState == W_OFF) {
      std::string m = memoryShort(kWifiNeeds);
      if (!m.empty()) return done("Wi-Fi does not start: " + m, "");
      WiFi.persistent(false);  // the OS keeps the networks (registry), not the driver
      WiFi.setAutoReconnect(true);
      if (!WiFi.mode(WIFI_STA)) return done("Wi-Fi does not start (out of memory?)", "");
      WiFi.setHostname("iotdash");
      setWifi(W_IDLE);
    }
  } else if (wState != W_OFF) {
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    {
      std::lock_guard<std::recursive_mutex> lk(mu);
      wSsid.clear();
    }
    setWifi(W_OFF);
  }
  done("", "");
}

static void wifiRequest(const std::string &op, const rt::Json &a, Done done) {
  if (op == "wifi.power") return wifiPower(boolArg(a, "on", true), done);
  if (op == "wifi.state") return done("", wifiStateJson());
  if (wState == W_OFF) return done("Wi-Fi is off", "");
  if (op == "wifi.scan") {
    bool start;
    {
      std::lock_guard<std::recursive_mutex> lk(mu);
      start = wScanWaiters.empty();
      wScanWaiters.push_back(done);
    }
    if (start && WiFi.scanNetworks(true, false) == WIFI_SCAN_FAILED) {
      std::vector<Done> waiters;
      {
        std::lock_guard<std::recursive_mutex> lk(mu);
        waiters.swap(wScanWaiters);
      }
      for (auto &w : waiters) w("Wi-Fi is busy, try again", "");
    }
    return;
  }
  if (op == "wifi.connect") {
    std::string ssid = a.strOr("ssid", ""), pass = a.strOr("pass", "");
    WiFi.disconnect(false, false);
    {
      std::lock_guard<std::recursive_mutex> lk(mu);
      wSsid = ssid;
      wPass = pass;
      wFailures = 0;
      wConnectStart = millis();
    }
    setWifi(W_CONNECTING);
    WiFi.begin(ssid.c_str(), pass.empty() ? nullptr : pass.c_str());
    return done("", "");
  }
  if (op == "wifi.disconnect") {
    {
      std::lock_guard<std::recursive_mutex> lk(mu);
      wState = W_IDLE;  // before the driver's disconnect event: not a drop to reconnect
      wSsid.clear();
      wPass.clear();
    }
    WiFi.disconnect(false, false);
    setWifi(W_IDLE);
    return done("", "");
  }
  done("unknown operation " + op, "");
}

// ============================================================ Bluetooth LE central

struct Chr {
  std::string uuid;
  uint16_t def = 0, val = 0, end = 0, cccd = 0;
  uint8_t props = 0;
};
struct Svc {
  std::string uuid;
  uint16_t start = 0, end = 0;
  std::vector<Chr> chrs;
};
struct Conn {
  std::string id, name;
  uint8_t type = 0;
  uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
  uint16_t mtu = 23;
  bool ready = false;
  std::vector<Svc> svcs;
  size_t disc = 0;  // service whose characteristics are being discovered
  Done connected;   // the pending bt.connect
};
struct Seen {
  std::string name;
  int rssi = 0;
  uint8_t type = 0;
  std::vector<std::string> services;
};

static bool bOn = false, bSynced = false, bScanning = false;
static uint8_t bOwnAddr = 0;
static SemaphoreHandle_t bSync = nullptr;
static std::map<std::string, Seen> bSeen;  // by id, this scan
static Done bScanDone;
static std::vector<Conn *> bConns;

static std::string addrStr(const ble_addr_t &a) {
  char b[18];
  snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", a.val[5], a.val[4], a.val[3], a.val[2], a.val[1], a.val[0]);
  return b;
}
static bool parseAddr(const std::string &s, uint8_t type, ble_addr_t &out) {
  unsigned v[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[5], &v[4], &v[3], &v[2], &v[1], &v[0]) != 6) return false;
  out.type = type;
  for (int i = 0; i < 6; i++) out.val[i] = (uint8_t)v[i];
  return true;
}
// "ffe0" for 16-bit (also 128-bit ones on the Bluetooth base), else the full form
static std::string uuidStr(const ble_uuid_t *u) {
  char b[40];
  if (u->type == BLE_UUID_TYPE_16) {
    snprintf(b, sizeof b, "%04x", BLE_UUID16(u)->value);
    return b;
  }
  if (u->type == BLE_UUID_TYPE_32) {
    snprintf(b, sizeof b, "%08lx", (unsigned long)BLE_UUID32(u)->value);
    return b;
  }
  const uint8_t *v = BLE_UUID128(u)->value;  // little endian
  snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", v[15], v[14], v[13],
           v[12], v[11], v[10], v[9], v[8], v[7], v[6], v[5], v[4], v[3], v[2], v[1], v[0]);
  std::string s = b;
  static const char *kBase = "-0000-1000-8000-00805f9b34fb";
  if (s.compare(8, std::string::npos, kBase) == 0 && s.compare(0, 4, "0000") == 0) return s.substr(4, 4);
  return s;
}
static std::string normUuid(std::string s) {
  for (auto &c : s) c = (char)tolower((unsigned char)c);
  if (s.rfind("0x", 0) == 0) s = s.substr(2);
  if (s.size() == 36 && s.compare(8, std::string::npos, "-0000-1000-8000-00805f9b34fb") == 0 && s.compare(0, 4, "0000") == 0)
    return s.substr(4, 4);
  return s;
}
static std::string propsJson(uint8_t p) {
  std::string o = "[";
  auto add = [&](const char *n) { o += (o.size() > 1 ? ",\"" : "\"") + std::string(n) + "\""; };
  if (p & BLE_GATT_CHR_PROP_READ) add("read");
  if (p & BLE_GATT_CHR_PROP_WRITE) add("write");
  if (p & BLE_GATT_CHR_PROP_WRITE_NO_RSP) add("writeNoResponse");
  if (p & BLE_GATT_CHR_PROP_NOTIFY) add("notify");
  if (p & BLE_GATT_CHR_PROP_INDICATE) add("indicate");
  return o + "]";
}
static std::string hex(const uint8_t *p, size_t n) {
  static const char *d = "0123456789abcdef";
  std::string s(n * 2, '0');
  for (size_t i = 0; i < n; i++) {
    s[2 * i] = d[p[i] >> 4];
    s[2 * i + 1] = d[p[i] & 15];
  }
  return s;
}
static std::string unhex(const std::string &h) {
  std::string out;
  for (size_t i = 0; i + 1 < h.size(); i += 2) out += (char)strtol(h.substr(i, 2).c_str(), nullptr, 16);
  return out;
}

static std::string btStateJson() {
  std::lock_guard<std::recursive_mutex> lk(mu);
  std::string c = "[";
  for (auto *k : bConns)
    if (k->ready) c += (c.size() > 1 ? "," : "") + std::string("{\"id\":\"") + k->id + "\",\"name\":" + q(k->name) + "}";
  return "{\"type\":\"state\",\"power\":" + std::string(bOn ? "true" : "false") + ",\"scanning\":" +
         (bScanning ? "true" : "false") + ",\"connected\":" + c + "]}";
}
static void btEvent() { emit("bt", btStateJson()); }

static Conn *connById(const std::string &id) {
  for (auto *k : bConns)
    if (k->id == id) return k;
  return nullptr;
}
static Conn *connByHandle(uint16_t h) {
  for (auto *k : bConns)
    if (k->handle == h) return k;
  return nullptr;
}
static void dropConn(Conn *k) {
  bConns.erase(std::remove(bConns.begin(), bConns.end(), k), bConns.end());
  delete k;
}
static Chr *findChr(Conn *k, const std::string &svc, const std::string &chr) {
  std::string s = normUuid(svc), c = normUuid(chr);
  for (auto &sv : k->svcs)
    if (sv.uuid == s)
      for (auto &ch : sv.chrs)
        if (ch.uuid == c) return &ch;
  return nullptr;
}
static std::string deviceJson(const std::string &id, const Seen &s) {
  std::string sv = "[";
  for (auto &u : s.services) sv += (sv.size() > 1 ? ",\"" : "\"") + u + "\"";
  return "{\"id\":\"" + id + "\",\"name\":" + q(s.name) + ",\"rssi\":" + std::to_string(s.rssi) +
         ",\"type\":" + std::to_string(s.type) + ",\"services\":" + sv + "]}";
}

static void connectFinished(Conn *k, const std::string &err);

// ---- GATT discovery: services, then the characteristics of each in turn
static int onChr(uint16_t conn, const ble_gatt_error *e, const ble_gatt_chr *chr, void *) {
  std::lock_guard<std::recursive_mutex> lk(mu);
  Conn *k = connByHandle(conn);
  if (!k) return 0;
  if (e->status == 0 && chr) {
    Svc &sv = k->svcs[k->disc];
    if (!sv.chrs.empty()) sv.chrs.back().end = chr->def_handle - 1;
    Chr c;
    c.uuid = uuidStr(&chr->uuid.u);
    c.def = chr->def_handle;
    c.val = chr->val_handle;
    c.end = sv.end;
    c.props = chr->properties;
    sv.chrs.push_back(c);
    return 0;
  }
  if (e->status != BLE_HS_EDONE) {
    connectFinished(k, "discovery failed (" + std::to_string(e->status) + ")");
    return 0;
  }
  // next service
  if (++k->disc < k->svcs.size()) {
    Svc &n = k->svcs[k->disc];
    int rc = ble_gattc_disc_all_chrs(conn, n.start, n.end, onChr, nullptr);
    if (rc) connectFinished(k, "discovery failed (" + std::to_string(rc) + ")");
    return 0;
  }
  connectFinished(k, "");
  return 0;
}
static int onSvc(uint16_t conn, const ble_gatt_error *e, const ble_gatt_svc *svc, void *) {
  std::lock_guard<std::recursive_mutex> lk(mu);
  Conn *k = connByHandle(conn);
  if (!k) return 0;
  if (e->status == 0 && svc) {
    Svc s;
    s.uuid = uuidStr(&svc->uuid.u);
    s.start = svc->start_handle;
    s.end = svc->end_handle;
    k->svcs.push_back(s);
    return 0;
  }
  if (e->status != BLE_HS_EDONE) {
    connectFinished(k, "discovery failed (" + std::to_string(e->status) + ")");
    return 0;
  }
  k->disc = 0;
  if (k->svcs.empty()) {
    connectFinished(k, "");
    return 0;
  }
  int rc = ble_gattc_disc_all_chrs(conn, k->svcs[0].start, k->svcs[0].end, onChr, nullptr);
  if (rc) connectFinished(k, "discovery failed (" + std::to_string(rc) + ")");
  return 0;
}
static int onMtu(uint16_t conn, const ble_gatt_error *e, uint16_t mtu, void *) {
  std::lock_guard<std::recursive_mutex> lk(mu);
  Conn *k = connByHandle(conn);
  if (!k) return 0;
  if (e->status == 0) k->mtu = mtu;
  int rc = ble_gattc_disc_all_svcs(conn, onSvc, nullptr);
  if (rc) connectFinished(k, "discovery failed (" + std::to_string(rc) + ")");
  return 0;
}

static void connectFinished(Conn *k, const std::string &err) {
  Done d = k->connected;
  k->connected = nullptr;
  if (!err.empty()) {
    if (k->handle != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(k->handle, BLE_ERR_REM_USER_CONN_TERM);
    else dropConn(k);
    if (d) reply(d, err, "");
    return;
  }
  k->ready = true;
  std::string sv = "[";
  for (auto &s : k->svcs) {
    std::string cs = "[";
    for (auto &c : s.chrs)
      cs += (cs.size() > 1 ? "," : "") + std::string("{\"uuid\":\"") + c.uuid + "\",\"props\":" + propsJson(c.props) + "}";
    sv += (sv.size() > 1 ? "," : "") + std::string("{\"uuid\":\"") + s.uuid + "\",\"chars\":" + cs + "]}";
  }
  if (d)
    reply(d, "", "{\"id\":\"" + k->id + "\",\"name\":" + q(k->name) + ",\"type\":" + std::to_string(k->type) +
                     ",\"mtu\":" + std::to_string(k->mtu) + ",\"services\":" + sv + "]}");
  btEvent();
}

static int onGap(ble_gap_event *ev, void *) {
  std::lock_guard<std::recursive_mutex> lk(mu);
  switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
      std::string id = addrStr(ev->disc.addr);
      bool fresh = !bSeen.count(id);
      Seen &s = bSeen[id];
      s.rssi = ev->disc.rssi;
      s.type = ev->disc.addr.type;
      ble_hs_adv_fields f;
      bool named = false;
      if (ble_hs_adv_parse_fields(&f, ev->disc.data, ev->disc.length_data) == 0) {
        if (f.name && f.name_len && s.name.empty()) {
          s.name.assign((const char *)f.name, f.name_len);
          while (!s.name.empty() && (s.name.back() == ' ' || s.name.back() == '\0')) s.name.pop_back();
          named = !s.name.empty();
        }
        auto addSvc = [&](const std::string &u) {
          if (std::find(s.services.begin(), s.services.end(), u) == s.services.end()) s.services.push_back(u);
        };
        for (int i = 0; i < f.num_uuids16; i++) addSvc(uuidStr(&f.uuids16[i].u));
        for (int i = 0; i < f.num_uuids128; i++) addSvc(uuidStr(&f.uuids128[i].u));
      }
      if (fresh || named) emit("bt", "{\"type\":\"found\",\"device\":" + deviceJson(id, s) + "}");
      return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE: {
      bScanning = false;
      std::vector<std::pair<int, std::string>> list;
      for (auto &kv : bSeen) list.push_back({kv.second.rssi, deviceJson(kv.first, kv.second)});
      std::sort(list.begin(), list.end(), [](auto &a, auto &b) { return a.first > b.first; });
      std::string out = "[";
      for (auto &d : list) out += (out.size() > 1 ? "," : "") + d.second;
      if (bScanDone) reply(bScanDone, "", out + "]");
      bScanDone = nullptr;
      btEvent();
      return 0;
    }
    case BLE_GAP_EVENT_CONNECT: {
      Conn *k = nullptr;
      for (auto *c : bConns)
        if (c->handle == BLE_HS_CONN_HANDLE_NONE && c->connected) k = c;
      if (!k) return 0;
      if (ev->connect.status != 0) {
        connectFinished(k, "cannot connect (" + std::to_string(ev->connect.status) + ")");
        return 0;
      }
      k->handle = ev->connect.conn_handle;
      int rc = ble_gattc_exchange_mtu(k->handle, onMtu, nullptr);
      if (rc) rc = ble_gattc_disc_all_svcs(k->handle, onSvc, nullptr);
      if (rc) connectFinished(k, "discovery failed (" + std::to_string(rc) + ")");
      return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
      Conn *k = connByHandle(ev->disconnect.conn.conn_handle);
      if (!k) return 0;
      if (k->connected) {
        Done d = k->connected;
        k->connected = nullptr;
        reply(d, "disconnected while connecting (" + std::to_string(ev->disconnect.reason) + ")", "");
      }
      bool wasReady = k->ready;
      std::string id = k->id;
      dropConn(k);
      if (wasReady)
        emit("bt", "{\"type\":\"disconnected\",\"id\":\"" + id + "\",\"reason\":" + std::to_string(ev->disconnect.reason) + "}");
      btEvent();
      return 0;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
      Conn *k = connByHandle(ev->notify_rx.conn_handle);
      if (!k) return 0;
      uint16_t len = OS_MBUF_PKTLEN(ev->notify_rx.om);
      uint8_t buf[512];
      len = std::min<uint16_t>(len, sizeof buf);
      os_mbuf_copydata(ev->notify_rx.om, 0, len, buf);
      for (auto &s : k->svcs)
        for (auto &c : s.chrs)
          if (c.val == ev->notify_rx.attr_handle) {
            emit("bt", "{\"type\":\"notify\",\"id\":\"" + k->id + "\",\"service\":\"" + s.uuid + "\",\"char\":\"" +
                           c.uuid + "\",\"data\":\"" + hex(buf, len) + "\"}");
            return 0;
          }
      return 0;
    }
    case BLE_GAP_EVENT_MTU: {
      if (Conn *k = connByHandle(ev->mtu.conn_handle)) k->mtu = ev->mtu.value;
      return 0;
    }
    default:
      return 0;
  }
}

static void onSyncCb() {
  ble_hs_util_ensure_addr(0);
  ble_hs_id_infer_auto(0, &bOwnAddr);
  bSynced = true;
  if (bSync) xSemaphoreGive(bSync);
}
static void onResetCb(int reason) { bSynced = false; }
static void hostTask(void *) {
  nimble_port_run();  // returns after nimble_port_stop()
  nimble_port_freertos_deinit();
}

static void btPower(bool on, Done done) {
  if (on == bOn) return done("", "");
  if (on) {
    std::string m = memoryShort(kBleNeeds);
    if (!m.empty()) return done("Bluetooth does not start: " + m, "");
    if (!bSync) bSync = xSemaphoreCreateBinary();
    esp_err_t r = nimble_port_init();
    if (r != ESP_OK) return done(std::string("Bluetooth does not start: ") + esp_err_to_name(r), "");
    ble_hs_cfg.sync_cb = onSyncCb;
    ble_hs_cfg.reset_cb = onResetCb;
    nimble_port_freertos_init(hostTask);
    bOn = true;
    if (xSemaphoreTake(bSync, pdMS_TO_TICKS(3000)) != pdTRUE) {
      nimble_port_stop();
      nimble_port_deinit();
      bOn = false;
      return done("Bluetooth did not come up", "");
    }
  } else {
    {
      std::lock_guard<std::recursive_mutex> lk(mu);
      if (bScanning) ble_gap_disc_cancel();
      bScanning = false;
      for (auto *k : bConns) {
        if (k->connected) reply(k->connected, "Bluetooth turned off", "");
        delete k;
      }
      bConns.clear();
      if (bScanDone) reply(bScanDone, "Bluetooth turned off", "");
      bScanDone = nullptr;
    }
    nimble_port_stop();
    nimble_port_deinit();
    bOn = bSynced = false;
  }
  done("", "");
  btEvent();
}

// GATT read / write answers (arg: a heap Done)
static int onRead(uint16_t, const ble_gatt_error *e, ble_gatt_attr *attr, void *arg) {
  auto *d = (Done *)arg;
  if (e->status == 0 && attr) {
    uint16_t len = std::min<uint16_t>(OS_MBUF_PKTLEN(attr->om), 512);
    uint8_t buf[512];
    os_mbuf_copydata(attr->om, 0, len, buf);
    reply(*d, "", "{\"data\":\"" + hex(buf, len) + "\"}");
  } else {
    reply(*d, "read failed (" + std::to_string(e->status) + ")", "");
  }
  delete d;
  return 0;
}
static int onWrite(uint16_t, const ble_gatt_error *e, ble_gatt_attr *, void *arg) {
  auto *d = (Done *)arg;
  reply(*d, e->status == 0 ? "" : "write failed (" + std::to_string(e->status) + ")", "");
  delete d;
  return 0;
}
// CCCD of a characteristic: found among its descriptors, then written
struct Sub {
  uint16_t conn, val;
  bool on;
  uint8_t props;
  uint16_t cccd = 0;
  Done done;
};
static int onCccdWritten(uint16_t, const ble_gatt_error *e, ble_gatt_attr *, void *arg) {
  auto *s = (Sub *)arg;
  reply(s->done, e->status == 0 ? "" : "subscribe failed (" + std::to_string(e->status) + ")", "");
  delete s;
  return 0;
}
static void writeCccd(Sub *s) {
  uint8_t v[2] = {0, 0};
  if (s->on) v[0] = (s->props & BLE_GATT_CHR_PROP_NOTIFY) ? 1 : 2;
  int rc = ble_gattc_write_flat(s->conn, s->cccd, v, 2, onCccdWritten, s);
  if (rc) {
    reply(s->done, "subscribe failed (" + std::to_string(rc) + ")", "");
    delete s;
  }
}
static int onDsc(uint16_t conn, const ble_gatt_error *e, uint16_t, const ble_gatt_dsc *dsc, void *arg) {
  auto *s = (Sub *)arg;
  if (e->status == 0 && dsc) {
    if (ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) s->cccd = dsc->handle;
    return 0;
  }
  {
    std::lock_guard<std::recursive_mutex> lk(mu);
    if (Conn *k = connByHandle(conn))
      for (auto &sv : k->svcs)
        for (auto &c : sv.chrs)
          if (c.val == s->val) c.cccd = s->cccd ? s->cccd : (uint16_t)(s->val + 1);
  }
  if (!s->cccd) s->cccd = s->val + 1;  // the usual place
  writeCccd(s);
  return 0;
}

static void btRequest(const std::string &op, const rt::Json &a, Done done) {
  if (op == "bt.power") return btPower(boolArg(a, "on", true), done);
  if (op == "bt.state") return done("", btStateJson());
  if (!bOn || !bSynced) return done("Bluetooth is off", "");
  std::lock_guard<std::recursive_mutex> lk(mu);
  if (op == "bt.scan") {
    if (bScanning) {
      if (bScanDone) reply(bScanDone, "", "[]");
      ble_gap_disc_cancel();
    }
    bSeen.clear();
    ble_gap_disc_params p = {};
    p.passive = 0;  // scan requests: names come in the scan response
    p.filter_duplicates = 0;
    int ms = (int)a.numOr("ms", 8000);
    int rc = ble_gap_disc(bOwnAddr, std::max(1000, std::min(ms, 60000)), &p, onGap, nullptr);
    if (rc) return done("scan failed (" + std::to_string(rc) + ")", "");
    bScanning = true;
    bScanDone = done;
    btEvent();
    return;
  }
  if (op == "bt.stop") {
    if (bScanning) {
      ble_gap_disc_cancel();
      bScanning = false;
      if (bScanDone) reply(bScanDone, "", "[]");
      bScanDone = nullptr;
      btEvent();
    }
    return done("", "");
  }
  std::string id = a.strOr("id", "");
  if (op == "bt.connect") {
    if (Conn *k = connById(id)) {
      if (k->ready) {  // already: answer with what is known
        Done d = done;
        k->connected = d;
        connectFinished(k, "");
        return;
      }
      return done("already connecting", "");
    }
    uint8_t type = (uint8_t)a.numOr("type", bSeen.count(id) ? bSeen[id].type : 0);
    ble_addr_t addr;
    if (!parseAddr(id, type, addr)) return done("bad device id", "");
    if (bScanning) {  // the controller cannot scan and connect at once
      ble_gap_disc_cancel();
      bScanning = false;
      if (bScanDone) reply(bScanDone, "", "[]");
      bScanDone = nullptr;
    }
    auto *k = new Conn;
    k->id = id;
    k->type = type;
    k->name = a.strOr("name", bSeen.count(id) ? bSeen[id].name.c_str() : "");
    k->connected = done;
    bConns.push_back(k);
    int rc = ble_gap_connect(bOwnAddr, &addr, 10000, nullptr, onGap, nullptr);
    if (rc) {
      dropConn(k);
      return done(rc == BLE_HS_EALREADY || rc == BLE_HS_EBUSY ? "Bluetooth is busy, try again"
                                                               : "cannot connect (" + std::to_string(rc) + ")",
                  "");
    }
    return;
  }
  Conn *k = connById(id);
  if (op == "bt.disconnect") {
    if (!k) return done("", "");
    if (k->handle != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(k->handle, BLE_ERR_REM_USER_CONN_TERM);
    else {
      if (k->connected) reply(k->connected, "cancelled", "");
      ble_gap_conn_cancel();
      dropConn(k);
    }
    return done("", "");
  }
  if (!k || !k->ready) return done("not connected", "");
  Chr *c = findChr(k, a.strOr("service", ""), a.strOr("char", ""));
  if (!c) return done("no such characteristic", "");
  if (op == "bt.read") {
    int rc = ble_gattc_read(k->handle, c->val, onRead, new Done(done));
    if (rc) done("read failed (" + std::to_string(rc) + ")", "");
    return;
  }
  if (op == "bt.write") {
    std::string data = unhex(a.strOr("data", ""));
    bool rsp = boolArg(a, "response", true) && (c->props & BLE_GATT_CHR_PROP_WRITE);
    if (!rsp) {
      int rc = ble_gattc_write_no_rsp_flat(k->handle, c->val, data.data(), data.size());
      return done(rc ? "write failed (" + std::to_string(rc) + ")" : "", "");
    }
    int rc = ble_gattc_write_flat(k->handle, c->val, data.data(), data.size(), onWrite, new Done(done));
    if (rc) done("write failed (" + std::to_string(rc) + ")", "");
    return;
  }
  if (op == "bt.notify") {
    if (!(c->props & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)))
      return done("the characteristic does not notify", "");
    auto *s = new Sub{k->handle, c->val, boolArg(a, "on", true), c->props, c->cccd, done};
    if (s->cccd) return writeCccd(s);
    int rc = ble_gattc_disc_all_dscs(k->handle, c->val, c->end, onDsc, s);
    if (rc) {
      delete s;
      done("subscribe failed (" + std::to_string(rc) + ")", "");
    }
    return;
  }
  done("unknown operation " + op, "");
}

// ============================================================ entry points

void begin(std::function<void(std::function<void()>)> post,
           std::function<void(const std::string &, const std::string &)> event) {
  gPost = std::move(post);
  gEvent = std::move(event);
}

void request(const std::string &op, const std::string &argsJson, Done done) {
  rt::Json a;
  rt::Json::parse(argsJson.data(), argsJson.size(), a);
  if (op.rfind("wifi.", 0) == 0) return wifiRequest(op, a, done);
  if (op.rfind("bt.", 0) == 0) return btRequest(op, a, done);
  done("unknown operation " + op, "");
}

void tick() {
  uint32_t now = millis();
  WifiState s;
  uint32_t start;
  {
    std::lock_guard<std::recursive_mutex> lk(mu);
    s = wState;
    start = wConnectStart;
  }
  if (s == W_CONNECTING && now - start > 20000 && WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(false, false);
    setWifi(W_FAILED, "timed out");
  }
  // signal strength now and then while connected (a change of at least 5 dB is news)
  if (s == W_CONNECTED && now - wLastRssiCheck > 10000) {
    wLastRssiCheck = now;
    int r = WiFi.RSSI();
    if (r && std::abs(r - wLastRssi) >= 5) {
      wLastRssi = r;
      wifiEvent();
    }
  }
}

}  // namespace radio
