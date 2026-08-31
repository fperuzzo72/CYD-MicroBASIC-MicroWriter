#include "tft_renderer.h"

#include <EpdFont.h>
#include <Utf8.h>

// RGB565. The green is a phosphor green rather than pure 0x07E0, which on an
// LCD reads as a highlighter rather than a CRT.
const TftRenderer::Palette TftRenderer::PhosphorGreen = {0x2FE7, 0x0000};
const TftRenderer::Palette TftRenderer::PhosphorAmber = {0xFD60, 0x0000};
const TftRenderer::Palette TftRenderer::PaperWhite = {0x0000, 0xFFFF};
// MSX BASIC boots white (TMS9918 colour 15) on dark blue (colour 4). That blue
// is RGB 89,85,224 in the usual TMS9918 palette, which is a periwinkle rather
// than the navy people tend to remember, and is what the machine actually put
// on a television. 0x5ABC is it in RGB565.
const TftRenderer::Palette TftRenderer::MsxBlue = {0xFFFF, 0x5ABC};

void TftRenderer::begin() {
  // Required for every pushImage in this file, and a silent wrong-colour bug
  // without it.
  //
  // TFT_eSPI's drawing primitives (fillRect, drawFastHLine, drawString) take a
  // colour and put its bytes on the wire in the panel's order. pushImage does
  // not: it treats the array as raw bytes to stream, and only swaps each
  // 16-bit value when _swapBytes is set, which it is not by default. So a
  // buffer filled with values built in code goes out byte-reversed, and
  // 0x2FE7 green arrives as 0xE72F. The background stays right, because it was
  // drawn by fillRect, and only the composed text comes out wrong, which is
  // what makes it look like a font bug rather than a colour bug.
  tft_.setSwapBytes(true);
  tft_.fillScreen(palette_.paper);
}

void TftRenderer::setOrientation(const Orientation o) {
  orientation_ = o;
  // Only the two landscape orientations are meaningful here. The panel is
  // native portrait, so both landscape values map to a TFT_eSPI rotation and
  // the two portrait values pass through as the native ones.
  switch (o) {
    case LandscapeCounterClockwise: tft_.setRotation(1); break;
    case LandscapeClockwise:        tft_.setRotation(3); break;
    case Portrait:                  tft_.setRotation(0); break;
    case PortraitInverted:          tft_.setRotation(2); break;
  }
}

void TftRenderer::tapToLogical(const float nx, const float ny, int& outX, int& outY) const {
  outX = static_cast<int>(nx * tft_.width());
  outY = static_cast<int>(ny * tft_.height());
}

const EpdFontFamily* TftRenderer::family(const int fontId) const {
  const auto it = fontMap_.find(fontId);
  return (it == fontMap_.end()) ? nullptr : &it->second;
}

// Mixes two RGB565 values, `alpha` being how much of `a` survives, 0 to 255.
// Done per channel at native depth rather than by unpacking to 8-bit and back,
// which would cost accuracy in green for nothing.
static uint16_t blend565(const uint16_t a, const uint16_t b, const uint8_t alpha) {
  const uint32_t inv = 255u - alpha;
  const uint32_t r = (((a >> 11) & 0x1F) * alpha + ((b >> 11) & 0x1F) * inv) / 255u;
  const uint32_t g = (((a >> 5) & 0x3F) * alpha + ((b >> 5) & 0x3F) * inv) / 255u;
  const uint32_t bl = ((a & 0x1F) * alpha + (b & 0x1F) * inv) / 255u;
  return static_cast<uint16_t>((r << 11) | (g << 5) | bl);
}

uint16_t TftRenderer::resolveColor(const Color color) const {
  switch (color) {
    case Black:     return palette_.ink;
    case DarkGray:  return blend565(palette_.ink, palette_.paper, 128);  // ~50%, matching the dither density it replaces
    case LightGray: return blend565(palette_.ink, palette_.paper, 64);   // ~25%, likewise
    case White:     return palette_.paper;
    case Clear:     return palette_.paper;  // nothing here composites, so Clear reads as paper
  }
  return palette_.ink;
}

