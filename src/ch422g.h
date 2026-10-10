#pragma once

#include <Arduino.h>
#include <Wire.h>

// CH422G IO expander. It has no ordinary I2C address: each "address" is a command.
//   0x24 WR_SET: bit0 = EXIO0..7 are outputs
//   0x38 WR_IO:  output levels of EXIO0..7
class CH422G {
 public:
  bool begin(TwoWire &wire, uint8_t initial) {
    wire_ = &wire;
    if (!send(0x24, 0x01)) return false;
    return write(initial);
  }

  bool write(uint8_t value) {
    out_ = value;
    return send(0x38, value);
  }
  bool set(uint8_t mask, bool high) { return write(high ? (out_ | mask) : (out_ & ~mask)); }
  uint8_t value() const { return out_; }
  // Pin levels of EXIO0..7 as the chip reads them (0x26 RD_IO), -1 on a bus error.
  int readPins() {
    if (wire_->requestFrom((uint8_t)0x26, (uint8_t)1) != 1) return -1;
    return wire_->read();
  }

 private:
  bool send(uint8_t cmd, uint8_t data) {
    wire_->beginTransmission(cmd);
    wire_->write(data);
    return wire_->endTransmission() == 0;
  }

  TwoWire *wire_ = nullptr;
  uint8_t out_ = 0;
};
