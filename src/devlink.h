#pragma once
// Development link over the UART (CH343 USB serial): framed binary messages interleaved with
// plain-text log lines.
//
//   frame: 0xA5 0x5A type:u8 len:u32le payload[len] crc32(payload):u32le
//
// host -> device: EVAL (utf-8 js)  SHOT  STATS  PING  OPEN (app id)  WIFI (ssid \0 pass)  RESET
//                 TOUCH (u8 phase, u16 x, u16 y)
//                 FS_WRITE (u8 flags 1=first 2=last, u16 pathLen, path, u32 offset, data)
//                 FS_DELETE / FS_MKDIR / FS_LIST (card path)  FS_FORMAT  OS_RELOAD
//                 APPS_CHANGED (app id: installed or removed)
// device -> host: ACK (u8 type, u8 ok, text)  SHOT (u16 w, u16 h, RLE565)  TEXT (utf-8)

#include <Arduino.h>

#include <functional>
#include <string>
#include <vector>

class DevLink {
 public:
  enum Type : uint8_t {
    T_FILE = 1, T_BUNDLE = 2, T_COMMIT = 3, T_EVAL = 4, T_SHOT = 5, T_STATS = 6, T_PING = 7, T_OPEN = 8,
    T_WIFI = 9, T_RESET = 10, T_TOUCH = 11,
    // OS card access: write (chunked), delete (recursive), mkdir, list (JSON), format, reload the system
    T_FS_WRITE = 12, T_FS_DELETE = 13, T_FS_MKDIR = 14, T_FS_LIST = 15, T_FS_FORMAT = 16, T_OS_RELOAD = 17,
    T_APPS_CHANGED = 18,
    T_KIOSK_OFF = 19,  // kiosk mode off without its password (whoever has the board on USB)
    R_ACK = 0x81, R_SHOT = 0x82, R_TEXT = 0x83,
  };
  using Handler = std::function<void(uint8_t type, const uint8_t *data, size_t len)>;

  void begin(uint32_t baud, Handler h);
  void poll();  // read pending bytes, dispatch complete frames (main task)
  void send(uint8_t type, const uint8_t *data, size_t len);
  // A frame written in pieces (its length known up front): begin, write..., end.
  void begin(uint8_t type, size_t len);
  void write(const uint8_t *data, size_t len);
  void end();
  void ack(uint8_t type, bool ok, const std::string &text);
  void log(const char *line);  // thread-safe log line

 private:
  Handler handler_;
  SemaphoreHandle_t txLock_ = nullptr;
  uint32_t streamCrc_ = 0;
  // receive state machine
  enum State { MAGIC1, MAGIC2, TYPE, LEN, PAYLOAD, CRC } state_ = MAGIC1;
  uint8_t type_ = 0;
  uint32_t len_ = 0, got_ = 0, crc_ = 0;
  int lenBytes_ = 0, crcBytes_ = 0;
  uint32_t lastByteMs_ = 0;
  uint8_t *buf_ = nullptr;
  size_t cap_ = 0;
  std::string pendingText_;
};
