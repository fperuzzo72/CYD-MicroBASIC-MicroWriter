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

## 2026-08-31 -- SD confirmed, and milestone 1 closes with nothing open

A card went in and mounted first try:

```
SD: SDHC, 15193 MB
  overlays/  kernel7.img  kernel.img  kernel7l.img  ... and 40 more
```

Which is a Raspberry Pi boot card, and a better test than a blank one would
have been: it proves the mount and directory read against a real populated
FAT32 volume, with the panel driving HSPI at the same time the card is driving
VSPI. The two buses genuinely do not contend, which was the one thing about
this board that looked easier than the PaperS3 and now is.

That was the last PENDING row. Every claim in HARDWARE.md's table is now
something the board did, not something a datasheet said.

Worth remembering when real work starts: this firmware creates folders and
writes at the root of whatever card is in the slot. Development wants a card
that is not somebody's Pi.

## 2026-08-31 -- Milestone 2: the render layer, and three rounds of measuring it

`GfxRenderer` has close to two hundred methods and almost all of them exist for
electrophoretic paper. Rather than port it, the first thing was to count what
MicroBASIC and MicroWriter actually call across every file in
`port-staging/src`. The answer is fifteen: `drawText`, `getTextWidth`,
`getLineHeight`, `fillRect`, `drawRect`, `clearScreen`, `displayBuffer`,
`insertFont`, `getFontMap`, `getSdCardFonts`, `setFontCacheManager`,
`setOrientation`, `getOrientation`, `getScreenWidth`/`getScreenHeight`, and
`tapToLogical`. `TftRenderer` implements those with matching signatures, so
ported code compiles against it without edits.

The glyph bit walk had to match `renderCharImpl` exactly, and the part that
matters is that `pixelPosition` runs continuously through a glyph and is never
padded at row boundaries. Any font whose width is not a multiple of eight
shears if you assume per-row alignment. The two unscii cells here are 15 and 12
wide, so the demo grid is itself the test, and it renders clean.

Metrics agree with the geometry exactly: 32 columns of 15x30 measure 480
pixels, 40 columns of 12x24 measure 480. That is the advance arithmetic
(previous advance and current kern summed in 12.4 fixed point, then snapped
together) carried over unchanged and confirmed working.

**Three rounds on speed**, because the first number was bad and guessing at the
cause would have been the wrong move:

| Approach | Per glyph | Full 32x10 repaint |
|---|---|---|
| Horizontal runs straight to the panel | 518 us | 196 ms |
| Composed in a scratch, colour-keyed push | 264 us | 115 ms |
| Composed opaque, one push per row | n/a | **65 ms** |

Instrumenting the split was what made it obvious: the full-band clear was 30ms
of the original 196ms and is the hardware floor (480x304 at 16bpp over 80MHz
SPI is ~29ms of transfer), which left 166ms for 320 glyphs against a transfer
floor near 90us each. So five sixths of the glyph time was SPI transaction
setup, not pixels. `drawFastHLine` opens and closes a transaction per call, and
a glyph is dozens of runs.

The opaque path composes background and glyphs together and pushes the whole
rectangle in one transfer, which also removes the need to clear first. One cell
now costs 240us, so a keystroke is imperceptible.

**One bug on the way, worth naming because it looks like something else.**
After switching to `pushImage` the colours came out wrong while the background
stayed right. That reads like a font or palette bug and is neither:
`TFT_eSPI`'s drawing primitives put a colour's bytes on the wire in panel
order, but `pushImage` streams the array raw and only swaps each 16-bit value
when `_swapBytes` is set, which it is not by default. So 0x2FE7 green arrived
as 0xE72F. `setSwapBytes(true)` in `TftRenderer::begin()`, with the reasoning
written next to it.

Next: generate the 10x20 and 8x16 unscii sizes so all four SCREEN modes exist,
then milestone 3, touch and the on-screen keyboard.
