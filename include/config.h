#pragma once

// Feste Vorgaben der Zählwerk-Firmware.
//
// Alles hier lässt sich über einen Build-Flag überschreiben, ohne die Quelle
// anzufassen — Zugangsdaten kommen aus secrets.ini (Vorlage:
// secrets.ini.example) und stehen deshalb nicht im Repo.

// --- Hardware -------------------------------------------------------------
// Beide Pins liegen an ADC1. Das ist Pflicht, nicht Geschmack: ADC2 ist
// blockiert, sobald das WLAN läuft.
#ifndef ZW_PIN_A
#define ZW_PIN_A 34  // Piezo unter Hälfte A
#endif
#ifndef ZW_PIN_B
#define ZW_PIN_B 35  // Piezo unter Hälfte B
#endif

// --- Eigener Accesspoint --------------------------------------------------
#ifndef ZW_AP_SSID
#define ZW_AP_SSID "Zaehlwerk"
#endif
#ifndef ZW_AP_PASS
#define ZW_AP_PASS "pingpong"
#endif
