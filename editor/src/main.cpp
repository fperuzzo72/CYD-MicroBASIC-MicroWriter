// Milestone 4: the real terminal.
//
// The forty-line echo buffer from milestone 3 is gone. screen_editor.cpp is
// here instead: the character grid, its scrolling, and the logical-line
// tracking that makes "type a numbered line and it is program text, type
// anything else and it runs" work. It came over from the PaperS3 with one real
// change, described below.
//
// There is still no interpreter. Enter handles CLS and SCREEN itself, because
// those are terminal operations rather than language ones, and answers anything
// else the way a BASIC does when it does not understand. Milestone 5 replaces
// that with the real thing.
//
// The one real change: screen_editor derives its row count and centring margin
// from a band it is given, rather than reading them from a per-mode table of
// numbers measured against a 960x540 panel.
//
// The band is the whole terminal area, always, so the grid keeps its full 19
// rows and the on-screen keyboard is painted over the bottom of them. That is
// the PaperS3's behaviour and it is a deliberate choice here: the plan for this
// device is a physical keyboard, with the on-screen one as the fallback it is
// there. The cost, worth stating because it is real, is that while the
// on-screen keyboard is up the cursor can sit behind it, since a terminal's
// cursor lives at the bottom of the used area. Making the band shrink instead
// is a one-line change in applyBand() if that ever becomes the common case.

#include <Arduino.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <builtinFonts/unscii_10x20.h>
#include <builtinFonts/unscii_12x24.h>
#include <builtinFonts/unscii_15x30.h>
#include <builtinFonts/unscii_8x16.h>

#include <cstring>

#include "board_fnk0103n.h"
#include "config.h"
#include "osk.h"
#include "screen_editor.h"
#include "tft_renderer.h"

static TFT_eSPI tft;
static TftRenderer renderer(tft);
static Preferences prefs;

static const EpdFont fontUnscii15x30(&unscii_15x30);
static const EpdFont fontUnscii12x24(&unscii_12x24);
static const EpdFont fontUnscii10x20(&unscii_10x20);
static const EpdFont fontUnscii8x16(&unscii_8x16);

static constexpr int STATUS_BAR_H = 16;
static constexpr int OSK_ROWS = 6;
static constexpr int OSK_ROW_H = 32;
static constexpr int OSK_H = OSK_ROWS * OSK_ROW_H;  // 192

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

static int g_palette = 0;
static bool g_oskVisible = true;
static bool g_cursorOn = true;

// Carried over from the PaperS3's main.cpp unchanged. Three-byte forms are not
// reachable from this keyboard, but are handled rather than silently truncated
// if that ever changes.
static int codepointToUtf8(uint32_t cp, char* out) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  out[0] = static_cast<char>(0xE0 | (cp >> 12));
  out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[2] = static_cast<char>(0x80 | (cp & 0x3F));
  return 3;
}

// The terminal owns everything below the status bar, whether or not the
// keyboard is drawn over part of it. See the note at the top of this file.
static void applyBand() {
  screenEditorSetBand(STATUS_BAR_H, SCREEN_H - STATUS_BAR_H);
}

// One row, composed and pushed in a single transfer. This is the path a
// keystroke takes, and the reason drawTextOpaque exists.
static void drawTerminalRow(const int r) {
  char utf8Row[SCREEN_EDITOR_MAX_COLS * 3 + 1];
  const int cols = screenEditorCols();
  int o = 0;
  for (int c = 0; c < cols; c++) {
    o += codepointToUtf8(screenEditorGetCell(r, c), utf8Row + o);
  }
  utf8Row[o] = '\0';
  const int y = screenEditorMarginY() + r * screenEditorCellH();
  renderer.drawTextOpaque(screenEditorFontId(), 0, y, SCREEN_W, screenEditorCellH(), 0, y, utf8Row);
}

static void drawCursor(const bool on) {
  const int cx = screenEditorGetCursorCol() * screenEditorCellW();
  const int cy = screenEditorMarginY() + screenEditorGetCursorRow() * screenEditorCellH();
  if (on) {
    renderer.fillRect(cx, cy, screenEditorCellW(), screenEditorCellH(), true);
  } else {
    drawTerminalRow(screenEditorGetCursorRow());
  }
}

