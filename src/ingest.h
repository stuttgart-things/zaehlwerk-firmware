#pragma once

#include <WString.h>
#include <stdint.h>

#include <string>

// The path to zaehlwerk-api: the address, the counter behind `event_id`, and
// one JSON request.
//
// Shared by the points of #17 and the check-in of #63: one source, one counter
// and one place that knows the address. The address is kept in NVS and set on
// the board's page (#66), and so is the check-in token (#65).
//
// Blocking. Call get() and post() from a task of their own, never from the
// sampler and never from loop(), where they would hold up the web server for
// seconds. The report task and the fob task take turns: one request at a time.
namespace ingest {

// Reads the address and the counter from NVS. Says on serial whether an
// address is set and where it came from.
void begin();

// Whether there is an API to talk to at all.
bool configured();
// The address, copied: the page may change it while a request is out.
String baseUrl();
const char *source();
// Where the address came from: "NVS", "secrets.ini", "cleared on the page" or
// "" when there is none.
const char *urlOrigin();

enum class UrlChange { Set, Cleared, Unchanged, Refused };
// Takes effect at once. An empty string clears it, and a cleared address stays
// cleared: the seed from secrets.ini does not come back until NVS is erased,
// as for the check-in token below. `why` is the page's text for a refusal.
UrlChange setBaseUrl(const String &url, const char **why);

// The last event id handed out for this source. Kept in NVS, so a restart
// carries on above it rather than starting again at one, which zaehlwerk
// would answer as a duplicate until the source had been quiet for a while.
uint32_t lastEventId();
// The next id, handed out in RAM. Thread-safe. Not stored yet: an id has to be
// stored before it is sent, not before it is queued, because only a sent id
// can collide with one sent after a reboot.
uint32_t nextEventId();
// Stores the counter up to `id` before a request carrying it goes out. False
// if NVS could not be written, in which case the request must not go.
bool persistThrough(uint32_t id);

// SNTP, for https only (ADR-0008): started once an https address is in force,
// never for http, so a board without one keeps a clock at 1970 as before.
// Asks the gateway DHCP handed out first, then ZW_NTP_FALLBACK.
void startClock();
// The clock has been set by SNTP (it reads 2026 or later).
bool clockSet();

// zaehlwerk's CHECKIN_TOKEN (#65), the bearer of every check-in. Kept like the
// wifi credentials, not like the OTA password: stored in NVS and set on the
// board's page, so a released image, built without secrets.ini, can be given
// one. ZW_CHECKIN_TOKEN seeds it only while NVS has none.
//
// The token stays in this file. Nothing outside it can read it back: the page,
// serial and the diagnostic log learn whether one is set, never what it is.
bool haveCheckinToken();

enum class TokenChange { Set, Cleared, Refused };
// Takes effect at once. An empty string clears it, and a cleared token stays
// cleared: the seed from secrets.ini does not come back until NVS is erased.
// A token checkin::tokenUsable() refuses is not stored.
TokenChange setCheckinToken(const String &token);

// Who a request is from. Points carry no identity and send no token;
// a check-in names a player and sends the bearer.
enum class Auth { None, Checkin };

// GET and POST against baseUrl() + path. Return the HTTP status, or
// report::NO_WIFI / report::NO_ANSWER when nobody answered,
// report::TLS_FAILED when the server's certificate did not verify against the
// trust anchors, or report::NO_TIME when https waits for the clock. `answer` holds the
// body of the response. A POST with Auth::Checkin and no token set returns
// checkin::NO_TOKEN, and nothing is sent.
int get(const char *path, std::string &answer, uint32_t timeoutMs);
int post(const char *path, const std::string &body, std::string &answer, uint32_t timeoutMs,
         Auth auth = Auth::None);

}  // namespace ingest
