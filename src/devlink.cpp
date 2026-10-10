#include "devlink.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_crc.h>

#include <cstdarg>

static DevLink *gLink = nullptr;

// ESP-IDF log lines: through Serial and the TX lock like ours. Written straight to the port
// they would overtake the tail of a frame still queued in Serial's TX buffer.
static int idfLog(const char *fmt, va_list ap) {
  char b[200];
  int n = vsnprintf(b, sizeof b, fmt, ap);
  if (n <= 0) return n;
  size_t len = strnlen(b, sizeof b);
  while (len && (b[len - 1] == '\n' || b[len - 1] == '\r')) b[--len] = 0;
  if (gLink) gLink->log(b);
  return n;
}

void DevLink::begin(uint32_t baud, Handler h) {
  handler_ = h;
  txLock_ = xSemaphoreCreateMutex();
  // internal RAM (the UART interrupt fills them): the tools send at most 4 KB per frame and
  // wait for its answer
  Serial.setRxBufferSize(6 * 1024);
  Serial.setTxBufferSize(2048);
  Serial.begin(baud);
  gLink = this;
  esp_log_set_vprintf(idfLog);
}

static uint32_t crc32(const uint8_t *d, size_t n) { return esp_rom_crc32_le(0, d, n); }

void DevLink::send(uint8_t type, const uint8_t *data, size_t len) {
  uint8_t hdr[7] = {0xA5, 0x5A, type, (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24)};
  uint32_t c = crc32(data, len);
  uint8_t tail[4] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24)};
  xSemaphoreTake(txLock_, portMAX_DELAY);
  Serial.write(hdr, sizeof hdr);
  size_t off = 0;
  while (off < len) {
    size_t n = std::min<size_t>(4096, len - off);
    Serial.write(data + off, n);
    off += n;
  }
  Serial.write(tail, 4);
  xSemaphoreGive(txLock_);
}

void DevLink::begin(uint8_t type, size_t len) {
  uint8_t hdr[7] = {0xA5, 0x5A, type, (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24)};
  xSemaphoreTake(txLock_, portMAX_DELAY);  // held until end(): no log line in the middle
  Serial.write(hdr, sizeof hdr);
  streamCrc_ = 0;
}

void DevLink::write(const uint8_t *data, size_t len) {
  streamCrc_ = esp_rom_crc32_le(streamCrc_, data, len);
  Serial.write(data, len);
}

void DevLink::end() {
  uint32_t c = streamCrc_;
  uint8_t tail[4] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24)};
  Serial.write(tail, 4);
  xSemaphoreGive(txLock_);
}

void DevLink::ack(uint8_t type, bool ok, const std::string &text) {
  std::vector<uint8_t> p;
  p.push_back(type);
  p.push_back(ok ? 1 : 0);
  p.insert(p.end(), text.begin(), text.end());
  send(R_ACK, p.data(), p.size());
}

void DevLink::log(const char *line) {
  if (!txLock_) {
    Serial.println(line);
    return;
  }
  // a line logged by the task that is sending a frame would land inside it: dropped
  if (xSemaphoreGetMutexHolder(txLock_) == xTaskGetCurrentTaskHandle()) return;
  xSemaphoreTake(txLock_, portMAX_DELAY);
  Serial.println(line);
  xSemaphoreGive(txLock_);
}

void DevLink::poll() {
  int avail = Serial.available();
  uint32_t now = millis();
  // a frame that stops half-way is junk (a header cut short while the board reset, line
  // noise): the tools send each frame in one go. Without this a garbled length would swallow
  // every later frame.
  // nothing more for 500 ms in the middle of a frame: a stale fragment, resync (judged while the
  // line is idle: bytes waiting after a busy engine loop are the rest of the frame, not junk)
  if (state_ != MAGIC1 && avail == 0 && now - lastByteMs_ > 500) state_ = MAGIC1;
  if (avail > 0) lastByteMs_ = now;
  while (avail-- > 0) {
    int ch = Serial.read();
    if (ch < 0) break;
    uint8_t b = (uint8_t)ch;
    switch (state_) {
      case MAGIC1:
        if (b == 0xA5) state_ = MAGIC2;
        else if (b == '\n') {
          // plain text line (debug commands typed in a terminal)
          if (!pendingText_.empty() && handler_) handler_(R_TEXT, (const uint8_t *)pendingText_.data(), pendingText_.size());
          pendingText_.clear();
        } else if (b >= 0x20 && b < 0x7F && pendingText_.size() < 256) {
          pendingText_ += (char)b;
        }
        break;
      case MAGIC2:
        state_ = b == 0x5A ? TYPE : MAGIC1;
        break;
      case TYPE:
        type_ = b;
        len_ = 0;
        lenBytes_ = 0;
        state_ = LEN;
        break;
      case LEN:
        len_ |= (uint32_t)b << (8 * lenBytes_);
        if (++lenBytes_ == 4) {
          if (len_ > 12 * 1024 * 1024) {
            state_ = MAGIC1;
            break;
          }
          if (cap_ < len_ + 1) {
            free(buf_);
            cap_ = len_ + 1;
            buf_ = (uint8_t *)heap_caps_malloc(cap_, MALLOC_CAP_SPIRAM);
            if (!buf_) {
              cap_ = 0;
              state_ = MAGIC1;
              break;
            }
          }
          got_ = 0;
          crc_ = 0;
          crcBytes_ = 0;
          state_ = len_ ? PAYLOAD : CRC;
        }
        break;
      case PAYLOAD: {
        buf_[got_++] = b;
        // bulk copy what is already buffered
        int more = std::min<int>(Serial.available(), (int)(len_ - got_));
        if (more > 0) {
          got_ += Serial.readBytes(buf_ + got_, more);
          avail = Serial.available();
        }
        if (got_ >= len_) state_ = CRC;
        break;
      }
      case CRC:
        crc_ |= (uint32_t)b << (8 * crcBytes_);
        if (++crcBytes_ == 4) {
          state_ = MAGIC1;
          if (crc_ == crc32(buf_, len_)) {
            if (handler_) handler_(type_, buf_, len_);
          } else {
            ack(type_, false, "crc mismatch");
          }
        }
        break;
    }
  }
}
