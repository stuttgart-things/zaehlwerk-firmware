#include "ingest.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <string.h>
#include <time.h>

#include "checkin.h"
#include "config.h"
#include "tls_trust_anchors.h"
#include "report.h"

namespace ingest {
namespace {

// The same namespace and counter key as the check-in client in #64, so both
// count on one counter once they meet.
const char *NVS_NAMESPACE = "ingest";
const char *KEY_EVENT_ID = "event_id";
// Present and empty means cleared on the page, which is not the same as never
// set: only a missing key lets the seed from secrets.ini in.
const char *KEY_URL = "api_url";
const char *KEY_TOKEN = "checkin_tok";  // the same rule: present and empty is cleared

const int32_t CONNECT_TIMEOUT_MS = 2000;
// An answer from zaehlwerk is a few hundred bytes. Anything far larger is not
// one, and reading it would only cost heap.
const int ANSWER_MAX = 8192;

// Written from the web handler on loop()'s core, read by the report task on
// the other one, so behind a lock. Fixed buffers, so nothing allocates while
// the lock is held.
char url_[97] = {0};
const char *origin_ = "";
portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;

uint32_t lastEventId_ = 0;  // handed out
uint32_t persisted_ = 0;    // stored in NVS

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

void putUrl(const char *u, const char *origin) {
  portENTER_CRITICAL(&lock_);
  memset(url_, 0, sizeof(url_));
  strncpy(url_, u, sizeof(url_) - 1);
  origin_ = origin;
  portEXIT_CRITICAL(&lock_);
}

void loadUrl() {
  Preferences p;
  // isKey() first, as in net.cpp: getString() on a missing key logs an error,
  // and on a first boot a missing key is the normal state.
  if (p.begin(NVS_NAMESPACE, true)) {
    if (p.isKey(KEY_URL)) {
      String u = p.getString(KEY_URL, "");
      p.end();
      const bool usable = u.length() && report::checkUrl(u.c_str()) == report::UrlProblem::None;
      putUrl(usable ? report::normaliseUrl(u.c_str()).c_str() : "",
             usable ? "NVS" : "cleared on the page");
      return;
    }
    p.end();
  }
  // Not copied into NVS, like the wifi seed in net.cpp: erasing NVS should fall
  // back to the seed rather than to whatever was stored once.
  const char *seed = ZW_API_URL;
  if (seed[0] && report::checkUrl(seed) == report::UrlProblem::None) {
    putUrl(report::normaliseUrl(seed).c_str(), "secrets.ini");
  } else {
    if (seed[0]) Serial.println("[zaehlwerk] ZW_API_URL from secrets.ini is not usable, ignored");
    putUrl("", "");
  }
}

// One client per scheme, kept between requests: the report task and the fob
// reader take turns on it behind requestLock_, and a TLS handshake (RSA-2048 leaf, ECDSA intermediate and root, ECDHE) is
// the expensive part of a request on an ESP32, in time and in heap. With the
// connection kept open, a board that polls every two seconds does it once.
HTTPClient http_;
WiFiClient plain_;
WiFiClientSecure *secure_ = nullptr;
String connectedTo_;  // the base URL the open connection belongs to
int lastTlsError_ = 0;
// Held for a whole request, so a check-in waits for a point in flight rather
// than sharing its connection. Never taken by the sampler or by loop().
SemaphoreHandle_t requestLock_ = nullptr;

void closeAll() {
  plain_.stop();
  if (secure_) {
    secure_->stop();
    delete secure_;
    secure_ = nullptr;
  }
}

// The client for this request: the open connection if there is one to the
// same address, otherwise a fresh one. A fresh TLS client trusts the edge root
// and nothing else; there is no path in this file that calls setInsecure().
WiFiClient &clientFor(const String &base, bool tls, bool *reused) {
  if (base != connectedTo_) {
    closeAll();
    connectedTo_ = base;
  }
  if (!tls) {
    *reused = plain_.connected();
    return plain_;
  }
  if (secure_ && secure_->connected()) {
    *reused = true;
    return *secure_;
  }
  *reused = false;
  delete secure_;
  secure_ = new WiFiClientSecure();
  secure_->setCACert(TLS_TRUST_ANCHORS_PEM);
  // The default is two minutes; a handshake that has not finished in ten
  // seconds is a network that lost packets, and the queue waits behind it.
  secure_->setHandshakeTimeout(10);
  return *secure_;
}

int once(WiFiClient &client, bool tls, const char *method, const String &url,
         const std::string *body, const std::string &bearer, std::string &answer,
         uint32_t timeoutMs) {
  http_.setReuse(true);
  // Connecting is quick or it is not happening. The answer gets a few seconds:
  // zaehlwerk answers in milliseconds, and a gateway with nobody behind it
  // answers 503 at once, so a long wait here is a network that lost packets.
  http_.setConnectTimeout(tls ? 10000 : CONNECT_TIMEOUT_MS);
  http_.setTimeout(timeoutMs);
  if (!http_.begin(client, url)) return report::NO_ANSWER;
  int status;
  if (body) {
    http_.addHeader("Content-Type", "application/json");
    // The HTTP client keeps its own copy of the header until end() below; it
    // does not log request headers.
    if (!bearer.empty()) http_.addHeader("Authorization", bearer.c_str());
    status = http_.sendRequest(method, reinterpret_cast<uint8_t *>(const_cast<char *>(body->data())),
                               body->size());
  } else {
    status = http_.sendRequest(method);
  }
  if (status > 0) {
    const int size = http_.getSize();
    if (size <= ANSWER_MAX) answer = http_.getString().c_str();
    // An answer left unread would be the start of the next one on a kept
    // connection, so a body too large to read closes it.
    else client.stop();
    if ((int)answer.size() > ANSWER_MAX) answer.clear();
  }
  // Keeps the connection open when the server allows it (HTTP/1.1, no
  // "Connection: close"), closes it otherwise.
  http_.end();
  return status;
}

int request(const char *method, const char *path, const std::string *body,
            const std::string &bearer, std::string &answer, uint32_t timeoutMs) {
  answer.clear();
  const String base = baseUrl();
  if (!base.length()) return report::NO_ANSWER;
  // The two failures that look identical from the table and have different
  // fixes: no network at all, or a network and nobody answering on it.
  if (WiFi.status() != WL_CONNECTED) {
    closeAll();
    return report::NO_WIFI;
  }
  const bool tls = report::isHttps(base.c_str());
#if ZW_TLS_WAIT_FOR_TIME
  if (tls && !clockSet()) return report::NO_TIME;
#endif
  const String url = base + path;

  bool reused = false;
  int status = once(clientFor(base, tls, &reused), tls, method, url, body, bearer, answer, timeoutMs);
  if (status <= 0 && reused) {
    // A kept connection the server or the Gateway closed while it idled. Once
    // more on a fresh one, at once: the request is the same, with the same
    // event_id, so if the first did arrive the second is a duplicate.
    closeAll();
    connectedTo_ = base;
    answer.clear();
    status = once(clientFor(base, tls, &reused), tls, method, url, body, bearer, answer, timeoutMs);
  }
  if (status > 0) {
    lastTlsError_ = 0;
    return status;
  }

  // Nobody answered. With TLS, say whether the certificate was the reason, and
  // print mbedTLS's reason once per change rather than on every retry.
  int code = 0;
  if (tls && secure_) {
    char why[96] = {0};
    code = secure_->lastError(why, sizeof(why));
    if (code < -1 && code != lastTlsError_)
      Serial.printf("[zaehlwerk] TLS to %s failed: %s (-0x%04x)\n", base.c_str(), why, -code);
  }
  lastTlsError_ = code;
  closeAll();
  connectedTo_ = base;
  // -1 is a TCP connect that failed or a handshake that timed out; anything
  // below is mbedTLS refusing, the certificate check among it.
  return code < -1 ? report::TLS_FAILED : report::NO_ANSWER;
}

int locked(const char *method, const char *path, const std::string *body,
           const std::string &bearer, std::string &answer, uint32_t timeoutMs) {
  if (!requestLock_) return report::NO_ANSWER;  // begin() has not run
  xSemaphoreTake(requestLock_, portMAX_DELAY);
  const int status = request(method, path, body, bearer, answer, timeoutMs);
  xSemaphoreGive(requestLock_);
  return status;
}

}  // namespace

void begin() {
  if (!requestLock_) requestLock_ = xSemaphoreCreateMutex();
  Preferences p;
  if (p.begin(NVS_NAMESPACE, true)) {
    lastEventId_ = p.isKey(KEY_EVENT_ID) ? p.getUInt(KEY_EVENT_ID, 0) : 0;
    p.end();
  }
  persisted_ = lastEventId_;
  loadUrl();
  const char *token = loadToken();
  if (configured())
    Serial.printf("[zaehlwerk] %s (from %s) as %s, last event_id %u\n", baseUrl().c_str(),
                  urlOrigin(), source(), (unsigned)lastEventId_);
  else
    Serial.printf("[zaehlwerk] no address%s%s, nothing is reported; the board counts on its own\n",
                  urlOrigin()[0] ? ", " : "", urlOrigin());
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

bool configured() {
  portENTER_CRITICAL(&lock_);
  const bool have = url_[0] != '\0';
  portEXIT_CRITICAL(&lock_);
  return have;
}

String baseUrl() {
  char u[sizeof(url_)];
  portENTER_CRITICAL(&lock_);
  memcpy(u, url_, sizeof(u));
  portEXIT_CRITICAL(&lock_);
  return String(u);
}

const char *source() { return ZW_INGEST_SOURCE; }

const char *urlOrigin() {
  portENTER_CRITICAL(&lock_);
  const char *o = origin_;
  portEXIT_CRITICAL(&lock_);
  return o;
}

UrlChange setBaseUrl(const String &url, const char **why) {
  *why = "";
  String u = url;
  u.trim();
  const bool clear = u.isEmpty();
  if (!clear) {
    const report::UrlProblem p = report::checkUrl(u.c_str());
    if (p != report::UrlProblem::None) {
      *why = report::urlProblemText(p);
      return UrlChange::Refused;
    }
    u = report::normaliseUrl(u.c_str()).c_str();
  }
  // Nothing to write when NVS already says exactly this. A seed taken over or
  // cleared on the page is a change, because it outlives the next flash.
  const bool inNvs = strcmp(urlOrigin(), "NVS") == 0;
  const bool clearedInNvs = strcmp(urlOrigin(), "cleared on the page") == 0;
  if ((!clear && inNvs && u == baseUrl()) || (clear && clearedInNvs)) return UrlChange::Unchanged;
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, false)) {
    *why = "Nicht gespeichert: NVS laesst sich nicht schreiben.";
    return UrlChange::Refused;
  }
  p.putString(KEY_URL, u);
  p.end();
  putUrl(u.c_str(), clear ? "cleared on the page" : "NVS");
  return clear ? UrlChange::Cleared : UrlChange::Set;
}

