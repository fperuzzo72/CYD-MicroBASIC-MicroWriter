#pragma once

#include <cstddef>
#include <cstdint>

// Dates on the files this device writes to the SD card.
//
// This is a rewrite of the PaperS3's sd_datetime, not a port, because almost
// nothing it does applies here. That version registers an SdFat callback
// (`FsDateTime::setCallback`) and fills it from a BM8563 RTC. This board has
// no RTC, and its SD library is not SdFat: it is Arduino's, which goes through
// ESP-IDF's FATFS, and that is configured with `FF_FS_NORTC = 0`, meaning
// timestamps are already enabled and taken from `get_fattime()`, which reads
// the system clock. So there is no callback to register and nothing to wire:
// set the system clock and every file written after that is dated.
//
// What is left is the part that was always the hard bit anyway, which is
// having a time to set it to.
//
// Where the time comes from
// -------------------------
// Nothing on this board keeps time across a power cycle, so the clock is
// seeded at boot from the best of two things, in this order:
//
//   1. The newest file already on the card. If this machine has ever known the
//      real time, it wrote it into a timestamp, and reading that back is using
//      a fact the device recorded rather than inventing one.
//   2. The firmware's build date. Not the real time, but plausible, sortable,
//      and above all not 1970: FAT cannot represent anything before 1980, so an
//      unset clock produces files dated 1980-01-01 and a card full of those
//      cannot be ordered at all.
//
// The first of those is the X4's idea, adapted. MicroBASIC on the X4 has no
// clock either and reads the last valid timestamp the CrossPoint reader left in
// `/.crosspoint/state.json` on the same card. There is no reader here and no
// such file, but the principle carries: prefer something the device itself
// wrote while it knew, over anything guessed now. What this device wrote is its
// own saved programs.
//
// It matters more here than it looks. WiFi is milestone 8 and is the piece that
// gives way if flash runs short, so the build date may be the only real source
// this machine ever gets. Without step 1, every boot resets to the same instant
// and every file ever saved carries an identical timestamp, which defeats the
// one thing timestamps are for.
//
// The real time arrives from SNTP the first time the network is up. Until then
// sdDateTimeHasClock() answers false, so callers can say "approximate" rather
// than imply a precision that is not there.
//
// Everything is UTC, deliberately, and that is carried over from the PaperS3
// unchanged: a file timestamp is a reference, not a calendar appointment, and
// UTC avoids carrying a timezone setting and DST rules for something nothing
// here reads back.

// Seeds the system clock from the build date. Call once at boot, before
// anything writes to the card.
void sdDateTimeSetup();

// True once the clock has been set from the network. False means every
// timestamp since boot is the build date.
bool sdDateTimeHasClock();

// Sets the clock from SNTP. Needs an association already up; returns false on
// timeout, leaving the build-date seed in place.
bool sdDateTimeSyncFromNetwork(uint32_t timeoutMs = 5000);

// The current UTC time as "YYYY-MM-DD HH:MM", into a buffer of at least 17
// bytes. For status lines and logs.
void sdDateTimeFormat(char* out, size_t outSize);
