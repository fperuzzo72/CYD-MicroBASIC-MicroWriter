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
| `SCREEN 0` | 32 x 10 | 15x30 | 1.875x | `unscii_15x30.h` |
| `SCREEN 1` | 40 x 12 | 12x24 | 1.5x | `unscii_12x24.h`, **boots here** |
| `SCREEN 2` | 48 x 15 | 10x20 | 1.25x | `unscii_10x20.h` |
| `SCREEN 3` | 60 x 19 | 8x16 | 1.0x, native | `unscii_8x16.h`, unresampled |

All four are generated and drawn on the panel. Each one measures exactly 480
pixels across its full column count, and each reports a line height equal to
its cell height.

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

`SCREEN 3` is the only mode whose rows divide the 304-pixel band exactly, at 19
rows with no margin, and it is also the only one drawn from unscii-16 at its
own native size with no resampling and no stem-width cap. Those two facts are
unrelated and both pleasant.

`SCREEN 1` is the boot mode. 40 columns is exactly MSX BASIC's text width,
which fell out of the panel arithmetic rather than being aimed at.

The old 64-column tier is gone deliberately: 480 divided by 64 is 7.5, not a
whole number of pixels. Rather than carry the two earlier devices' 32/48/64/80
lineage onto a panel that cannot hold it, the tiers are re-cut to what 480
actually divides by.

## The render budget

Measured on the board, 80MHz SPI, 480x304 terminal band.

| Path | Cost |
|---|---|
| Full repaint, 32x10 grid of 15x30 glyphs | 65 ms |
| One cell rewritten | 240 us |
| Full-band `fillRect` alone | 30 ms |
| On-screen keyboard, all six rows | 37 ms |
| One terminal row, 60 cols at 8x16 | 3.7 ms |
| First paint, terminal and keyboard | 100 ms |
| Full screen, terminal and keyboard | 91 ms |

The 30ms is the hardware floor: 480x304 pixels at 16bpp over 80MHz SPI is
about 29ms of pure transfer, so nothing can clear the band faster than that.

Getting to 65ms took two rounds of measurement, both worth knowing about
before touching this code:

- Drawing glyphs as horizontal runs straight to the panel cost **518us a
  glyph**, against a transfer floor near 90us. Almost all of it was per-run SPI
  transaction setup, because every `drawFastHLine` opens and closes its own.
- Composing a string into a scratch buffer and pushing it colour-keyed brought
  that to **264us**. Better, but a colour-keyed push still opens an address
  window per run of inked pixels, and text is made of short runs.
- Composing background and glyphs together and pushing the rectangle **opaque**
  brought the full repaint to **65us a row**, one transfer per row, and removed
  the separate clear entirely. That is `drawTextOpaque`, and it is what the
  character grid should use. `drawText` stays transparent for UI text that has
  to sit on top of something.

240us for one cell means a keystroke is imperceptible. A full repaint at 65ms
is only reached when the terminal scrolls, and if that ever feels slow the
ST7796 has a hardware vertical scroll that would make it nearly free.

## The flash budget

The single hardest constraint. 3MB of app partition, against a PaperS3 binary
of roughly 1.7MB that carries WiFi, an HTTP server, a NimBLE HID host, the
interpreter and its fonts.

Measured so far, on the real board:

| Build | Flash | Static RAM | Free heap at boot |
|---|---|---|---|
| Milestone 1 bring-up | 358257 of 3211264 (11.2%) | 22236 of 327680 | 335KB |

The free-heap figure is the one that governs the render layer: it is what is
actually available at runtime, after the WiFi and BLE stacks are not yet in the
picture. Expect it to fall sharply once they are.

The four proportional prose fonts are the obvious risk: `notosans_*` and
`ubuntu_*` in `port-staging/lib/EpdFont/builtinFonts/` are about 2.6MB of
headers between them. All four cannot be embedded here. Either one goes in and
the rest live on the SD card through the `SdCardFont` path that already
exists, or they all move to the card. That decision belongs to the MicroWriter
milestone, not before.

