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

// Two machines from this one file.
//
// MICROWRITER excludes the interpreter, the character-grid terminal, and the
// command dispatch built on them (see platformio.ini). What is left is the
// writing machine MicroBASIC grew out of: the browser is open from boot and
// never closes, because there is nothing behind it to go back to.
//
// The guards below are all of the difference. They are #if rather than runtime
// checks for the reason platformio.ini gives: the excluded files are not
// compiled, so their symbols do not exist to be called.

#include <Arduino.h>
#include <BleKeyboardHost.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <Utf8.h>
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
#include "file_browser.h"
#include "file_manager.h"
#include "input_handler.h"
#include "osk.h"
#include "sd_datetime.h"
#include "text_editor.h"
#include "tft_renderer.h"
#include "wifi_sync.h"

#if !MICROWRITER
#include "screen_editor.h"
#include "tb_bridge.h"
#endif

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

// BleHid is a macro from BleKeyboardHost.h expanding to getInstance(), not a
// variable. Declaring one of that name here shadowed nothing and expanded to
// nonsense; the header already provides the accessor.

// --- Pairing a keyboard, without a pairing screen ---------------------------
//
// Ported from the PaperS3's own auto-pair, whose reasoning holds here: with no
// bond yet, scan and take the first connectable HID device seen; with a bond,
// let NimBLE reconnect on its own and only start scanning again after a long
// idle stretch, so a keyboard that is merely asleep is not raced against.
//
// The BLE button forces a scan immediately, which is what to press when a
// second keyboard needs pairing while the first is still bonded.
static constexpr uint32_t kBleScanDurationMs = 5000;
static constexpr uint32_t kBleScanRetryDelayMs = 3000;
static constexpr uint32_t kBleBondedRetryGraceMs = 20000;
static uint32_t g_bleIdleSinceMs = 0;
static uint32_t g_bleNextScanAt = 0;
static bool g_bleScanArmed = false;

static void bleKbdAutoPair() {
  const uint32_t now = millis();

  if (BleHid.isConnected() || BleHid.isConnecting() || BleHid.isScanning()) {
    g_bleIdleSinceMs = 0;  // something is in flight; only measure genuine idle
  } else if (g_bleIdleSinceMs == 0) {
    g_bleIdleSinceMs = now;
  }

  if (BleHid.isConnected() || BleHid.isConnecting()) return;
  if (BleHid.pairedCount() > 0 && (now - g_bleIdleSinceMs) < kBleBondedRetryGraceMs) return;

  if (BleHid.isScanning()) {
    g_bleScanArmed = true;
    return;
  }
  if (g_bleScanArmed) {
    g_bleScanArmed = false;
    for (uint8_t i = 0; i < BleHid.deviceCount(); i++) {
      const freeink::DiscoveredDevice& d = BleHid.device(i);
      if (d.hid && d.connectable) {
        BleHid.connect(d.addr);
        break;
      }
    }
    BleHid.releaseScanResults();
    g_bleNextScanAt = now + kBleScanRetryDelayMs;
    return;
  }
  if (now >= g_bleNextScanAt) {
    BleHid.startScan(kBleScanDurationMs);
  }
}

static void forceBlePairingNow() {
  BleHid.disconnect();
  g_bleIdleSinceMs = 0;
  g_bleNextScanAt = 0;
  g_bleScanArmed = false;
  BleHid.startScan(kBleScanDurationMs);
}

static int g_palette = 0;
// Folded away at boot, and the reasoning is that the cost is asymmetric.
// Someone who is going to use the on-screen keyboard already has a hand on the
// panel, so the one tap on KBD costs them nothing. Someone with a BLE keyboard
// who booted into a keyboard they did not want would have to reach for the
// screen to get rid of it, which is the thing a real keyboard exists to avoid,
// and until they did they would be working in 6 rows instead of 18.
//
// The PaperS3 decides this at runtime instead, showing the keyboard whenever no
// BLE keyboard is connected. That does not carry: at boot nothing is connected
// yet, because pairing takes a few seconds, so it would appear and then vanish.
static bool g_oskVisible = false;
static bool g_cursorOn = true;

#if !MICROWRITER
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
#endif  // !MICROWRITER

// Owned by input_handler.cpp, which is where the PaperS3 keeps it too. Set by
// file_browser.cpp whenever something it did needs repainting, and cleared here
// after the repaint.
extern bool screenDirty;

// --- The prose side ---------------------------------------------------------
//
// The PaperS3 draws notes in NotoSans, a proportional font. Those headers are
// about 2.6MB between the four weights and none is linked here, on a board with
// 3.2MB of app partition that still has WiFi and BLE to fit.
//
// So MicroWriter writes in unscii, the same monospace cell the terminal uses:
// already in the binary, costs nothing, and gives 60 characters a line at 8x16.
// It also simplifies the wiring, because text_editor.cpp asks the caller for a
// per-codepoint width and with a monospace font that is a constant.
//
// Reversible, not permanent. Embedding NotoSans 14 regular and bold would be
// roughly 630KB of the 2.7MB still free, and the only code that would change is
// editorGlyphWidth() below.
static constexpr int FONT_PROSE = FONT_SCREEN_MONO_3;  // 8x16, 60 columns
static constexpr int FONT_LIST = FONT_SCREEN_MONO_2;   // 10x20, easier to hit in a list
static constexpr int PROSE_CELL_W = 8;
static constexpr int PROSE_MARGIN = 4;

