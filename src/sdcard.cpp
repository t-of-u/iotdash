#include "sdcard.h"

#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

#include "board.h"

static constexpr spi_host_device_t kHost = SPI2_HOST;
static constexpr int kMosi = 11, kSck = 12, kMiso = 13;

static bool initBus(spi_dma_chan_t dma);

bool SdCard::begin(CH422G &io) {
  io_ = &io;
  if (busReady_) return true;
  busReady_ = initBus(SDSPI_DEFAULT_DMA);
  return busReady_;
}

// SD power-up: at least 74 clocks with the card deselected, then it listens for CMD0.
void SdCard::powerUpClocks() {
  io_->set(board::EXIO_SD_CS, true);
  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = 400 * 1000;
  dev.mode = 0;
  dev.spics_io_num = -1;
  dev.queue_size = 1;
  spi_device_handle_t h;
  if (spi_bus_add_device(kHost, &dev, &h) == ESP_OK) {
    uint8_t ff[16];
    memset(ff, 0xFF, sizeof ff);
    spi_transaction_t t = {};
    t.length = sizeof ff * 8;
    t.tx_buffer = ff;
    spi_device_polling_transmit(h, &t);
    spi_bus_remove_device(h);
  }
  io_->set(board::EXIO_SD_CS, false);  // selected from now on
}

// CMD0 through the SPI peripheral: the R1 byte, or -1 when the card stays silent.
static int spiCmd0(CH422G &io, int hz) {
  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = hz;
  dev.mode = 0;
  dev.spics_io_num = -1;
  dev.queue_size = 1;
  spi_device_handle_t h;
  if (spi_bus_add_device(kHost, &dev, &h) != ESP_OK) return -2;
  auto xfer = [&](const uint8_t *tx, uint8_t *rx, size_t n) {
    spi_transaction_t t = {};
    t.length = n * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    spi_device_polling_transmit(h, &t);
  };
  uint8_t ff[32], rx[32];
  memset(ff, 0xFF, sizeof ff);
  io.set(board::EXIO_SD_CS, true);
  for (int i = 0; i < 4; i++) xfer(ff, rx, sizeof ff);
  io.set(board::EXIO_SD_CS, false);
  xfer(ff, rx, 2);
  int r1 = -1;
  for (int attempt = 0; attempt < 3 && r1 < 0; attempt++) {
    uint8_t cmd[24];
    memset(cmd, 0xFF, sizeof cmd);
    const uint8_t c0[6] = {0x40, 0, 0, 0, 0, 0x95};
    memcpy(cmd, c0, 6);
    memset(rx, 0xAA, sizeof rx);
    xfer(cmd, rx, sizeof cmd);
    for (size_t i = 6; i < sizeof cmd; i++)
      if (rx[i] != 0xFF) {
        r1 = rx[i];
        break;
      }
  }
  io.set(board::EXIO_SD_CS, true);
  spi_bus_remove_device(h);
  return r1;
}

static bool initBus(spi_dma_chan_t dma) {
  spi_bus_config_t bus = {};
  bus.mosi_io_num = kMosi;
  bus.miso_io_num = kMiso;
  bus.sclk_io_num = kSck;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 16 * 1024;
  return spi_bus_initialize(kHost, &bus, dma) == ESP_OK;
}

