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
// core 1, and #33 is where that is being tried. Whatever it is, the link to
// zaehlwerk-api below stays off it.
#ifndef ZW_SENSOR_CORE
#define ZW_SENSOR_CORE 0
#endif

// --- zaehlwerk-api (#17, #41, #66, ADR-0008) --------------------------------
// Where zaehlwerk-api is, without a trailing slash: https://zaehlwerk.<edge
// domain> behind the Gateway, or http://10.0.0.5:8080 for a laptop. This
// only seeds the board while NVS holds no address: once one is set or cleared
// on the board's page, that wins (#66). Empty, as in a released image, means
// the board reports nowhere and counts on its own, exactly as without this
// feature. An IP or a name the router's DNS knows; a .local name is not
// resolved by the HTTP client. https is verified against the edge root only
// (include/tls_trust_anchors.h).
#ifndef ZW_API_URL
#define ZW_API_URL ""
#endif

// The `source` of the ingest contract (zaehlwerk ADR-0002). The counter behind
// `event_id` is per source and kept in NVS, so two boards need two names.
#ifndef ZW_INGEST_SOURCE
#define ZW_INGEST_SOURCE ZW_DEVICE_ID
#endif

// With an https address, wait for SNTP to set the clock before the first
// request, and retry until then (ADR-0008). Note: the mbedTLS of this Arduino
// core is built without MBEDTLS_HAVE_TIME_DATE, so it does not check the
// certificate's validity dates at all, and a board at 1970 would verify. The
// wait is kept anyway, so that a core built with date checks does not turn
// into "not yet valid" at the table. 0 tries https without a clock.
#ifndef ZW_TLS_WAIT_FOR_TIME
#define ZW_TLS_WAIT_FOR_TIME 1
#endif

// Where SNTP asks first: the gateway DHCP handed out (on the table's box the
// Opal router, which runs an NTP server), then this one.
#ifndef ZW_NTP_FALLBACK
#define ZW_NTP_FALLBACK "pool.ntp.org"
#endif

// The link to zaehlwerk-api runs on the core that does not sample (ADR-0003).
// Not a fixed number on purpose: today that is core 1, once #33 lands core 0.
#ifndef ZW_REPORT_CORE
#define ZW_REPORT_CORE (ZW_SENSOR_CORE == 0 ? 1 : 0)
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
