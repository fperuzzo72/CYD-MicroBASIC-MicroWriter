# Development log

Newest last. The same convention as the two earlier projects: an entry goes in
when something was learned, not when something was typed.

## 2026-08-31 -- Repository created, board identified from Freenove's own files

The port target was described as a "CYD Freenove FNK0103N 3.5 inch". Before
writing anything, that was pinned down against the material already cloned in
`../Freenove_ESP32_Display`, because "CYD" covers boards with different chips,
different panels and different buses.

What came out of it:

- The chip is a **classic ESP32**, not an S3. Freenove's own flasher for this
  exact board runs `esptool --chip esp32`. This is the single most consequential
  fact of the port: it means no PSRAM, 4MB of flash, and an LX6 core.
- The board is the same design as the **FNK0114N**, whose TFT_eSPI setup file
  Freenove ships. That file is where every panel and touch pin here comes from.
- Touch is **XPT2046**, resistive and single-point, sharing the panel's HSPI
  bus. The SD card has VSPI to itself.
- The pins were already proven once, in
  `../MSX-emulator-for-Cheap-Yellow-Display`, against this same board.

Full provenance table in HARDWARE.md.

## 2026-08-31 -- Milestone 1 written and building, not yet flashed

`editor/src/main.cpp` is a bring-up that gives each unknown its own visible
verdict rather than one pass/fail for the board as a whole: a geometry proof
first (border on all four edges, named corner markers, so a rotation or offset
error looks different from a "nothing draws" error), then a report block, then
the SD probe, then touch calibration.

Both envs build:

```
fnk0103n     RAM 22236 / 327680 (6.8%)   Flash 358257 / 3145728 (11.4%)
microwriter  identical, no sources excluded yet
```

That flash number is the baseline every later milestone gets measured against.

One environment fix was needed and is worth recording because it is not
obvious from the error: `pio run` failed at `Building bootloader.bin` with
`ModuleNotFoundError: No module named 'intelhex'`. Nothing to do with this
project. The `tool-esptoolpy` version that `espressif32@6.12.0` pulls
(esptool 4.9.0) imports `intelhex`, which was missing from `../.pio-venv`.
Fixed with `.pio-venv/bin/python -m pip install intelhex`.

The platform is pinned to `espressif32@6.12.0` rather than floating, for the
same reason the PaperS3 project pins its packages by URL: an explicit version
resolves the same way on every machine and every env.

## 2026-08-31 -- Milestone 1 flashed. Four things went wrong, none of them the board

The bring-up runs, and the board reported this for itself:

```
panel  : 480x320, rotation 1
chip   : ESP32-D0WD-V3 rev 3, 2 core(s) @ 240 MHz
flash  : 4 MB
heap   : 335 KB free
psram  : none (as expected)
touch  : calibration loaded from NVS
```

The geometry proof rendered correctly, border on all four edges and the corner
markers where they belong. Touch calibrated on the four corners and the values
came back from NVS on the next boot, which is what actually proves both the
touch and the reworked partition layout. SD is the only item still PENDING, for
want of a card.

Everything inferred from Freenove's files held up. Classic ESP32, 4MB, no
PSRAM. The 335KB of free heap is the number the whole port has to live inside,
and it is now measured rather than estimated.

Four failures on the way there, all in tooling, worth writing down because none
of them announces what it actually is.

**1. The CH340 will not hold 921600 baud.** esptool connects, reads the chip
ID, switches baud, then dies with `Unable to verify flash chip connection
(Serial data stream stopped: Possible serial noise or corruption)`. Reads like
a bad cable or a bad board; it is neither. 460800 is reliable and is now set in
platformio.ini.

**2. PlatformIO does not read partitions.csv to place the app.** The first
layout put a 32K NVS at 0x9000, which pushed the app partition to the next
64K-aligned boundary at 0x20000. esptool wrote the app at 0x10000 anyway,
because
`platform-espressif32/builder/main.py:305` does
`ESP32_APP_OFFSET = board.get("upload.offset_address", "0x10000")`, a hardcoded
default. The bootloader found erased flash at 0x20000 and reset forever, with
no panic and no serial output at all, because the second-stage bootloader fails
silently. From the outside the board looks dead: the backlight never comes on,
since the line that switches it on is never reached. 644 boot loops in 18
seconds of capture, all `rst:0x3 (SW_RESET)`, and not one line of application
output.

**3. boot_app0.bin lands at 0xe000 unconditionally.**
`framework-arduinoespressif32/tools/platformio-build.py:215` appends
`("0xe000", boot_app0.bin)` to FLASH_EXTRA_IMAGES with no flag to disable it.
So 0xe000-0x10000 gets 8K written on every upload no matter what the partition
table says lives there. Caught by reading the esptool output rather than by
losing data to it. Putting NVS across that range would have meant every reflash
silently wiping 8K out of the middle of it, taking touch calibration and WiFi
credentials along.

The answer to 2 and 3 together is the layout now in partitions.csv: otadata
declared at 0xe000, which is honestly what that write is; the app at the
standard 0x10000 so no build flag has to be kept in sync with the table; NVS
moved to after the app where it gets its full 32K untouched; and coredump
filling the 20K between the partition table and the boot_app0 slot, which
cannot hold anything else.

**4. Leftovers from the previous firmware.** This unit shipped with NerdMiner,
and whatever it left at 0x9000 made every boot log
`esp_core_dump_flash: size of core dump image: -4` preceded by a line of
garbage characters. It looked like the new coredump partition was misplaced. It
was not: `erase_region 0x9000 0x5000` cleared it and the boot has been clean
since. Anyone starting from a board with other firmware on it should run
`pio run -t erase` once before the first upload.