uint32_t lastEventId() {
  portENTER_CRITICAL(&lock_);
  const uint32_t id = lastEventId_;
  portEXIT_CRITICAL(&lock_);
  return id;
}

uint32_t nextEventId() {
  portENTER_CRITICAL(&lock_);
  const uint32_t id = ++lastEventId_;
  portEXIT_CRITICAL(&lock_);
  return id;
}

bool persistThrough(uint32_t id) {
  portENTER_CRITICAL(&lock_);
  const bool stored = id <= persisted_;
  portEXIT_CRITICAL(&lock_);
  if (stored) return true;
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, false)) return false;
  const bool ok = p.putUInt(KEY_EVENT_ID, id) == sizeof(uint32_t);
  p.end();
  if (!ok) return false;
  portENTER_CRITICAL(&lock_);
  if (id > persisted_) persisted_ = id;
  portEXIT_CRITICAL(&lock_);
  return true;
}

void startClock() {
  static bool started = false;
  // SNTP keeps pointers to its server names, so they live here for good.
  static char gateway[16] = {0};
  if (started || WiFi.status() != WL_CONNECTED) return;
  started = true;
  strncpy(gateway, WiFi.gatewayIP().toString().c_str(), sizeof(gateway) - 1);
  // UTC: the board shows no time, it only needs one a certificate accepts.
  configTime(0, 0, gateway, ZW_NTP_FALLBACK);
  Serial.printf("[zaehlwerk] SNTP: asking %s, then %s\n", gateway, ZW_NTP_FALLBACK);
}

bool clockSet() {
  // 2026-01-01T00:00:00Z. Anything before is the clock counting from 1970.
  return time(nullptr) >= 1767225600;
}

int get(const char *path, std::string &answer, uint32_t timeoutMs) {
  return locked("GET", path, nullptr, std::string(), answer, timeoutMs);
}

int post(const char *path, const std::string &body, std::string &answer, uint32_t timeoutMs,
         Auth auth) {
  std::string bearer;
  if (auth == Auth::Checkin) {
    std::string token = copyToken();
    // No token, no request: zaehlwerk would refuse it, and an unauthenticated
    // check-in is the thing #65 exists to stop.
    if (token.empty()) {
      answer.clear();
      return checkin::NO_TOKEN;
    }
    bearer = checkin::authorization(token);
    wipe(token);
  }
  const int status = locked("POST", path, &body, bearer, answer, timeoutMs);
  wipe(bearer);
  return status;
}

}  // namespace ingest
