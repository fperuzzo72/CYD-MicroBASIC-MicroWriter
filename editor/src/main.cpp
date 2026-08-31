// Milestone 3: touch and the on-screen keyboard.
//
// Milestone 1 proved the board, milestone 2 proved the drawing. This proves
// input: osk.cpp comes over from the PaperS3 with nothing changed but its
// include and the renderer's type name, because TftRenderer matches
// GfxRenderer's signatures. It emits standard USB HID keycodes and a standard
// modifier bitmask, which is the same wire format input_handler.cpp already
// expects, so the editor and the interpreter will accept touch input without
// knowing where it came from.
//
// The echo area here is NOT the terminal. screen_editor.cpp is milestone 4;
// this is forty lines of line buffer whose only job is to show that a tapped
// key becomes a character on screen. Nothing here should survive into the
// real thing.
//
// The layout question this milestone exists to answer: the keyboard needs six
// rows and this panel is 320 pixels tall, so how many terminal rows are left
// while it is up, and are the keys big enough to hit on a resistive panel.
// Tap the status bar to fold the keyboard away and see the full terminal.

#include <Arduino.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <builtinFonts/unscii_10x20.h>
#include <builtinFonts/unscii_12x24.h>
#include <builtinFonts/unscii_15x30.h>
#include <builtinFonts/unscii_8x16.h>

#include "board_fnk0103n.h"
#include "osk.h"
#include "tft_renderer.h"

static TFT_eSPI tft;
static TftRenderer renderer(tft);
static Preferences prefs;

static constexpr int FONT_SCREEN_0 = -2000000001;  // 32 columns
static constexpr int FONT_SCREEN_1 = -2000000002;  // 40 columns
static constexpr int FONT_SCREEN_2 = -2000000003;  // 48 columns
static constexpr int FONT_SCREEN_3 = -2000000004;  // 60 columns

static const EpdFont fontUnscii15x30(&unscii_15x30);
static const EpdFont fontUnscii12x24(&unscii_12x24);
static const EpdFont fontUnscii10x20(&unscii_10x20);
static const EpdFont fontUnscii8x16(&unscii_8x16);

static constexpr int STATUS_BAR_H = 16;

// Six rows of keys at 32 pixels each. 32 is the number this milestone is
// really testing: on a 74mm-wide panel a 2-half-unit key comes out about
// 4.6mm across, which is fine for a fingernail or a stylus and small for a
// fingertip. If it turns out to be too small in the hand, the fix is fewer
// rows (fold Esc and the arrows into a modifier layer), not smaller type.
static constexpr int OSK_ROWS = 6;
static constexpr int OSK_ROW_H = 32;
static constexpr int OSK_H = OSK_ROWS * OSK_ROW_H;  // 192

struct ScreenMode {
  const char* name;
  int fontId;
  int cellW;
  int cellH;
  int cols;
};

static const ScreenMode kModes[] = {
    {"SCR0 32c", FONT_SCREEN_0, 15, 30, 32},
    {"SCR1 40c", FONT_SCREEN_1, 12, 24, 40},
    {"SCR2 48c", FONT_SCREEN_2, 10, 20, 48},
    {"SCR3 60c", FONT_SCREEN_3, 8, 16, 60},
};
static constexpr int kModeCount = sizeof(kModes) / sizeof(kModes[0]);

struct NamedPalette {
  const char* name;
  TftRenderer::Palette palette;
};
static const NamedPalette kPalettes[] = {
    {"msx", TftRenderer::MsxBlue},
    {"green", TftRenderer::PhosphorGreen},
    {"amber", TftRenderer::PhosphorAmber},
    {"paper", TftRenderer::PaperWhite},
};
static constexpr int kPaletteCount = sizeof(kPalettes) / sizeof(kPalettes[0]);

// Boots in SCREEN 3, the 60-column mode: the most rows the band will hold, and
// legible on the panel because unscii-16 is drawn for this cell size rather
// than resampled down to it. Confirmed by looking at all four on hardware.
static int g_mode = 3;
static int g_palette = 0;
static bool g_oskVisible = true;

