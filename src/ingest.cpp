#include "ingest.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <string.h>
#include <time.h>

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

// One client per scheme, kept between requests: the task is the only caller,
// and a TLS handshake (RSA-2048 leaf, ECDSA intermediate and root, ECDHE) is
// the expensive part of a request on an ESP32, in time and in heap. With the
// connection kept open, a board that polls every two seconds does it once.
HTTPClient http_;
WiFiClient plain_;
WiFiClientSecure *secure_ = nullptr;
String connectedTo_;  // the base URL the open connection belongs to
int lastTlsError_ = 0;

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
         const std::string *body, std::string &answer, uint32_t timeoutMs) {
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

int request(const char *method, const char *path, const std::string *body, std::string &answer,
            uint32_t timeoutMs) {
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
  int status = once(clientFor(base, tls, &reused), tls, method, url, body, answer, timeoutMs);
  if (status <= 0 && reused) {
    // A kept connection the server or the Gateway closed while it idled. Once
    // more on a fresh one, at once: the request is the same, with the same
    // event_id, so if the first did arrive the second is a duplicate.
    closeAll();
    connectedTo_ = base;
    answer.clear();
    status = once(clientFor(base, tls, &reused), tls, method, url, body, answer, timeoutMs);
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

}  // namespace

void begin() {
  Preferences p;
  if (p.begin(NVS_NAMESPACE, true)) {
    lastEventId_ = p.isKey(KEY_EVENT_ID) ? p.getUInt(KEY_EVENT_ID, 0) : 0;
    p.end();
  }
  persisted_ = lastEventId_;
  loadUrl();
  if (configured())
    Serial.printf("[zaehlwerk] %s (from %s) as %s, last event_id %u\n", baseUrl().c_str(),
                  urlOrigin(), source(), (unsigned)lastEventId_);
  else
    Serial.printf("[zaehlwerk] no address%s%s, nothing is reported; the board counts on its own\n",
                  urlOrigin()[0] ? ", " : "", urlOrigin());
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
  return request("GET", path, nullptr, answer, timeoutMs);
}

int post(const char *path, const std::string &body, std::string &answer, uint32_t timeoutMs) {
  return request("POST", path, &body, answer, timeoutMs);
}

}  // namespace ingest