The other risk is WiFi and NimBLE in the same binary on a classic ESP32, and it
is now the sharper of the two.

**The order was decided: the keyboard outranks WiFi.** Both if they fit, and if
they do not, WiFi is what goes. A BASIC computer you cannot type on is not one;
one you cannot sync over the network is a BASIC computer with a memory card,
which is what every machine this one imitates actually was.

**Measured, and neither constraint bound. Kept below as written, because being
wrong about this shaped the milestone order and that is worth remembering.

Measured, and the constraint turned out to be somewhere else.** BLE costs
296764 bytes of flash and about 145KB of heap. Flash is not the problem: the
build sits at 791793 of 3211264, leaving 2.4MB, and a WiFi stack with a web
server is 400-600KB. Both fit with room to spare.

Heap is the problem, or will be. Free heap at the prompt fell from 266KB to
121KB when NimBLE came in. WiFi wants its own, and the two stacks running at
once on a 320KB part is the real question milestone 8 has to answer. It is a
runtime measurement, not a build one, so it cannot be settled by looking at a
binary.

The way out, if it comes to that, is that they never need to be up at the same
time: SYNC is a thing you start deliberately and finish, not a service that
runs. Shutting the BLE stack down for the duration of a sync and bringing it
back afterwards costs a reconnect, and both `BleKeyboardHost::end()` and
`begin()` already exist for exactly that.

## Milestones

Each one ends with something demonstrable on the board, not with a file
compiling.

1. **Hardware bring-up.** Panel geometry, touch calibration, SD mount.
   **Done**, except the SD probe, which needs a card. Panel geometry and touch
   confirmed on the board on 2026-08-31; see docs/HARDWARE.md.
   `editor/src/main.cpp`.
2. **Render layer.** **Done**, all four SCREEN modes.
   `editor/src/tft_renderer.{h,cpp}` offers the eighteen `GfxRenderer` methods
   that `port-staging/src` actually calls, with matching signatures, so ported
   code compiles against it unchanged. Metrics agree exactly with the grid:
   every column count measures 480 pixels. Speed is in "The render budget"
   below.
3. **Touch and the on-screen keyboard.** **Done.** `osk.cpp` came over with
   nothing changed but its include and the renderer's type name: its layout was
   already written in proportional half-units against a caller-given rectangle,
   so it re-flowed to 480x320 on its own. Six rows at 32 pixels, which leaves
   **7 terminal rows of 60 columns** while the keyboard is up, against 19 with
   it folded away. Redrawing the keyboard costs 34ms, so it only happens when
   it actually looks different, which is a modifier arming or a one-shot Shift
   clearing itself. Keys are square here, not rounded as on the PaperS3: at half
   the size the dual-legend Shift hint landed on the corner curve. Squaring them
   also cut the keyboard's draw time by a third, because a filled rectangle is
   one block where a rounded one is four arcs walked pixel by pixel.
4. **Terminal.** **Done.** `screen_editor.cpp` and `config.h` are ported. The
   character grid, its scrolling and the continuation-chain logical-line
   tracking all came over unchanged; the one real change is that the row count
   and centring margin are derived from a band the caller sets, rather than
   being two more columns of the per-mode table. The PaperS3 stores both, and
   its numbers are measurements of a 960x540 panel with a 30px bar: correct
   there, meaningless anywhere else. Enter handles CLS and SCREEN, which are
   terminal operations rather than language ones, and answers anything else the
   way a BASIC does when it does not understand.
5. **Interpreter.** **Done.** `fetch.sh` pulls Stefan Lenz's IoT BASIC at its
   pinned commit and applies the six patches; `tb_bridge.cpp`, `tb_runtime.cpp`,
   `terminal_input.cpp` and `input_handler.cpp` are ported. The one real change
   is that the runtime's file I/O moves from freeink-sdk's `SDCardManager` and
   its SdFat `FsFile` handles to Arduino's `SD` library, because the card is on
   its own bus here and pulling in an e-paper SDK for file handles is not a
   trade worth making. Paths are unchanged, so a card moves between this and the
   PaperS3.

   Flash with the interpreter linked: 466429 of 3211264 (14.5%). RAM 48796 of
   327680, which is the interpreter's 16KB of program memory plus its state.

   The `microwriter` env stops building at this milestone, on purpose and with
   one `#error` rather than a wall of linker failures: what it excludes is
   exactly what `main.cpp` draws, and its replacement is milestone 7.