static int editorGlyphWidth(uint32_t /*cp*/) { return PROSE_CELL_W; }

static int advanceWidth(const char* str, const int nbytes) {
  int w = 0;
  const auto* p = reinterpret_cast<const unsigned char*>(str);
  const unsigned char* end = p + nbytes;
  while (p < end) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    w += editorGlyphWidth(cp);
  }
  return w;
}

// The visible slice of the panel, and how many grid rows fit in it.
static int viewBandH() { return SCREEN_H - STATUS_BAR_H - (g_oskVisible ? OSK_H : 0); }

#if !MICROWRITER
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
#endif  // !MICROWRITER

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

// Per-button widths rather than one number, because the labels are not one
// length. COLOR and EDITOR need 60: "EDITOR" is 48px in the 8x16 cell and
// "green", "amber" and "paper" are 40. The other four hold nothing longer than
// "SYNC" at 32px, so 52 is generous for them and the 32px it releases goes to
// the nameplate.
//
// Order matches BarButtonId: SCR, COLOR, EDITOR, SYNC, KBD, BLE.
//
// A width of zero removes a button: it is neither drawn nor hit, and its space
// falls to the nameplate. MicroWriter drops SCR, which is a terminal mode, and
// EDITOR, which would open what is already open.
#if MICROWRITER
static constexpr int kBtnW[BTN_COUNT] = {0, 60, 0, 52, 52, 52};
static constexpr int BTN_TOTAL_W = 0 + 60 + 0 + 52 + 52 + 52;  // 216
#else
static constexpr int kBtnW[BTN_COUNT] = {52, 60, 60, 52, 52, 52};
static constexpr int BTN_TOTAL_W = 52 + 60 + 60 + 52 + 52 + 52;  // 328
#endif
static constexpr int TITLE_W = SCREEN_W - BTN_TOTAL_W;

static int btnX(const int index) {
  int x = TITLE_W;
  for (int i = 0; i < index; i++) x += kBtnW[i];
  return x;
}

// A bar cell: filled, framed, with a label over a value. The nameplate is one
// of these too, which is what makes the bar read as a bar rather than as five
// boxes and some floating text.
static void drawBarCell(const int x, const int w, const char* label, const char* value,
                        const bool active) {
  const int inset = 1;
  renderer.fillRect(x, 0, w, STATUS_BAR_H, active);
  renderer.drawRect(x + inset, inset, w - 2 * inset, STATUS_BAR_H - 2 * inset, !active);

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
  renderer.drawText(FONT_SCREEN_MONO_3, x + (w - lw) / 2, 0, label, !active);
  if (value && *value) {
    const int vw = renderer.getTextWidth(FONT_SCREEN_MONO_3, value);
    renderer.drawText(FONT_SCREEN_MONO_3, x + (w - vw) / 2, 14, value, !active);
  }
}

static void drawBarButton(const int index, const char* label, const char* value, const bool active) {
  if (kBtnW[index] == 0) return;  // not on this machine
  drawBarCell(btnX(index), kBtnW[index], label, value, active);
}

static void drawStatusBar() {
  // No clear first, and that is deliberate. The nameplate and the buttons tile
  // the bar exactly (TITLE_W is defined as SCREEN_W minus the button widths, so
  // the cells always sum to the full width, MicroWriter's zero-width entries
  // included), and every cell fills its own rectangle before drawing into it.
  // Clearing first would be a second pass over pixels that are about to be
  // painted anyway, which is precisely what made the bar flash on every
  // keystroke. Same fix as the band, in the one place it had been missed.

  // The nameplate is drawn as a bar cell like the buttons, so the whole strip
  // is framed and reads as one thing. What the machine is on top, which board
  // it is underneath.
#if MICROWRITER
  drawBarCell(0, TITLE_W, "MicroWriter CYD", "FNK0103-N", false);
#else
  drawBarCell(0, TITLE_W, "MicroBASIC CYD", "FNK0103-N", false);
#endif

  char scr[8] = "-";
#if !MICROWRITER
  snprintf(scr, sizeof(scr), "%d", screenEditorGetMode());
#endif

  drawBarButton(BTN_SCR, "SCR", scr, false);
  drawBarButton(BTN_COLOR, "COLOR", kPalettes[g_palette].name, false);
  drawBarButton(BTN_EDIT, "EDITOR", "--", false);
  drawBarButton(BTN_SYNC, "SYNC", isWifiSyncActive() ? "on" : "--", isWifiSyncActive());
  drawBarButton(BTN_KBD, "KBD", g_oskVisible ? "on" : "off", g_oskVisible);
  const char* bleValue = BleHid.isConnected()   ? "on"
                         : BleHid.isScanning()  ? "scan"
                         : BleHid.pairedCount() ? "wait"
                                                : "--";
  drawBarButton(BTN_BLE, "BLE", bleValue, BleHid.isConnected());
}

