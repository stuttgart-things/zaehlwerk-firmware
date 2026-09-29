#pragma once

// The fixed settings of the Zählwerk firmware.
//
// Everything here can be overridden with a build flag without touching the
// source. Credentials come from secrets.ini (template: secrets.ini.example) and
// are therefore not in the repository.

// --- hardware -------------------------------------------------------------
// Both pins are on ADC1. That is a requirement, not a preference: ADC2 is
// unavailable as soon as wifi is running.
#ifndef ZW_PIN_A
#define ZW_PIN_A 34  // piezo under half A
#endif
#ifndef ZW_PIN_B
#define ZW_PIN_B 35  // piezo under half B
#endif

// --- network --------------------------------------------------------------
// The name for mDNS and for espota — zaehlwerk.local once name resolution is
// in place (#14).
#ifndef ZW_HOSTNAME
#define ZW_HOSTNAME "zaehlwerk"
#endif

// Empty means OTA stays off. Deliberately without a default: an open update
// path on somebody else's wifi is worse than none. The value comes from
// secrets.ini.
#ifndef ZW_OTA_PASS
#define ZW_OTA_PASS ""
#endif

// --- the board's own access point -----------------------------------------
#ifndef ZW_AP_SSID
#define ZW_AP_SSID "Zaehlwerk"
#endif
#ifndef ZW_AP_PASS
#define ZW_AP_PASS "pingpong"
#endif
