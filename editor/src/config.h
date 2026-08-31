#pragma once

#include <cstdint>
#include <cstddef>

// Ported from MicroWriter-BASIC-PaperS3's config.h, trimmed to what this
// device's build actually uses. The prose-editor, BLE and WiFi constants are
// not here because the modules that read them are not ported yet; re-add each
// one alongside the file that needs it rather than up front.

// --- Key Event (for input queue) ---
struct KeyEvent {
  uint8_t keyCode;
  uint8_t modifiers;
  bool pressed;
};

static constexpr int INPUT_QUEUE_SIZE = 50;

// Screen Editor's monospace fonts, one per SCREEN mode. Arbitrary sentinels,
// the same scheme both earlier devices use: they only have to not collide.
#define FONT_SCREEN_MONO_0 (-2000000001)  // SCREEN 0, 32 col
#define FONT_SCREEN_MONO_1 (-2000000002)  // SCREEN 1, 40 col
#define FONT_SCREEN_MONO_2 (-2000000003)  // SCREEN 2, 48 col
#define FONT_SCREEN_MONO_3 (-2000000004)  // SCREEN 3, 60 col, boots here

// Storage for the largest grid across all four SCREEN modes. On this panel the
// widest is SCREEN 3 at 60 columns, and the tallest is also SCREEN 3 at 19 rows
// (304px of band divided by its 16px cell, exactly). The PaperS3's numbers were
// 80 and 22; anything sized from those here would waste 40% of the array.
static constexpr int SCREEN_EDITOR_MAX_COLS = 60;
static constexpr int SCREEN_EDITOR_MAX_ROWS = 19;

// Longest single BASIC line the screen editor will hand to the interpreter.
// Callers size their own stack buffers from this. The interpreter has its own,
// smaller input buffer; this only has to be big enough that nothing here
// truncates before it does.
static constexpr int MAX_PROGRAM_LINE_LEN = 160;

static constexpr int MAX_FILENAME_LEN = 64;
static constexpr int MAX_TITLE_LEN = 40;

// --- Prose side: the editor buffer and the file list ---------------------
// Added with text_editor.cpp and file_manager.cpp rather than up front, which
// is what the note at the top of this file said would happen.

// One entry in a browsed folder. `title` is what the browser shows for a note,
// MicroWriter's convention where the filename is an implementation detail; for
// a program it is the filename itself, because LOAD "X" has to find the file,
// so what is listed must be what LOAD takes.
struct FileInfo {
  char filename[MAX_FILENAME_LEN];
  char title[MAX_TITLE_LEN];
  unsigned long modTime;
};

static constexpr int MAX_FILES = 50;

// A hard ceiling on how large a file the editor will open, not a function of
// free memory: the buffer is allocated once and reused, so a note that fits
// today keeps fitting. Unchanged from both earlier devices at 16KB, which is
// 5% of this board's RAM and leaves the interpreter's own 16KB untouched.
static constexpr size_t TEXT_BUFFER_SIZE = 16384;

// Wrapped display lines the editor will track for one buffer.
static constexpr int MAX_LINES = 1024;

// Auto-save, carried over unchanged. Saving writes to the card and never
// touches the panel, so neither of these causes a visible redraw. The editor
// also saves on the way out.
static constexpr unsigned long AUTO_SAVE_IDLE_MS = 10000;   // 10s after the last keystroke
static constexpr unsigned long AUTO_SAVE_MAX_MS = 120000;   // and every 2min while still typing

#define DBG_PRINTF(fmt, ...)  Serial.printf(fmt, ##__VA_ARGS__)
#define DBG_PRINTLN(s)        Serial.println(s)
#define DBG_PRINT(s)          Serial.print(s)

// --- HID Keycodes ---
static constexpr uint8_t HID_KEY_A          = 0x04;
static constexpr uint8_t HID_KEY_C          = 0x06;
static constexpr uint8_t HID_KEY_ENTER      = 0x28;
static constexpr uint8_t HID_KEY_ESCAPE     = 0x29;
static constexpr uint8_t HID_KEY_BACKSPACE  = 0x2A;
static constexpr uint8_t HID_KEY_CAPSLOCK   = 0x39;
static constexpr uint8_t HID_KEY_RIGHT      = 0x4F;
static constexpr uint8_t HID_KEY_LEFT       = 0x50;
static constexpr uint8_t HID_KEY_DOWN       = 0x51;
static constexpr uint8_t HID_KEY_UP         = 0x52;
static constexpr uint8_t HID_KEY_HOME       = 0x4A;
static constexpr uint8_t HID_KEY_END        = 0x4D;
static constexpr uint8_t HID_KEY_PAGE_UP    = 0x4B;
static constexpr uint8_t HID_KEY_PAGE_DOWN  = 0x4E;

// --- HID Modifier Masks ---
static constexpr uint8_t MOD_CTRL_LEFT   = 0x01;
static constexpr uint8_t MOD_SHIFT_LEFT  = 0x02;
static constexpr uint8_t MOD_ALT_LEFT    = 0x04;
static constexpr uint8_t MOD_CTRL_RIGHT  = 0x10;
static constexpr uint8_t MOD_SHIFT_RIGHT = 0x20;
static constexpr uint8_t MOD_ALT_RIGHT   = 0x40;

inline bool isCtrl(uint8_t mod) {
  return (mod & MOD_CTRL_LEFT) || (mod & MOD_CTRL_RIGHT);
}
inline bool isShift(uint8_t mod) {
  return (mod & MOD_SHIFT_LEFT) || (mod & MOD_SHIFT_RIGHT);
}
