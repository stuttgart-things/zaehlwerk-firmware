#include "ingest.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <string.h>

#include "config.h"
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

int request(const char *method, const char *path, const std::string *body, std::string &answer,
            uint32_t timeoutMs) {
  answer.clear();
  const String base = baseUrl();
  if (!base.length()) return report::NO_ANSWER;
  // The two failures that look identical from the table and have different
  // fixes: no network at all, or a network and nobody answering on it.
  if (WiFi.status() != WL_CONNECTED) return report::NO_WIFI;

  WiFiClient client;
  HTTPClient http;
  http.setReuse(false);
  // Connecting is quick or it is not happening. The answer gets a few seconds:
  // zaehlwerk answers in milliseconds, and a gateway with nobody behind it
  // answers 503 at once, so a long wait here is a network that lost packets.
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
  http.setTimeout(timeoutMs);
  if (!http.begin(client, base + path)) return report::NO_ANSWER;
  int status;
  if (body) {
    http.addHeader("Content-Type", "application/json");
    status = http.sendRequest(method, reinterpret_cast<uint8_t *>(const_cast<char *>(body->data())),
                              body->size());
  } else {
    status = http.sendRequest(method);
  }
  if (status > 0) {
    const int size = http.getSize();
    if (size <= ANSWER_MAX) answer = http.getString().c_str();
    if ((int)answer.size() > ANSWER_MAX) answer.clear();
  }
  http.end();
  return status > 0 ? status : report::NO_ANSWER;
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

int get(const char *path, std::string &answer, uint32_t timeoutMs) {
  return request("GET", path, nullptr, answer, timeoutMs);
}

int post(const char *path, const std::string &body, std::string &answer, uint32_t timeoutMs) {
  return request("POST", path, &body, answer, timeoutMs);
}

}  // namespace ingest