// The prose editor: the wrapped lines text_editor.cpp computed, the selection
// inverted, and a caret. Ported from the PaperS3's drawEditorUi with the font
// swapped and the band taken from this panel's own geometry.
static void drawEditorUi() {
  const int lh = renderer.getLineHeight(FONT_PROSE);
  const int bandH = viewBandH();
  const int visible = bandH / lh;

  editorSetMaxLineWidthPx(SCREEN_W - 2 * PROSE_MARGIN);
  editorSetVisibleLines(visible);
  editorSetPageJumpLines(visible > 1 ? visible - 1 : 1);
  editorRecalculateLines();

  const char* buf = editorGetBuffer();
  const int lineCount = editorGetLineCount();
  const int top = editorGetViewportStart();
  const int cursorLine = editorGetCursorLine();

  // Every row of the band, not only the rows that have text. A row past the end
  // still gets an opaque empty draw, which is what removes the need to clear the
  // band first. Clearing and then drawing is two passes over the same pixels,
  // and the first one is what reads as the whole screen flashing on every
  // keystroke.
  for (int row = 0; row < visible; row++) {
    const int i = top + row;
    const int y0 = STATUS_BAR_H + row * lh;
    if (i >= lineCount) {
      renderer.drawTextOpaque(FONT_PROSE, 0, y0, SCREEN_W, lh, PROSE_MARGIN, y0, "");
      continue;
    }
    const int start = editorGetLinePosition(i);
    const int end = (i + 1 < lineCount) ? editorGetLinePosition(i + 1) : static_cast<int>(editorGetLength());

    char line[128];
    int n = end - start;
    if (n < 0) n = 0;
    if (n > static_cast<int>(sizeof(line)) - 1) n = static_cast<int>(sizeof(line)) - 1;
    memcpy(line, buf + start, n);
    // Trim the newline the wrap kept, so it is not drawn as a glyph.
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    line[n] = '\0';

    const int y = STATUS_BAR_H + row * lh;
    renderer.drawTextOpaque(FONT_PROSE, 0, y, SCREEN_W, lh, PROSE_MARGIN, y, line);

    if (editorHasSelection()) {
      const int selA = editorGetSelectionStart();
      const int selB = editorGetSelectionEnd();
      const int a = (selA > start) ? selA : start;  // clip to this line
      const int b = (selB < start + n) ? selB : start + n;
      if (b > a) {
        const int preBytes = a - start;
        const int selBytes = b - a;
        const int x1 = PROSE_MARGIN + advanceWidth(line, preBytes);
        const int w = advanceWidth(line + preBytes, selBytes);
        char mid[128];
        int mn = selBytes;
        if (mn > static_cast<int>(sizeof(mid)) - 1) mn = static_cast<int>(sizeof(mid)) - 1;
        memcpy(mid, line + preBytes, mn);
        mid[mn] = '\0';
        // One opaque push: the fill and the inverted text arrive together
        // rather than as a black rectangle followed by white glyphs.
        renderer.drawTextOpaque(FONT_PROSE, x1, y, w, lh, x1, y, mid, false);
      }
    }

    if (i == cursorLine) {
      int c = editorGetCursorCol();
      if (c > n) c = n;
      renderer.fillRect(PROSE_MARGIN + advanceWidth(line, c), y, 2, lh, true);
    }
  }
}

// Naming a new file, or retitling an open one. A new file has no name until
// this is confirmed, which is why it comes before the editor rather than after.
static void drawTitleUi() {
  // Only three lines, so unlike the editor and the list this one cannot cover
  // the band by drawing it. It is also not a per-keystroke path -- the title
  // screen repaints while a name is being typed, but it is three short rows.
  renderer.fillRect(0, STATUS_BAR_H, SCREEN_W, viewBandH(), false);
  const int lh = renderer.getLineHeight(FONT_LIST);
  renderer.drawText(FONT_LIST, 8, STATUS_BAR_H + 8, "Title:");

  char shown[MAX_TITLE_LEN + 2];
  snprintf(shown, sizeof(shown), "%s_", browserTitleBuffer());
  renderer.drawText(FONT_LIST, 8, STATUS_BAR_H + 8 + lh + 8, shown);
  renderer.drawText(FONT_PROSE, 8, STATUS_BAR_H + 8 + 2 * (lh + 8), "Enter to confirm, Esc to cancel");
}

static void drawCentered(const char* line1, const char* line2) {
  renderer.fillRect(0, STATUS_BAR_H, SCREEN_W, viewBandH(), false);
  const int lh = renderer.getLineHeight(FONT_LIST);
  const int y = STATUS_BAR_H + viewBandH() / 2 - (line2 ? lh : lh / 2);
  renderer.drawText(FONT_LIST, (SCREEN_W - renderer.getTextWidth(FONT_LIST, line1)) / 2, y, line1);
  if (line2) {
    renderer.drawText(FONT_LIST, (SCREEN_W - renderer.getTextWidth(FONT_LIST, line2)) / 2, y + lh, line2);
  }
}

