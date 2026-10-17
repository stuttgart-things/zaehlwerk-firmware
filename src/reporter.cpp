#include "reporter.h"

#include <Arduino.h>
#include <esp_random.h>

#include "config.h"
#include "diag.h"
#include "ingest.h"

// The task must not share the sampler's core (ADR-0003). Not a fixed number:
// today the sampler is on core 0 and this runs on core 1 beside loop(); once
// the sampler moves (#10, #33) this follows to core 0.
static_assert(ZW_REPORT_CORE != ZW_SENSOR_CORE,
              "the zaehlwerk link must not share the sampler's core (ADR-0003)");

namespace reporter {
namespace {

// zaehlwerk answers in milliseconds and a gateway with nobody behind it
// answers 503 at once; a request that takes longer than this is lost on the
// network and counts as no answer.
const uint32_t TIMEOUT_MS = 5000;
const char *INGEST_PATH = "/ingest/piezo";
const char *CURRENT_PATH = "/matches/current";

report::Link link_;
SemaphoreHandle_t lock_ = nullptr;
TaskHandle_t task_ = nullptr;
// Bumped on every change of address. An answer to a request that went out
// before it is from the old server and is not read.
volatile uint32_t generation_ = 0;
bool announce_ = false;

struct Guard {
  Guard() { xSemaphoreTake(lock_, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(lock_); }
};

void wake() {
  if (task_) xTaskNotifyGive(task_);
}

void task(void *) {
  for (;;) {
    report::Request r;
    uint32_t gen;
    {
      Guard g;
      gen = generation_;
      r = link_.next(millis(), esp_random());
    }
    if (r.type == report::Request::Type::None) {
      // Woken early by a new event or a new address.
      uint32_t wait = r.waitMs > 1000 ? 1000 : r.waitMs;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait) + 1);
      continue;
    }

    std::string answer;
    int status;
    if (r.type == report::Request::Type::Join) {
      status = ingest::get(CURRENT_PATH, answer, TIMEOUT_MS);
    } else {
      // Stored before it goes, so a reboot cannot send this id again for
      // another point. If it cannot be stored, it does not go.
      if (!ingest::persistThrough(r.event.eventId)) {
        Guard g;
        if (gen == generation_) link_.notPersisted(r, millis(), esp_random());
        continue;
      }
      status = ingest::post(INGEST_PATH, report::body(ingest::source(), r.event), answer, TIMEOUT_MS);
    }

    Guard g;
    if (gen == generation_) link_.answered(r, status, answer, millis(), esp_random());
  }
}

void startTask() {
  if (task_) return;
  // 8 KB: the HTTP client, a String for the URL and the answer, and the
  // parse. Priority 1, like the fob reader in #64: below the sampler (3).
  xTaskCreatePinnedToCore(task, "zaehlwerk", 8192, nullptr, 1, &task_, ZW_REPORT_CORE);
  Serial.printf("[zaehlwerk] link task on core %d\n", ZW_REPORT_CORE);
}

void queue(report::Kind kind, char half, uint8_t delta, bool manual, uint32_t pointId,
           uint32_t rallyId) {
  Guard g;
  const report::Admit a = link_.admit(kind, manual);
  if (a != report::Admit::Queue) {
    link_.refused(a, kind, half, delta, pointId, rallyId);
    return;
  }
  link_.push(kind, half, delta, ingest::nextEventId(), millis(), pointId, rallyId);
  wake();
}

String players(const report::State &s) {
  String j = ",\"players\":{\"a\":\"";
  for (char c : s.players[0]) j += (c == '"' || c == '\\') ? '\'' : c;
  j += "\",\"b\":\"";
  for (char c : s.players[1]) j += (c == '"' || c == '\\') ? '\'' : c;
  j += "\"},\"set_number\":" + String(s.setNumber);
  return j;
}

}  // namespace

void begin() {
  lock_ = xSemaphoreCreateMutex();
  ingest::begin();
  if (!ingest::configured()) return;  // nothing more: the board is what it was
  {
    Guard g;
    link_.configure(true, millis());
  }
  // Said in the log from the first tick(), not here: diag::begin() comes later
  // in setup(), and the session event has to stay the first of the session.
  announce_ = true;
  startTask();
}

