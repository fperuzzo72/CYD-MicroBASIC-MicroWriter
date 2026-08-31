# Porting MicroBASIC and MicroWriter to the FNK0103N

The source is `MicroWriter-BASIC-PaperS3`, whose `editor/src` and `editor/lib`
sit verbatim in `port-staging/`. Files move from there into `editor/src/` one
at a time as each is ported, the same method that got the X4 firmware onto the
PaperS3. Nothing is edited in place in `port-staging/`: it stays a clean copy
of what is known to work somewhere else.

## What actually changes

Three things, and they are not the same size.

**The display is a different kind of display.** Not a smaller e-paper, a
colour TFT over SPI. That deletes the whole EPD refresh model: no waveform
choice, no partial-versus-full refresh, no ghosting, no blocking wait for a
panel update. It also deletes the full-screen framebuffer, because there is no
PSRAM to put one in. What replaces it is simpler than what it replaces: dirty
cells drawn straight out to SPI, immediately.

**The touch is resistive and single-point.** The touch UI inherited from the
PaperS3 assumes a GT911: precise, capacitive, multi-point. An XPT2046 needs
per-unit calibration, reads one point, drifts with pressure, and wants a
fingernail more than a fingertip. The interaction model survives, the hit
targets do not.

**Everything is smaller.** Half the pixels in each axis, a quarter of the
flash, no PSRAM, and a slower single-issue LX6 core instead of an S3.

## Screen geometry

The panel is 480x320 in landscape. The PaperS3's 960x540 gave four SCREEN
modes at 32/48/64/80 columns, all dividing 960 exactly. Here 480 divides
cleanly by 32, 48 and 80, but 64 gives 7.5 pixels a column, and at 6 pixels a
column an 80-column mode is about 0.9mm a character on a 3.5" panel. Carrying
the old column counts over for lineage's sake would buy two modes nobody can
read.

Proposed instead, with a 16-pixel status bar (one native unscii row, so the
bar draws in the base font) leaving a 304-pixel terminal band:

| Mode | Columns x Rows | Cell | Scale from unscii-16 | Font |
|---|---|---|---|---|
| `SCREEN 0` | 32 x 10 | 15x30 | 1.875x | `unscii_15x30.h`, already generated |
| `SCREEN 1` | 40 x 12 | 12x24 | 1.5x | `unscii_12x24.h`, already generated |
| `SCREEN 2` | 48 x 15 | 10x20 | 1.25x | needs generating |
| `SCREEN 3` | 60 x 19 | 8x16 | 1.0x, native | unscii-16 unresampled |

Two of the four fonts come straight out of the PaperS3 build with no work at
all, and `SCREEN 3` needs no resampling because it is unscii-16 at its own
size. Only the 10x20 has to be generated, with the tools already in
`research/fonts/tools/`. `SCREEN 3` divides the 304-pixel band exactly at 19
rows; the others take a small centred margin, the same way the non-exact modes
already do on both earlier devices.

Boot mode should be `SCREEN 1` (40 columns). The X4 boots at 48 and the
PaperS3 at 64, each picked for what read well on that panel; 40 is the
equivalent judgement for this one, and it is also the column count most of the
home computers this thing is imitating actually had.

This table is a proposal, not a decision. It is the first thing to revisit
once the bring-up has been on the real panel and the glyphs can be looked at.

## The flash budget

The single hardest constraint. 3MB of app partition, against a PaperS3 binary
of roughly 1.7MB that carries WiFi, an HTTP server, a NimBLE HID host, the
interpreter and its fonts.

Measured so far, with the bring-up only:

| Build | Flash | RAM |
|---|---|---|
| Milestone 1 bring-up | 358KB of 3MB | 22KB of 320KB |

The four proportional prose fonts are the obvious risk: `notosans_*` and
`ubuntu_*` in `port-staging/lib/EpdFont/builtinFonts/` are about 2.6MB of
headers between them. All four cannot be embedded here. Either one goes in and
the rest live on the SD card through the `SdCardFont` path that already
exists, or they all move to the card. That decision belongs to the MicroWriter
milestone, not before.

The other risk is WiFi and NimBLE in the same binary on a classic ESP32. If
that turns out not to fit, the on-screen keyboard becomes the only input
method, which on a touch panel is a defensible machine rather than a
compromise. Measure before deciding.

## Milestones

Each one ends with something demonstrable on the board, not with a file
compiling.

1. **Hardware bring-up.** Panel geometry, touch calibration, SD mount.
   Written, compiles, not yet flashed. `editor/src/main.cpp`.
2. **Render layer.** A TFT-backed renderer offering the same primitives as
   `GfxRenderer`, plus a glyph blit that expands EpdFont's 1bpp glyphs into
   16-bit colour with a foreground and background. Done right, `EpdFont`,
   `screen_editor`, `text_editor` and `osk` port with their drawing calls
   untouched. Proven by drawing all four SCREEN grids.
3. **Touch and the on-screen keyboard.** Calibration persisted in NVS, mapped
   into the event shape `osk.cpp` already expects, then `osk.cpp` itself with
   new key geometry. The pressure here is vertical: a usable QWERTY on a
   320-pixel-tall screen leaves the terminal about half its rows while the
   keyboard is up. Expect to redesign, not rescale.
4. **Terminal.** `screen_editor.cpp` and the geometry half of `config.h`.
5. **Interpreter.** `patches/tinybasic/fetch.sh`, then `tb_bridge.cpp`,
   `tb_runtime.cpp`, `terminal_input.cpp`. At this point it is MicroBASIC.
6. **Storage.** `file_manager.cpp`, `file_browser.cpp`, and `sd_datetime.cpp`
   with the RTC path removed and only SNTP left.
7. **Prose editor.** `text_editor.cpp`, and the `microwriter` env becomes a
   real second machine rather than the same binary with a flag.
8. **Network.** `wifi_sync.cpp` and `web_files_page.h`. Measure the binary
   here; this is where the flash budget gets tested.
9. **BLE keyboard.** `BleKeyboardHost` and NimBLE, if milestone 8 leaves room.

## What is not coming across

- `ota_apps.cpp` and `docs/DUAL_BOOT.md`. Both exist for the PaperS3's shared
  dual-boot contract with CrossPoint. This board has one app partition and one
  firmware.
- `vc_browser.cpp` and the CrossPoint reading-progress side of sync.
- Everything in `freeink-sdk`. It is an e-paper SDK; nothing in it applies. Its
  useful abstraction, the `hal/` seam, is worth copying as a shape rather than
  as code.
- The BM8563 RTC path in `sd_datetime.cpp`.