6. **Storage and the prose editor**, which are one milestone and not two.
   Splitting them was a guess made before reading the code: `file_manager.cpp`
   and `file_browser.cpp` both include `text_editor.h`, and the browser calls
   about twenty of its functions, because it is what dispatches editing keys to
   the editor. There is no order in which one lands without the other.

   `sd_datetime.cpp` was the one genuinely separable piece and is **done**,
   rewritten rather than ported: this board has no RTC and no SdFat, and
   ESP-IDF's FATFS is built with `FF_FS_NORTC = 0`, so timestamps already come
   from the system clock and there is no callback to register. What is left is
   having a time to set, which follows the X4's principle of preferring
   something the device itself wrote over anything guessed now.

   `text_editor.cpp`, `file_manager.cpp` and `file_browser.cpp` are **done** and
   reachable from the EDITOR button and the MENU command. MicroWriter writes in
   unscii rather than NotoSans: those headers are 2.6MB across four weights and
   this build has WiFi and BLE still to fit. Reversible for about 630KB if
   proportional type is wanted.

   The `microwriter` env builds and boots. It excludes the interpreter, the
   character-grid terminal and the command dispatch built on them, so the
   browser opens at boot and never closes: there is nothing behind it to go back
   to. Its bar drops SCR, which is a terminal mode, and EDITOR, which would open
   what is already open, and the nameplate takes their space.

   | Build | Flash | RAM |
   |---|---|---|
   | MicroBASIC | 495029 of 3211264 | 75036 of 327680 |
   | MicroWriter | 441833 of 3211264 | 48792 of 327680 |
8. **Network.** **Done.** `wifi_sync.cpp`, `web_files_page.h` and `sd_backup.h`
   ported, with the same SdFat-to-Arduino-SD substitution the storage files
   needed. Sync verified in both directions on the board, and a program uploaded
   over WiFi runs.

   The mDNS name follows the machine: `microbasic-cyd.local` or
   `microwriter-cyd.local`.

   **Both radios fit.** NimBLE and WiFi run at once inside the heap left over,
   with the keyboard live while the HTTP server serves. The contingency of
   stopping BLE for the duration of a sync is not needed.
9. **BLE keyboard.** **Ported and running**, out of plan order because it was
   the expensive question. `BleKeyboardHost` came across from freeink-sdk with
   one substitution: its `BoardConfig.h` include, which supplied four build
   switches and not the one assumed, replaced by a local `BleHostConfig.h`
   carrying those four with their defaults. `CONFIG_BT_NIMBLE_EXT_ADV` is
   deliberately not set: extended advertising is a Bluetooth 5 feature and this
   is a classic ESP32 with a 4.2 radio.

   Auto-pairing, the passkey display and the BLE button's live state are all
   ported. **Verified on the board**: a keyboard pairs, types, and loads, edits
   and creates programs.

   (Original note, kept because the reasoning stands.) No longer conditional on
   milestone 8 leaving room: the plan for this device is a physical keyboard
   with the on-screen one as fallback, the same shape as the PaperS3. That
   makes it a requirement rather than a nice-to-have, and it moves the flash
   question from "does BLE fit" to "what gives way if WiFi and BLE together do
   not". Worth measuring before milestone 8 rather than after.

## What is not coming across

- `ota_apps.cpp` and `docs/DUAL_BOOT.md`. Both exist for the PaperS3's shared
  dual-boot contract with CrossPoint. This board has one app partition and one
  firmware.
- `vc_browser.cpp` and the CrossPoint reading-progress side of sync.
- Everything in `freeink-sdk`. It is an e-paper SDK; nothing in it applies. Its
  useful abstraction, the `hal/` seam, is worth copying as a shape rather than
  as code.
- The BM8563 RTC path in `sd_datetime.cpp`.
