#pragma once

#include <EpdFontFamily.h>
#include <TFT_eSPI.h>

#include <map>

// The drawing layer for this device, standing where GfxRenderer stands on the
// PaperS3.
//
// GfxRenderer has close to two hundred methods, and almost all of them exist
// for electrophoretic paper: grayscale planes, Bayer dithering, strip targets,
// framebuffer loans, waveform choice, async refresh. None of that applies to a
// TFT. Rather than port it and leave nine tenths dead, this offers exactly the
// surface MicroBASIC and MicroWriter actually call, which turns out to be
// fifteen methods (counted across every .cpp and .h in port-staging/src). The
// signatures match GfxRenderer's, so ported code calls this without edits.
//
// Two things are genuinely different underneath, and both are simplifications:
//
//   No framebuffer. There is no PSRAM, and 480x320 at 16bpp would be 307KB of
//   the 335KB free. Drawing goes straight out over SPI, so displayBuffer() has
//   nothing to flush and is a no-op kept only so callers need not change.
//
//   No refresh model. Nothing ghosts, nothing needs a full refresh every N
//   partial ones, and nothing blocks waiting for a panel. The RefreshMode
//   argument callers pass is accepted and ignored.
//
// What is deliberately NOT implemented yet, because unscii needs none of it and
// the prose fonts are milestone 7: ligatures, combining-mark anchoring,
// superscript and subscript scaling, and BiDi. Each has a marked spot in
// drawText where it slots back in, and getTextWidth would have to learn the
// same rules on the same day.

class TftRenderer {
 public:
  // Kept from GfxRenderer so ported call sites compile untouched, though only
  // the two landscape values mean anything on this panel.
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };

  // Ink and paper, as RGB565.
  //
  // The two earlier devices were black on white because electrophoretic paper
  // gives no other choice. A backlit TFT does, and white paper at full
  // backlight in a dim room is a glare. These are the three worth having; the
  // default is one line to change in tft_renderer.cpp.
  struct Palette {
    uint16_t ink;
    uint16_t paper;
  };
  static const Palette PhosphorGreen;  // the home-micro look, and the default
  static const Palette PhosphorAmber;  // the terminal look
  static const Palette PaperWhite;     // what the PaperS3 and the X4 look like
  static const Palette MsxBlue;        // white on TMS9918 colour 4, the MSX BASIC screen

  explicit TftRenderer(TFT_eSPI& tft) : tft_(tft) {}

  // --- Setup ---
  void begin();
  void insertFont(int fontId, EpdFontFamily font) { fontMap_.emplace(fontId, font); }
  void removeFont(int fontId) { fontMap_.erase(fontId); }
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap_; }
  void setOrientation(Orientation o);
  Orientation getOrientation() const { return orientation_; }
  void setPalette(const Palette& p) { palette_ = p; }
  const Palette& getPalette() const { return palette_; }

  // --- Screen ---
  int getScreenWidth() const { return tft_.width(); }
  int getScreenHeight() const { return tft_.height(); }
  void clearScreen() const { tft_.fillScreen(palette_.paper); }
  // Immediate mode: there is nothing buffered to push. Kept so ported code
  // that calls it every frame does not have to be edited.
  void displayBuffer(int /*refreshMode*/ = 0) const {}
  void tapToLogical(float nx, float ny, int& outX, int& outY) const;

  // --- Drawing ---
  // `state` and `black` carry GfxRenderer's meaning: true is ink, false is
  // paper. Text is drawn transparently, touching only the pixels a glyph
  // actually inks, because callers rely on that (fill a rect with ink, then
  // draw text over it with black=false).
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  // Opaque text into a caller-given rectangle: background and glyphs are
  // composed together and pushed as one image, so the rectangle needs no
  // separate clear and the panel sees a single transfer.
  //
  // This exists for the character grid, where the background is known and the
  // whole cell row is being rewritten anyway. It is several times faster than
  // clearing and then drawing transparently, and the measurements are in the
  // .cpp. UI text that has to sit on top of something keeps using drawText.
  //
  // The rectangle is what gets painted, not the text's own extent: pass the
  // full row and every pixel between and after the glyphs is cleared too.
  void drawTextOpaque(int fontId, int rectX, int rectY, int rectW, int rectH, int textX, int textY,
                      const char* text, bool black = true,
                      EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // --- Metrics ---
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getLineHeight(int fontId) const;

  // Frees the text scratch. Only worth calling before a phase that needs the
  // heap more than text needs to be fast; it is reallocated on the next
  // drawText.
  void releaseTextScratch();

 private:
  const EpdFontFamily* family(int fontId) const;
  // Direct-to-panel glyph blit, one horizontal run per SPI transaction. The
  // fallback path, used when a string's bounding box will not fit the scratch.
  void renderGlyphDirect(const EpdFontData* fontData, const EpdGlyph* glyph, int cursorX, int cursorY,
                         uint16_t colour) const;
  // Glyph blit into the scratch buffer, clipped to it.
  void renderGlyphToScratch(const EpdFontData* fontData, const EpdGlyph* glyph, int cursorX, int cursorY,
                            int bandX0, int bandY0, int bandW, int bandH, uint16_t colour) const;

  TFT_eSPI& tft_;
  std::map<int, EpdFontFamily> fontMap_;
  Orientation orientation_ = LandscapeCounterClockwise;
  Palette palette_ = PhosphorGreen;

  // Composition scratch for drawText. A string is assembled here and pushed in
  // one transaction rather than as hundreds of one-line writes, which on this
  // panel is the difference between 518us and something a terminal can live
  // with. See the note above drawText in the .cpp for the measurements.
  //
  // Sized on first use and kept. The cap is one full-width row of the tallest
  // SCREEN cell: 480x34 at 16bpp, about 32KB of the 342KB free. Anything
  // larger falls back to the direct path rather than growing without bound.
  static constexpr size_t kScratchMaxPixels = 480 * 34;
  mutable uint16_t* scratch_ = nullptr;
  mutable size_t scratchPixels_ = 0;
};