// --- The sync screens ------------------------------------------------------
//
// wifi_sync.cpp holds the state machine and the web server; these draw it.
// Ported from the PaperS3's own, with the fonts swapped and the band taken from
// this panel.

static void drawNetworkList() {
  const int rowH = renderer.getLineHeight(FONT_LIST) + 4;
  const int rows = viewBandH() / rowH;
  const int count = getNetworkCount();
  const int sel = getSelectedNetwork();

  if (count == 0) {
    const char* msg = getSyncStatusText();
    drawCentered(msg[0] ? msg : "No networks found", "Esc to cancel");
    return;
  }

  static int scrollTop = 0;
  if (sel < scrollTop) scrollTop = sel;
  if (sel >= scrollTop + rows) scrollTop = sel - rows + 1;
  if (scrollTop > count - rows) scrollTop = count > rows ? count - rows : 0;
  if (scrollTop < 0) scrollTop = 0;

  for (int row = 0; row < rows; row++) {
    const int i = scrollTop + row;
    const int y = STATUS_BAR_H + row * rowH;
    const bool selected = (i == sel) && i < count;
    char line[80];
    if (i >= count) {
      line[0] = '\0';
    } else {
      snprintf(line, sizeof(line), "%s%s%s", isNetworkSaved(i) ? "[saved] " : "", getNetworkSSID(i),
               isNetworkEncrypted(i) ? "  (locked)" : "");
    }
    const int ty = y + (rowH - renderer.getLineHeight(FONT_LIST)) / 2;
    renderer.drawTextOpaque(FONT_LIST, 0, y, SCREEN_W, rowH, 8, ty, line, !selected);
  }
  const int usedH = rows * rowH;
  if (viewBandH() > usedH) {
    renderer.fillRect(0, STATUS_BAR_H + usedH, SCREEN_W, viewBandH() - usedH, false);
  }
}

static void drawPasswordEntry() {
  renderer.fillRect(0, STATUS_BAR_H, SCREEN_W, viewBandH(), false);
  const int lh = renderer.getLineHeight(FONT_LIST);

  char header[48];
  snprintf(header, sizeof(header), "Password for %s:", getNetworkSSID(getSelectedNetwork()));
  renderer.drawText(FONT_LIST, 8, STATUS_BAR_H + 8, header);

  char dots[MAX_FILENAME_LEN];
  const int n = getPasswordLen();
  const int shown = n < static_cast<int>(sizeof(dots)) - 1 ? n : static_cast<int>(sizeof(dots)) - 1;
  for (int i = 0; i < shown; i++) dots[i] = '*';
  dots[shown] = '\0';
  renderer.drawText(FONT_LIST, 8, STATUS_BAR_H + 8 + lh + 8,
                    dots[0] ? dots : "(type it, Enter when done)");
}

static void drawWifiUi() {
  switch (getSyncState()) {
    case SyncState::SCANNING:       drawCentered("Scanning for networks...", nullptr); break;
    case SyncState::NETWORK_LIST:   drawNetworkList(); break;
    case SyncState::PASSWORD_ENTRY: drawPasswordEntry(); break;
    case SyncState::CONNECTING:     drawCentered(getSyncStatusText(), "Esc to cancel"); break;
    case SyncState::SYNCING: {
      char line2[64];
      snprintf(line2, sizeof(line2), "Sent: %d  Received: %d  %s", getSyncFilesSent(),
               getSyncFilesReceived(), isPcConnected() ? "(connected)" : "");
      drawCentered(getSyncStatusText(), line2);
      break;
    }
    case SyncState::DONE:           drawCentered(getSyncStatusText(), "Returning..."); break;
    case SyncState::CONNECT_FAILED: drawCentered(getSyncStatusText(), "Enter to retry, Esc to cancel"); break;
    case SyncState::SAVE_PROMPT:    drawCentered("Save this password?", "Up = Yes    Down = No"); break;
    case SyncState::FORGET_PROMPT:  drawCentered("Forget the saved password?", "Up = Yes    Down = No"); break;
  }
}