static constexpr int MAX_ROWS = 20;
static constexpr int MAX_COLS = 64;
static char g_text[MAX_ROWS][MAX_COLS + 1];
static int g_row = 0;
static int g_col = 0;

static int termY() { return STATUS_BAR_H; }
static int termH() { return SCREEN_H - STATUS_BAR_H - (g_oskVisible ? OSK_H : 0); }
static int termRows() {
  const int r = termH() / kModes[g_mode].cellH;
  return r > MAX_ROWS ? MAX_ROWS : r;
}
static int termCols() {
  const int c = kModes[g_mode].cols;
  return c > MAX_COLS ? MAX_COLS : c;
}

static void clearText() {
  for (int r = 0; r < MAX_ROWS; r++) g_text[r][0] = '\0';
  g_row = 0;
  g_col = 0;
}

static void drawStatusBar() {
  const ScreenMode& m = kModes[g_mode];
  renderer.fillRect(0, 0, SCREEN_W, STATUS_BAR_H, false);

  char line[96];
  snprintf(line, sizeof(line), " %s  %s  %s%s%s  [tap: mode | kbd | palette]", m.name,
           kPalettes[g_palette].name, oskShiftArmed() ? "SHF " : "", oskCtrlArmed() ? "CTL " : "",
           oskCapsLockOn() ? "CAPS" : "");

  tft.setTextFont(2);
  tft.setTextColor(renderer.getPalette().ink, renderer.getPalette().paper);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(line, 2, 0);
}

static void drawTerminal() {
  const ScreenMode& m = kModes[g_mode];
  const int rows = termRows();
  for (int r = 0; r < rows; r++) {
    const int y = termY() + r * m.cellH;
    renderer.drawTextOpaque(m.fontId, 0, y, SCREEN_W, m.cellH, 0, y, g_text[r]);
  }
  // Whatever is left between the last full row and the keyboard, wiped so a
  // mode change never leaves a stripe of the previous cell height behind.
  const int used = rows * m.cellH;
  if (termH() > used) {
    renderer.fillRect(0, termY() + used, SCREEN_W, termH() - used, false);
  }
}

static void drawCursor(const bool on) {
  const ScreenMode& m = kModes[g_mode];
  if (g_row >= termRows()) return;
  renderer.fillRect(g_col * m.cellW, termY() + g_row * m.cellH, m.cellW, m.cellH, on);
}

static void drawAll() {
  renderer.clearScreen();
  drawStatusBar();
  drawTerminal();
  if (g_oskVisible) oskDraw();
}

// Scrolls the echo buffer up by one line. A terminal, not a document: what
// goes off the top is gone.
static void scrollUp() {
  for (int r = 0; r + 1 < termRows(); r++) {
    strncpy(g_text[r], g_text[r + 1], MAX_COLS);
    g_text[r][MAX_COLS] = '\0';
  }
  g_text[termRows() - 1][0] = '\0';
  g_row = termRows() - 1;
  g_col = 0;
}

