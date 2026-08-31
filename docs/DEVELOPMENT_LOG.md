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
