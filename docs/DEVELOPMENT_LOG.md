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

## 2026-08-31 -- An MSX palette, and what the machine will actually look like

The render demo now boots into a screen shaped like MSX BASIC's: what the
machine is, how much memory is free, a blank line, `Ok`, and a blinking block
cursor. The numbers in it are read from the board at that moment rather than
mocked up, which is the point of showing it on the panel instead of in a
mockup somewhere else.

Colours are the real TMS9918 ones: white (colour 15) on dark blue (colour 4,
RGB 89,85,224). That blue is a periwinkle rather than the navy people tend to
remember, and it is what the machine actually put on a television. 0x5ABC in
RGB565.

SCREEN 1 here is 40 columns, which is exactly MSX BASIC's text width. That fell
out of the panel arithmetic rather than being aimed at, but it is a good reason
to make SCREEN 1 the boot mode.

The status bar draws on the paper colour rather than as a solid ink bar. On a
home micro the screen is one background colour throughout and a bright bar
across the top breaks the illusion the palette exists to create. The real
status bar can decide this on its own terms when it is designed.

Timings at SCREEN 1, which is a smaller cell than SCREEN 0: full repaint 41ms,
one cell 160us.

## 2026-08-31 -- All four SCREEN fonts generated, and one of them bypasses the pipeline

`research/fonts/tools/emit_epdfont_header.py` was retargeted from the PaperS3's
sizes to this panel's four, and all four now render on the board with exact
metrics: every column count measures 480 pixels, every line height equals its
cell height.

Two of the four already existed in the PaperS3 build and were regenerated
rather than copied. They came out **byte-identical** to that build, which is a
free and rather strong check: the pipeline is deterministic and the retargeting
changed no behaviour, so the two new sizes came out of exactly the same path
that was already validated on hardware.

**8x16 deliberately bypasses the resize pipeline.** unscii-16's source cells
are 8x16, so the area-coverage resize would be an identity transform, but the
`cap_stem_width` post-pass that follows it would still run over glyphs a human
drew to be read at precisely this size. Nothing for it to fix, something for it
to break. It goes through plain `HexFont` and emits the source bitmap untouched.

That also settles the "10x20 is the smallest still readable" floor the two
earlier projects set, which was a pixel count on their panels rather than a
physical size. This panel is 480px across roughly 74mm, so an 8px cell is
1.23mm wide; the PaperS3's own smallest mode is 1.30mm on its panel. Nearly the
same character on the eye, and unlike a resampled cell this one is a font drawn
for its size.

The 64-column tier from the two earlier devices is dropped. 480/64 is 7.5, not
a whole number of pixels, and carrying the old 32/48/64/80 lineage onto a panel
half the width would have produced tiers nobody can read. The four are re-cut
to what 480 divides by.

Flash with all four fonts linked: 356589 bytes of 3211264.

## 2026-08-31 -- Milestone 3, and a correction to the milestone 2 entry

**First the correction.** The milestone 2 entry says `TftRenderer` implements
the fifteen `GfxRenderer` methods the ported code calls. The count was wrong,
and so was the method that produced it. The scan used a regex anchored with
`\b` before `renderer`, which never matches inside `g_renderer` because an
underscore is a word character, so every call made through a pointer named
`g_renderer` was invisible to it. `osk.cpp` calls the renderer that way
throughout.

Redone properly, by extracting the public method names from `GfxRenderer.h`
and grepping each one against `port-staging/src`, the surface is **eighteen**.
The three missed were `fillRoundedRect`, `drawRoundedRect` and
`supportsAsyncRefresh`, all now implemented. The first two brought
`GfxRenderer`'s `Color` enum with them.

`Color` is the one place where this device is straightforwardly better than the
two before it. On e-paper `LightGray` and `DarkGray` are Bayer dither
densities faked out of pure black and white, and `osk.cpp`'s own comment worries
that "a dense checkerboard immediately behind small text can read as visually
busier than a real gray keycap would ... worth a second look on the physical
panel specifically for that". Here they are real blends between the palette's
ink and paper. There is nothing to look at.

**Milestone 3 itself was almost free.** `osk.cpp` and `osk.h` came over with
two substitutions, the include and the renderer's type name, and compiled
first try. That is the whole return on having matched `GfxRenderer`'s
signatures instead of inventing a cleaner interface: the keyboard's layout was
already written in proportional half-units against a caller-given rectangle
(`g_unitPx = width / kUnitsPerRow`, `g_rowH = height / kRowCount`), so it
re-flowed from 960x540 to 480x320 with no geometry work at all.

