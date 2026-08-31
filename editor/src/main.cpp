// Milestone 2: the render layer, proven on the panel.
//
// Milestone 1 established that the board works. This establishes that
// MicroBASIC's own drawing calls can reach it: TftRenderer offers the fifteen
// GfxRenderer methods that port-staging/src actually calls, and EpdFont's glyph
// data is blitted straight to the TFT with no framebuffer in between.
//
// What this proves, and why each part is here:
//
//   The glyph bit walk is right. A full grid of printable ASCII at two
//   different cell sizes. The unscii cells are 15 and 12 pixels wide, neither a
//   multiple of eight, which is exactly the case that comes out sheared if the
//   bitstream is read as byte-aligned per row. If the characters are legible,
//   the walk is correct.
//
//   The geometry works out. Both grids are drawn to the column and row counts
//   proposed in docs/PORTING_PLAN.md, so the numbers in that table can be
//   looked at rather than argued about.
//
//   The speed is known. Each full repaint is timed and reported. This is the
//   number that decides whether the terminal can repaint whole rows or has to
//   track dirty cells, and it is measured rather than guessed.
//
// Tap the left half of the screen to change SCREEN mode, the right half to
// change palette.

#include <Arduino.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <builtinFonts/unscii_12x24.h>
#include <builtinFonts/unscii_15x30.h>

#include "board_fnk0103n.h"
#include "tft_renderer.h"

static TFT_eSPI tft;
static TftRenderer renderer(tft);
static Preferences prefs;

// Font ids are arbitrary sentinels, the same scheme config.h uses on the two
// earlier devices: they only have to not collide with each other.
static constexpr int FONT_SCREEN_0 = -2000000001;  // 32 columns
static constexpr int FONT_SCREEN_1 = -2000000002;  // 40 columns

static const EpdFont fontUnscii15x30(&unscii_15x30);
static const EpdFont fontUnscii12x24(&unscii_12x24);

// The status bar is one native unscii row tall, so the terminal band below it
// is 304 pixels. See docs/PORTING_PLAN.md, "Screen geometry".
static constexpr int STATUS_BAR_H = 16;
static constexpr int BAND_Y = STATUS_BAR_H;
static constexpr int BAND_H = SCREEN_H - STATUS_BAR_H;  // 304

struct ScreenMode {
  const char* name;
  int fontId;
  int cellW;
  int cellH;
  int cols;
  int rows;
};

// Only the two modes whose fonts already exist in the PaperS3 build. SCREEN 2
// (48 columns, 10x20) and SCREEN 3 (60 columns, native 8x16) still have to be
// generated with research/fonts/tools/.
static const ScreenMode kModes[] = {
    {"SCREEN 0  32x10  15x30", FONT_SCREEN_0, 15, 30, 32, BAND_H / 30},
    {"SCREEN 1  40x12  12x24", FONT_SCREEN_1, 12, 24, 40, BAND_H / 24},
};
static constexpr int kModeCount = sizeof(kModes) / sizeof(kModes[0]);

struct NamedPalette {
  const char* name;
  TftRenderer::Palette palette;
};
static const NamedPalette kPalettes[] = {
    {"green", TftRenderer::PhosphorGreen},
    {"amber", TftRenderer::PhosphorAmber},
    {"paper", TftRenderer::PaperWhite},
};
static constexpr int kPaletteCount = sizeof(kPalettes) / sizeof(kPalettes[0]);

static int g_mode = 0;
static int g_palette = 0;
static uint32_t g_lastRepaintUs = 0;

// A row of printable ASCII, offset per row so the pattern is not a repeating
// stripe and every glyph in the font gets drawn somewhere.
static void fillPatternRow(char* out, const int cols, const int rowIndex) {
  for (int c = 0; c < cols; c++) {
    out[c] = static_cast<char>(0x20 + ((c + rowIndex * 7) % 95));
  }
  out[cols] = '\0';
}

static void drawStatusBar() {
  const ScreenMode& m = kModes[g_mode];
  renderer.fillRect(0, 0, SCREEN_W, STATUS_BAR_H, true);

  char line[80];
  snprintf(line, sizeof(line), " %s  %s  %lu ms", m.name, kPalettes[g_palette].name,
           static_cast<unsigned long>(g_lastRepaintUs / 1000));

  // The status bar uses a TFT_eSPI built-in font rather than an EpdFont: at 16
  // pixels tall there is no unscii size that fits yet, and generating one just
  // for the bar would be work spent before the bar's design is settled.
  tft.setTextFont(2);
  tft.setTextColor(renderer.getPalette().paper, renderer.getPalette().ink);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(line, 2, 0);
}

