#include "screen_editor.h"

#include "config.h"
#include <Utf8.h>
#include <cstdio>
#include <cstring>
#include <strings.h>  // strcasecmp

// Ported from MicroBASIC's own screen_editor.cpp. Two real changes for this
// panel: the MODES table below (960x540 landscape geometry instead of the
// X4's own panel -- see README's "SCREEN modes" table for the derivation),
// and marginX -> marginY throughout, since on THIS panel every column count
// divides 960 exactly (960 = 2^6*3*5) so the margin that exists in two of
// the four modes falls on the ROW axis instead of the column axis -- the
// opposite of the X4, where columns needed centering and rows never did.
// sd_backup.h and SDCardManager were unused dead includes in the original,
// dropped rather than carried over.
struct ScreenModeInfo {
  int cols, cellW, cellH, fontId;
};

// Geometry for this panel: 480x320 landscape. Every column count divides 480
// exactly, so no mode needs a horizontal margin; what varies is the row count,
// and unlike the two earlier devices that is NOT a constant here.
//
// The row count and the centring margin are computed from a band the caller
// sets, rather than being two more columns of this table. The PaperS3 stores
// both, and its numbers are measurements of a 960x540 panel with a 30px status
// bar: correct there, meaningless anywhere else, and silently wrong if either
// number moves. Deriving them means this table carries only what is genuinely
// per-mode, and the panel's own geometry lives in one place at the caller.
//
// Today the caller always passes the whole area below the status bar, so the
// grid keeps its full row count and the on-screen keyboard is drawn over the
// bottom of it, as on the PaperS3. screenEditorSetBand handles a band that
// shrinks anyway, because that is what makes a mode change safe and it is what
// this device would need if the on-screen keyboard ever became the normal way
// in rather than the fallback.
static const ScreenModeInfo MODES[4] = {
    {32, 15, 30, FONT_SCREEN_MONO_0},
    {40, 12, 24, FONT_SCREEN_MONO_1},
    {48, 10, 20, FONT_SCREEN_MONO_2},
    {60,  8, 16, FONT_SCREEN_MONO_3},
};

// The band the terminal draws into, in panel pixels, set by the caller. Zero
// until then, which would give zero rows, so screenEditorRows() floors at 1.
static int bandTop = 0;
static int bandH = 0;

static int currentMode = 3;  // SCREEN 3 (60-col), the boot mode here -- see docs/PORTING_PLAN.md

static uint32_t grid[SCREEN_EDITOR_MAX_ROWS][SCREEN_EDITOR_MAX_COLS];
static int cursorRow = 0;
static int cursorCol = 0;

// rowIsContinuation[r] == true means row r is the wrapped tail of row r-1,
// i.e. they're one logical line that ran past the right margin. Set only by
// the two places that can wrap (typing past the last column, and terminal
// output doing the same); cleared wherever a genuinely new line starts.
//
// The logical line's start and end are *derived* from these flags rather
// than tracked in a variable, which is both simpler and what fixes the real
// bug this replaced: the old code reset a `logicalLineStartRow` on every
// deliberate cursor move, so LISTing a line longer than the screen width
// and then arrowing onto its second row made Enter read only that tail.
// MSX walks the continuation chain in both directions from wherever the
// cursor happens to be, and reads the whole logical line regardless of
// where in it you pressed Enter -- which is exactly what makes "LIST, cursor
// up, edit in place, Enter" work on a long line. See docs/DEVELOPMENT_LOG.md.
static bool rowIsContinuation[SCREEN_EDITOR_MAX_ROWS];