std::string SdCard::probe() {
  if (!busReady_ || card_) return "bus not ready or card mounted";
  std::string out;
  char b[48];
  auto r = [&](const char *what, int v) {
    snprintf(b, sizeof b, "%s%s %s", out.empty() ? "" : " | ", what, v == -1 ? "--" : std::to_string(v).c_str());
    out += b;
  };
  r("spi+dma", spiCmd0(*io_, 400 * 1000));
  gpio_pullup_en((gpio_num_t)kMiso);
  r("spi+dma+pullup", spiCmd0(*io_, 400 * 1000));
  spi_bus_free(kHost);
  if (initBus(SPI_DMA_DISABLED)) {
    gpio_pullup_en((gpio_num_t)kMiso);
    r("spi nodma+pullup", spiCmd0(*io_, 400 * 1000));
    gpio_pullup_dis((gpio_num_t)kMiso);
    r("spi nodma", spiCmd0(*io_, 400 * 1000));
    spi_bus_free(kHost);
  } else {
    out += " | no-DMA bus init failed";
  }
  // continuity: each line has a 10K pull-up on the card side of its 0R link. With the pin an
  // input pulled down internally (~45K) it reads 1 only if that path is intact.
  {
    gpio_config_t t = {};
    t.pin_bit_mask = (1ULL << kMosi) | (1ULL << kSck) | (1ULL << kMiso);
    t.mode = GPIO_MODE_INPUT;
    t.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&t);
    delay(2);
    snprintf(b, sizeof b, " | pull-ups mosi %d sck %d miso %d", gpio_get_level((gpio_num_t)kMosi),
             gpio_get_level((gpio_num_t)kSck), gpio_get_level((gpio_num_t)kMiso));
    out += b;
    gpio_pulldown_dis((gpio_num_t)kMosi);
    gpio_pulldown_dis((gpio_num_t)kSck);
    gpio_pulldown_dis((gpio_num_t)kMiso);
  }
  // bit-banged: CMD0 then CMD8, raw bytes after each
  gpio_config_t o = {};
  o.pin_bit_mask = (1ULL << kMosi) | (1ULL << kSck);
  o.mode = GPIO_MODE_INPUT_OUTPUT;
  gpio_config(&o);
  gpio_config_t in = {};
  in.pin_bit_mask = 1ULL << kMiso;
  in.mode = GPIO_MODE_INPUT;
  in.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&in);
  auto bit = [&](int v) {
    gpio_set_level((gpio_num_t)kMosi, v);
    delayMicroseconds(4);
    int r = gpio_get_level((gpio_num_t)kMiso);  // sampled before the rising edge
    gpio_set_level((gpio_num_t)kSck, 1);
    delayMicroseconds(4);
    gpio_set_level((gpio_num_t)kSck, 0);
    return r;
  };
  auto byte = [&](uint8_t v) {
    uint8_t r = 0;
    for (int i = 7; i >= 0; i--) r = (uint8_t)((r << 1) | bit((v >> i) & 1));
    return r;
  };
  gpio_set_level((gpio_num_t)kSck, 0);
  // MISO with no command: 32 bits back to back (no per-byte work), card deselected / selected
  for (int sel = 0; sel < 2; sel++) {
    io_->set(board::EXIO_SD_CS, sel == 0);
    delay(1);
    uint32_t w = 0;
    for (int i = 0; i < 32; i++) w = (w << 1) | (uint32_t)bit(1);
    snprintf(b, sizeof b, " | idle cs%s %08lx", sel ? "low" : "high", (unsigned long)w);
    out += b;
    // and without clocking: is MISO steady?
    int lows = 0;
    for (int i = 0; i < 200; i++) {
      lows += gpio_get_level((gpio_num_t)kMiso) == 0;
      delayMicroseconds(5);
    }
    snprintf(b, sizeof b, " still-lows %d/200", lows);
    out += b;
  }
  io_->set(board::EXIO_SD_CS, true);
  for (int i = 0; i < 20; i++) byte(0xFF);
  io_->set(board::EXIO_SD_CS, false);
  byte(0xFF);
  const uint8_t cmds[2][6] = {{0x40, 0, 0, 0, 0, 0x95}, {0x48, 0, 0, 0x01, 0xAA, 0x87}};
  for (int c = 0; c < 2; c++) {
    for (uint8_t x : cmds[c]) byte(x);
    out += c ? " | bb cmd8:" : " | bb cmd0:";
    for (int i = 0; i < 8; i++) {
      snprintf(b, sizeof b, " %02x", byte(0xFF));
      out += b;
    }
  }
  // SD (native) mode, 1 bit: CMD on the MOSI pin, DAT0 on MISO, DAT3 = CS held high.
  // CMD0, then CMD8: an alive card answers R7 (48 bits starting 0x08 ... 0x01 0xAA).
  {
    io_->set(board::EXIO_SD_CS, true);
    gpio_num_t cmdPin = (gpio_num_t)kMosi, clk = (gpio_num_t)kSck;
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << kMosi;
    c.mode = GPIO_MODE_INPUT_OUTPUT;
    c.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&c);
    auto clock = [&]() {
      delayMicroseconds(3);
      gpio_set_level(clk, 1);
      delayMicroseconds(3);
      gpio_set_level(clk, 0);
    };
    auto sendCmd = [&](const uint8_t *f) {
      gpio_set_direction(cmdPin, GPIO_MODE_INPUT_OUTPUT);
      for (int i = 0; i < 6; i++)
        for (int k = 7; k >= 0; k--) {
          gpio_set_level(cmdPin, (f[i] >> k) & 1);
          clock();
        }
      gpio_set_level(cmdPin, 1);
    };
    gpio_set_level(cmdPin, 1);
    for (int i = 0; i < 100; i++) clock();
    const uint8_t c0[6] = {0x40, 0, 0, 0, 0, 0x95}, c8[6] = {0x48, 0, 0, 0x01, 0xAA, 0x87};
    sendCmd(c0);
    for (int i = 0; i < 16; i++) clock();
    sendCmd(c8);
    gpio_set_direction(cmdPin, GPIO_MODE_INPUT);
    int start = -1;
    for (int i = 0; i < 80 && start < 0; i++) {
      delayMicroseconds(3);
      gpio_set_level(clk, 1);
      if (gpio_get_level(cmdPin) == 0) start = i;
      delayMicroseconds(3);
      gpio_set_level(clk, 0);
    }
    out += " | sd-mode cmd8:";
    if (start < 0) {
      out += " no answer";
    } else {
      uint8_t resp[6] = {};
      int bitsRead = 1;  // the start bit (0) is bit 47
      for (; bitsRead < 48; bitsRead++) {
        delayMicroseconds(3);
        gpio_set_level(clk, 1);
        int v = gpio_get_level(cmdPin);
        delayMicroseconds(3);
        gpio_set_level(clk, 0);
        resp[bitsRead / 8] |= (uint8_t)(v << (7 - bitsRead % 8));
      }
      for (uint8_t x : resp) {
        snprintf(b, sizeof b, " %02x", x);
        out += b;
      }
      snprintf(b, sizeof b, " (after %d clocks)", start);
      out += b;
    }
    for (int i = 0; i < 8; i++) clock();
  }
  gpio_set_direction((gpio_num_t)kMosi, GPIO_MODE_INPUT_OUTPUT);
  gpio_set_level((gpio_num_t)kMosi, 0);
  gpio_set_level((gpio_num_t)kSck, 1);
  delayMicroseconds(5);
  int m0 = gpio_get_level((gpio_num_t)kMosi), k1 = gpio_get_level((gpio_num_t)kSck);
  gpio_set_level((gpio_num_t)kMosi, 1);
  gpio_set_level((gpio_num_t)kSck, 0);
  delayMicroseconds(5);
  int m1 = gpio_get_level((gpio_num_t)kMosi), k0 = gpio_get_level((gpio_num_t)kSck);
  snprintf(b, sizeof b, " | pads mosi %d%d sck %d%d", m0, m1, k0, k1);
  out += b;
  io_->set(board::EXIO_SD_CS, true);
  gpio_reset_pin((gpio_num_t)kMosi);
  gpio_reset_pin((gpio_num_t)kSck);
  gpio_reset_pin((gpio_num_t)kMiso);
  busReady_ = initBus(SDSPI_DEFAULT_DMA);
  if (!busReady_) out += " | SPI bus re-init failed";
  return out;
}