static void drawGrid() {
  const ScreenMode& m = kModes[g_mode];
  char row[128];

  // No separate clear: each row is composed with its own background and pushed
  // as one image, so the band never needs wiping first. That is worth roughly
  // 30ms on its own, which is what the full-band fillRect used to cost.
  const uint32_t start = micros();
  for (int r = 0; r < m.rows; r++) {
    fillPatternRow(row, m.cols, r);
    const int rowY = BAND_Y + r * m.cellH;
    renderer.drawTextOpaque(m.fontId, 0, rowY, SCREEN_W, m.cellH, 0, rowY, row);
  }
  const uint32_t end = micros();
  g_lastRepaintUs = end - start;

  // A single cell, which is what a keystroke actually costs. The number that
  // decides whether typing feels immediate.
  const uint32_t cellStart = micros();
  renderer.drawTextOpaque(m.fontId, 0, BAND_Y, m.cellW, m.cellH, 0, BAND_Y, "A");
  const uint32_t cellUs = micros() - cellStart;

  drawStatusBar();

  const int glyphs = m.cols * m.rows;
  Serial.printf("%s %s | full repaint %lu us (%lu ms) for %d glyphs | one cell %lu us\n", m.name,
                kPalettes[g_palette].name, static_cast<unsigned long>(g_lastRepaintUs),
                static_cast<unsigned long>(g_lastRepaintUs / 1000), glyphs,
                static_cast<unsigned long>(cellUs));
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== CYD MicroBASIC/MicroWriter -- milestone 2, render layer ===");

  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);

  tft.init();

  // Touch calibration is whatever milestone 1 stored. If it is missing the
  // demo still runs; only the tap-to-switch stops working, which is not what
  // this milestone is proving.
  uint16_t touchCal[5];
  prefs.begin("cyd", true);
  if (prefs.getBytesLength("touchcal") == sizeof(touchCal)) {
    prefs.getBytes("touchcal", touchCal, sizeof(touchCal));
    tft.setTouch(touchCal);
    Serial.println("touch: calibration loaded from NVS");
  } else {
    Serial.println("touch: NO calibration in NVS, taps will be wrong");
  }
  prefs.end();

  renderer.setOrientation(TftRenderer::LandscapeCounterClockwise);
  renderer.setPalette(kPalettes[g_palette].palette);
  renderer.insertFont(FONT_SCREEN_0, EpdFontFamily(&fontUnscii15x30));
  renderer.insertFont(FONT_SCREEN_1, EpdFontFamily(&fontUnscii12x24));
  renderer.begin();

  // Metrics, printed once. A monospace font should report a text width that is
  // exactly the column count times the cell width; if it does not, the advance
  // arithmetic is wrong and every layout built on it would be too.
  for (int i = 0; i < kModeCount; i++) {
    char row[128];
    fillPatternRow(row, kModes[i].cols, 0);
    Serial.printf("%s -> lineHeight %d (expect %d), width of %d cols %d (expect %d)\n", kModes[i].name,
                  renderer.getLineHeight(kModes[i].fontId), kModes[i].cellH, kModes[i].cols,
                  renderer.getTextWidth(kModes[i].fontId, row), kModes[i].cols * kModes[i].cellW);
  }
  Serial.printf("heap: %u KB free\n", static_cast<unsigned>(ESP.getFreeHeap() / 1024));

  drawGrid();
}

void loop() {
  uint16_t x = 0, y = 0;
  if (tft.getTouch(&x, &y)) {
    if (x < SCREEN_W / 2) {
      g_mode = (g_mode + 1) % kModeCount;
    } else {
      g_palette = (g_palette + 1) % kPaletteCount;
      renderer.setPalette(kPalettes[g_palette].palette);
    }
    drawGrid();
    // Long enough that one press is one change on a resistive panel, which
    // chatters far more than the capacitive one on the PaperS3.
    delay(350);
  }
  delay(10);
}
