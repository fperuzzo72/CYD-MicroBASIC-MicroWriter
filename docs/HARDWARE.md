# FNK0103N hardware, and where every number here came from

The board is a Freenove FNK0103N: a 3.5" ST7796 panel on a classic
ESP32-WROOM-32E, the shape of board the hobby world calls a "Cheap Yellow
Display". Freenove sells the same physical design under more than one product
code, which is the only reason the file names below do not all say FNK0103.

Nothing on this page is inferred from a photograph or from what a similar
board does. Each line says which file it came from, so a wrong pin can be
traced to a wrong source rather than argued about.

## Provenance

| Fact | Source |
|---|---|
| Chip is a classic ESP32, not an S3 | `Freenove_ESP32_Display/NerdMiner-FNK0103/NerdMiner-FNK0103N(3.5'' ST7796 TN).bat`, which flashes with `esptool --chip esp32` |
| Panel is ST7796, 320x480 | Same filename, and `Datasheet/3.5inch_ESP32-32E_ST7796_E32R35T_E32N35T_V1.0/` |
| Panel and touch pins | `Libraries/FNK0114N_3.5inch_ST7796/TFT_eSPI_Setups_v1.4.zip`, file `TFT_eSPI_Setups/FNK0114N_3.5_320x480_ST7796.h` |
| Touch controller is XPT2046 | `Datasheet/3.5inch_.../DataSheet/XPT2046.pdf`, and `TOUCH_CS 33` in the setup above |
| SD card pins | `Sketches/Sketch_06.1_SD_Test`, cross-checked against the same pins already working in `MSX-emulator-for-Cheap-Yellow-Display` |
| Module is WROOM-32E with no PSRAM suffix | `Datasheet/.../DataSheet/esp32-wroom-32e_esp32-wroom-32ue_datasheet_en.pdf` is the only module datasheet in the bundle, and no R2/R8 part appears anywhere |

FNK0103N and FNK0114N are the same hardware under different product codes.
That equivalence is the one claim here that rests on inspection rather than a
single document: the two share the panel, the controller, the pinout in
Freenove's own setup file, and the board outline. It has been holding up in
the MSX build for this board.

## Pins

Repeated in `editor/src/board_fnk0103n.h`, which is the version the code
reads, and again in `editor/platformio.ini` as `-D` flags, because TFT_eSPI
takes pins only as build macros. All three have to move together.

| Function | Bus | Pins |
|---|---|---|
| ST7796 panel | HSPI | MISO 12, MOSI 13, SCLK 14, CS 15, DC 2, RST tied to board RST (-1), BL 27 active HIGH |
| XPT2046 touch | HSPI, shared with the panel | CS 33 |
| microSD | VSPI, its own bus | SCK 18, MISO 19, MOSI 23, CS 5 |

The separate SD bus is one of the few things that got simpler coming from the
PaperS3, where the card shares with the panel and there is arbitration to get
right.

## Confirmed on the real board

Nothing yet. Milestone 1 (`editor/src/main.cpp`) exists to fill this section
in. It compiles; it has not been flashed. Until a line moves up here, treat
everything above as well-sourced but unverified.

| Item | Status |
|---|---|
| Panel draws, correct rotation and no clipped edge | PENDING |
| Touch reads and calibrates | PENDING |
| SD mounts and lists a directory | PENDING |
| PSRAM genuinely absent | PENDING (the bring-up prints it either way) |
| Real flash size is 4MB | PENDING |

## What this board does not have

Named explicitly, because each one deletes code that exists in the PaperS3
tree rather than merely changing it.

- **No PSRAM.** About 320KB of DRAM for everything. A 480x320 framebuffer at
  16bpp is 307KB on its own, so there is no full-screen buffer and never will
  be. Drawing goes per dirty cell straight to SPI.
- **No RTC.** The BM8563 and everything in `sd_datetime.cpp` that reads it are
  gone. Time comes from SNTP, and there is nothing to hold it across a power
  cycle.
- **No IMU or tilt sensor.**
- **4MB of flash**, against 16MB, with an app partition of 3MB. This is the
  constraint most likely to force a real decision later. See PORTING_PLAN.md,
  "The flash budget".