int screenEditorGetMode() { return currentMode; }
int screenEditorCols() { return MODES[currentMode].cols; }
int screenEditorRows() {
  int r = bandH / MODES[currentMode].cellH;
  if (r > SCREEN_EDITOR_MAX_ROWS) r = SCREEN_EDITOR_MAX_ROWS;
  if (r < 1) r = 1;  // before the caller has set a band
  return r;
}
int screenEditorCellW() { return MODES[currentMode].cellW; }
int screenEditorCellH() { return MODES[currentMode].cellH; }
// Whatever the rows do not divide evenly is split above and below, the same
// way both earlier devices centre their non-exact modes.
int screenEditorMarginY() {
  return bandTop + (bandH - screenEditorRows() * MODES[currentMode].cellH) / 2;
}
int screenEditorFontId() { return MODES[currentMode].fontId; }

void screenEditorReset() {
  int cols = screenEditorCols();
  int rows = screenEditorRows();
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++)
      grid[r][c] = ' ';
    rowIsContinuation[r] = false;
  }
  cursorRow = 0;
  cursorCol = 0;
}

// Walk up/down the continuation chain to find the full extent of the logical
// line the cursor is currently sitting anywhere within.
static int logicalLineStartRow() {
  int r = cursorRow;
  while (r > 0 && rowIsContinuation[r]) r--;
  return r;
}

static int logicalLineEndRow() {
  int r = cursorRow;
  int rows = screenEditorRows();
  while (r + 1 < rows && rowIsContinuation[r + 1]) r++;
  return r;
}

void screenEditorSetMode(int n) {
  if (n < 0) n = 0;
  if (n > 3) n = 3;
  currentMode = n;
  screenEditorReset();
}

uint32_t screenEditorGetCell(int row, int col) {
  if (row < 0 || row >= screenEditorRows() || col < 0 || col >= screenEditorCols()) return ' ';
  return grid[row][col];
}

int screenEditorGetCursorRow() { return cursorRow; }
int screenEditorGetCursorCol() { return cursorCol; }

static void clampCursor() {
  int cols = screenEditorCols();
  int rows = screenEditorRows();
  if (cursorRow < 0) cursorRow = 0;
  if (cursorRow >= rows) cursorRow = rows - 1;
  if (cursorCol < 0) cursorCol = 0;
  if (cursorCol >= cols) cursorCol = cols - 1;
}

// Navigation no longer has to maintain any logical-line state: the
// continuation flags travel with the rows themselves, so moving the cursor
// simply lands you somewhere within whatever logical line owns that row.
void screenEditorMoveCursor(int dRow, int dCol) {
  cursorRow += dRow;
  cursorCol += dCol;
  clampCursor();
}

void screenEditorGoHome() { cursorCol = 0; }

// Absolute placement, for the interpreter's LOCATE (decoded from VT52 in
// tb_runtime.cpp's outch). Deliberately leaves the continuation flags alone,
// exactly as screenEditorMoveCursor does: jumping the cursor says where to
// print next, not that the rows it lands between stopped belonging together.
void screenEditorSetCursor(int row, int col) {
  cursorRow = row;
  cursorCol = col;
  clampCursor();
}

void screenEditorGoEnd() {
  int cols = screenEditorCols();
  int last = -1;
  for (int c = 0; c < cols; c++)
    if (grid[cursorRow][c] != (uint32_t)' ') last = c;
  cursorCol = (last < 0) ? 0 : ((last + 1 < cols) ? last + 1 : cols - 1);
}

void screenEditorGoFirstRow() {
  cursorRow = 0;
  clampCursor();
}

void screenEditorGoLastRow() {
  cursorRow = screenEditorRows() - 1;
  clampCursor();
}