// A radius of zero means a square corner, and it has to be handled rather than
// passed through: TFT_eSPI's round-rect primitives draw four quarter-circle
// arcs and a zero radius is not a case they are written for. Callers use zero
// deliberately (osk.cpp does), so it takes the plain-rectangle path.
void TftRenderer::fillRoundedRect(const int x, const int y, const int width, const int height,
                                  const int cornerRadius, const Color color) const {
  const uint16_t colour = resolveColor(color);
  if (cornerRadius <= 0) {
    tft_.fillRect(x, y, width, height, colour);
    return;
  }
  tft_.fillRoundRect(x, y, width, height, cornerRadius, colour);
}

void TftRenderer::drawRoundedRect(const int x, const int y, const int width, const int height,
                                  const int lineWidth, const int cornerRadius, const bool state) const {
  const uint16_t colour = state ? palette_.ink : palette_.paper;
  for (int i = 0; i < lineWidth; i++) {
    // Each inset ring needs a radius one smaller, and it must not go negative
    // on the inner rings of a thick border round a small radius.
    const int r = cornerRadius - i;
    if (r <= 0) {
      tft_.drawRect(x + i, y + i, width - 2 * i, height - 2 * i, colour);
    } else {
      tft_.drawRoundRect(x + i, y + i, width - 2 * i, height - 2 * i, r, colour);
    }
  }
}

void TftRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  tft_.fillRect(x, y, width, height, state ? palette_.ink : palette_.paper);
}

void TftRenderer::drawRect(const int x, const int y, const int width, const int height, const bool state) const {
  tft_.drawRect(x, y, width, height, state ? palette_.ink : palette_.paper);
}

void TftRenderer::drawRect(const int x, const int y, const int width, const int height, const int lineWidth,
                           const bool state) const {
  const uint16_t colour = state ? palette_.ink : palette_.paper;
  for (int i = 0; i < lineWidth; i++) {
    tft_.drawRect(x + i, y + i, width - 2 * i, height - 2 * i, colour);
  }
}

// One glyph, transparently: only inked pixels are written, everything else is
// left as it was.
//
// The bit walk is the part that has to match GfxRenderer's exactly, and the
// thing to know is that `pos` runs continuously through the whole glyph and is
// NOT reset or padded at row boundaries. Any font whose width is not a multiple
// of eight comes out sheared if you assume per-row byte alignment, which is a
// bug this project has already paid for once on the emitter side.
//
// Pixels go out as horizontal runs rather than one at a time. Every SPI write
// carries a command and an address window, so a run of eight inked pixels as
// one drawFastHLine costs a fraction of eight drawPixel calls. Typical text
// gives runs of two to five, which is already most of the win.
void TftRenderer::renderGlyphDirect(const EpdFontData* fontData, const EpdGlyph* glyph, const int cursorX,
                              const int cursorY, const uint16_t colour) const {
  // Uncompressed, RAM-resident fonts only. Compressed groups and SD-card
  // fonts route through a decompressor on the PaperS3; neither exists here
  // yet, and both would hook in at exactly this line.
  if (fontData->groups != nullptr) return;
  const uint8_t* bitmap = &fontData->bitmap[glyph->dataOffset];

  const int x0 = cursorX + glyph->left;
  const int y0 = cursorY - glyph->top;
  const int w = glyph->width;
  const int h = glyph->height;
  const bool is2Bit = fontData->is2Bit;

  int pos = 0;
  for (int gy = 0; gy < h; gy++) {
    int runStart = -1;
    for (int gx = 0; gx < w; gx++, pos++) {
      bool ink;
      if (is2Bit) {
        const uint8_t byte = bitmap[pos >> 2];
        const uint8_t shift = (3 - (pos & 3)) * 2;
        // Raw is 0 white to 3 black; GfxRenderer inverts it to 0 black to 3
        // white and then treats anything below 3 as ink in black-and-white
        // mode. Two-level output here, so the grays collapse to ink the same
        // way they do there.
        ink = (3 - ((byte >> shift) & 0x3)) < 3;
      } else {
        ink = (bitmap[pos >> 3] >> (7 - (pos & 7))) & 1;
      }

      if (ink) {
        if (runStart < 0) runStart = gx;
      } else if (runStart >= 0) {
        tft_.drawFastHLine(x0 + runStart, y0 + gy, gx - runStart, colour);
        runStart = -1;
      }
    }
    if (runStart >= 0) {
      tft_.drawFastHLine(x0 + runStart, y0 + gy, w - runStart, colour);
    }
  }
}

