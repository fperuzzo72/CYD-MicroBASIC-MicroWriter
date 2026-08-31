#pragma once

// The four build switches this library reads, with their defaults.
//
// On the PaperS3 these come from freeink-sdk's BoardConfig.h, which is that
// SDK's capability table keyed to its own board profiles. This board is not one
// of them, and pulling in a whole e-paper SDK's board table for four #defines
// would be a poor trade. They are declared here instead, where the code that
// reads them lives, and any of them can still be overridden from platformio.ini
// because every one is #ifndef-guarded.
//
// Removing the BoardConfig.h include without replacing these is what broke the
// first attempt at this port: FREEINK_BLE_HID_REQUIRE_MITM went undefined and
// the failure surfaced as an unrelated-looking error about getInstance().

// Compiles the real NimBLE central path. Off by default so the library can sit
// in a tree that is not using it; platformio.ini turns it on here.
#ifndef FREEINK_CAP_BLE_HID_HOST
#define FREEINK_CAP_BLE_HID_HOST 0
#endif

// List devices that advertise no name, as bare addresses. Useful during
// bring-up, noise afterwards.
#ifndef FREEINK_BLE_HID_SHOW_UNNAMED_DEVICES
#define FREEINK_BLE_HID_SHOW_UNNAMED_DEVICES 0
#endif

// Demand Passkey Entry pairing rather than accepting Just Works. Off: most
// keyboards do not offer it, and those that require it negotiate it themselves.
#ifndef FREEINK_BLE_HID_REQUIRE_MITM
#define FREEINK_BLE_HID_REQUIRE_MITM 0
#endif

// Per-advertisement scan logging.
#ifndef FREEINK_BLE_KEYBOARD_SCAN_DEBUG
#define FREEINK_BLE_KEYBOARD_SCAN_DEBUG 0
#endif
