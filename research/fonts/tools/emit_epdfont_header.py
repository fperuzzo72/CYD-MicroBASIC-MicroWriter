#!/usr/bin/env python3
"""Emits EpdFontData C headers for this device's four SCREEN sizes (see
docs/PORTING_PLAN.md, "Screen geometry"), bypassing fontconvert.py
entirely (it only accepts FreeType-loadable font files -- TTF/OTF/BDF --
not raw pixel arrays like the ones this project's own Unscii resizer
produces).

Reuses generate_screen_fonts.py's actual glyph pipeline -- UnsciiScreenFont
(area-coverage resize + stem-width cap + the ç/Ç fix) for both sizes, since
both are non-integer scales of unscii-16 (1.375x and 2.75x) -- so the
firmware fonts are pixel-identical to the same pipeline validated on the
X4's own 64-col/80-col sizes, not a second, potentially-diverging
implementation.

Format matches editor/lib/EpdFont/builtinFonts/*.h exactly (verified
against EpdFontData.h and the pixel-placement math in GfxRenderer.cpp's
drawText()/renderChar()). Two steps, not one -- drawText() itself adds
the font's ascender to y *before* renderChar ever sees it:

    yPos    = y + fontData.ascender          // GfxRenderer::drawText()
    screenX = x    + glyph.left + glyphX     // GfxRenderer::renderChar()
    screenY = yPos - glyph.top  + glyphY

Every glyph here is the *entire* fixed-size cell (true monospace, not
proportionally trimmed like the project's prose fonts), so every glyph
gets identical left=0, top=0, width=cell_w, height=cell_h. For `y` in
drawText(fontId, x, y, ...) to land on the pixel row of the *top* of the
cell (no baseline-offset math needed by callers), ascender must be 0,
matching top=0 -- NOT cell_h, which an earlier (X4-era) version of this
script emitted, and which pushed every drawn character down by exactly
one full cell height (the cursor, drawn with fillRect() directly, doesn't
go through drawText() at all, so it stayed put -- that mismatch was the
tell). 1-bit packing (not the 2-bit grayscale mode the prose fonts use),
MSB-first, packed as one continuous bitstream across the whole glyph (NOT
per-row byte-aligned -- see pack_bits_contiguous()'s docstring for why that
distinction matters and what it broke before this was fixed).

advanceX is 12.4 fixed-point (4 fractional bits), NOT whole pixels -- see
EpdFontData.h's fp4 namespace and GfxRenderer.cpp's fp4::toPixel() call
sites. This is the one field that changed shape between the X4-era
EpdFontData this pipeline was written against and the version this repo's
EpdFont/GfxRenderer was carried from (crosspoint-reader-m5papers3): the
X4-era headers emitted advanceX as whole pixels, which reads as 1/16 of
the intended cell width under the new renderer -- compiles clean, renders
wrong, and there is no error to catch it. Every emitted advanceX here is
`cell_w << 4` for exactly that reason; do not "simplify" it back to
cell_w.

Usage:
    python3 emit_epdfont_header.py
writes both headers directly into
../../../editor/lib/EpdFont/builtinFonts/.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from generate_screen_fonts import HexFont, UnsciiScreenFont  # noqa: E402

SRC = os.path.join(os.path.dirname(__file__), "..", "src")
OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "..",
                        "editor", "lib", "EpdFont", "builtinFonts")


def pack_bits_contiguous(flat_bits):
    """flat_bits: row-major 0/1 values for the WHOLE glyph (width*height long),
    packed as one continuous MSB-first bitstream -- NOT per-row byte-aligned.

    This has to match GfxRenderer.cpp's renderCharImpl exactly: it tracks a
    single running `pixelPosition` across the entire glyph (`pixelPosition =
    glyphY * width + glyphX`, incremented once per pixel with no reset at row
    boundaries) and indexes the bitmap as `bitmap[pixelPosition >> 3]` -- i.e.
    zero padding between rows, padding only at the very end of the last byte.

    An earlier version of this function packed each row into its own
    ceil(width/8) bytes (padding every row out to a byte boundary). For any
    width that is a multiple of 8 that's identical to this scheme, which is
    exactly why 16x32/24x48 (the X4's SCREEN 0/1) always looked fine while
    12x24/10x20 (SCREEN 2/3) were quietly garbled from the second row of
    every glyph onward -- confirmed illegible on real X4 hardware, and
    reproduced here on the PaperS3's 11x22/22x44 fonts before this fix.
    """
    nbytes = (len(flat_bits) + 7) // 8
    value = 0
    for b in flat_bits:
        value = (value << 1) | (1 if b else 0)
    value <<= nbytes * 8 - len(flat_bits)
    return [(value >> (8 * (nbytes - 1 - i))) & 0xFF for i in range(nbytes)]


def build_intervals(codepoints):
    codepoints = sorted(codepoints)
    intervals = []
    start = prev = codepoints[0]
    for cp in codepoints[1:]:
        if cp == prev + 1:
            prev = cp
            continue
        intervals.append((start, prev))
        start = prev = cp
    intervals.append((start, prev))
    return intervals


def emit_header(font, cell_w, cell_h, name, source_note):
    codepoints = sorted(
        cp for cp in font.glyphs if (0x20 <= cp <= 0x7E) or (0xA0 <= cp <= 0xFF)
    )
    intervals = build_intervals(codepoints)
    for first, last in intervals:
        for cp in range(first, last + 1):
            assert cp in font.glyphs, f"{name}: gap at U+{cp:04X}"

    bitmap_bytes = bytearray()
    glyph_entries = []  # (width, height, advanceX, left, top, dataLength, dataOffset)
    for cp in codepoints:
        bits = font.get_cell_bits(chr(cp))
        glyph_offset = len(bitmap_bytes)
        flat_bits = [b for row in bits for b in row]
        bitmap_bytes.extend(pack_bits_contiguous(flat_bits))
        data_length = len(bitmap_bytes) - glyph_offset
        glyph_entries.append((cell_w, cell_h, cell_w << 4, 0, 0, data_length, glyph_offset))

    out = []
    out.append("// Generated by research/fonts/tools/emit_epdfont_header.py -- do not hand-edit.")
    out.append(f"// {source_note}")
    out.append("// Public domain / CC0 (Unscii, viznut) -- see research/fonts/src/unscii-LICENSE.")
    out.append("#pragma once")
    out.append("#include <cstdint>")
    out.append('#include "EpdFontData.h"')
    out.append("")

    out.append(f"static const uint8_t {name}Bitmaps[{len(bitmap_bytes)}] = {{")
    for i in range(0, len(bitmap_bytes), 16):
        chunk = bitmap_bytes[i:i + 16]
        out.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    out.append("};")
    out.append("")

    out.append(f"static const EpdGlyph {name}Glyphs[{len(glyph_entries)}] = {{")
    for w, h, adv, left, top, dlen, doff in glyph_entries:
        out.append(f"    {{ {w}, {h}, {adv}, {left}, {top}, {dlen}, {doff} }},")
    out.append("};")
    out.append("")

    out.append(f"static const EpdUnicodeInterval {name}Intervals[{len(intervals)}] = {{")
    running_offset = 0
    for first, last in intervals:
        out.append(f"    {{ 0x{first:X}, 0x{last:X}, {running_offset} }},")
        running_offset += last - first + 1
    out.append("};")
    out.append("")

    out.append(f"static const EpdFontData {name} = {{")
    out.append(f"    {name}Bitmaps,")
    out.append(f"    {name}Glyphs,")
    out.append(f"    {name}Intervals,")
    out.append(f"    {len(intervals)},")
    out.append(f"    {cell_h},  // advanceY")
    out.append("    0,  // ascender -- MUST be 0 to match glyph.top=0, see module docstring")
    out.append("    0,   // descender")
    out.append("    false,  // is2Bit")
    out.append("};")

    return "\n".join(out) + "\n", len(codepoints), len(intervals), len(bitmap_bytes)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    u16 = os.path.join(SRC, "unscii-16.hex")

    # This panel is 480x320 in landscape with a 16px status bar, so the
    # terminal band is 304px. Every column count below divides 480 exactly.
    #
    # Note what is NOT here: the PaperS3's 30x60 and 20x40, and the X4-era
    # portrait 11x22 and 22x44. Those were sized for panels twice this one's
    # width and would give 16 and 24 columns here, which is not a terminal.
    #
    # The 64-column tier from the two earlier devices is gone too, and
    # deliberately: 480/64 is 7.5, not a whole number of pixels. Rather than
    # carry the old 32/48/64/80 lineage onto a panel that cannot hold it, the
    # tiers are re-cut to what 480 actually divides by.
    # 8x16 goes through plain HexFont, not UnsciiScreenFont, and that is the
    # point rather than an oversight. unscii-16's source cells ARE 8x16, so the
    # area-coverage resize would be an identity transform, but the
    # cap_stem_width post-pass that follows it would still run over glyphs a
    # human designed to be read at exactly this size. There is nothing for it
    # to fix and something for it to break. The native path emits the source
    # bitmap untouched.
    #
    # It also answers the "10x20 is the smallest still readable" floor the two
    # earlier projects set, which was a pixel count on their panels rather than
    # a physical size. This panel is 480px across about 74mm, so an 8px cell is
    # 1.23mm wide. The PaperS3's own smallest mode, 12x24, is 1.30mm on its
    # panel. Nearly the same character on the eye, and unlike a resampled cell
    # this one is a font drawn for its size.
    jobs = [
        ("unscii_15x30", UnsciiScreenFont(u16, 8, 16, 15, 30), 15, 30,
         "SCREEN 0 (32-col): area-coverage resize (1.875x) + stem-width cap + cedilla fix. "
         "480/15=32 cols exact; 304/30=10 rows (300px), 2px margin top and bottom."),
        ("unscii_12x24", UnsciiScreenFont(u16, 8, 16, 12, 24), 12, 24,
         "SCREEN 1 (40-col): area-coverage resize (1.5x) + stem-width cap + cedilla fix. "
         "480/12=40 cols exact; 304/24=12 rows (288px), 8px margin top and bottom. This is the "
         "boot mode: 40 columns is exactly MSX BASIC's text screen width."),
        ("unscii_10x20", UnsciiScreenFont(u16, 8, 16, 10, 20), 10, 20,
         "SCREEN 2 (48-col): area-coverage resize (1.25x) + stem-width cap + cedilla fix. "
         "480/10=48 cols exact; 304/20=15 rows (300px), 2px margin top and bottom. The same "
         "cell the two earlier devices treat as their 'smallest still readable' floor."),
        ("unscii_8x16", HexFont(u16, 8, 16), 8, 16,
         "SCREEN 3 (60-col): unscii-16 at its NATIVE size, no resampling and no stem-width cap. "
         "480/8=60 cols exact; 304/16=19 rows (304px), exact, no margin at all. See the note in "
         "main() for why this one bypasses the resize pipeline entirely."),
    ]

    for name, font, cell_w, cell_h, note in jobs:
        text, n_glyphs, n_intervals, n_bytes = emit_header(font, cell_w, cell_h, name, note)
        out_path = os.path.join(OUT_DIR, f"{name}.h")
        with open(out_path, "w") as f:
            f.write(text)
        print(f"wrote {name}.h: {n_glyphs} glyphs, {n_intervals} interval(s), {n_bytes} bitmap bytes")


if __name__ == "__main__":
    main()