static void drawTerminal() {
  const int rows = screenEditorRows();
  for (int r = 0; r < rows; r++) drawTerminalRow(r);
  // The band's centring margin, above and below the rows, is not covered by any
  // row's own rectangle and would otherwise keep whatever the previous SCREEN
  // mode left there.
  const int top = STATUS_BAR_H;
  const int height = SCREEN_H - STATUS_BAR_H;
  const int used = rows * screenEditorCellH();
  const int margin = screenEditorMarginY() - top;
  if (margin > 0) renderer.fillRect(0, top, SCREEN_W, margin, false);
  const int below = top + height - (screenEditorMarginY() + used);
  if (below > 0) renderer.fillRect(0, screenEditorMarginY() + used, SCREEN_W, below, false);
}

static void drawStatusBar() {
  char line[96];
  snprintf(line, sizeof(line), " SCR%d %dx%d  %s  %s%s%s", screenEditorGetMode(), screenEditorCols(),
           screenEditorRows(), kPalettes[g_palette].name, oskShiftArmed() ? "SHF " : "",
           oskCtrlArmed() ? "CTL " : "", oskCapsLockOn() ? "CAPS" : "");
  renderer.fillRect(0, 0, SCREEN_W, STATUS_BAR_H, false);
  tft.setTextFont(2);
  tft.setTextColor(renderer.getPalette().ink, renderer.getPalette().paper);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(line, 2, 0);
}

static void drawAll() {
  renderer.clearScreen();
  drawStatusBar();
  drawTerminal();
  if (g_oskVisible) oskDraw();
  drawCursor(g_cursorOn);
}

// screen_editor.cpp calls this when a running program needs its output on the
// panel mid-run. Nothing is buffered here, so there is nothing to flush; it
// stays because the interpreter will call it in milestone 5.
void screenEditorFlushDisplay() {}

// Trims and upper-cases a logical line into `out`, the way a BASIC reads its
// input: case-insensitive commands, leading and trailing space ignored.
static void normaliseLine(const char* in, char* out, const size_t outSize) {
  while (*in == ' ') in++;
  size_t n = 0;
  while (*in && n + 1 < outSize) out[n++] = static_cast<char>(toupper(*in++));
  while (n > 0 && out[n - 1] == ' ') n--;
  out[n] = '\0';
}

// Everything Enter does until there is an interpreter.
//
// CLS and SCREEN are here rather than waiting for milestone 5 because they are
// terminal operations, not language ones: they change this file's state and
// nothing else. Everything else gets the answer a BASIC gives when it does not
// understand, which is honest about what is and is not built.
static void handleEnter() {
  char raw[MAX_PROGRAM_LINE_LEN];
  screenEditorGetLogicalLineText(raw, sizeof(raw));

  char cmd[MAX_PROGRAM_LINE_LEN];
  normaliseLine(raw, cmd, sizeof(cmd));

  if (cmd[0] == '\0') {
    screenEditorStartNewInputLine();
  } else if (isdigit(static_cast<unsigned char>(cmd[0]))) {
    // A numbered line is program text. With no interpreter to store it, the
    // only thing to do is what a real machine does visually: leave it on the
    // screen and drop to a fresh input line.
    screenEditorStartNewInputLine();
  } else if (strcmp(cmd, "CLS") == 0) {
    screenEditorReset();
    screenEditorTermPrintLine("Ok");
  } else if (strncmp(cmd, "SCREEN ", 7) == 0 && cmd[7] >= '0' && cmd[7] <= '3' && cmd[8] == '\0') {
    screenEditorSetMode(cmd[7] - '0');
    applyBand();
    screenEditorTermPrintLine("Ok");
  } else {
    screenEditorClearLogicalLine();
    screenEditorTermPrintLine("Syntax error");
    screenEditorTermPrintLine("Ok");
  }
  drawTerminal();
  drawStatusBar();
}