// Moves the grid up by `n` rows within a window of `totalRows`, clearing what
// is vacated at the bottom and bringing the cursor with it.
//
// `totalRows` is passed rather than read from screenEditorRows() because the
// band shrinking is one of the two callers, and by the time it shifts, the row
// count has ALREADY changed. Shifting within the new, smaller count would move
// the rows that are staying and leave behind exactly the ones that need to
// come up.
static void shiftRowsUp(int n, int totalRows) {
  if (n <= 0) return;
  if (n > totalRows) n = totalRows;
  const int cols = screenEditorCols();
  for (int r = 0; r + n < totalRows; r++) {
    for (int c = 0; c < cols; c++) grid[r][c] = grid[r + n][c];
    rowIsContinuation[r] = rowIsContinuation[r + n];
  }
  for (int r = totalRows - n; r < totalRows; r++) {
    for (int c = 0; c < cols; c++) grid[r][c] = ' ';
    rowIsContinuation[r] = false;
  }
  // Row 0 can't be a continuation of anything once whatever preceded it has
  // scrolled off -- an extremely long wrapped line loses its true head here,
  // same best-effort limit the old code had.
  rowIsContinuation[0] = false;
  cursorRow -= n;
  if (cursorRow < 0) cursorRow = 0;
}

static void scrollUp() {
  const int rows = screenEditorRows();
  const int cursorWas = cursorRow;
  shiftRowsUp(1, rows);
  // Every existing caller scrolls to make room at the BOTTOM and expects the
  // cursor to stay on the last row, not to ride up with the text.
  cursorRow = cursorWas;
  if (cursorRow >= rows) cursorRow = rows - 1;
}

// Tells the terminal which slice of the panel it owns.
//
// Growing is free: the rows below simply become visible again. Shrinking has to
// scroll, or the cursor ends up outside the grid. Shrinking happens on a SCREEN
// mode change today; it would also happen if the band ever followed the
// on-screen keyboard.
void screenEditorSetBand(int top, int height) {
  const int oldRows = screenEditorRows();
  bandTop = top;
  bandH = height;
  const int newRows = screenEditorRows();
  if (newRows < oldRows && cursorRow > newRows - 1) {
    shiftRowsUp(cursorRow - (newRows - 1), oldRows);
  }
  if (cursorRow >= newRows) cursorRow = newRows - 1;
  if (cursorCol >= screenEditorCols()) cursorCol = screenEditorCols() - 1;
}

// Advances the cursor to the next row, marking it as a continuation of the
// row just left -- the shared tail of "typed past the last column" and
// "printed past the last column", which are the only two ways a logical line
// legitimately spans rows.
static void wrapToNextRow() {
  int rows = screenEditorRows();
  cursorCol = 0;
  if (cursorRow < rows - 1) {
    cursorRow++;
    rowIsContinuation[cursorRow] = true;
  } else {
    scrollUp();  // cursorRow stays at rows-1
    rowIsContinuation[cursorRow] = true;
  }
}

void screenEditorInsertCodepoint(uint32_t cp) {
  int cols = screenEditorCols();
  grid[cursorRow][cursorCol] = cp;
  cursorCol++;
  if (cursorCol >= cols) wrapToNextRow();
}

void screenEditorBackspace() {
  int cols = screenEditorCols();
  if (cursorCol > 0) {
    cursorCol--;
  } else if (cursorRow > 0 && rowIsContinuation[cursorRow]) {
    // Only cross the row boundary when this row is the wrapped tail of the one
    // above, i.e. they are one logical line and the character before the
    // cursor really is up there. On a line the user started fresh, backspace
    // at column 0 must do nothing -- otherwise it walks back into, and eats,
    // whatever unrelated text happens to be on the previous row.
    //
    // Whether crossing also severs the tie (clears rowIsContinuation[cursorRow])
    // depends on this row's own content, not on the fact that a boundary got
    // crossed. Clearing unconditionally (an earlier version did) orphans
    // whatever text is still sitting here whenever the cursor was moved away
    // mid-line rather than backspaced here linearly: navigate onto the
    // wrapped row, press Backspace once, and Enter on the row above then
    // reads only that row, silently dropping everything still on this one.
    // Never clearing (the version after that) fixes that but breaks the
    // opposite, rarer case: backspace this row down to empty, cross the
    // boundary, then type something fresh into it -- Enter now glues it to
    // the row above, because the leftover flag says it's still that row's
    // continuation. The two cases are told apart by whether this row is
    // empty *before* this call touches anything (the deletion below only
    // ever touches the row being backed INTO, never this one): empty means
    // there was nothing left to continue, so the tie is genuinely gone;
    // non-empty means it's still one logical line with the row above.
    bool tailEmpty = true;
    for (int c = 0; c < cols; c++) {
      if (grid[cursorRow][c] != (uint32_t)' ') { tailEmpty = false; break; }
    }
    if (tailEmpty) rowIsContinuation[cursorRow] = false;
    cursorRow--;
    cursorCol = cols - 1;
  } else {
    return;
  }
  grid[cursorRow][cursorCol] = ' ';
}