static void drawBrowserUi() {
  if (getBrowserState() == BrowserState::EDIT) {
    drawEditorUi();
    return;
  }
  if (getBrowserState() == BrowserState::TITLE) {
    drawTitleUi();
    return;
  }

  const int rowH = renderer.getLineHeight(FONT_LIST) + 4;
  const int rows = viewBandH() / rowH;
  const bool menu = getBrowserState() == BrowserState::MENU;
  const int count = menu ? BROWSER_MENU_COUNT : getFileCount();
  const int sel = getBrowserSelection();

  const char* status = browserStatusText();
  if (count == 0) {
    drawCentered(status[0] ? status : "Nothing here", "Esc to go back");
    return;
  }

  // Minimal-scroll window: move only far enough to keep the selection in view.
  static int scrollTop = 0;
  if (sel < scrollTop) scrollTop = sel;
  if (sel >= scrollTop + rows) scrollTop = sel - rows + 1;
  if (scrollTop > count - rows) scrollTop = count > rows ? count - rows : 0;
  if (scrollTop < 0) scrollTop = 0;

  for (int row = 0; row < rows; row++) {
    const int i = scrollTop + row;
    const int y = STATUS_BAR_H + row * rowH;
    const bool selected = (i == sel) && i < count;
    const char* label = (i >= count) ? "" : (menu ? browserMenuLabel(i) : getFileList()[i].title);
    const int ty = y + (rowH - renderer.getLineHeight(FONT_LIST)) / 2;
    renderer.drawTextOpaque(FONT_LIST, 0, y, SCREEN_W, rowH, 8, ty, label, !selected);
  }
  // The rows do not always divide the band exactly; whatever is left below the
  // last one would otherwise keep the previous screen.
  const int usedH = rows * rowH;
  if (viewBandH() > usedH) {
    renderer.fillRect(0, STATUS_BAR_H + usedH, SCREEN_W, viewBandH() - usedH, false);
  }

  if (status[0]) {
    const int y = STATUS_BAR_H + viewBandH() - renderer.getLineHeight(FONT_PROSE);
    renderer.drawText(FONT_PROSE, 8, y, status);
  }
}

// Repaints the band and nothing else: no clear, no status bar, no keyboard.
// Every path it reaches covers the band opaquely, so the panel goes straight
// from the old content to the new one with nothing blanked in between.
//
// This is what a keystroke uses. drawAll() below is for when the LAYOUT
// changed -- the keyboard folding, the palette, the SCREEN mode -- where the
// keyboard and the gaps between its keys really do have to be repainted.
// THE one place that decides which screen is showing.
//
// This used to be written out in drawBand() and again in drawAll(), and the
// key-routing chain in loop() makes a third. The PaperS3 records what that
// costs: two of its chains disagreed, the browser first in one and second in
// the other, and SYNC drew but could not be typed into. Two of the three are
// now the same function and cannot drift; the third still has to be kept in
// step by hand, and says so where it is.
static void drawCurrentScreen() {
  if (isWifiSyncActive()) {
    drawWifiUi();
    return;
  }
#if MICROWRITER
  drawBrowserUi();  // the only other screen this machine has
#else
  if (isBrowserActive()) {
    drawBrowserUi();
  } else {
    drawTerminal();
  }
#endif
}

static void drawBand() { drawCurrentScreen(); }

// Whether the character-grid terminal is the screen in front. Everything that
// draws terminal furniture -- the block cursor, and the row repaint that erases
// it -- has to ask this first.
//
// It exists because asking it twice produced two different answers. The cursor
// blink checked only isBrowserActive(), so with the sync screen up it kept
// blinking a block onto it, and worse, erasing that block repaints a whole
// terminal row: the IP address a sync is useless without was being wiped by a
// line of the screen behind it, twice a second.
static bool terminalIsShowing() {
#if MICROWRITER
  return false;  // there is no terminal on this machine
#else
  return !isWifiSyncActive() && !isBrowserActive();
#endif
}

// The paint chain. Its order and the key-routing chain in loop() must match:
// the PaperS3 records that they disagreed once, and nothing noticed until
// MicroWriter, where the browser is always open, so a screen drew but could
// not be typed into because its keys were going to whatever was behind it.
static void drawAll() {
  renderer.clearScreen();
  drawStatusBar();
  drawCurrentScreen();
  if (g_oskVisible) oskDraw();
#if !MICROWRITER
  // Only the terminal has a blinking block cursor; the editor draws its own
  // caret and the sync screens have none.
  if (terminalIsShowing()) drawCursor(g_cursorOn);
#endif
}

// Called by the runtime's byield() while a program is running, so a loop's
// PRINT output appears as it goes rather than all at once when the program
// stops. On e-paper this had to block on a panel refresh; here the pixels are
// already out by the time drawTerminal() returns, so it is just a repaint.
//
// byield() throttles this by time rather than by print volume, which matters:
// a full repaint is 143ms and doing one per PRINT would make a program crawl.
#if !MICROWRITER
void screenEditorFlushDisplay() {
  drawTerminal();
}
#endif

// Defined below, next to the bar it acts on. Declared here because the hook
// that serves a running program sits above it and needs to call it.
static bool handleBarTap(int x, bool duringRun);

// --- What is not built here yet -------------------------------------------
//
// terminal_input.cpp and tb_bridge.cpp reach out to four commands and two
// button hooks. Defining them here, out loud, rather than stubbing them into
// silence: a MENU command that does nothing looks like a bug, and one that says
// what is missing is a to-do list you can read from the machine itself.

// This board has no front panel buttons: a reset and a boot button on the back,
// where the X4 has a d-pad and the PaperS3 has a power key. Nothing to rearm.
void physicalButtonsRearm() {}