static void onOskKey(const uint8_t hid, const uint8_t modifiers) {
  const int rowBefore = screenEditorGetCursorRow();

  switch (hid) {
    case OSK_HID_BACKSPACE: screenEditorBackspace(); break;
    case OSK_HID_LEFT:      screenEditorMoveCursor(0, -1); break;
    case OSK_HID_RIGHT:     screenEditorMoveCursor(0, 1); break;
    case HID_KEY_UP:        screenEditorMoveCursor(-1, 0); break;
    case HID_KEY_DOWN:      screenEditorMoveCursor(1, 0); break;
    case OSK_HID_ESCAPE:
      screenEditorReset();
      drawTerminal();
      drawCursor(g_cursorOn);
      return;
    default: {
      const char ch = oskHidToChar(hid, modifiers);
      if (ch == '\n') {
        handleEnter();
        drawCursor(g_cursorOn);
        return;
      }
      if (ch == 0) return;  // Tab and the modifiers themselves have nothing to insert
      screenEditorInsertCodepoint(static_cast<uint32_t>(static_cast<unsigned char>(ch)));
      break;
    }
  }

  // Typing usually touches one row. It touches two when it wraps onto the next,
  // and the whole screen when that wrap scrolled, which is the only case worth
  // repainting everything for.
  const int rowNow = screenEditorGetCursorRow();
  if (rowNow == rowBefore) {
    drawTerminalRow(rowNow);
  } else if (rowNow == rowBefore + 1) {
    drawTerminalRow(rowBefore);
    drawTerminalRow(rowNow);
  } else {
    drawTerminal();
  }
  drawCursor(g_cursorOn);
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== CYD MicroBASIC/MicroWriter -- milestone 4, terminal ===");

  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);
  tft.init();

  uint16_t touchCal[5];
  prefs.begin("cyd", true);
  if (prefs.getBytesLength("touchcal") == sizeof(touchCal)) {
    prefs.getBytes("touchcal", touchCal, sizeof(touchCal));
    tft.setTouch(touchCal);
  } else {
    Serial.println("touch: NO calibration in NVS, taps will be wrong");
  }
  prefs.end();

  renderer.setOrientation(TftRenderer::LandscapeCounterClockwise);
  renderer.setPalette(kPalettes[g_palette].palette);
  renderer.insertFont(FONT_SCREEN_MONO_0, EpdFontFamily(&fontUnscii15x30));
  renderer.insertFont(FONT_SCREEN_MONO_1, EpdFontFamily(&fontUnscii12x24));
  renderer.insertFont(FONT_SCREEN_MONO_2, EpdFontFamily(&fontUnscii10x20));
  renderer.insertFont(FONT_SCREEN_MONO_3, EpdFontFamily(&fontUnscii8x16));
  renderer.begin();

  applyBand();
  screenEditorReset();

  char banner[64];
  screenEditorTermPrintLine("MicroBASIC 0.1");
  snprintf(banner, sizeof(banner), "%u Bytes free", static_cast<unsigned>(ESP.getFreeHeap()));
  screenEditorTermPrintLine(banner);
  screenEditorTermPrintLine("");
  screenEditorTermPrintLine("Ok");

  oskInit(renderer, FONT_SCREEN_MONO_2, FONT_SCREEN_MONO_3, 0, SCREEN_H - OSK_H, SCREEN_W, OSK_H,
          onOskKey);

  const uint32_t t0 = micros();
  drawAll();
  Serial.printf("first paint: %lu us\n", static_cast<unsigned long>(micros() - t0));

  const uint32_t t1 = micros();
  drawTerminalRow(0);
  Serial.printf("one terminal row: %lu us\n", static_cast<unsigned long>(micros() - t1));
  Serial.printf("terminal: %d cols x %d rows (keyboard %s) | heap %u KB\n", screenEditorCols(),
                screenEditorRows(), g_oskVisible ? "up" : "down",
                static_cast<unsigned>(ESP.getFreeHeap() / 1024));
}

void loop() {
  uint16_t x = 0, y = 0;
  if (tft.getTouch(&x, &y)) {
    if (y < STATUS_BAR_H + 8) {
      if (x < SCREEN_W / 3) {
        screenEditorSetMode((screenEditorGetMode() + 1) % 4);
        applyBand();
      } else if (x < 2 * SCREEN_W / 3) {
        g_oskVisible = !g_oskVisible;
      } else {
        g_palette = (g_palette + 1) % kPaletteCount;
        renderer.setPalette(kPalettes[g_palette].palette);
      }
      drawAll();
    } else if (g_oskVisible) {
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
    delay(220);
    return;
  }

  static uint32_t lastBlink = 0;
  if (millis() - lastBlink > 500) {
    lastBlink = millis();
    g_cursorOn = !g_cursorOn;
    drawCursor(g_cursorOn);
  }
  delay(10);
}