// Reads the *entire* logical line the cursor is within -- from the top of
// its continuation chain to the bottom -- not just up to the cursor. That's
// the MSX rule: where you happen to be within the line when you press Enter
// doesn't change what gets read.
void screenEditorGetLogicalLineText(char* out, int outSize) {
  int cols = screenEditorCols();
  int start = logicalLineStartRow();
  int end = logicalLineEndRow();
  int n = 0;
  int lastNonSpace = -1;
  for (int r = start; r <= end && n < outSize - 1; r++) {
    for (int c = 0; c < cols && n < outSize - 1; c++) {
      uint32_t cp = grid[r][c];
      // Latin-1, one byte per character, matching what the keyboard already
      // puts into BASIC strings (see pushProgramKey in input_handler.cpp).
      // This used to substitute '?' for everything above ASCII, on the
      // reasoning that parsing a command never needed more -- true when a
      // typed line was only ever a command, and wrong the moment it could be
      // BASIC source: PRINT "acao" with a cedilla and a tilde reached the
      // interpreter as a??o. Above 0xFF there is still no byte to write.
      char ch = (cp <= 0xFF) ? (char)(unsigned char)cp : '?';
      out[n] = ch;
      if (ch != ' ') lastNonSpace = n;
      n++;
    }
  }
  out[lastNonSpace + 1] = '\0';
}

void screenEditorClearLogicalLine() {
  int cols = screenEditorCols();
  int start = logicalLineStartRow();
  int end = logicalLineEndRow();
  for (int r = start; r <= end; r++) {
    for (int c = 0; c < cols; c++) grid[r][c] = ' ';
    rowIsContinuation[r] = false;
  }
  cursorRow = start;
  cursorCol = 0;
}

void screenEditorStartNewInputLine() {
  int rows = screenEditorRows();
  // Descend from the *end* of the logical line the cursor is currently
  // within, not from wherever the cursor happens to be sitting. Enter can
  // be pressed from any row of a wrapped line (e.g. after arrowing back up
  // to edit it), and the new line always belongs after the whole thing --
  // not after just the one physical row the cursor was on. Without this,
  // Enter from the first row of a 2+-row wrap landed the cursor on the
  // wrap's own second row (still mid-logical-line), which read as a fresh
  // line but wasn't: typing there and pressing Enter again would append to
  // the line just registered instead of starting a new one.
  cursorRow = logicalLineEndRow();
  cursorCol = 0;
  if (cursorRow < rows - 1) {
    cursorRow++;
  } else {
    scrollUp();
  }
  rowIsContinuation[cursorRow] = false;  // a genuinely new line, not a wrap
}

int screenEditorRowsLeftOnScreen() {
  return screenEditorRows() - cursorRow;
}

void screenEditorTermPrint(const char* utf8Text) {
  const unsigned char* p = (const unsigned char*)utf8Text;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&p)) != 0) {
    if (cp == '\n') {
      screenEditorStartNewInputLine();
    } else {
      screenEditorInsertCodepoint(cp);
    }
  }
}

void screenEditorTermPrintLine(const char* utf8Text) {
  screenEditorTermPrint(utf8Text);
  screenEditorTermPrint("\n");
}