void pumpPhysicalButtonsForProgram() {
  // ==========================================================================
  // READ THIS BEFORE CHANGING ANYTHING HERE, INCLUDING ADDING AN EARLY RETURN.
  //
  // The name is wrong for this device and is kept anyway. It says "physical
  // buttons" because on the X4 that is the d-pad it reads, and it is kept so
  // this file still diffs cleanly against the two machines it shares code
  // with. What it actually is:
  //
  //     THE ONLY PATH INPUT HAS INTO A RUNNING PROGRAM.
  //
  // The interpreter's byield() calls it every sixteen statements. During a RUN,
  // loop() is blocked inside the interpreter for the entire duration, so
  // nothing else polls anything: not the keyboard, not the panel, not the bar.
  // Whatever is not read here is not read at all until the program ends, and a
  // program that does not end is a machine that cannot be stopped.
  //
  // Every input this device has must be served from here, and the list has
  // grown twice since it was written:
  //
  //   BLE keyboard    Esc and Ctrl+C, the way a program is stopped by someone
  //                   with a real keyboard. Added last and missing at first,
  //                   which is why Esc did nothing on a paired keyboard.
  //   Status bar      KBD, COLOR. The rest are refused while running, on
  //                   purpose: see handleBarTap.
  //   On-screen keys  Esc and Ctrl+C again, for when there is no keyboard.
  //
  // This function has been the cause of three separate bugs, all the same
  // shape: something the machine gained was not added here, and the symptom
  // appeared somewhere else entirely. It was an empty stub, so no program could
  // be stopped at all. It drew nothing, so an armed Ctrl was invisible and
  // Ctrl+C took three attempts. It returned early when the on-screen keyboard
  // was hidden, so folding it away for a BLE keyboard silently disabled the bar
  // and the panel here too.
  //
  // So: if this device grows another way to reach it, this is where that
  // belongs. And an early return at the top of this function will disable more
  // than whatever you were thinking about when you wrote it.
  // ==========================================================================

  // BLE first, and unconditionally: a keyboard is the one input that does not
  // go through the panel at all, so it must not sit behind the panel's
  // throttle.
  BleHid.poll();
  freeink::KeyEvent bleEv;
  while (BleHid.popKey(bleEv)) {
    enqueueKeyEvent(bleEv.keycode, bleEv.mods, bleEv.pressed);
  }

  // Then the panel, throttled by time rather than run on every call: byield()
  // reaches here hundreds of times a second and a touch read is an SPI
  // transaction. 25ms is far faster than a finger and costs the program almost
  // nothing.
  static uint32_t lastPoll = 0;
  const uint32_t now = millis();
  if (now - lastPoll < 25) return;
  lastPoll = now;

  uint16_t x = 0, y = 0;
  if (!tft.getTouch(&x, &y)) return;

  static uint32_t lastTap = 0;
  if (now - lastTap < 220) return;  // the same interval debounces the resistive panel
  lastTap = now;

  // The bar answers whether or not the on-screen keyboard is up. This used to
  // sit behind an early return on g_oskVisible, which was true when this hook
  // served nothing but the keyboard and wrong the moment there was another way
  // in: with the keyboard folded away for a BLE one, the whole function gave up
  // before reading anything, so a running program could not be reached from the
  // bar either.
  if (y < STATUS_BAR_H) {
    if (handleBarTap(x, /*duringRun=*/true)) drawAll();
    return;
  }

  if (!g_oskVisible) return;  // below the bar there is nothing to tap

  // Arming a modifier has to be visible here too. Ctrl+C is how a program is
  // stopped from the panel, and tapping Ctrl armed it correctly while drawing
  // nothing: no inverted key, no way to tell it had registered. So it gets
  // tapped again, which disarms it, and the C that follows is an ordinary
  // letter.
  const bool shiftWas = oskShiftArmed();
  const bool ctrlWas = oskCtrlArmed();
  const bool capsWas = oskCapsLockOn();
  oskHandleTap(x, y);  // enqueues through onOskKey, which is all a break needs
  if (oskShiftArmed() != shiftWas || oskCtrlArmed() != ctrlWas || oskCapsLockOn() != capsWas) {
    oskDraw();
  }
}

// One place for "this does not exist yet", writing wherever the machine in hand
// can actually show a line: the terminal on MicroBASIC, the browser's own
// status line on MicroWriter.
static void notify(const char* text) {
#if MICROWRITER
  browserSetStatus(text);
  screenDirty = true;
#else
  screenEditorTermPrintLine(text);
#endif
}

static void notBuiltYet(const char* what) {
  char msg[64];
  snprintf(msg, sizeof(msg), "?%s not built yet", what);
  notify(msg);
}

// Milestone 8. See docs/PORTING_PLAN.md, and the flash budget note there:
// this is the one that may not fit alongside the BLE keyboard, and if it comes
// to that, this is what gives way.
void startWifiSyncFromCommand() {
  wifiSyncStart();
  screenDirty = true;
}

// The prose editor, reached from the EDITOR button and from the MENU command.
void startEditorFromCommand() {
  browserStart();
  screenDirty = true;
}

// READER switches firmware slots in the PaperS3's shared dual-boot layout with
// CrossPoint. This board has a single app partition and no reader, so there is
// nowhere for it to go.
void startReaderSwitchFromCommand() { notify("?READER needs a second app slot"); }

