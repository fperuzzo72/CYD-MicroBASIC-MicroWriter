// Milestone 1: hardware bring-up for the Freenove FNK0103N.
//
// This is not MicroBASIC yet. It proves the three things the whole port rests
// on, on the real board, before a single feature is carried over from the
// PaperS3: the panel draws with the right geometry and orientation, the
// resistive touch reads and can be calibrated, and the SD card mounts on its
// own bus while the panel is using the other one.
//
// The order matters. On the PaperS3 port a display that stayed dark cost days
// because several unknowns were live at once (see that repo's
// DEVELOPMENT_LOG). Here each unknown gets its own visible verdict, on screen
// and on serial, so a failure names itself.
//
// What you should see on a working board:
//   1. The backlight comes on and the screen is not white noise.
//   2. A one-pixel border touching all four edges, with a filled square in
//      each corner and the corner's name next to it. If the border is cut off
//      or the labels are rotated wrong, the rotation or the panel offset is
//      wrong, not the drawing.
//   3. A report block: chip, flash, free heap, PSRAM (expected: none), and
//      the SD verdict.
//   4. Touch calibration, once, the first time. After that a crosshair that
//      follows your finger, with mapped and raw coordinates printed live.
//
// Hold the bottom-left corner for two seconds to force recalibration.

#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <Preferences.h>
#include <TFT_eSPI.h>

#include "board_fnk0103n.h"

static TFT_eSPI tft;

// The SD card is on VSPI, its own bus. TFT_eSPI has HSPI (USE_HSPI_PORT in
// platformio.ini), so the two never contend and neither needs to yield to the
// other. Worth stating because it is one of the few things that got easier
// coming from the PaperS3, where panel and card shared a bus.
static SPIClass sdSpi(VSPI);

// Touch calibration is five uint16_t that TFT_eSPI produces and consumes.
// It lives in NVS rather than a file: ten bytes do not justify mounting a
// filesystem, and it has to be readable before the SD card is known good.
static Preferences prefs;
static constexpr const char* PREFS_NAMESPACE = "cyd";
static constexpr const char* PREFS_KEY_TOUCH_CAL = "touchcal";
static uint16_t touchCal[5] = {0, 0, 0, 0, 0};

static bool sdMounted = false;

// --- On-screen report ------------------------------------------------------
// A cursor that only moves down, so the report reads as a log. Font 2 is
// TFT_eSPI's 16px built-in, which is the smallest that stays readable on a
// 3.5" panel at arm's length.
static int reportY = 0;
static constexpr int REPORT_LINE_H = 18;
static constexpr int REPORT_X = 12;

static void reportReset(int startY) {
  reportY = startY;
}

static void reportLine(uint16_t colour, const char* fmt, ...) {
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  Serial.println(buf);

  tft.setTextFont(2);
  tft.setTextColor(colour, TFT_BLACK);
  tft.drawString(buf, REPORT_X, reportY);
  reportY += REPORT_LINE_H;
}

// --- Geometry proof --------------------------------------------------------
// Drawn before anything else is trusted. If the border is clipped on one edge
// or a corner label sits where another should, the panel's rotation or its
// row/column offset is wrong. That is a different bug from "nothing draws",
// and this is what tells the two apart at a glance.
static void drawGeometryProof() {
  const int w = tft.width();
  const int h = tft.height();

  tft.fillScreen(TFT_BLACK);
  tft.drawRect(0, 0, w, h, TFT_WHITE);

  const int m = 10;  // corner marker size
  tft.fillRect(0,         0,         m, m, TFT_RED);
  tft.fillRect(w - m,     0,         m, m, TFT_GREEN);
  tft.fillRect(0,         h - m,     m, m, TFT_BLUE);
  tft.fillRect(w - m,     h - m,     m, m, TFT_YELLOW);

  tft.setTextFont(2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.drawString("TL", m + 4, 2);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("TR", w - m - 4, 2);
  tft.setTextDatum(BL_DATUM);
  tft.setTextColor(TFT_BLUE, TFT_BLACK);
  tft.drawString("BL", m + 4, h - 2);
  tft.setTextDatum(BR_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("BR", w - m - 4, h - 2);
  tft.setTextDatum(TL_DATUM);
}

// --- SD --------------------------------------------------------------------
static bool probeSdCard() {
  sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

  // 20MHz rather than the default 4MHz: the card is alone on this bus, and a
  // slow mount is the one part of boot the user actually waits through.
  if (!SD.begin(PIN_SD_CS, sdSpi, 20000000)) {
    reportLine(TFT_RED, "SD: mount FAILED (card in? FAT32?)");
    return false;
  }

  const uint8_t type = SD.cardType();
  if (type == CARD_NONE) {
    reportLine(TFT_RED, "SD: no card detected");
    return false;
  }

  const char* typeName = (type == CARD_MMC)  ? "MMC"
                       : (type == CARD_SD)   ? "SDSC"
                       : (type == CARD_SDHC) ? "SDHC"
                                             : "unknown";
  reportLine(TFT_GREEN, "SD: %s, %llu MB", typeName, SD.cardSize() / (1024ULL * 1024ULL));

  // List the root, capped. Proof that reads work, not just that the card
  // answered its identify command.
  File root = SD.open("/");
  if (!root) {
    reportLine(TFT_ORANGE, "SD: mounted but / would not open");
    return true;
  }
  int shown = 0;
  for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (shown < 4) {
      reportLine(TFT_LIGHTGREY, "  %s%s", entry.name(), entry.isDirectory() ? "/" : "");
    }
    shown++;
    entry.close();
  }
  root.close();
  if (shown > 4) {
    reportLine(TFT_LIGHTGREY, "  ... and %d more", shown - 4);
  } else if (shown == 0) {
    reportLine(TFT_LIGHTGREY, "  (root is empty)");
  }
  return true;
}

// --- Touch -----------------------------------------------------------------
static bool loadTouchCalibration() {
  prefs.begin(PREFS_NAMESPACE, /*readOnly=*/true);
  const size_t len = prefs.getBytesLength(PREFS_KEY_TOUCH_CAL);
  bool ok = false;
  if (len == sizeof(touchCal)) {
    prefs.getBytes(PREFS_KEY_TOUCH_CAL, touchCal, sizeof(touchCal));
    ok = true;
  }
  prefs.end();
  return ok;
}

static void saveTouchCalibration() {
  prefs.begin(PREFS_NAMESPACE, /*readOnly=*/false);
  prefs.putBytes(PREFS_KEY_TOUCH_CAL, touchCal, sizeof(touchCal));
  prefs.end();
}

static void runTouchCalibration() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Touch each corner arrow", tft.width() / 2, tft.height() / 2 - 20);
  tft.drawString("Use a fingernail or stylus", tft.width() / 2, tft.height() / 2 + 4);
  tft.setTextDatum(TL_DATUM);
  delay(1200);

  tft.calibrateTouch(touchCal, TFT_MAGENTA, TFT_BLACK, 18);
  tft.setTouch(touchCal);
  saveTouchCalibration();

  Serial.print("touch calibration:");
  for (int i = 0; i < 5; i++) Serial.printf(" %u", touchCal[i]);
  Serial.println();
}