void tick(void (*apply)(const report::State &), bool mock) {
  if (!task_) return;
  if (announce_) {
    announce_ = false;
    diag::link("address", ingest::urlOrigin(), "", 0, 0,
               String(",\"url\":\"") + ingest::baseUrl() + "\",\"source\":\"" +
                   ingest::source() + "\"");
  }
  report::Record recs[8];
  size_t n = 0;
  report::State adopted;
  bool adopt = false;
  report::State match;
  {
    Guard g;
    while (n < 8 && link_.takeRecord(recs[n])) n++;
    adopt = link_.takeAdoption(adopted);
    // Only when there is something to say: loop() comes by here thousands of
    // times a second, and copying names each time would be heap churn for
    // nothing.
    if (n) match = link_.match();
  }
  for (size_t i = 0; i < n; i++) {
    const report::Record &r = recs[i];
    if (r.type == report::Record::Type::Ingest) {
      diag::ingest(r.status, r.reason, r.kind == report::Kind::Undo ? "undo" : "point", r.eventId,
                   r.half, r.delta, r.matchId, ingest::source(), r.http, r.attempt, r.queuedMs,
                   r.waitMs, r.pointRef, r.rallyRef);
    } else {
      String extra;
      if (strcmp(r.status, "joined") == 0 && match.matchId == r.matchId) extra = players(match);
      // How long zaehlwerk was gone, once it is back; the wait before the
      // next try while it is not.
      if (r.waitMs)
        extra += String(strcmp(r.status, "back") == 0 ? ",\"after_ms\":" : ",\"retry_in_ms\":") +
                 r.waitMs;
      diag::link(r.status, r.reason, r.matchId, r.http, r.attempt, extra);
      Serial.printf("[zaehlwerk] %s%s%s%s%s\n", r.status, r.reason[0] ? " (" : "", r.reason,
                    r.reason[0] ? ")" : "", r.matchId[0] ? (String(" ") + r.matchId).c_str() : "");
    }
  }
  if (adopt && apply && !mock) apply(adopted);
}

void rally(char winner, const String &sequence, uint32_t pointId, uint32_t rallyId, bool mock) {
  if (!task_ || mock) return;
  const report::Verdict v = report::fromRally(winner, std::string(sequence.c_str()));
  if (!v.half) return;
  queue(report::Kind::Point, v.half, v.delta, false, v.delta ? pointId : 0, rallyId);
}

void manualPoint(char half, uint32_t pointId, bool mock) {
  if (!task_ || mock) return;
  queue(report::Kind::Point, half == 'B' ? 'B' : 'A', 1, true, pointId, 0);
}

void undo(uint32_t pointId, bool mock) {
  if (!task_ || mock) return;
  queue(report::Kind::Undo, 0, 0, false, pointId, 0);
}

String statusJson(bool mock) {
  report::Status s;
  {
    Guard g;
    s = link_.status(millis());
  }
  const String url = ingest::baseUrl();
  return String(report::statusJson(s, std::string(url.c_str()), ingest::source(),
                                   ingest::lastEventId(), mock)
                    .c_str());
}

bool setUrl(const String &url, String &message) {
  const char *why = "";
  const ingest::UrlChange c = ingest::setBaseUrl(url, &why);
  switch (c) {
    case ingest::UrlChange::Refused:
      message = why;
      return false;
    case ingest::UrlChange::Unchanged:
      message = "Unveraendert.";
      return true;
    case ingest::UrlChange::Set:
    case ingest::UrlChange::Cleared:
      break;
  }
  const bool on = c == ingest::UrlChange::Set;
  {
    Guard g;
    generation_ = generation_ + 1;
    link_.configure(on, millis());
  }
  diag::link("address", on ? "set" : "cleared", "", 0, 0,
             on ? String(",\"url\":\"") + ingest::baseUrl() + "\"" : String());
  Serial.printf("[zaehlwerk] address %s on the page%s%s\n", on ? "set" : "cleared",
                on ? ": " : "", on ? ingest::baseUrl().c_str() : "");
  if (on) {
    startTask();
    wake();
    message = "Gespeichert. Das Board sucht jetzt das laufende Match.";
  } else {
    message = "Getrennt. Das Board zaehlt wieder allein.";
  }
  return true;
}

}  // namespace reporter