void SdCard::holdCs(int level) {
  hold_ = level;
  if (io_) io_->set(board::EXIO_SD_CS, level != 0);  // released: deselected until the next mount
}

void SdCard::drivePin(int gpio, int level) {
  if (gpio < kMosi || gpio > kMiso || card_) return;
  if (!manual_) {
    if (busReady_) spi_bus_free(kHost);
    busReady_ = false;
    manual_ = true;
  }
  gpio_num_t g = (gpio_num_t)gpio;
  gpio_reset_pin(g);
  if (level < 0) {
    gpio_set_direction(g, GPIO_MODE_INPUT);
    gpio_set_pull_mode(g, GPIO_FLOATING);
  } else {
    gpio_set_direction(g, GPIO_MODE_OUTPUT);
    gpio_set_level(g, level);
  }
  pinLevel_[gpio - kMosi] = level < 0 ? -1 : level;
}

void SdCard::autoMode() {
  hold_ = -1;
  if (io_) io_->set(board::EXIO_SD_CS, true);
  if (manual_) {
    for (int g : {kMosi, kSck, kMiso}) gpio_reset_pin((gpio_num_t)g);
    manual_ = false;
    busReady_ = initBus(SDSPI_DEFAULT_DMA);
  }
  for (int &l : pinLevel_) l = -2;
}

