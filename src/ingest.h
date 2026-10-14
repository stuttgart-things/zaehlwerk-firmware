#pragma once

#include <stdint.h>

#include <string>

// The path to zaehlwerk-api: a JSON POST and the counter behind `event_id`.
//
// Written for the check-in (#63), the first thing this board sends there, and
// meant to be what the points of #17 go through as well: one source, one
// counter, one place that knows the URL. It is deliberately plain — no queue,
// no batching. A check-in a minute late would claim a half somebody may have
// left (zaehlwerk ADR-0010), so it has nothing to queue; the points' offline
// queue belongs to #17.
//
// Blocking. Call it from a task of its own, never from the sampler and never
// from loop(), where it would hold up the web server for seconds.
namespace ingest {

// Reads the counter from NVS.
void begin();

// Whether there is an API to talk to at all (ZW_API_URL).
bool configured();
const char *baseUrl();
const char *source();

// The last event id handed out for this source. Kept in NVS, so a restart
// carries on above it rather than starting again at one, which zaehlwerk would
// answer as a duplicate until the source has been quiet for a while.
uint32_t lastEventId();
// Stored before the request goes, so a reboot in the middle cannot reuse it.
void storeEventId(uint32_t id);

// POSTs a JSON body to baseUrl() + path. Returns the HTTP status, or
// checkin::NO_WIFI / checkin::NO_ANSWER when nobody answered. `answer` holds
// the body of the response.
int post(const char *path, const std::string &body, std::string &answer, uint32_t timeoutMs);

}  // namespace ingest