void TftRenderer::renderGlyphToScratch(const EpdFontData* fontData, const EpdGlyph* glyph, const int cursorX,
                                       const int cursorY, const int bandX0, const int bandY0, const int bandW,
                                       const int bandH, const uint16_t colour) const {
  if (fontData->groups != nullptr) return;
  const uint8_t* bitmap = &fontData->bitmap[glyph->dataOffset];

  const int x0 = cursorX + glyph->left - bandX0;
  const int y0 = cursorY - glyph->top - bandY0;
  const int w = glyph->width;
  const int h = glyph->height;
  const bool is2Bit = fontData->is2Bit;

  int pos = 0;
  for (int gy = 0; gy < h; gy++) {
    const int sy = y0 + gy;
    // The row walk cannot be skipped when the row is off-band, because `pos`
    // has to keep counting: the bitstream is continuous across rows and
    // skipping ahead would desynchronise every row after it.
    const bool rowVisible = (sy >= 0 && sy < bandH);
    uint16_t* rowPtr = rowVisible ? scratch_ + static_cast<size_t>(sy) * bandW : nullptr;
    for (int gx = 0; gx < w; gx++, pos++) {
      bool ink;
      if (is2Bit) {
        const uint8_t byte = bitmap[pos >> 2];
        const uint8_t shift = (3 - (pos & 3)) * 2;
        ink = (3 - ((byte >> shift) & 0x3)) < 3;
      } else {
        ink = (bitmap[pos >> 3] >> (7 - (pos & 7))) & 1;
      }
      if (!ink || !rowVisible) continue;
      const int sx = x0 + gx;
      if (sx < 0 || sx >= bandW) continue;
      rowPtr[sx] = colour;
    }
  }
}

void TftRenderer::releaseTextScratch() {
  free(scratch_);
  scratch_ = nullptr;
  scratchPixels_ = 0;
}

