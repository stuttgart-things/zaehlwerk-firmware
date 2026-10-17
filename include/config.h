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

// Which core samples. The sketch's arrangement is core 0; ADR-0003 moves it to
// core 1, and #33 is where that is being tried. Whatever it is, the fob reader
// below stays off it.
#ifndef ZW_SENSOR_CORE
#define ZW_SENSOR_CORE 0
#endif

// --- the fob reader at the hub (#63, ADR-0007) ----------------------------
// An RC522 on VSPI. None of these touch the piezo inputs: 34 and 35 are
// input-only pins on ADC1, and the reader's lines are all elsewhere.
//
//   RC522   ESP32
//   SDA  -> GPIO 5   (chip select; a strapping pin, but idle high is its
//                     default, so the reader does not disturb the boot)
//   SCK  -> GPIO 18
//   MOSI -> GPIO 23
//   MISO -> GPIO 19
//   RST  -> GPIO 22
//   IRQ     not connected — the reader is polled
//   3.3V -> 3V3      never 5 V: the RC522 is a 3.3 V part
//   GND  -> GND
#ifndef ZW_PIN_RC522_SCK
#define ZW_PIN_RC522_SCK 18
#endif
#ifndef ZW_PIN_RC522_MISO
#define ZW_PIN_RC522_MISO 19
#endif
#ifndef ZW_PIN_RC522_MOSI
#define ZW_PIN_RC522_MOSI 23
#endif
#ifndef ZW_PIN_RC522_SS
#define ZW_PIN_RC522_SS 5
#endif
#ifndef ZW_PIN_RC522_RST
#define ZW_PIN_RC522_RST 22
#endif

// The LEDs at the reader, each through a resistor to GND. ADC2 pins, which is
// fine for an output: only analogRead on ADC2 is lost to wifi. -1 leaves one
// out; with only one fitted the patterns still differ, but red and green are
// what make an unknown fob readable at a glance.
#ifndef ZW_PIN_LED_GREEN
#define ZW_PIN_LED_GREEN 25
#endif
#ifndef ZW_PIN_LED_RED
#define ZW_PIN_LED_RED 26
#endif

// How long a fob waits for a press before it is forgotten.
#ifndef ZW_CHECKIN_WINDOW_MS
#define ZW_CHECKIN_WINDOW_MS 5000
#endif

// The reader is polled on the core that does not sample (ADR-0003). Not a
// fixed number on purpose: today that is core 1, once #33 lands it is core 0.
#ifndef ZW_FOB_CORE
#define ZW_FOB_CORE (ZW_SENSOR_CORE == 0 ? 1 : 0)
#endif

// --- zaehlwerk-api --------------------------------------------------------
// Where the ingest routes are, without a trailing slash: http://10.0.0.5:8080.
// Empty means the board talks to no API, and a fob is answered "unavailable"
// at once instead of waiting for a press. An IP or a name the router's DNS
// knows; a .local name is not resolved by the HTTP client.
#ifndef ZW_API_URL
#define ZW_API_URL ""
#endif

// zaehlwerk's CHECKIN_TOKEN, sent as a bearer on every check-in (#65). This
// only seeds the board while NVS holds none: once a token is set or cleared on
// the board's page, that wins. Empty here, so a released image carries none and
// is given one through the page. Without a token no check-in is sent.
#ifndef ZW_CHECKIN_TOKEN
#define ZW_CHECKIN_TOKEN ""
#endif

// The `source` of the ingest contract (zaehlwerk ADR-0002). The counter behind
// `event_id` is per source and kept in NVS, so two boards need two names.
#ifndef ZW_INGEST_SOURCE
#define ZW_INGEST_SOURCE ZW_DEVICE_ID
#endif

// --- network --------------------------------------------------------------
// The name for mDNS and for espota — zaehlwerk.local once name resolution is
// in place (#14).
#ifndef ZW_HOSTNAME
#define ZW_HOSTNAME "zaehlwerk"
#endif

// The network the board joins. Empty means it goes straight to its own access
// point. This only seeds the first connection — once credentials are stored in
// NVS through the web UI, those win (ADR-0002).
#ifndef ZW_STA_SSID
#define ZW_STA_SSID ""
#endif
#ifndef ZW_STA_PASS
#define ZW_STA_PASS ""
#endif

// How long to wait for the network before carrying an access point instead.
// Too short and a slow router loses the board to its own AP; too long and the
// tournament case waits. Adjustable in the web UI, stored in NVS.
#ifndef ZW_STA_TIMEOUT_MS
#define ZW_STA_TIMEOUT_MS 15000
#endif

// Empty means OTA stays off. Deliberately without a default: an open update
// path on somebody else's wifi is worse than none. The value comes from
// secrets.ini.
#ifndef ZW_OTA_PASS
#define ZW_OTA_PASS ""
#endif

// The port the board listens on for the sink's answer. A sink that is simply
// not running looks exactly like one that is listening, because UDP says
// nothing — so the board asks, and holds what it cannot deliver until somebody
// answers.
#ifndef ZW_DIAG_PORT
#define ZW_DIAG_PORT 9009
#endif

// Names this board in the log. Two tables in one sink file are otherwise only
// distinguishable by their session id, which says nothing to a person.
#ifndef ZW_DEVICE_ID
#define ZW_DEVICE_ID "piezo-1"
#endif

// --- the board's own access point -----------------------------------------
#ifndef ZW_AP_SSID
#define ZW_AP_SSID "Zaehlwerk"
#endif
#ifndef ZW_AP_PASS
#define ZW_AP_PASS "pingpong"
#endif
