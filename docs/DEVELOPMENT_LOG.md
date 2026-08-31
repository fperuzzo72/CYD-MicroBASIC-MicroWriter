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

## 2026-08-31 -- Milestone 4: the real terminal, and a policy decision reversed

`screen_editor.cpp` and `config.h` are ported. The character grid, the
scrolling, and the continuation-chain logical-line tracking (the thing that
makes "LIST, arrow up onto a wrapped line, edit in place, Enter" read the whole
line) came over with no changes at all.

The one real change is that the row count and the centring margin are derived
from a band the caller sets, instead of being two more columns of the per-mode
table. The PaperS3 stores both, and its numbers are measurements of a 960x540
panel with a 30px status bar. Correct there, meaningless anywhere else, and
silently wrong if either moves, which is the same shape of problem as the
keycap inset two entries ago.

**The policy on that band was decided, then reversed, and the reversal is the
right call.** The first implementation had the band shrink when the on-screen
keyboard came up: 19 rows folded away, 7 with it open, content scrolling to
keep the cursor visible. The reasoning was that on this device the keyboard is
the only way in, so drawing rows behind it would leave the cursor hidden, since
a terminal's cursor lives at the bottom of the used area.

That reasoning was wrong about the premise, not the logic. The plan for this
device includes a physical keyboard, the same as the PaperS3, with the
on-screen one as the fallback it is there. So the band is the whole area below
the status bar, always, and the keyboard is painted over the bottom of the grid.

The mechanism stayed even though the policy changed, and it earns its place:
it is what makes a SCREEN mode change safe (the row count changes with the cell
height), it replaces the hardcoded margins, and making the band follow the
keyboard again is one line in `applyBand()` if the on-screen keyboard ever
becomes the normal way in.

Hiding the keyboard reveals everything that was behind it. The grid is never
truncated, only covered.

Timings: one terminal row of 60 columns at 8x16 is 3.7ms, and the first full
paint with the keyboard up is 143ms, against 99ms when the grid only drew the
7 visible rows. About 44ms of that is rows composed, pushed, and immediately
covered by the keyboard. Skipping them while the keyboard is up would return it
and costs one condition in the row loop.

Enter handles CLS and SCREEN, because those are terminal operations rather than
language ones, and answers anything else the way a BASIC does when it does not
understand. Milestone 5 replaces that with the interpreter.

## 2026-08-31 -- Milestone 5: the interpreter, and a wrong call corrected mid-port

`fetch.sh` pulled Stefan Lenz's IoT BASIC at its pinned commit and all six
patches applied unchanged. `tb_bridge.cpp`, `tb_runtime.cpp`,
`terminal_input.cpp` and `input_handler.cpp` came over.

**The integration turned out to be a deletion.** `main.cpp` had grown a
hand-rolled Enter handler in milestone 4, with its own CLS and SCREEN and its
own idea of which rows to redraw. All of it is gone. The chain that replaced it
was already written, in three files that had never met on this device:

```
osk.cpp emits a HID keycode and modifier byte
  -> enqueueKeyEvent(), the same call a BLE keyboard makes
    -> processAllInput() in terminal_input.cpp
      -> screen_editor for editing keys, tbExecuteLine() for Enter
        -> the interpreter, printing back through the runtime's outch()
```