// Text is composed into a scratch buffer and pushed to the panel in one call,
// rather than drawn run by run straight to the panel.
//
// The reason is measured, on this board, at 80MHz SPI. Drawing a 32x10 grid of
// 15x30 glyphs as horizontal runs cost 518us a glyph, 166ms for the grid.
// Pushing the same pixels costs about 90us a glyph, so roughly 85% of that was
// per-run SPI transaction setup: every drawFastHLine opens and closes its own
// transaction, and a glyph is dozens of runs.
//
// Transparency is kept, because callers depend on it: they fill a rectangle
// with ink and then draw text over it with black=false. The scratch is filled
// with a colour key and only inked pixels are written, then TFT_eSPI's
// colour-keyed pushImage skips the rest. The key is magenta, TFT_eSPI's own
// convention, and no palette here uses it.
void TftRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style) const {
  const EpdFontFamily* font = family(fontId);
  if (!font || !text || !*text) return;
  const EpdFontData* fontData = font->getData(style);
  if (!fontData) return;

  const uint16_t colour = black ? palette_.ink : palette_.paper;

  // Cursor advance, carried over from GfxRenderer unchanged and for its
  // reason: the previous glyph's advance and the current pair's kern are
  // summed in 12.4 fixed point and snapped together, so the same character
  // pair always steps by the same number of pixels wherever it falls on the
  // line. Snapping them separately and adding the integers drifts by a pixel.
  //
  // Pass one measures. Nothing is drawn, so the bounding box is known before a
  // single pixel is placed, which is what lets the whole string go out as one
  // image.
  int penX = x;
  uint32_t prevCp = 0;
  int32_t prevAdvanceFp = 0;
  int minX = INT32_MAX, maxX = INT32_MIN, maxTop = 0, maxBelow = 0;
  bool any = false;

  const auto* cursor = reinterpret_cast<const unsigned char*>(text);
  while (*cursor) {
    const uint32_t cp = utf8NextCodepoint(&cursor);
    if (cp == 0) break;
    if (prevCp != 0) {
      penX += fp4::toPixel(prevAdvanceFp + font->getKerning(prevCp, cp, style));
    }
    const EpdGlyph* glyph = font->getGlyph(cp, style);
    prevAdvanceFp = glyph ? glyph->advanceX : 0;
    prevCp = cp;
    if (!glyph || glyph->width == 0 || glyph->height == 0) continue;
    any = true;
    const int gx0 = penX + glyph->left;
    if (gx0 < minX) minX = gx0;
    if (gx0 + glyph->width > maxX) maxX = gx0 + glyph->width;
    if (glyph->top > maxTop) maxTop = glyph->top;
    const int below = glyph->height - glyph->top;
    if (below > maxBelow) maxBelow = below;
  }
  if (!any) return;

  const int bandX0 = minX;
  const int bandY0 = y - maxTop;
  const int bandW = maxX - minX;
  const int bandH = maxTop + maxBelow;
  const size_t needed = static_cast<size_t>(bandW) * static_cast<size_t>(bandH);

  // Ligature substitution, combining-mark anchoring and sup/sub scaling all
  // belong in the walk below, between decoding and measuring, and in the pass
  // above in the same place. See the header.
  const bool useScratch = (bandW > 0 && bandH > 0 && needed <= kScratchMaxPixels);
  if (useScratch && needed > scratchPixels_) {
    free(scratch_);
    scratch_ = static_cast<uint16_t*>(malloc(needed * sizeof(uint16_t)));
    scratchPixels_ = scratch_ ? needed : 0;
  }

  static constexpr uint16_t kTransparent = TFT_MAGENTA;
  const bool composing = useScratch && scratch_ != nullptr;
  if (composing) {
    for (size_t i = 0; i < needed; i++) scratch_[i] = kTransparent;
  } else {
    // Direct path. Held inside one SPI transaction so the runs at least do not
    // pay for a fresh transaction each.
    tft_.startWrite();
  }

  // Pass two draws, replaying the identical advance arithmetic.
  penX = x;
  prevCp = 0;
  prevAdvanceFp = 0;
  cursor = reinterpret_cast<const unsigned char*>(text);
  while (*cursor) {
    const uint32_t cp = utf8NextCodepoint(&cursor);
    if (cp == 0) break;
    if (prevCp != 0) {
      penX += fp4::toPixel(prevAdvanceFp + font->getKerning(prevCp, cp, style));
    }
    const EpdGlyph* glyph = font->getGlyph(cp, style);
    prevAdvanceFp = glyph ? glyph->advanceX : 0;
    prevCp = cp;
    if (!glyph || glyph->width == 0 || glyph->height == 0) continue;
    if (composing) {
      renderGlyphToScratch(fontData, glyph, penX, y, bandX0, bandY0, bandW, bandH, colour);
    } else {
      renderGlyphDirect(fontData, glyph, penX, y, colour);
    }
  }

  if (composing) {
    tft_.pushImage(bandX0, bandY0, bandW, bandH, scratch_, kTransparent);
  } else {
    tft_.endWrite();
  }
}

