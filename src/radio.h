#pragma once
// The board's radios behind Platform::radio (lib/rt/engine.h): a WiFi station (Arduino WiFi)
// and a Bluetooth LE central (NimBLE host API: scan, connect, GATT discovery, read, write,
// notifications). Requests come from the engine thread; answers and events are handed to
// `post`, which runs them on the engine thread (the platform's pollAsync queue).
//
// Bluetooth is off until something needs it: powering it on brings up the controller and the
// NimBLE host task (internal RAM), powering it off gives that back.

#include <functional>
#include <string>

namespace radio {

using Done = std::function<void(const std::string &err, const std::string &json)>;

// post(fn): run fn on the engine thread soon. event(name, json): an unsolicited event, called
// on the engine thread (through post).
void begin(std::function<void(std::function<void()>)> post,
           std::function<void(const std::string &, const std::string &)> event);
void request(const std::string &op, const std::string &argsJson, Done done);
void tick();  // engine thread: timeouts, signal strength refresh
std::string wifiStateJson();

}  // namespace radio
