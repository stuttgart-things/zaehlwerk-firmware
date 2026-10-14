#include "ingest.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>

#include "checkin.h"
#include "config.h"

namespace ingest {
namespace {

const char *NVS_NAMESPACE = "ingest";
const char *KEY_EVENT_ID = "event_id";

const int32_t CONNECT_TIMEOUT_MS = 2000;

uint32_t lastEventId_ = 0;

}  // namespace

void begin() {
  Preferences p;
  if (p.begin(NVS_NAMESPACE, true)) {
    lastEventId_ = p.getUInt(KEY_EVENT_ID, 0);
    p.end();
  }
  if (configured())
    Serial.printf("[ingest] %s as %s, last event_id %u\n", baseUrl(), source(),
                  (unsigned)lastEventId_);
  else
    Serial.println("[ingest] no ZW_API_URL — nothing goes to zaehlwerk-api, check-in is off");
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

int post(const char *path, const std::string &body, std::string &answer, uint32_t timeoutMs) {
  answer.clear();
  // The two failures that look identical from the table and have different
  // fixes (#3): no network at all, or a network and nobody answering on it.
  if (WiFi.status() != WL_CONNECTED) return checkin::NO_WIFI;

  WiFiClient client;
  HTTPClient http;
  // Connecting is quick or it is not happening; the answer may take as long as
  // zaehlwerk needs to ask Schmetterpause.
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
  http.setTimeout(timeoutMs);
  if (!http.begin(client, String(baseUrl()) + path)) return checkin::NO_ANSWER;
  http.addHeader("Content-Type", "application/json");
  const int status =
      http.POST(reinterpret_cast<uint8_t *>(const_cast<char *>(body.data())), body.size());
  if (status > 0) answer = http.getString().c_str();
  http.end();
  return status > 0 ? status : checkin::NO_ANSWER;
}

}  // namespace ingest