The layout answer this milestone existed to produce: six rows at 32 pixels is
192 pixels of keyboard, leaving **7 terminal rows of 60 columns** while typing,
against 19 with the keyboard folded away. A 2-half-unit key comes out about
4.6mm across on this 74mm-wide panel, which is a fingernail or stylus target
rather than a fingertip one. Whether that is acceptable is a question for the
hand, not the arithmetic.

Timings: the keyboard alone is 49ms to draw, the whole screen with it 103ms.
49ms is enough to feel, so the keyboard is redrawn only when it actually looks
different, which is a modifier arming or a one-shot Shift clearing itself on
the next character. A plain keystroke changes one row of text and nothing else.

The echo area in `main.cpp` is not the terminal and none of it should survive:
`screen_editor.cpp` is milestone 4. It exists to show that a tapped key becomes
a character.

## 2026-08-31 -- Two keyboard fixes from actually using it

Both came from tapping the thing, which is the only way either would have
surfaced.

**Square keycaps, not rounded.** The PaperS3 uses a 6px corner radius and it
looks right there. Here the small Shift-hint that digit and symbol keys carry
in their top-right corner lands on the curve, because the keys are half the
size and the radius was not scaled with them. Radius goes to 0.

That needed a guard in the renderer: TFT_eSPI's round-rect primitives draw four
quarter-circle arcs and a zero radius is not a case they are written for, so
`fillRoundedRect` and `drawRoundedRect` take a plain-rectangle path when the
radius is zero or less. `drawRoundedRect` also had to stop its per-ring radius
going negative on the inner rings of a thick border.

It came with a speedup nobody was looking for: the keyboard's draw dropped from
49ms to 34ms, a third off, because a filled rectangle is one block where a
rounded one is four arcs walked pixel by pixel.

**Rows 3 and 4 now reach the right edge.** Both summed to 31 of 32 half-units,
leaving a ragged gap while row 0's Enter ran to the edge, and the right Shift at
2 units is 30 pixels on this panel, wide enough for "Sh" and no more.

Widening the right Shift alone would have broken the three-column cluster rows 3
and 4 line up in (`/` over Left, Up over Down, right Shift over Right). The
arithmetic says why the original was stuck: with the left Shift at width `a`,
keeping Up above Down forces Space to `a+10`, and both right-hand keys then come
out at `10-a` each. At `a=7` that is 2 and 2, which is where they were.

So the width comes out of the left Shift, 7 to 6, which had plenty to spare.
Both right-hand keys become 4 units, 60px, and every column of the cluster still
lines up:

```
row 3:  Shift(6) | 10 keys      | /  | ^  | Shift(4)
row 4:  Ctrl(5) Alt(3) Space(16)| <  | v  | >    (4)
units:  0                     24| 24 | 26 | 28  32
```

Confirmed on the panel before this: the keys are hittable with a fingertip, not
just a fingernail, and one-shot Shift behaves.

## 2026-08-31 -- The keycap gap was a constant that did not travel

Both of these came from looking at the keyboard on the panel rather than at the
code, and the second is the more interesting.

**Row 4 keeps its arrows one size.** The previous change widened the right
Shift and the Right arrow together, because the arithmetic of the three-column
cluster forces those two keys to the same width. Seen on the panel, the three
arrows matching each other matters more than the bottom-right key matching the
Shift above it. So Right goes back to 2 half-units and the last two are an
unlabelled Shift instead of a gap. It lands directly under the right half of
row 3's Shift, which is where a second Shift belongs anyway, and a filler that
does something beats a filler that does not.

**The inset was tuned for a panel twice this wide.** `kInset` was 4 in both
projects, and that is 13% of the PaperS3's 30px half-unit but 27% of this
panel's 15px one. The margin doubled in visual weight without anyone touching
it, and the keys read as small squares swimming in space. It is now derived
from the unit (`g_unitPx / 7`), which lands at 4 on the PaperS3's geometry and 2
here, and stays right on whatever comes next.

Keycaps went from 22x24 to 26x28 pixels with the gap between them halving from
8px to 4px. Drawing the keyboard costs 36.7ms rather than 34ms, which is
just the extra area.

Worth keeping in mind for the rest of the port: this is the first constant found
that was silently wrong rather than visibly broken, and it was wrong because it
was absolute where it should have been proportional. There are probably others.
Anything in the ported sources expressed in pixels rather than in a ratio of
something is a candidate.
