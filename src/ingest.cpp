#include "ingest.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <string.h>

#include "checkin.h"
#include "config.h"

namespace ingest {
namespace {

const char *NVS_NAMESPACE = "ingest";
const char *KEY_EVENT_ID = "event_id";
// Present and empty means cleared on the page, which is not the same as never
// set: only a missing key lets the seed from secrets.ini in.
const char *KEY_TOKEN = "checkin_tok";

const int32_t CONNECT_TIMEOUT_MS = 2000;

uint32_t lastEventId_ = 0;

// Written from the web handler on loop()'s core, read by the fob task on the
// other one, so behind a lock. A fixed buffer, zeroed whole when it changes.
char token_[checkin::TOKEN_MAX + 1] = {0};
portMUX_TYPE tokenLock_ = portMUX_INITIALIZER_UNLOCKED;

void putToken(const char *t) {
  portENTER_CRITICAL(&tokenLock_);
  memset(token_, 0, sizeof(token_));
  strncpy(token_, t, sizeof(token_) - 1);
  portEXIT_CRITICAL(&tokenLock_);
}

// A copy for the one request. The caller zeroes it when the request is done.
std::string copyToken() {
  char t[sizeof(token_)];
  portENTER_CRITICAL(&tokenLock_);
  memcpy(t, token_, sizeof(t));
  portEXIT_CRITICAL(&tokenLock_);
  std::string out(t);
  memset(t, 0, sizeof(t));
  return out;
}

void wipe(std::string &s) {
  if (!s.empty()) memset(&s[0], 0, s.size());
  s.clear();
}

// Where the token came from, for the one line at startup. Never the token.
const char *loadToken() {
  Preferences p;
  // isKey() first, as in net.cpp: getString() on a missing key logs an error,
  // and on a first boot a missing key is the normal state.
  if (p.begin(NVS_NAMESPACE, true)) {
    if (p.isKey(KEY_TOKEN)) {
      String t = p.getString(KEY_TOKEN, "");
      p.end();
      const bool usable = checkin::tokenUsable(t.c_str());
      putToken(usable ? t.c_str() : "");
      if (t.length()) memset(&t[0], 0, t.length());
      return usable ? "from NVS" : "cleared on the page";
    }
    p.end();
  }
  if (checkin::tokenUsable(ZW_CHECKIN_TOKEN)) {
    // Not copied into NVS, like the wifi seed in net.cpp: erasing NVS should
    // fall back to the seed rather than to whatever was stored once.
    putToken(ZW_CHECKIN_TOKEN);
    return "from secrets.ini";
  }
  return nullptr;
}

}  // namespace

void begin() {
  Preferences p;
  if (p.begin(NVS_NAMESPACE, true)) {
    lastEventId_ = p.getUInt(KEY_EVENT_ID, 0);
    p.end();
  }
  const char *token = loadToken();
  if (configured())
    Serial.printf("[ingest] %s as %s, last event_id %u\n", baseUrl(), source(),
                  (unsigned)lastEventId_);
  else
    Serial.println("[ingest] no ZW_API_URL — nothing goes to zaehlwerk-api, check-in is off");
  // Whether there is one and where it came from. Never the token (#65).
  if (haveCheckinToken())
    Serial.printf("[ingest] check-in token set (%s)\n", token);
  else
    Serial.printf("[ingest] no check-in token%s%s — no check-in is sent; set one on the "
                  "board's page\n",
                  token ? ", " : "", token ? token : "");
}

bool haveCheckinToken() {
  portENTER_CRITICAL(&tokenLock_);
  const bool have = token_[0] != '\0';
  portEXIT_CRITICAL(&tokenLock_);
  return have;
}

TokenChange setCheckinToken(const String &token) {
  const bool clear = token.isEmpty();
  if (!clear && !checkin::tokenUsable(token.c_str())) return TokenChange::Refused;
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, false)) return TokenChange::Refused;
  p.putString(KEY_TOKEN, token);
  p.end();
  putToken(token.c_str());
  return clear ? TokenChange::Cleared : TokenChange::Set;
}

bool configured() { return baseUrl()[0] != '\0'; }
const char *baseUrl() { return ZW_API_URL; }
const char *source() { return ZW_INGEST_SOURCE; }

uint32_t lastEventId() { return lastEventId_; }

void storeEventId(uint32_t id) {
  lastEventId_ = id;
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, false)) return;
  p.putUInt(KEY_EVENT_ID, id);
  p.end();
}

int post(const char *path, const std::string &body, std::string &answer, uint32_t timeoutMs,
         Auth auth) {
  answer.clear();
  std::string bearer;
  if (auth == Auth::Checkin) {
    std::string token = copyToken();
    // No token, no request: zaehlwerk would refuse it, and an unauthenticated
    // check-in is the thing #65 exists to stop.
    if (token.empty()) return checkin::NO_TOKEN;
    bearer = checkin::authorization(token);
    wipe(token);
  }
  // The two failures that look identical from the table and have different
  // fixes (#3): no network at all, or a network and nobody answering on it.
  if (WiFi.status() != WL_CONNECTED) {
    wipe(bearer);
    return checkin::NO_WIFI;
  }

  WiFiClient client;
  HTTPClient http;
  // Connecting is quick or it is not happening; the answer may take as long as
  // zaehlwerk needs to ask Schmetterpause.
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
  http.setTimeout(timeoutMs);
  if (!http.begin(client, String(baseUrl()) + path)) {
    wipe(bearer);
    return checkin::NO_ANSWER;
  }
  http.addHeader("Content-Type", "application/json");
  // The HTTP client keeps its own copy of the header until it goes out of
  // scope at the end of this function; it does not log request headers.
  if (!bearer.empty()) http.addHeader("Authorization", bearer.c_str());
  wipe(bearer);
  const int status =
      http.POST(reinterpret_cast<uint8_t *>(const_cast<char *>(body.data())), body.size());
  if (status > 0) answer = http.getString().c_str();
  http.end();
  return status > 0 ? status : checkin::NO_ANSWER;
}

}  // namespace ingest
