#include "sd_datetime.h"

#include <Arduino.h>
#include <FS.h>
#include <SD.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

bool clockFromNetwork = false;

// Parses __DATE__, which the compiler emits as "Mmm dd yyyy" with a space
// where a leading zero would go. Returns false rather than guessing if it does
// not look like that, so a toolchain that formats it differently degrades to
// "no seed" instead of to a wrong date.
bool parseBuildDate(struct tm& out) {
  static const char* kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char monthName[4] = {0};
  int day = 0, year = 0;
  if (sscanf(__DATE__, "%3s %d %d", monthName, &day, &year) != 3) return false;
  const char* found = strstr(kMonths, monthName);
  if (!found) return false;

  int hour = 0, minute = 0, second = 0;
  if (sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) return false;

  out = {};
  out.tm_year = year - 1900;
  out.tm_mon = static_cast<int>(found - kMonths) / 3;
  out.tm_mday = day;
  out.tm_hour = hour;
  out.tm_min = minute;
  out.tm_sec = second;
  return true;
}

// Newest modification time anywhere under `dir`, one level down. Zero if the
// card is not mounted, the directory does not exist, or it holds nothing.
//
// One level is deliberate: the two directories this firmware writes are flat,
// and a recursive walk of somebody else's card at every boot is a cost with no
// return.
time_t newestFileTime(const char* dir) {
  File d = SD.open(dir);
  if (!d || !d.isDirectory()) return 0;
  time_t newest = 0;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (!f.isDirectory()) {
      const time_t t = f.getLastWrite();
      if (t > newest) newest = t;
    }
    f.close();
  }
  d.close();
  return newest;
}

}  // namespace

void sdDateTimeSetup() {
  // UTC, and set before anything else so the seed below is not reinterpreted
  // through a local zone this project does not carry.
  setenv("TZ", "UTC0", 1);
  tzset();

  time_t seconds = 0;
  struct tm built;
  if (parseBuildDate(built)) {
    const time_t fromBuild = mktime(&built);
    if (fromBuild > 0) seconds = fromBuild;
  }

  // Whatever this machine last wrote, if it is later than the build date. A
  // card carrying files from a session that had the real time pulls the clock
  // forward; a card that does not changes nothing. Compared rather than
  // preferred outright, because a file could predate this firmware.
  const time_t fromCard = newestFileTime("/MicroBASIC/programs");
  if (fromCard > seconds) seconds = fromCard;

  if (seconds <= 0) return;

  struct timeval tv = {};
  tv.tv_sec = seconds;
  settimeofday(&tv, nullptr);
}

bool sdDateTimeHasClock() { return clockFromNetwork; }

bool sdDateTimeSyncFromNetwork(const uint32_t timeoutMs) {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  // configTime() is asynchronous, so wait for the clock to actually move past
  // something no seeded build date could be. 2020 rather than 1980: the seed
  // is already a real build date, so "later than 1980" would be true before
  // SNTP answered and this would report success without having synced.
  const uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    const time_t now = time(nullptr);
    struct tm utc;
    gmtime_r(&now, &utc);
    if (utc.tm_year + 1900 >= 2020 && now > 1600000000) {
      clockFromNetwork = true;
      return true;
    }
    delay(100);
  }
  return false;
}

void sdDateTimeFormat(char* out, const size_t outSize) {
  const time_t now = time(nullptr);
  struct tm utc;
  gmtime_r(&now, &utc);
  snprintf(out, outSize, "%04d-%02d-%02d %02d:%02d", utc.tm_year + 1900, utc.tm_mon + 1,
           utc.tm_mday, utc.tm_hour, utc.tm_min);
}