std::string SdCard::lineState() const {
  std::string s = std::string("cs ") + (io_ && (io_->value() & board::EXIO_SD_CS) ? "high" : "low");
  if (!manual_) return s + " spi";
  const char *names[3] = {"mosi", "sck", "miso"};
  for (int i = 0; i < 3; i++) {
    int l = pinLevel_[i];
    s += std::string(" ") + names[i] + (l == -1 ? " z" : l == 0 ? " 0" : l == 1 ? " 1" : " -");
  }
  return s;
}

bool SdCard::mount(std::string &err, bool formatIfEmpty) {
  if (card_) return true;
  if (manual_) {
    err = "card lines driven by hand for measuring";
    return false;
  }
  if (hold_ >= 0) {
    err = std::string("card select held ") + (hold_ ? "high" : "low") + " for measuring";
    return false;
  }
  if (!busReady_) {
    err = "SPI bus not ready";
    return false;
  }
  powerUpClocks();
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = kHost;
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;  // 20 MHz
  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.host_id = kHost;
  slot.gpio_cs = SDSPI_SLOT_NO_CS;
  esp_vfs_fat_sdmmc_mount_config_t mc = {};
  mc.format_if_mount_failed = formatIfEmpty;
  mc.max_files = 8;
  mc.allocation_unit_size = 16 * 1024;
  sdmmc_card_t *card = nullptr;
  esp_err_t e = esp_vfs_fat_sdspi_mount(kRoot, &host, &slot, &mc, &card);
  card_ = card;
  if (e == ESP_OK) return true;
  card_ = nullptr;
  // ESP_FAIL: the card answered but holds no FAT file system
  err = e == ESP_FAIL ? "no file system (format the card)" : std::string("no card (") + esp_err_to_name(e) + ")";
  io_->set(board::EXIO_SD_CS, true);
  return false;
}

void SdCard::unmount() {
  if (!card_) return;
  esp_vfs_fat_sdcard_unmount(kRoot, (sdmmc_card_t *)card_);
  card_ = nullptr;
  io_->set(board::EXIO_SD_CS, true);
}

bool SdCard::format(std::string &err) {
  if (!card_) return mount(err, true);  // mounting formats a card without a file system
  esp_err_t e = esp_vfs_fat_sdcard_format(kRoot, (sdmmc_card_t *)card_);
  if (e != ESP_OK) err = std::string("format failed: ") + esp_err_to_name(e);
  return e == ESP_OK;
}

bool SdCard::info(uint64_t &total, uint64_t &free, std::string &type, std::string &name) {
  if (!card_) return false;
  const sdmmc_card_t *c = (const sdmmc_card_t *)card_;
  if (esp_vfs_fat_info(kRoot, &total, &free) != ESP_OK) total = free = 0;
  type = (c->ocr & (1u << 30)) ? (total > 32ull * 1024 * 1024 * 1024 ? "SDXC" : "SDHC") : "SDSC";  // OCR CCS bit
  name = c->cid.name;
  return true;
}
