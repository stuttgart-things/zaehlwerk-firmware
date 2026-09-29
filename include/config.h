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

// --- Netz ---
// Name für mDNS und für espota. Vollständig also zaehlwerk.local, sobald die
// Namensauflösung steht (#14).
#ifndef ZW_HOSTNAME
#define ZW_HOSTNAME "zaehlwerk"
#endif

// Leer heißt: OTA bleibt aus. Absichtlich kein Vorgabewert — ein offener
// Update-Pfad in einem fremden WLAN ist schlimmer als gar keiner. Der Wert
// kommt aus secrets.ini.
#ifndef ZW_OTA_PASS
#define ZW_OTA_PASS ""
#endif

// --- Eigener Accesspoint --------------------------------------------------
#ifndef ZW_AP_SSID
#define ZW_AP_SSID "Zaehlwerk"
#endif
#ifndef ZW_AP_PASS
#define ZW_AP_PASS "pingpong"
#endif
