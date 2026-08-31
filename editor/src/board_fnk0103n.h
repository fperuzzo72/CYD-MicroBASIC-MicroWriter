#pragma once

// Freenove FNK0103N: 3.5" ST7796 320x480 panel on an ESP32-WROOM-32E.
//
// This header is the single source of truth for pins in this project. The
// same numbers appear in platformio.ini as -D flags for TFT_eSPI (that
// library only takes pins as build macros, never from a header), so the two
// places have to move together. Change a pin here and not there and TFT_eSPI
// keeps driving the old one, silently.
//
// Provenance, file by file, is in docs/HARDWARE.md. In short: everything
// below comes from Freenove's own material (the
// FNK0114N_3.5_320x480_ST7796.h setup inside TFT_eSPI_Setups_v1.4.zip, plus
// sketches 06.1 and 12.3), with the chip confirmed as a classic ESP32 by the
// `--chip esp32` in the flasher Freenove ships for this exact board.

// --- Panel: ST7796 on HSPI ---
// The bus is shared with the touch controller. The SD card has its own bus,
// see below.
static constexpr int PIN_TFT_MISO = 12;
static constexpr int PIN_TFT_MOSI = 13;
static constexpr int PIN_TFT_SCLK = 14;
static constexpr int PIN_TFT_CS   = 15;
static constexpr int PIN_TFT_DC   =  2;
static constexpr int PIN_TFT_RST  = -1;  // tied to the board's RST, not a GPIO
static constexpr int PIN_TFT_BL   = 27;  // backlight, active HIGH

// The controller's native portrait resolution. This firmware runs landscape
// (rotation 1 or 3), so 480 wide by 320 tall. See docs/PORTING_PLAN.md,
// "Screen geometry", for what that does to the SCREEN modes.
static constexpr int PANEL_NATIVE_W = 320;
static constexpr int PANEL_NATIVE_H = 480;
static constexpr int SCREEN_W = 480;  // landscape
static constexpr int SCREEN_H = 320;

// --- Touch: XPT2046, resistive, sharing the panel's HSPI bus ---
// Resistive and single-point. No gestures, no two fingers, and the raw
// reading needs per-unit calibration. That is a real change to the touch UI
// inherited from the PaperS3, which assumes a capacitive GT911. See the
// porting plan.
static constexpr int PIN_TOUCH_CS = 33;

// --- microSD: VSPI, a bus of its own ---
// A separate bus is good news: on the PaperS3 the card shares with the panel
// and there was arbitration to get right. Here there is none.
static constexpr int PIN_SD_SCK  = 18;
static constexpr int PIN_SD_MISO = 19;
static constexpr int PIN_SD_MOSI = 23;
static constexpr int PIN_SD_CS   =  5;

// --- What this board does NOT have, and the PaperS3 did ---
// No PSRAM: this board's WROOM-32E is the variant without it, which leaves
// roughly 320KB of usable DRAM for everything. A 480x320 framebuffer at 16bpp
// would be 307KB on its own, so drawing is per-dirty-cell straight out to
// SPI rather than composed in RAM.
// No RTC: file timestamps depend on SNTP, with nothing holding the clock
// across a power cycle.
// No IMU, no tilt sensor.