void TftRenderer::drawTextOpaque(const int fontId, const int rectX, const int rectY, const int rectW,
                                 const int rectH, const int textX, const int textY, const char* text,
                                 const bool black, const EpdFontFamily::Style style) const {
  const EpdFontFamily* font = family(fontId);
  if (!font || rectW <= 0 || rectH <= 0) return;
  const EpdFontData* fontData = font->getData(style);
  if (!fontData) return;

  const size_t needed = static_cast<size_t>(rectW) * static_cast<size_t>(rectH);
  if (needed > kScratchMaxPixels) {
    // Too big to compose. Fall back to the two-step the caller would otherwise
    // have written by hand, which is correct if slower.
    fillRect(rectX, rectY, rectW, rectH, !black);
    if (text) drawText(fontId, textX, textY, text, black, style);
    return;
  }
  if (needed > scratchPixels_) {
    free(scratch_);
    scratch_ = static_cast<uint16_t*>(malloc(needed * sizeof(uint16_t)));
    scratchPixels_ = scratch_ ? needed : 0;
  }
  if (!scratch_) {
    fillRect(rectX, rectY, rectW, rectH, !black);
    if (text) drawText(fontId, textX, textY, text, black, style);
    return;
  }

  const uint16_t ink = black ? palette_.ink : palette_.paper;
  const uint16_t bg = black ? palette_.paper : palette_.ink;
  for (size_t i = 0; i < needed; i++) scratch_[i] = bg;

  if (text && *text) {
    int penX = textX;
    uint32_t prevCp = 0;
    int32_t prevAdvanceFp = 0;
    const auto* cursor = reinterpret_cast<const unsigned char*>(text);
    while (*cursor) {
      const uint32_t cp = utf8NextCodepoint(&cursor);
      if (cp == 0) break;
      if (prevCp != 0) {
        penX += fp4::toPixel(prevAdvanceFp + font->getKerning(prevCp, cp, style));
      }
      const EpdGlyph* glyph = font->getGlyph(cp, style);
      prevAdvanceFp = glyph ? glyph->advanceX : 0;
      prevCp = cp;
      if (!glyph || glyph->width == 0 || glyph->height == 0) continue;
      renderGlyphToScratch(fontData, glyph, penX, textY, rectX, rectY, rectW, rectH, ink);
    }
  }

  // One address window, one burst. This is the whole point: the panel sees
  // rectW*rectH pixels in a single transfer instead of one per inked run.
  tft_.pushImage(rectX, rectY, rectW, rectH, scratch_);
}

// Walks the same loop as drawText without drawing, so a string's measured width
// and its drawn extent can never disagree. Anything added to one has to be
// added to the other.
int TftRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  const EpdFontFamily* font = family(fontId);
  if (!font || !text) return 0;

  int penX = 0;
  uint32_t prevCp = 0;
  int32_t prevAdvanceFp = 0;

  const auto* cursor = reinterpret_cast<const unsigned char*>(text);
  while (*cursor) {
    const uint32_t cp = utf8NextCodepoint(&cursor);
    if (cp == 0) break;
    if (prevCp != 0) {
      const int8_t kernFp = font->getKerning(prevCp, cp, style);
      penX += fp4::toPixel(prevAdvanceFp + kernFp);
    }
    const EpdGlyph* glyph = font->getGlyph(cp, style);
    prevAdvanceFp = glyph ? glyph->advanceX : 0;
    prevCp = cp;
  }
  // The last glyph's own advance is part of the width, and is never folded in
  // by the loop above because there is no following pair to snap it with.
  return penX + fp4::toPixel(prevAdvanceFp);
}

int TftRenderer::getLineHeight(const int fontId) const {
  const EpdFontFamily* font = family(fontId);
  if (!font) return 0;
  const EpdFontData* fontData = font->getData(EpdFontFamily::REGULAR);
  return fontData ? fontData->advanceY : 0;
}