`osk.h` promised exactly this two milestones ago ("the SAME wire format
`input_handler.cpp::enqueueKeyEvent()` already expects"). It held. `onOskKey`
is now one line.

**The file I/O had to be rewritten, and I called it wrong first.** Grepping
`tb_runtime.cpp` for `SDCardManager`, `SD.` and `sdCard` found only the include
and a comment, so it went down as another dead include like the two before it.
It is not: the file reaches the card through `SdMan` and `FsFile`, neither of
which those patterns match. The compiler found what the grep missed, which is
the second time on this port that a regex has been the weak link.

The replacement is Arduino's `SD` library. `SDCardManager` wraps SdFat and
lives in freeink-sdk, which is an e-paper SDK with a panel driver and a power
manager attached; pulling all of that in for file handles is not a trade worth
making when this board's card sits on its own bus with nothing to arbitrate.
The two APIs line up almost exactly, so it was a substitution across about
twenty lines rather than a rewrite. Two places genuinely differ: open modes are
strings rather than `O_*` flags, and `File::name()` returns a full path where
`FsFile::getName()` filled a buffer with a bare name, so the separator has to
be stripped or `FILES` would list paths and `LOAD` would not find them.

Paths are deliberately unchanged from the PaperS3, so a card carries between
the two machines.

**Six symbols were missing and are now defined out loud.** Two are physical
button hooks, and this board genuinely has none, so they are permanent no-ops
rather than milestones. `SYNC` and `EDITOR` say they are not built yet.
`READER` and `VC` say they are PaperS3-only, because they exist for that
device's shared dual-boot layout with CrossPoint and have nowhere to go here. A
command that does nothing looks like a bug; one that says what is missing is a
to-do list readable from the machine itself.

Flash with the interpreter linked: 466429 of 3211264, 14.5%. RAM 48796 of
327680, which is the 16KB of BASIC program memory plus interpreter state. Free
heap at the prompt drops from 321KB to 266KB.

The `microwriter` env stops building here. What it excludes is exactly what
`main.cpp` draws, and the replacement is milestone 7. It fails on one `#error`
saying so rather than eighteen linker errors, which is the difference between a
note and a puzzle.

## 2026-08-31 -- The keyboard vanished, and the fix was the optimisation

Typing the first character of a program line erased the on-screen keyboard,
while the keys kept responding to taps in the space where they used to be.

The cause is the interpreter integration, not the keyboard. Milestone 4 redrew
one terminal row per keystroke; milestone 5 routes keystrokes through
`processAllInput()`, which can change any part of the screen, so `main.cpp`
started repainting the whole terminal after each one. The grid keeps its full
height and the keyboard is painted over the bottom of it, so repainting the
grid paints over the keyboard. `osk.cpp` hit-tests coordinates and has no idea
whether it is still on screen, which is why the machine looked like it had lost
its keyboard while still obeying it.

The obvious fix is to redraw the keyboard afterwards, at 37ms a keystroke. The
right one is to stop drawing the rows that are behind it, which cannot erase
what it does not touch. That is also the 44ms saving noted two entries ago as
worth one condition in the row loop, so the bug and the optimisation are the
same change. First paint went from 145ms to 100ms.

A row straddling the keyboard's top edge counts as hidden: skipping it leaves
the keyboard's own edge showing, where drawing it would put a stripe of text
across the top row of keys.

The blinking cursor needed the same guard for the same reason. With the cursor
on a row behind the keyboard it was punching a block through the keys twice a
second.

**What this exposes is not a bug, it is the cost of a decision.** With the
keyboard up at SCREEN 3 only seven of the nineteen rows are visible, and the
terminal does not scroll until the cursor reaches row 18. So typing past the
seventh line puts the cursor somewhere real but invisible until the keyboard is
folded away. That is the accepted consequence of drawing the full grid, which
is right once a physical keyboard is the normal way in, and awkward until
milestone 9 makes that true. `applyBand()` is still one line from making the
band follow the keyboard if the wait proves annoying.

## 2026-08-31 -- Esc could not stop a program, and the hook to fix it was stubbed out

`RUN` on a `20 GOTO 10` could not be stopped. Esc did nothing, and the keyboard
looked frozen for as long as the program ran, which was forever.

The cause was a stub written two commits earlier. `tb_runtime.cpp`'s `byield()`
calls `pumpPhysicalButtonsForProgram()` every sixteen statements, and the
comment right above that call says exactly why it exists:

> The d-pad only reaches a running program through here: loop() is blocked
> inside the interpreter for the whole run.

It was stubbed as a permanent no-op on the reasoning that a board with no
buttons has no buttons to pump. That is true about buttons and wrong about the
hook. Its job is not "read the d-pad", it is "get input to a running program",
and on this device the input device is the panel. With it empty, nothing polled
the touchscreen during a run, so Esc never reached the queue, `checkch()` never
saw a break, and the machine kept drawing and printing while being unstoppable.

Now it polls the panel and dispatches to the keyboard, which enqueues exactly
as `loop()` would. Throttled to 25ms rather than run on every call, because
`byield()` reaches it hundreds of times a second and a touch read is an SPI
transaction; 25ms is far faster than a finger and the same interval debounces
the resistive panel.

Worth generalising: the three stubs still standing (`SYNC`, `EDITOR`, and the
two PaperS3-only commands) were checked again after this. Those are genuinely
unbuilt features, and they announce themselves on screen rather than doing
nothing. This one was different in kind: a hook the ported code depends on for
correctness, emptied because its name described the X4's hardware rather than
its purpose. A stub is safe when it stands for a feature and dangerous when it
stands for a mechanism.

## 2026-08-31 -- The band follows the keyboard after all

Reversed again, and this time from use rather than from reasoning. The full
grid is right once a physical keyboard is the normal way in, but there is no
physical keyboard yet, so in practice every line typed past the seventh went to
a row that existed, held text, and could not be seen. Nineteen rows of which
twelve are blind is worse than seven that all work.

The mechanism was kept through the previous reversal precisely so this would be
one line, and it was. Folding the keyboard away repaints everything and the
terminal returns to nineteen rows; folding it in scrolls the content so the
cursor survives, which `screenEditorSetBand` already did.

Revisit at milestone 9. This is a decision about which input device is normal,
not about the panel, and it changes when that does.

## 2026-08-31 -- Caps Lock lit the labels and changed nothing else

Tapping Caps made the keyboard's letter keys show uppercase, and then typed
lowercase into the terminal. Which is a good symptom, because it says the
keyboard knew and the machine did not.

There were two Caps Lock flags:

```
osk.cpp:210           g_capsLockOn        toggled by the Caps key, read by the labels
input_handler.cpp:25  capsLockOn = false; // reserved: nothing currently sets this true
input_handler.cpp:61  bool shifted = isShift(modifiers) ^ capsLockOn;
```

The keystroke that reaches the screen editor goes through `hidToAscii()`, and
its flag was the one nobody set. The comment beside it said so in plain words
and had been true on the PaperS3 too; it just never showed there, because that
device is normally driven by a BLE keyboard whose own Caps Lock the host does
see.

`osk.h` had already named the fix, two milestones before the bug appeared:

> Duplicated here rather than shared because input_handler.cpp isn't ported yet

It is ported now, so the duplication went rather than being patched. `osk.cpp`
no longer keeps a Caps Lock, no longer reimplements `hidToAscii`'s rules, and
sends `HID_KEY_CAPSLOCK` as a key when Caps is tapped. `enqueueKeyEvent()`
consumes that key and toggles the one flag there is, which is what a real
keyboard does: the modifier byte has no Caps bit, so the keyboard reports a
press and the host holds the state.

Thirty-one lines of duplicated conversion table deleted with it.

The shape of this is worth noting next to the `pumpPhysicalButtonsForProgram`
entry above. Both bugs were dormant code that described itself accurately and
was believed anyway: one a stub whose comment explained the mechanism it was
standing in for, the other a flag whose comment said nothing set it. Reading
those comments as descriptions of the past rather than statements about the
present is what cost the time in both cases.

A BLE keyboard's Caps key will now work through this same path with nothing
added, which is the part that matters for milestone 9.

## 2026-08-31 -- A status bar with buttons, and the height to press them

The bar carried three invisible gestures: tap the left third for SCREEN, the
middle for the keyboard, the right for the palette. Discoverable only by being
told, which is not a user interface.

Six buttons now, modelled on the PaperS3's bar with two differences. READER and
VC are gone, since both exist for that device's dual-boot layout with
CrossPoint and have nowhere to go here. SCR and COLOR take their place.

BLE, SYNC and EDIT are drawn before they work, deliberately. A button showing
"--" says the machine has a place for that and does not have it yet, which is a
truer picture than a bar that sprouts a new button every few weeks. Tapping one
prints what is missing, through the same functions the MENU commands use, so
there is one place saying it rather than two that can drift.

**The bar went from 16px to 32px, and the arithmetic is why it is exactly 32.**
16 was enough to read and not enough to hit. 32 matches the keyboard's own row
height, gives each button a label over a value, and keeps the divisions exact
where they matter: 320 - 32 = 288, which is 18 rows of the 8x16 cell with
nothing left over, and 288 - 192 of keyboard leaves 96, which is 6. It costs one
terminal row, 19 to 18.

Each button shows its state rather than just its name: KBD says on or off, SCR
says which mode, COLOR says which palette. The Shift and Caps indicators that
used to sit in the bar are gone, not lost: the keyboard draws its own armed
keys inverted, which is the same information where the finger already is.

Six rows of terminal while typing now, down from seven. That is the number to
watch: if it gets uncomfortable before milestone 9, the answer is a shorter
keyboard rather than a shorter bar, because the bar is now the only thing on
screen that can be pressed on purpose.

## 2026-08-31 -- Descenders measured, and the terminal gets a window instead of a size

**Two pixels, and they were measurable rather than guessable.** The value line
of each bar button had "green" and "paper" hanging their tails past the button
border. Reading the ink extents straight out of `unscii_8x16.h` rather than
eyeballing it: g, p, q and y ink rows 6 to 15 of the 16-row cell, with no
padding at the bottom at all, while capitals only reach row 12. Drawn at y=16
inside a 32px bar, a descender lands on row 31 and the border is on row 30.
Moving the value to y=14 puts its ink at rows 16 to 29, exactly inside the
usable area, with three clear pixels under the label.

**The terminal band is now a window, and this is the third arrangement.** The
first gave the terminal the whole panel and painted the keyboard over it, which
left the cursor behind the keys. The second shrank the grid to what the keyboard
left, which kept the cursor visible and threw away every row that no longer
fitted, so folding the keyboard away revealed blank space where the history had
been.

The third keeps the grid at its full 18 rows always and moves a window over it.
Six rows show while the keyboard is up, eighteen when it is folded away, and
nothing is ever discarded because the grid never changes size. The window
follows the cursor rather than pinning to the bottom, so a fresh screen shows
its top and a full one shows its end, which is where a terminal's attention is.

The part that makes this the right answer rather than just a better one: it
stops being a decision about which keyboard is normal. Both earlier
arrangements had to be re-argued as soon as the premise moved, and both were
re-argued, twice. A window behaves the same whether the on-screen keyboard is
the only way in or an occasional visitor beside a BLE one, which is exactly
what milestone 9 will need without anyone having to remember to change it back.

`screenEditorSetBand` keeps its job, sizing the grid to the panel. What moved
out of it is the assumption that the grid and the visible area are the same
rectangle.

## 2026-08-31 -- Ctrl armed invisibly, which is why Ctrl+C took three tries

Two symptoms, one cause, and the cause was a function I wrote three entries ago
that does its job and only its job.

`pumpPhysicalButtonsForProgram()` is the only thing polling the panel while a
program runs, since `loop()` is blocked inside the interpreter. It dispatched
taps to the keyboard and drew nothing. Everywhere else, arming Shift or Ctrl is
followed by a redraw so the key shows inverted; there, it was not.

So Ctrl armed correctly and looked like it had not. The natural response is to
tap it again, which disarms it, and the C that follows is then an ordinary
letter. Two taps out of three do nothing and the third works, which is exactly
the "third attempt" reported. The break itself was never broken: `Esc` and
`Ctrl+C` both reach `pumpProgramInput()` and both set `breakPending`.

The same blind spot explains the status bar not responding mid-run: that path
never looked at the bar at all.

Both fixed by giving the running-program path the same two behaviours the idle
one has: redraw the keyboard when an armed modifier changes, and handle a bar
tap. The bar handler is now one function shared by both, rather than a copy,
because a copy is how the two came apart in the first place.

While a program has control, only KBD and COLOR are live. SCR would reset the
grid out from under a program printing into it, and the three placeholders
would interleave a "not built yet" line with the program's own output. Neither
is something a bar tap should be able to do mid-run.

## 2026-08-31 -- The machine's name in the bar

Buttons narrowed from 80px to 60px, which frees 120px on the left for
"MicroBASIC" over "CYD FNK0103-N", using the two lines the bar already had. The
PaperS3's bar does the same with the space its own buttons leave.

60px still clears every label: "EDITOR" is the longest at 48px in the 8x16
cell, and the palette names are 40px. Nothing had to be abbreviated to make
room.

## 2026-08-31 -- Milestones 6 and 7 are one milestone, and sd_datetime is a rewrite

**The plan had storage and the prose editor as separate milestones, and the
code says otherwise.** `file_manager.cpp` and `file_browser.cpp` both include
`text_editor.h`; the browser calls about twenty of its functions, because it is
what dispatches editing keys into the editor. There is no order in which one
lands without the other. The split was a guess written before reading them, and
it is now one milestone.

**`sd_datetime` was the separable piece, and porting it would have been the
wrong verb.** The PaperS3 version registers an SdFat callback
(`FsDateTime::setCallback`) and fills it from a BM8563 RTC. This board has
neither, and its SD library is not SdFat: Arduino's goes through ESP-IDF's
FATFS, which is configured `FF_FS_NORTC = 0`, so timestamps are already enabled
and taken from `get_fattime()`, which reads the system clock. There is no
callback to register. Set the clock and files are dated.

Which leaves the part that was always the hard bit: having a time to set it to.

The X4 answers that, and the answer travels even though its mechanism does not.
MicroBASIC there has no clock either, and reads the last valid timestamp the
CrossPoint reader left in `/.crosspoint/state.json` on the same card. No reader
here and no such file, but the principle is the thing: prefer something the
device itself wrote while it knew, over anything guessed now. What this device
wrote is its own saved programs, so the clock is seeded from the newest file
under `/MicroBASIC/programs`, or the firmware build date, whichever is later.

That matters more here than on either earlier machine. WiFi is milestone 8 and
is the piece that gives way if flash runs short, so the build date may be the
only real time this machine ever receives. Without the card seed, every boot
resets to the same instant and every file ever saved carries an identical
timestamp, which defeats the single thing a timestamp is for.

Confirmed on the board: `clock: 2026-08-31 13:50 UTC (seeded from build date)`.

## 2026-08-31 -- The prose side: text_editor, file_manager, file_browser

All three ported. `text_editor.cpp` needed nothing at all: it depends only on
`Utf8` and asks the caller for a per-codepoint glyph width, so the font is not
its problem. `file_manager.cpp` needed the same SdFat-to-Arduino-SD
substitution `tb_runtime.cpp` had, plus `rename`, plus one cast where
`File::read` takes `uint8_t*` and SdFat's took `void*`. `file_browser.cpp`
needed nothing: it is pure state, and drawing lives in `main.cpp`.

The constants the prose side needs went back into `config.h` now rather than at
the start, which is what the note at the top of that file said would happen:
`FileInfo`, `MAX_FILES`, `TEXT_BUFFER_SIZE`, `MAX_LINES` and the auto-save
intervals. RAM went from 48KB to 75KB, almost all of it the 16KB text buffer
and the 1024-entry line table.

**MicroWriter writes in a monospace font here, and that is a decision worth
naming.** The PaperS3 draws notes in NotoSans; those headers are about 2.6MB
across the four weights and none is linked in this build, on a board whose app
partition is 3.2MB and still has WiFi and BLE to fit. unscii is already in the
binary, costs nothing, gives 60 characters a line at 8x16, and makes the wiring
simpler because a monospace glyph width is a constant. Reversible: NotoSans 14
regular and bold would be roughly 630KB of the 2.7MB free, and the only code
that changes is `editorGlyphWidth()`.

**Two chains that must agree.** The PaperS3's `loop()` carries a warning worth
repeating: the key-routing chain and the paint chain have to test the same
conditions in the same order, and they disagreed there once. Nothing noticed
until MicroWriter, where the browser is always open, and then a screen drew but
could not be typed into because its keys were going to whatever was behind it.
Both chains here are written next to each other with that comment attached.

**Two process notes on my own working, because both cost time today.**

A `str.replace` that finds nothing is silent. The prose block was anchored on a
function I had deleted myself two changes earlier, so the edit did nothing and
the failure surfaced as six "not declared in this scope" errors that looked
like a missing include. Anchors are asserted now before replacing.

And `screenDirty` was already defined in `input_handler.cpp`. I declared a
second one in `main.cpp` from reading the PaperS3's `extern` and assuming it
had no owner here. The linker caught it, which is the good case; the bad case
would have been two variables with one name in different translation units.

## 2026-08-31 -- The whole screen flashed on every keystroke in the editor

Typing in the prose editor blanked and repainted the entire panel per
character.

The terminal never did this, and the difference says why. A keystroke there
redraws one row with `drawTextOpaque`, which composes background and glyphs
together and pushes them in a single transfer. A keystroke in the browser went
through `drawAll()`, which starts with `clearScreen()` and then paints
everything back. Two passes over the same pixels, and the first one is what the
eye sees as a flash.

This is milestone 2's lesson arriving somewhere it had not been applied. The
fix is not to redraw less, it is to redraw once: every path that owns the band
now covers all of it, including the rows past the end of the text and the rows
past the end of a list, which get an opaque empty draw rather than being left
to a prior clear. With the band fully covered there is nothing to clear.

`drawBand()` is what a keystroke calls now. `drawAll()`, with its clear, is
kept for when the layout itself changed: the keyboard folding away, the palette,
the SCREEN mode. Those genuinely need it, because the gaps between the
keyboard's keys are painted by nothing else.

Two screens still clear first and say so: the title prompt and the "nothing
here" message draw three short lines and cannot cover the band by drawing it.
Neither is a per-keystroke path.

## 2026-08-31 -- MicroWriter builds and boots, and milestone 6 closes

The `microwriter` env stopped at an `#error` for two milestones. It builds now,
and more to the point it runs: flashed to the board, the browser opens at boot
and lists the notes already on the card.

The guards are `#if` rather than runtime checks for the reason platformio.ini
gives: the excluded files are not compiled, so their symbols do not exist to be
called. What is guarded is the terminal's row maths, its drawing, the
interpreter's setup, and the key path that feeds it.

Two smaller decisions came out of doing it.

The bar drops SCR and EDITOR on MicroWriter. SCR is a terminal mode and there
is no terminal; EDITOR would open what is already open. Rather than leave two
dead buttons or restructure the table, a width of zero removes a button
entirely: it is not drawn, not hit, and its space falls to the nameplate, which
reads "MicroWriter CYD" there. The hit test had to learn to skip zero-width
entries, which is the sort of thing that would have gone quietly wrong.

`notify()` replaced the direct calls to `screenEditorTermPrintLine`. The four
"not built yet" answers had been writing to a terminal that MicroWriter does
not have; they now write wherever the machine in hand can show a line, which is
the browser's status line there.

| Build | Flash | RAM |
|---|---|---|
| MicroBASIC | 495029 of 3211264 (15.4%) | 75036 of 327680 |
| MicroWriter | 441833 of 3211264 (13.8%) | 48792 of 327680 |

Six milestones done, three to go: the network, the BLE keyboard, and whichever
of those two the flash budget forces a choice between. On today's numbers there
is 2.7MB of app partition unused, which is a much better position than the
plan assumed when it was written.

## 2026-08-31 -- BLE measured first, and the constraint moved

Done out of plan order, because it was the expensive question and the plan was
built around a guess about it.

| | Flash | Free heap at the prompt |
|---|---|---|
| Without BLE | 495029 | 266KB |
| With BLE | 791793 | 121KB |
| Cost | +296764 | -145KB |

**Flash is not the constraint and never was.** 791793 of 3211264 leaves 2.4MB,
and a WiFi stack with a web server is 400-600KB. The whole "which one gives
way" question, which shaped the plan from the first day, does not arise: both
fit with room to spare.

**Heap is the constraint, or will be.** 121KB free with NimBLE up, and WiFi
wants its own. Two stacks at once on a 320KB part is what milestone 8 actually
has to answer, and it is a runtime measurement, so no amount of staring at a
binary settles it.

There is a way out if it comes to that, and it is not a compromise: SYNC is
something you start and finish, not a service that runs. Bringing the BLE stack
down for the duration and back afterwards costs a reconnect, and `end()` and
`begin()` already exist for it. Worth knowing before milestone 8 rather than
discovering during it.

**Three things about the port itself.**

`BoardConfig.h` supplied four build switches, not the one I assumed. Removing
the include and defining only `FREEINK_CAP_BLE_HID_HOST` left
`FREEINK_BLE_HID_REQUIRE_MITM` undefined, and the failure surfaced as an error
about `getInstance()` having static linkage, which points nowhere near the
cause. They live in a local `BleHostConfig.h` now, with their defaults and the
reason, so the next person to read that file finds all four in one place.

`BleHid` is a macro in `BleKeyboardHost.h` expanding to `getInstance()`, not a
variable. Declaring one of that name shadowed nothing and expanded to nonsense.

`CONFIG_BT_NIMBLE_EXT_ADV` is deliberately absent where the PaperS3 sets it.
Extended advertising is a Bluetooth 5 feature and this is a classic ESP32 with
a 4.2 radio; carrying the flag over would be asking the stack for something the
hardware cannot do. That is the fourth time on this port that a PaperS3 setting
was right there and wrong here.

Auto-pairing, the passkey display and the BLE button's live state are ported.
`BLE: up` on the board. Untested against a real keyboard.

## 2026-08-31 -- Milestone 9 closes, and the function that caused three bugs gets a warning

The BLE keyboard pairs and works. Loading and editing a program from it works,
and so does creating a new one, which was milestone 6's last unverified path.

Two failures came first and shared a cause. `pumpPhysicalButtonsForProgram()`
opened with `if (!g_oskVisible) return;`, written when the on-screen keyboard
was the only thing it served. Folding that keyboard away to use a BLE one made
the whole function give up before reading anything, and since it is the only
path input has into a running program, neither the bar nor the panel answered
during a RUN. It had also never polled BLE, because BLE did not exist in this
project when it was written, so Esc from a real keyboard could not stop a
program either.

**That is the third bug in this one function, all the same shape.** It was an
empty stub, so nothing could stop a program at all. It drew nothing, so an
armed Ctrl was invisible and Ctrl+C took three tries. It returned early on a
hidden keyboard, so a BLE keyboard silently disabled the bar. Every time, the
machine gained an input and this function was not told.

The name is why. It says "physical buttons", which is the X4's d-pad, and it is
kept that way so these files still diff cleanly against the two machines they
are shared with. A rename would fix the reading and cost that, which on a
codebase where fixes are carried by hand between three devices is the worse
trade.

So the warning goes in the code instead, at both ends: a block at the top of
the function saying what it really is, listing every input that must be served
from it, naming the three bugs, and stating plainly that an early return here
disables more than whatever prompted it; and a note at the call site in
`byield()`, where a reader sees a function named for buttons on a board that
has none and would otherwise reasonably assume it is dead.

## 2026-08-31 -- The network ports, and the paint chain stops being written three times

`wifi_sync.cpp`, `web_files_page.h` and `sd_backup.h` came across with the same
SdFat-to-Arduino-SD substitution the other three storage files needed, plus two
API differences worth naming because they are not the ones already met:
`FsFile::isOpen()` becomes `if (!f)`, since Arduino's File has an explicit
operator bool, and `File::name()` returns a full path where `getName()` filled a
buffer with a bare one. The second matters more than it looks: the sync protocol
sends names, and a path there would be a name the other end could never match.

`PROGRAM_UPLOAD_MAX_SIZE` went back into `config.h`, the last of the constants
trimmed at the start and returned alongside the file that reads it.

**The paint chain was written out three times, and now it is written once.** The
PaperS3 records that two of its chains disagreed, the browser first in one and
second in the other, and SYNC drew but could not be typed into. Adding a third
screen here meant editing three places in the same way, which is the shape of
that bug being set up again. `drawCurrentScreen()` is now the single place that
decides which screen is showing; `drawBand()` and `drawAll()` both call it. The
key-routing chain in `loop()` still has to be kept in step by hand and says so
where it is, but two of the three can no longer drift.

**The status bar was flashing on every keystroke**, and it was the band bug in
the one place it had been missed: `drawStatusBar()` cleared the whole bar and
then drew cells that tile it exactly. `TITLE_W` is defined as the width the
buttons do not take, so the cells always cover the bar, MicroWriter's
zero-width entries included. The clear was a second pass over pixels about to
be painted anyway.

| | Flash | Free heap at the prompt |
|---|---|---|
| Before BLE | 495029 | 266KB |
| With BLE | 791793 | 121KB |
| With BLE and WiFi linked | 1278641 | 95KB |

WiFi costs 425KB of flash, and the build sits at 39.8% of the partition. The
95KB is with the WiFi stack compiled in but not started: `WiFi.begin()` has not
run. Whether it comes up alongside NimBLE in what is left is the question this
milestone exists to answer, and it is a runtime one.

## 2026-08-31 -- Milestone 8 closes, and both radios turn out to fit

Sync works in both directions, and `pacman.bas` uploaded over WiFi runs on the
machine. That is the whole port exercised in one action: a file crossing the
network, landing on the card, being read by the interpreter, drawn through the
render layer and driven from the keyboard.

**The question this milestone existed to answer is answered, and the answer is
that there was no problem.** NimBLE and WiFi run at the same time inside the
95KB of heap left after both are linked: the BLE keyboard kept working while
the HTTP server served a file. The contingency written down two entries ago,
bringing the BLE stack down for the duration of a sync and back afterwards,
does not need to exist.

Worth saying plainly, because the flash ceiling shaped this plan from the first
day: **the 4MB that looked like the dominant constraint never bound anything.**
The finished firmware is 1278641 bytes, 39.8% of its partition. The priority
decision about which of WiFi and the keyboard would give way was never called
on. Being wrong about that cost nothing here, but it did shape the milestone
order, and measuring earlier would have been cheap.

**Two bugs found by using it, both the same shape as several before.**

The mDNS hostname was still `microbasic-papers3`, and the interesting part is
that it *worked*. Because it answered, nothing forced it to be noticed, and the
address that answered was the one nobody on this device would guess. It follows
the machine now: `microbasic-cyd` or `microwriter-cyd`.

The cursor blink asked whether the terminal was in front by testing
`isBrowserActive()` alone. With the sync screen up it kept blinking a block
onto it, and erasing that block calls `drawTerminalRow()`, which repaints a
whole terminal row: the IP address a sync is useless without was being wiped by
a line of the screen behind it, twice a second. Three places were asking that
question and giving different answers, so it is one function now,
`terminalIsShowing()`.

That is the fourth time on this port that the same fix has applied: a condition
or a chain written out in more than one place drifted, and factoring it into one
function was both the bug fix and the guard against the next one.