// --- Setup / loop ----------------------------------------------------------
void setup() {
  Serial.begin(115200);
  // The CH340 needs a moment before the first bytes are not lost. Fixed wait
  // rather than a wait-for-Serial loop, which never returns on this board
  // because the UART bridge is always "connected".
  delay(400);
  Serial.println();
  Serial.println("=== CYD MicroBASIC/MicroWriter -- FNK0103N bring-up ===");

  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);

  tft.init();
  // Landscape, USB connector on the right. Flip to 3 if the board sits the
  // other way round on your desk; nothing else in this file assumes which.
  tft.setRotation(1);

  drawGeometryProof();
  delay(1500);

  tft.fillScreen(TFT_BLACK);
  reportReset(10);
  reportLine(TFT_CYAN,  "FNK0103N bring-up");
  reportLine(TFT_WHITE, "panel  : %dx%d, rotation %d", tft.width(), tft.height(), tft.getRotation());
  reportLine(TFT_WHITE, "chip   : %s rev %d, %d core(s) @ %d MHz",
             ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(), getCpuFrequencyMhz());
  reportLine(TFT_WHITE, "flash  : %u MB", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  reportLine(TFT_WHITE, "heap   : %u KB free", (unsigned)(ESP.getFreeHeap() / 1024));

  // Expected to report none. If a unit ever turns up with PSRAM, that changes
  // what the renderer can do (a full framebuffer becomes affordable), so the
  // bring-up says so out loud rather than leaving it assumed.
  const size_t psram = ESP.getPsramSize();
  if (psram > 0) {
    reportLine(TFT_YELLOW, "psram  : %u KB  <- unexpected, see PORTING_PLAN", (unsigned)(psram / 1024));
  } else {
    reportLine(TFT_WHITE, "psram  : none (as expected)");
  }

  sdMounted = probeSdCard();

  if (loadTouchCalibration()) {
    tft.setTouch(touchCal);
    reportLine(TFT_GREEN, "touch  : calibration loaded from NVS");
    reportLine(TFT_LIGHTGREY, "         hold bottom-left 2s to redo");
    delay(2500);
  } else {
    reportLine(TFT_YELLOW, "touch  : no calibration, running it now");
    delay(1500);
    runTouchCalibration();
  }

  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(sdMounted ? "SD ok" : "SD FAILED", REPORT_X, 4);
  tft.drawString("draw to test touch", REPORT_X, tft.height() - 22);
}

void loop() {
  static uint32_t cornerHoldStart = 0;

  uint16_t x = 0, y = 0;
  const bool pressed = tft.getTouch(&x, &y);

  if (pressed) {
    tft.fillCircle(x, y, 3, TFT_CYAN);

    uint16_t rawX = 0, rawY = 0;
    tft.getTouchRaw(&rawX, &rawY);

    char line[64];
    snprintf(line, sizeof(line), "x=%3u y=%3u   raw %4u,%4u  z=%4u",
             x, y, rawX, rawY, tft.getTouchRawZ());
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(line, REPORT_X, 4);

    // Bottom-left corner, held. Deliberately a corner the drawing test is
    // unlikely to wander into, and deliberately a hold rather than a tap, so
    // a stray press cannot wipe a good calibration.
    const bool inCorner = (x < 60) && (y > tft.height() - 60);
    if (inCorner) {
      if (cornerHoldStart == 0) {
        cornerHoldStart = millis();
      } else if (millis() - cornerHoldStart > 2000) {
        cornerHoldStart = 0;
        runTouchCalibration();
        tft.fillScreen(TFT_BLACK);
      }
    } else {
      cornerHoldStart = 0;
    }
  } else {
    cornerHoldStart = 0;
  }

  delay(10);
}