// VC opens the programs list with Enter bound to LOAD rather than to the
// editor, which is what file_browser.cpp's browserStartVc() already does. It
// was reachable all along; only this function was in the way.
//
// It was stubbed out as "PaperS3 only" on the assumption that it belonged with
// READER, from the name sitting next to it and nothing else. It does not: VC is
// a Volkov Commander, a program picker, and has no connection to CrossPoint at
// all. The X4 draws it as its own multi-column view (vc_browser.cpp); the
// PaperS3 reuses the file browser for it, which is what is ported here, and
// file_browser.h explains why that is the better trade.
//
// Typed-only, deliberately, and that is file_browser.h's reasoning kept: it is
// a shortcut for loading a program, not a second way into the editor, so it
// gets no status-bar button.
void startVcFromCommand() {
  browserStartVc();
  screenDirty = true;
}

// One bar handler, used both from loop() and from inside a running program.
//
// While a program has control only the two buttons that change nothing but the
// display are live. SCR would reset the grid out from under a program that is
// printing into it, and the three placeholders would interleave their "not
// built yet" line with the program's own output. Neither is a thing a bar tap
// should be able to do mid-run.
static bool handleBarTap(const int x, const bool duringRun) {
  if (x < TITLE_W) return false;  // the nameplate, not a button
  // Walked rather than divided: the buttons are no longer one width.
  int index = -1;
  for (int i = 0; i < BTN_COUNT; i++) {
    if (kBtnW[i] > 0 && x >= btnX(i) && x < btnX(i) + kBtnW[i]) {
      index = i;
      break;
    }
  }
  switch (index) {
    case BTN_KBD:
      // Only the window changes. The grid keeps all its rows either way, so
      // folding the keyboard away brings back the rows it was hiding rather
      // than revealing blanks where they used to be.
      g_oskVisible = !g_oskVisible;
      return true;
    case BTN_COLOR:
      g_palette = (g_palette + 1) % kPaletteCount;
      renderer.setPalette(kPalettes[g_palette].palette);
      return true;
#if !MICROWRITER
    case BTN_SCR:
      if (duringRun) return false;
      screenEditorSetMode((screenEditorGetMode() + 1) % 4);
      applyBand();
      return true;
#endif
    // Drawn before they work. They answer through the same functions the MENU
    // commands do, so there is one place saying what is missing.
    case BTN_BLE:
      if (duringRun) return false;
      forceBlePairingNow();
      return true;
    case BTN_SYNC:
      if (duringRun) return false;
      startWifiSyncFromCommand();
      return true;
    case BTN_EDIT:
      if (duringRun) return false;
      startEditorFromCommand();
      return true;
  }
  return false;
}

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
  Serial.println("=== CYD MicroBASIC/MicroWriter -- milestone 8, network ===");

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

#if !MICROWRITER
  applyBand();
  screenEditorReset();
#endif

  // The card, before the interpreter: tbSetup() probes for an autoexec.bas and
  // fsbegin() creates the program directory, and both need a mounted card. Its
  // own VSPI bus, so nothing here contends with the panel on HSPI.
  static SPIClass sdSpi(VSPI);
  sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  const bool sdOk = SD.begin(PIN_SD_CS, sdSpi, 20000000);
  Serial.printf("SD: %s\n", sdOk ? "mounted" : "NOT mounted");

  // Before anything writes to the card. An unset ESP32 clock starts at the
  // epoch, and FAT cannot represent 1970, so every file this machine saved
  // would be dated 1980-01-01 and a card full of them would not sort.
  sdDateTimeSetup();
  char stamp[20];
  sdDateTimeFormat(stamp, sizeof(stamp));
  Serial.printf("clock: %s UTC (%s)\n", stamp,
                sdDateTimeHasClock() ? "from network" : "seeded from build date");

  inputSetup();
  // Named after the machine, so a keyboard's own paired-devices list says which
  // of the two it belongs to.
#if MICROWRITER
  BleHid.begin("MicroWriter");
#else
  BleHid.begin("MicroBASIC");
#endif
  Serial.printf("BLE: %s | heap after stack init %u KB\n",
                BleHid.isRunning() ? "up" : "FAILED",
                static_cast<unsigned>(ESP.getFreeHeap() / 1024));
  editorInit();
  editorSetGlyphWidthFn(editorGlyphWidth);
  fileManagerSetup();

  // Quiet for the boot probe only: the interpreter looks for an autoexec.bas
  // with a plain open, so not finding one is the normal case rather than a
  // fault worth printing on a fresh screen.
#if MICROWRITER
  // The browser is this machine's whole interface, so it opens at boot and
  // never closes. There is nothing behind it to go back to.
  browserStart();
