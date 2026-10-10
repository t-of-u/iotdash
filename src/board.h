#pragma once

// Waveshare ESP32-S3-Touch-LCD-7: pins and panel timing.
// Source: Waveshare wiki + ESP32_Display_Panel BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7.h

namespace board {

// 7" IPS, ST7262, RGB565 parallel. Active area ~154 x 86 mm (pixels are not square).
constexpr int LCD_W = 800;
constexpr int LCD_H = 480;

// RGB timing. The panel refresh rate is pclk / ((W+hpw+hbp+hfp) * (H+vpw+vbp+vfp)):
// 16 MHz -> 16e6 / (820 * 500) = 39 Hz. Higher pclk needs more PSRAM bandwidth.
constexpr int LCD_PCLK_HZ = 16 * 1000 * 1000;
constexpr int LCD_HPW = 4, LCD_HBP = 8, LCD_HFP = 8;
constexpr int LCD_VPW = 4, LCD_VBP = 8, LCD_VFP = 8;
constexpr bool LCD_PCLK_ACTIVE_NEG = true;

constexpr int LCD_HSYNC = 46;
constexpr int LCD_VSYNC = 3;
constexpr int LCD_DE = 5;
constexpr int LCD_PCLK = 7;
// DATA0..15 = B3..B7, G2..G7, R3..R7 (RGB565 bit order)
constexpr int LCD_DATA[16] = {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40};

// Shared I2C bus: CH422G IO expander + GT911 touch.
constexpr int I2C_SDA = 8;
constexpr int I2C_SCL = 9;
constexpr int I2C_HZ = 400000;

constexpr int TOUCH_INT = 4;

// CH422G outputs (EXIOn = bit n).
constexpr uint8_t EXIO_TP_RST = 1 << 1;  // touch reset, active low
constexpr uint8_t EXIO_BL = 1 << 2;      // backlight enable
constexpr uint8_t EXIO_LCD_RST = 1 << 3; // LCD reset, active low
constexpr uint8_t EXIO_SD_CS = 1 << 4;   // TF card CS, active low
constexpr uint8_t EXIO_USB_SEL = 1 << 5; // 0 = GPIO19/20 to the USB port, 1 = to the CAN transceiver

}  // namespace board
