#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "ch422g.h"

// Minimal GT911 capacitive touch reader (polling, up to 5 points).
class GT911 {
 public:
  struct Point {
    uint16_t x, y, size;
    uint8_t id;
  };

  // Reset through the CH422G with INT held low so the chip takes address 0x5D.
  bool begin(TwoWire &wire, CH422G &io, uint8_t rstMask, int intPin) {
    wire_ = &wire;
    pinMode(intPin, OUTPUT);
    digitalWrite(intPin, LOW);
    io.set(rstMask, false);
    delay(10);
    io.set(rstMask, true);
    delay(10);
    pinMode(intPin, INPUT);
    delay(60);  // GT911 needs ~50 ms before it answers

    uint8_t id[4] = {0};
    if (!read(0x8140, id, 4)) return false;  // product id, e.g. "911"
    memcpy(productId_, id, 4);
    return true;
  }

  const char *productId() const { return productId_; }

  // Returns the number of touches (0 = released), -1 on a bus error, -2 when the chip has
  // no new sample yet (keep the previous state).
  int read(Point *pts, int max) {
    uint8_t status = 0;
    if (!read(0x814E, &status, 1)) return -1;
    if (!(status & 0x80)) return -2;
    int n = status & 0x0F;
    if (n > 5) n = 5;
    if (n > max) n = max;
    uint8_t buf[8 * 5];
    if (n > 0 && !read(0x814F, buf, 8 * n)) return -1;
    for (int i = 0; i < n; i++) {
      const uint8_t *p = buf + i * 8;
      pts[i] = {(uint16_t)(p[1] | p[2] << 8), (uint16_t)(p[3] | p[4] << 8), (uint16_t)(p[5] | p[6] << 8), p[0]};
    }
    uint8_t zero = 0;
    write(0x814E, &zero, 1);  // acknowledge so the chip produces the next sample
    lastCount_ = n;
    return n;
  }

 private:
  static constexpr uint8_t ADDR = 0x5D;

  bool read(uint16_t reg, uint8_t *buf, size_t len) {
    wire_->beginTransmission(ADDR);
    wire_->write(reg >> 8);
    wire_->write(reg & 0xFF);
    if (wire_->endTransmission(false) != 0) return false;
    if (wire_->requestFrom(ADDR, (uint8_t)len) != len) return false;
    for (size_t i = 0; i < len; i++) buf[i] = wire_->read();
    return true;
  }

  bool write(uint16_t reg, const uint8_t *buf, size_t len) {
    wire_->beginTransmission(ADDR);
    wire_->write(reg >> 8);
    wire_->write(reg & 0xFF);
    wire_->write(buf, len);
    return wire_->endTransmission() == 0;
  }

  TwoWire *wire_ = nullptr;
  char productId_[5] = {0};
  int lastCount_ = 0;
};