#else
  // Order is the whole point here, and it was wrong: screenEditorReset() used
  // to run AFTER tbSetup(), which wiped the interpreter's own startup banner
  // off the screen before anyone saw it.
  //
  // Now the machine says what it is and how much memory it has, then the
  // interpreter says what it is underneath, then the prompt. Whose computer it
  // is first, which BASIC second, which is the shape these machines had.
  //
  // No blank line anywhere: the four lines are one block.
  //
  // tbSetup() silences file failures around its own autoexec probe, so there is
  // no wrapper here; an earlier one duplicated that and did nothing.
  screenEditorReset();
  char banner[64];
  // 36 columns, so it fits whole in SCREEN 3, 2 and 1 (60, 48 and 40). Only
  // SCREEN 0, at 32, wraps it, which is what a real machine did too.
  screenEditorTermPrintLine("FSP MicroBASIC 0.1 for CYD FNK0103-N");
  snprintf(banner, sizeof(banner), "%u Bytes free", static_cast<unsigned>(ESP.getFreeHeap()));
  screenEditorTermPrintLine(banner);
  tbSetup();
  screenEditorTermPrintLine("Ok");
#endif

  oskInit(renderer, FONT_SCREEN_MONO_2, FONT_SCREEN_MONO_3, 0, SCREEN_H - OSK_H, SCREEN_W, OSK_H,
          onOskKey);

  const uint32_t t0 = micros();
  drawAll();
  Serial.printf("first paint: %lu us\n", static_cast<unsigned long>(micros() - t0));

#if !MICROWRITER
  const uint32_t t1 = micros();
  drawTerminalRow(0);
  Serial.printf("one terminal row: %lu us\n", static_cast<unsigned long>(micros() - t1));
  Serial.printf("terminal: %d cols x %d rows (keyboard %s) | heap %u KB\n", screenEditorCols(),
                screenEditorRows(), g_oskVisible ? "up" : "down",
                static_cast<unsigned>(ESP.getFreeHeap() / 1024));
#else
  Serial.printf("MicroWriter: browser open | heap %u KB\n",
                static_cast<unsigned>(ESP.getFreeHeap() / 1024));
#endif
}

void loop() {
  uint16_t x = 0, y = 0;
  if (tft.getTouch(&x, &y)) {
    if (y < STATUS_BAR_H) {
      handleBarTap(x, /*duringRun=*/false);
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

  // Keyboard first: a key arriving this iteration should be acted on this
  // iteration, not the next.
  bleKbdAutoPair();
  BleHid.poll();
  freeink::KeyEvent bleEv;
  while (BleHid.popKey(bleEv)) {
    enqueueKeyEvent(bleEv.keycode, bleEv.mods, bleEv.pressed);
  }

  // The BLE button shows live connection state, and that state changes without
  // anyone touching the screen: a keyboard can connect or time out on its own.
  // Without this the button would only catch up on whatever unrelated repaint
  // happened next.
  static bool wasBleConnected = false;
  if (BleHid.isConnected() != wasBleConnected) {
    wasBleConnected = BleHid.isConnected();
    drawStatusBar();
  }

  // Some keyboards require Passkey Entry rather than Just Works: the host shows
  // six digits and they are typed on the keyboard itself. Nothing surfaced it
  // on the PaperS3 at first, and a keyboard wanting it sat waiting for a code
  // nobody had been shown, which reads as "will not pair".
  uint32_t passkey;
  if (BleHid.takePairingPasskey(passkey)) {
    char msg[48];
    snprintf(msg, sizeof(msg), "[ble] pairing code: %06lu", static_cast<unsigned long>(passkey));
    notify(msg);
    screenDirty = true;
  }

  wifiSyncLoop();  // no-op unless the sync screen is up; polls scan, link, HTTP
  browserLoop();   // no-op unless the editor is open; drives auto-save

  // Key routing. This chain MUST stay in the same order as drawAll()'s, or
  // keys reach a screen that is not the one on the panel. See the note there.
  if (isWifiSyncActive()) {
    uint8_t code, mods;
    bool pressed;
    while (dequeueKeyEventForCaller(code, mods, pressed)) {
      if (pressed) syncHandleKey(code, mods);
    }
    if (screenDirty) {
      screenDirty = false;
      drawBand();
      drawStatusBar();
      return;
    }
  } else if (isBrowserActive()) {
    uint8_t code, mods;
    bool pressed;
    while (dequeueKeyEventForCaller(code, mods, pressed)) {
      if (pressed) browserHandleKey(code, mods);
    }
    if (screenDirty) {
      screenDirty = false;
      drawBand();
      drawStatusBar();
      return;
    }
  }
#if !MICROWRITER
  else if (processAllInput() > 0) {
    // Everything a keystroke does happens in here, including running a
    // program: a RUN sits inside this call for as long as it takes.
    drawTerminal();
    drawStatusBar();
    drawCursor(true);
    g_cursorOn = true;
    return;
  }
#endif

  if (screenDirty) {
    screenDirty = false;
    drawBand();
    drawStatusBar();
    return;
  }

#if !MICROWRITER
  if (!terminalIsShowing()) {
    delay(10);
    return;  // whatever is in front draws its own caret, or none
  }

  static uint32_t lastBlink = 0;
  if (millis() - lastBlink > 500) {
    lastBlink = millis();
    g_cursorOn = !g_cursorOn;
    drawCursor(g_cursorOn);
  }
#endif
  delay(10);
}
