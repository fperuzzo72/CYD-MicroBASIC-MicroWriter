// Milestone 5: the interpreter.
//
// This file no longer decides what Enter means, and that is the point. The
// chain is the one both earlier devices use, and every link of it was already
// written:
//
//   osk.cpp emits a USB HID keycode and modifier byte
//     -> enqueueKeyEvent(), the same call a BLE keyboard would make
//       -> processAllInput() in terminal_input.cpp
//         -> screen_editor for editing keys, tbExecuteLine() for Enter
//           -> the interpreter, printing back through the runtime's outch()
//
// osk.h promised this would work ("the SAME wire format
// input_handler.cpp::enqueueKeyEvent() already expects"), and it does. The
// placeholder Enter handler from milestone 4, with its hand-rolled CLS and
// SCREEN, is gone: those are real commands now.
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

#if MICROWRITER
#error "The microwriter env is not buildable yet: it excludes screen_editor and \
the interpreter, and main.cpp has nothing else to draw until text_editor.cpp is \
ported in milestone 7. Build -e fnk0103n. See docs/PORTING_PLAN.md."
#endif

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

#include <FS.h>
#include <SD.h>
#include <SPI.h>

#include "board_fnk0103n.h"
#include "config.h"
#include "input_handler.h"
#include "osk.h"
#include "screen_editor.h"
#include "tb_bridge.h"
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
  // No cursor while a program has control. A cursor means "waiting for you to
  // type", and nothing is; on a program that repaints cells in place it reads
  // as a block stuck to whatever it drew last.
  if (tbIsRunning()) return;
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

// Called by the runtime's byield() while a program is running, so a loop's
// PRINT output appears as it goes rather than all at once when the program
// stops. On e-paper this had to block on a panel refresh; here the pixels are
// already out by the time drawTerminal() returns, so it is just a repaint.
//
// byield() throttles this by time rather than by print volume, which matters:
// a full repaint is 143ms and doing one per PRINT would make a program crawl.
void screenEditorFlushDisplay() {
  drawTerminal();
}

// --- What is not built here yet -------------------------------------------
//
// terminal_input.cpp and tb_bridge.cpp reach out to four commands and two
// button hooks. Defining them here, out loud, rather than stubbing them into
// silence: a MENU command that does nothing looks like a bug, and one that says
// what is missing is a to-do list you can read from the machine itself.

// This board has no physical buttons. Not "none wired up yet": the FNK0103N has
// a reset and a boot button on the back and nothing on the front, where the X4
// has a d-pad and the PaperS3 has a power key. These two are permanent no-ops
// rather than milestones, which is why they say so instead of printing.
void physicalButtonsRearm() {}
void pumpPhysicalButtonsForProgram() {}

static void notBuiltYet(const char* what) {
  char msg[64];
  snprintf(msg, sizeof(msg), "?%s not built yet", what);
  screenEditorTermPrintLine(msg);
}

// Milestone 8. See docs/PORTING_PLAN.md, and the flash budget note there:
// this is the one that may not fit alongside the BLE keyboard, and if it comes
// to that, this is what gives way.
void startWifiSyncFromCommand() { notBuiltYet("SYNC"); }

// Milestone 7, the prose editor.
void startEditorFromCommand() { notBuiltYet("EDITOR"); }

// Neither of these is coming. Both exist for the PaperS3's shared dual-boot
// layout with CrossPoint: one switches firmware slots, the other browses that
// reader's library. This board has a single app partition and no CrossPoint.
// They stay reachable as commands only because terminal_input.cpp dispatches
// them; the honest answer is that they have nowhere to go.
void startReaderSwitchFromCommand() { screenEditorTermPrintLine("?READER is PaperS3 only"); }
void startVcFromCommand() { screenEditorTermPrintLine("?VC is PaperS3 only"); }

// A tapped key goes into the same queue a keyboard would feed, and nothing
// here interprets it. Everything that used to be in this function now lives in
// terminal_input.cpp, where it is shared with whatever else ever produces key
// events.
static void onOskKey(const uint8_t hid, const uint8_t modifiers) {
  enqueueKeyEvent(hid, modifiers, true);
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== CYD MicroBASIC/MicroWriter -- milestone 5, interpreter ===");

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

  // The card, before the interpreter: tbSetup() probes for an autoexec.bas and
  // fsbegin() creates the program directory, and both need a mounted card. Its
  // own VSPI bus, so nothing here contends with the panel on HSPI.
  static SPIClass sdSpi(VSPI);
  sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  const bool sdOk = SD.begin(PIN_SD_CS, sdSpi, 20000000);
  Serial.printf("SD: %s\n", sdOk ? "mounted" : "NOT mounted");

  inputSetup();

  // Quiet for the boot probe only: the interpreter looks for an autoexec.bas
  // with a plain open, so not finding one is the normal case rather than a
  // fault worth printing on a fresh screen.
  tbRuntimeSetQuiet(true);
  tbSetup();
  tbRuntimeSetQuiet(false);

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
    // Fall through rather than returning: the key just queued has to be
    // processed in this same pass, or nothing appears until the next touch.
  }

  // Everything a keystroke does happens in here, including running a program.
  // A RUN can sit inside this call for as long as the program takes.
  if (processAllInput() > 0) {
    drawTerminal();
    drawStatusBar();
    drawCursor(true);
    g_cursorOn = true;
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
