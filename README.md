# CYD MicroBASIC / MicroWriter

MicroBASIC and MicroWriter, ported to a Freenove FNK0103N: a 3.5" ST7796
touch panel on a classic ESP32, the shape of board the hobby world calls a
Cheap Yellow Display.

**MicroBASIC** is a 1980s-style BASIC computer: you switch it on, you get a
prompt, you type a numbered line and it becomes program text, you type
anything else and it runs. **MicroWriter** is the writing machine MicroBASIC
grew out of: the same tree with the interpreter and everything that talks to
it left out of the build entirely, not compiled and hidden.

Both come from [MicroWriter-BASIC-PaperS3](https://github.com/fperuzzo72/MicroWriter-BASIC-PaperS3),
which is itself a port of [MicroBASIC](https://github.com/fperuzzo72/MicroBASIC).
This is the third device in that line and by some distance the smallest.

## Status

Milestone 1, hardware bring-up, is **done**. Panel, rotation, chip, flash size,
the absence of PSRAM, the touch controller and the SD card are all confirmed on
real hardware rather than inferred, and the board reports 335KB of free heap at
boot, which is the budget the rest of the port lives inside. Evidence for every
row is in `docs/HARDWARE.md`.

Milestone 2, the render layer, is **done**. `TftRenderer` offers the eighteen
`GfxRenderer` methods the ported code actually calls, with matching signatures,
and draws EpdFont glyphs straight to the panel with no framebuffer in between.
A full character-grid repaint costs 65ms and a single cell 240us, both measured
on the board; how that got from an initial 196ms is in
`docs/PORTING_PLAN.md`, "The render budget".

Because the panel is backlit and colour, it is no longer black on white by
physics. Four palettes ship: **MSX blue** (white on TMS9918 colour 4, which is
the default), phosphor green, phosphor amber, and paper white, which is what
the two e-paper devices look like.

All four SCREEN modes exist and draw on the panel: 32, 40, 48 and 60 columns,
each measuring exactly 480 pixels across. The 40-column mode happens to be
exactly the width of MSX BASIC's text screen, so it is the boot mode, and the
render demo boots into something shaped like an MSX startup screen with this
board's own real numbers in it.

Milestone 3, touch and the on-screen keyboard, is **done**. `osk.cpp` came over
from the PaperS3 with nothing changed but its include and the renderer's type
name, and it emits standard USB HID keycodes, which is the same wire format the
editor and interpreter already expect. With the keyboard up the terminal has 7
rows of 60 columns; folded away, 19.

The interpreter and the real terminal are not ported yet. The PaperS3 sources sit verbatim in
`port-staging/` and move into `editor/src/` one at a time. See
`docs/PORTING_PLAN.md` for the order and for what is not coming across at all.

## The board, and why it matters

| | PaperS3 | FNK0103N |
|---|---|---|
| Chip | ESP32-S3 | classic ESP32 (LX6) |
| Flash | 16MB | 4MB |
| PSRAM | 8MB octal | none |
| Display | 960x540 mono e-paper, i80 parallel | 480x320 colour TFT, SPI |
| Touch | GT911 capacitive, multi-point | XPT2046 resistive, single point |
| Clock | BM8563 RTC | none, SNTP only |

Half the pixels in each axis, a quarter of the flash, and no PSRAM. The
display being a different *kind* of display is the largest single change: the
e-paper refresh model disappears entirely, and so does the full-screen
framebuffer, because 480x320 at 16bpp is 307KB and there is nowhere to put it.
What replaces it is simpler than what it replaces.

Pin table and the provenance of every number in it: `docs/HARDWARE.md`.

## Building

PlatformIO. Two envs, one per machine, from one source tree.

```bash
pio run -e fnk0103n -t upload      # MicroBASIC
```

```bash
pio run -e microwriter -t upload   # MicroWriter
```

```bash
pio device monitor -b 115200
```

Run these from `editor/`. Unlike the PaperS3, which holds two firmwares at
once in a shared dual-boot layout, this board has a single app partition:
switching machines means reflashing.

If the board has run other firmware before, erase it once first:

```bash
pio run -t erase
```

Leftovers from the previous image produce a `esp_core_dump_flash` error on
every boot otherwise. Upload speed is set to 460800 rather than 921600 on
purpose; the CH340 on this board does not hold the higher rate. Both are
explained in `docs/HARDWARE.md`.

The interpreter is not vendored. Fetch it before the first MicroBASIC build:

```bash
./patches/tinybasic/fetch.sh
```

That clones Stefan Lenz's IoT BASIC at a pinned commit, lays its portable core
into `editor/lib/TinyBasic/`, and applies this project's patches. It is
deliberately not in the tree; the reasoning is in the script's header.

## Layout

```
editor/            the firmware. platformio.ini, partitions.csv, src/
port-staging/      the PaperS3 sources, verbatim, waiting to be ported
docs/              HARDWARE.md, PORTING_PLAN.md, DEVELOPMENT_LOG.md
research/fonts/    BDF and hex sources, and the tools that emit EpdFont headers
patches/           fetch-and-patch recipes for the third-party interpreter
examples/          BASIC programs
```

`port-staging/` is never edited. It stays a clean copy of code that is known
to work on another device, so that a bug introduced during porting can always
be diffed against the thing it came from.

## Sibling projects

Three repositories, one lineage, separate histories:

- [MicroBASIC](https://github.com/fperuzzo72/MicroBASIC), the original, on the X4.
- [MicroWriter-BASIC-PaperS3](https://github.com/fperuzzo72/MicroWriter-BASIC-PaperS3), on the M5Stack PaperS3.
- this one.

They stay separate rather than becoming one tree with device flags: an e-paper
panel with a BLE keyboard and a resistive touch panel with an on-screen
keyboard are different machines, and one codebase serving both would be
conditional logic stacked on conditional logic. The cost is that a fix in
shared code has to be carried by hand, and that cost grows with each device.

## Licensing

This project's own code is MIT, see `LICENSE`. The fonts in `research/fonts/`
and the vendored libraries in `port-staging/lib/` carry their own licences;
see `NOTICE.md`. The BASIC interpreter is fetched at build time and never
distributed here, for the reasons in `patches/tinybasic/fetch.sh`.
