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

// Two unscii rows tall, not one.
//
// 16px was enough to read and not enough to hit: the bar carries six buttons
// now and a 16px target on a resistive panel is a miss waiting to happen. 32
// also matches the keyboard's own row height, and it keeps the arithmetic
// exact where it matters: 320 - 32 = 288, which is 18 rows of the 8x16 cell
// with nothing left over, and 288 - 192 of keyboard leaves 96, which is 6.
//
// It costs one terminal row, 19 to 18. Worth it for buttons that can be
// pressed on purpose.
static constexpr int STATUS_BAR_H = 32;
static constexpr int OSK_ROWS = 6;
static constexpr int OSK_ROW_H = 32;
static constexpr int OSK_H = OSK_ROWS * OSK_ROW_H;  // 192

struct NamedPalette {
  const char* name;
  TftRenderer::Palette palette;
};
static const NamedPalette kPalettes[] = {
    // Upper case because it is an acronym, where the other three are words.
    {"MSX", TftRenderer::MsxBlue},
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

// The terminal always owns everything below the status bar, so the grid is
// always its full height. What the keyboard changes is the WINDOW onto it, not
// its size.
//
// This is the third arrangement tried and the first that loses nothing. Giving
// the terminal the whole area and painting the keyboard over it left the cursor
// behind the keys. Shrinking the grid to fit kept the cursor visible but threw
// away the rows that no longer fitted, so folding the keyboard away revealed
// blank space where the history had been. A window keeps all 18 rows alive,
// shows the 6 the keyboard leaves room for, and folding it away brings the rest
// back because they were never gone.
//
// It also stops being a decision about which keyboard is normal. The window
// works the same whether the on-screen keyboard is the only way in or an
// occasional visitor next to a BLE one, which is what milestone 9 needs.
static void applyBand() {
  screenEditorSetBand(STATUS_BAR_H, SCREEN_H - STATUS_BAR_H);
}

// The visible slice of the panel, and how many grid rows fit in it.
static int viewBandH() { return SCREEN_H - STATUS_BAR_H - (g_oskVisible ? OSK_H : 0); }

static int visibleRows() {
  int n = viewBandH() / screenEditorCellH();
  if (n < 1) n = 1;
  if (n > screenEditorRows()) n = screenEditorRows();
  return n;
}

// First grid row shown. Follows the cursor rather than pinning to the bottom:
// on a fresh screen the cursor is near the top and the window should be too,
// and once the grid fills this settles on showing the last rows, which is where
// a terminal's attention is.
static int viewTopRow() {
  const int vis = visibleRows();
  int top = screenEditorGetCursorRow() - vis + 1;
  const int maxTop = screenEditorRows() - vis;
  if (top > maxTop) top = maxTop;
  if (top < 0) top = 0;
  return top;
}

// Whatever the visible rows do not divide evenly, split above and below. Not
// screenEditorMarginY(), which centres the WHOLE grid in the whole band and is
// the wrong number whenever the window is smaller than the grid.
static int viewOriginY() {
  return STATUS_BAR_H + (viewBandH() - visibleRows() * screenEditorCellH()) / 2;
}

// Where a grid row lands on the panel, or -1 when it is outside the window.
//
// Rows outside must not be drawn, and not only because it would be wasted: the
// keyboard is painted below the window, so a row drawn past it erases keys.
// That is what happened when a keystroke first started repainting the whole
// terminal. The keys kept working, because osk.cpp hit-tests coordinates and
// has no idea whether it is still on screen, so the machine looked like it had
// lost its keyboard while still obeying it.
static int rowScreenY(const int r) {
  const int rel = r - viewTopRow();
  if (rel < 0 || rel >= visibleRows()) return -1;
  return viewOriginY() + rel * screenEditorCellH();
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
  const int y = rowScreenY(r);
  if (y < 0) return;
  renderer.drawTextOpaque(screenEditorFontId(), 0, y, SCREEN_W, screenEditorCellH(), 0, y, utf8Row);
}

static void drawCursor(const bool on) {
  // No cursor while a program has control. A cursor means "waiting for you to
  // type", and nothing is; on a program that repaints cells in place it reads
  // as a block stuck to whatever it drew last.
  if (tbIsRunning()) return;
  // And none outside the window: the blink would otherwise punch a hole
  // through the keys twice a second.
  const int cy = rowScreenY(screenEditorGetCursorRow());
  if (cy < 0) return;
  const int cx = screenEditorGetCursorCol() * screenEditorCellW();
  if (on) {
    renderer.fillRect(cx, cy, screenEditorCellW(), screenEditorCellH(), true);
  } else {
    drawTerminalRow(screenEditorGetCursorRow());
  }
}

static void drawTerminal() {
  const int rows = screenEditorRows();
  for (int r = 0; r < rows; r++) drawTerminalRow(r);  // clips itself to the window
  // The band's centring margin, above and below the rows, is not covered by any
  // row's own rectangle and would otherwise keep whatever the previous SCREEN
  // mode left there.
  const int used = visibleRows() * screenEditorCellH();
  const int margin = viewOriginY() - STATUS_BAR_H;
  if (margin > 0) renderer.fillRect(0, STATUS_BAR_H, SCREEN_W, margin, false);
  const int below = STATUS_BAR_H + viewBandH() - (viewOriginY() + used);
  if (below > 0) renderer.fillRect(0, viewOriginY() + used, SCREEN_W, below, false);
}

// --- Status bar -----------------------------------------------------------
//
// Six buttons, modelled on the PaperS3's bar, with two differences. READER and
// VC are gone: both exist for that device's dual-boot layout with CrossPoint
// and have nowhere to go here. In their place are SCR and COLOR, which were
// tap-a-third-of-the-bar gestures until now, discoverable only by being told.
//
// BLE, SYNC and EDIT are drawn before they work, on purpose. A button that
// shows "--" says the machine has a place for that and does not have it yet,
// which is a truer picture than a bar that grows a new button every few weeks.
// Tapping one prints what is missing on the terminal.
//
// Each button is a label over a value, which is what the 32px height buys: SCR
// shows which mode is current, COLOR shows which palette, KBD shows whether
// the keyboard is up. Shift and Caps indicators are NOT here any more; the
// keyboard draws its own armed keys inverted, which is the same information
// where the finger already is.

// Left to right. The order is the PaperS3's, read off its own layout code,
// which places them right to left: BLE at the edge, then KBD, SYNC, EDITOR,
// READER, with the title filling whatever is left. READER is the one that has
// nowhere to go here, so SCR and COLOR take its place at the far left and
// everything else keeps the position a hand already knows.
enum BarButtonId { BTN_SCR = 0, BTN_COLOR, BTN_EDIT, BTN_SYNC, BTN_KBD, BTN_BLE, BTN_COUNT };
static constexpr int BTN_W = SCREEN_W / BTN_COUNT;  // 80

static void drawBarButton(const int index, const char* label, const char* value, const bool active) {
  const int x = index * BTN_W;
  const int inset = 1;
  renderer.fillRect(x, 0, BTN_W, STATUS_BAR_H, active);
  renderer.drawRect(x + inset, inset, BTN_W - 2 * inset, STATUS_BAR_H - 2 * inset, !active);

  // Label above, value below, both in the 8x16 cell so two lines fit the bar.
  // Drawn in the opposite state to the fill so an active button reads as
  // inverted, the same convention the keyboard uses for an armed key.
  //
  // The value sits at 14 rather than 16, and the two pixels matter. unscii's
  // descenders reach row 15 of the 16-row cell with no padding at all (measured
  // from the font data: g, p, q, y all ink rows 6 to 15), so a value drawn at
  // 16 puts the tail of "green" and "paper" on row 31, past the button's border
  // at row 30. At 14 the ink spans rows 16 to 29 and lands exactly inside the
  // usable area, while capitals only reach row 12, leaving the label clear.
  const int lw = renderer.getTextWidth(FONT_SCREEN_MONO_3, label);
  renderer.drawText(FONT_SCREEN_MONO_3, x + (BTN_W - lw) / 2, 0, label, !active);
  if (value && *value) {
    const int vw = renderer.getTextWidth(FONT_SCREEN_MONO_3, value);
    renderer.drawText(FONT_SCREEN_MONO_3, x + (BTN_W - vw) / 2, 14, value, !active);
  }
}

static void drawStatusBar() {
  renderer.fillRect(0, 0, SCREEN_W, STATUS_BAR_H, false);

  char scr[8];
  snprintf(scr, sizeof(scr), "%d", screenEditorGetMode());

  drawBarButton(BTN_SCR, "SCR", scr, false);
  drawBarButton(BTN_COLOR, "COLOR", kPalettes[g_palette].name, false);
  drawBarButton(BTN_EDIT, "EDITOR", "--", false);
  drawBarButton(BTN_SYNC, "SYNC", "--", false);
  drawBarButton(BTN_KBD, "KBD", g_oskVisible ? "on" : "off", g_oskVisible);
  drawBarButton(BTN_BLE, "BLE", "--", false);
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

// This board has no front panel buttons: a reset and a boot button on the back,
// where the X4 has a d-pad and the PaperS3 has a power key. Nothing to rearm.
void physicalButtonsRearm() {}

// Called by the runtime's byield() every sixteen statements while a program
// runs. Named for the d-pad it pumps on the X4, but its real job is the one
// thing loop() cannot do during a RUN: get input to a running program.
//
// loop() is blocked inside the interpreter for the entire run, so nothing else
// polls the panel. Without this, Esc never reaches the queue, BREAK never
// fires, and an endless program is endless: the machine still draws, still
// prints, and cannot be stopped. Which is exactly what a `20 GOTO 10` did
// before this existed, because it was stubbed out as a no-op on the reasoning
// that a board with no buttons has no buttons to pump. True about buttons,
// wrong about the hook.
//
// Throttled by time rather than run on every call: byield() reaches here
// hundreds of times a second and a touch read is an SPI transaction. 25ms is
// far faster than a finger and costs the program almost nothing. The same
// interval also debounces, which a resistive panel needs.
void pumpPhysicalButtonsForProgram() {
  if (!g_oskVisible) return;
  static uint32_t lastPoll = 0;
  const uint32_t now = millis();
  if (now - lastPoll < 25) return;
  lastPoll = now;

  uint16_t x = 0, y = 0;
  if (!tft.getTouch(&x, &y)) return;

  static uint32_t lastTap = 0;
  if (now - lastTap < 220) return;
  lastTap = now;
  oskHandleTap(x, y);  // enqueues through onOskKey, which is all a break needs
}

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
    if (y < STATUS_BAR_H) {
      switch (x / BTN_W) {
        case BTN_KBD:
          // Only the window changes. The grid keeps all 18 rows either way, so
          // folding the keyboard away brings back the rows it was hiding
          // rather than revealing blanks where they used to be. drawAll()
          // repaints, which it has to: those rows were never drawn.
          g_oskVisible = !g_oskVisible;
          break;
        case BTN_SCR:
          screenEditorSetMode((screenEditorGetMode() + 1) % 4);
          applyBand();
          break;
        case BTN_COLOR:
          g_palette = (g_palette + 1) % kPaletteCount;
          renderer.setPalette(kPalettes[g_palette].palette);
          break;
        // The three that are drawn before they work answer through the same
        // functions the MENU commands do, so there is one place saying what is
        // missing rather than two that could drift apart.
        case BTN_BLE:  screenEditorTermPrintLine("?BLE not built yet"); break;
        case BTN_SYNC: startWifiSyncFromCommand(); break;
        case BTN_EDIT: startEditorFromCommand(); break;
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
