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

Milestone 1, hardware bring-up, is written and builds. **It has not been
flashed to a board yet.** Nothing in `docs/HARDWARE.md`'s confirmation table
has moved to CONFIRMED, and until it does, the pin table is well-sourced but
unverified.

No MicroBASIC feature has been ported yet. The PaperS3 sources sit verbatim in
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
