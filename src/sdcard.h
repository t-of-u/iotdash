#pragma once
// TF card of the Waveshare ESP32-S3-Touch-LCD-7: SPI (MOSI 11, SCK 12, MISO 13) with the card
// select line on the CH422G IO expander. The SPI driver cannot toggle that pin, so the card is
// selected for good after the power-up clocks (it is the only device on the bus). FAT is
// mounted at /sd through the VFS: plain fopen/opendir work on it.

#include <string>

#include "ch422g.h"

class SdCard {
 public:
  static constexpr const char *kRoot = "/sd";

  bool begin(CH422G &io);  // SPI bus (once)
  bool mount(std::string &err, bool formatIfEmpty = false);
  void unmount();
  bool mounted() const { return card_ != nullptr; }
  // FAT on the whole card (mounts it if needed); everything on it is lost
  bool format(std::string &err);
  bool info(uint64_t &total, uint64_t &free, std::string &type, std::string &name);
  // Diagnostics: raw CMD0 with the card deselected and selected; the bytes read back.
  std::string probe();
  // Hold the card select at a level for measuring (mount attempts pause); -1 releases it.
  void holdCs(int level);
  // Measuring by hand: drive one of the card lines (11 MOSI, 12 SCK, 13 MISO) to 0 / 1, or -1 to
  // let it float (its 10K pull-up then shows). The SPI bus is let go until autoMode().
  void drivePin(int gpio, int level);
  void autoMode();  // back to normal: SPI bus, card select released, mounting resumes
  std::string lineState() const;  // "cs low sck 1 mosi z miso z" / "cs high spi"

 private:
  void powerUpClocks();
  CH422G *io_ = nullptr;
  bool busReady_ = false;
  void *card_ = nullptr;  // sdmmc_card_t (an anonymous struct: not forward-declarable)
  int hold_ = -1;
  bool manual_ = false;
  int pinLevel_[3] = {-2, -2, -2};  // MOSI, SCK, MISO: -2 SPI, -1 floating, 0/1 driven
};