static void onOskKey(const uint8_t hid, const uint8_t modifiers) {
  const ScreenMode& m = kModes[g_mode];

  if (hid == OSK_HID_BACKSPACE) {
    if (g_col > 0) {
      g_col--;
      g_text[g_row][g_col] = '\0';
    }
  } else if (hid == OSK_HID_ESCAPE) {
    clearText();
    drawTerminal();
    drawStatusBar();
    return;
  } else {
    const char ch = oskHidToChar(hid, modifiers);
    if (ch == '\n') {
      if (g_row + 1 < termRows()) {
        g_row++;
      } else {
        scrollUp();
      }
      g_col = 0;
    } else if (ch != 0) {
      if (g_col >= termCols()) {
        if (g_row + 1 < termRows()) {
          g_row++;
        } else {
          scrollUp();
        }
        g_col = 0;
      }
      g_text[g_row][g_col] = ch;
      g_text[g_row][g_col + 1] = '\0';
      g_col++;
    } else {
      return;  // arrows, Tab and the modifiers themselves: nothing to echo
    }
  }

  // Only the affected row is redrawn, not the screen. This is the path the
  // real terminal will live on, and it is the one measured at 160us a cell in
  // milestone 2.
  const int y = termY() + g_row * m.cellH;
  renderer.drawTextOpaque(m.fontId, 0, y, SCREEN_W, m.cellH, 0, y, g_text[g_row]);
  if (hid == OSK_HID_BACKSPACE) drawTerminal();
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== CYD MicroBASIC/MicroWriter -- milestone 3, touch keyboard ===");

  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);
  tft.init();

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
  renderer.insertFont(FONT_SCREEN_2, EpdFontFamily(&fontUnscii10x20));
  renderer.insertFont(FONT_SCREEN_3, EpdFontFamily(&fontUnscii8x16));
  renderer.begin();

  clearText();
  snprintf(g_text[0], MAX_COLS + 1, "MicroBASIC 0.1");
  snprintf(g_text[1], MAX_COLS + 1, "%u Bytes free", static_cast<unsigned>(ESP.getFreeHeap()));
  snprintf(g_text[3], MAX_COLS + 1, "Ok");
  g_row = 4;
  g_col = 0;

  // Key labels in the 48-column font, the small Shift hints in the 60-column
  // one. Both are already loaded for the SCREEN modes, so the keyboard costs
  // no extra flash. The PaperS3 uses proportional prose fonts here; those are
  // 2.6MB of headers between them and are milestone 7's problem.
  oskInit(renderer, FONT_SCREEN_2, FONT_SCREEN_3, 0, SCREEN_H - OSK_H, SCREEN_W, OSK_H, onOskKey);

  const uint32_t t0 = micros();
  drawAll();
  Serial.printf("full screen with keyboard: %lu us\n", static_cast<unsigned long>(micros() - t0));

  const uint32_t t1 = micros();
  oskDraw();
  Serial.printf("keyboard alone: %lu us\n", static_cast<unsigned long>(micros() - t1));
  Serial.printf("terminal while typing: %d rows of %d cols | heap %u KB\n", termRows(), termCols(),
                static_cast<unsigned>(ESP.getFreeHeap() / 1024));
}

void loop() {
  uint16_t x = 0, y = 0;
  if (tft.getTouch(&x, &y)) {
    if (y < STATUS_BAR_H + 8) {
      // The status bar carries the demo's own controls, so the keyboard keeps
      // the whole area it is drawn in.
      if (x < SCREEN_W / 3) {
        g_mode = (g_mode + 1) % kModeCount;
        if (g_row >= termRows()) g_row = termRows() - 1;
      } else if (x < 2 * SCREEN_W / 3) {
        g_oskVisible = !g_oskVisible;
        if (g_row >= termRows()) g_row = termRows() - 1;
      } else {
        g_palette = (g_palette + 1) % kPaletteCount;
        renderer.setPalette(kPalettes[g_palette].palette);
      }
      drawAll();
    } else if (g_oskVisible) {
      // Redrawing the keyboard costs 49ms, so it happens only when the
      // keyboard actually looks different: a modifier being armed, or a
      // one-shot Shift clearing itself on the next character. A plain
      // keystroke changed one row of text and nothing else, and the callback
      // already drew that.
      const bool shiftWas = oskShiftArmed();
      const bool ctrlWas = oskCtrlArmed();
      const bool capsWas = oskCapsLockOn();
      if (oskHandleTap(x, y)) {
        if (oskShiftArmed() != shiftWas || oskCtrlArmed() != ctrlWas || oskCapsLockOn() != capsWas) {
          oskDraw();
          drawStatusBar();
        }
      }
    }
    // Long enough that one press is one keystroke on a resistive panel, which
    // chatters far more than the capacitive one on the PaperS3.
    delay(220);
    return;
  }

  static uint32_t lastBlink = 0;
  static bool cursorOn = true;
  if (millis() - lastBlink > 500) {
    lastBlink = millis();
    cursorOn = !cursorOn;
    drawCursor(cursorOn);
  }
  delay(10);
}
