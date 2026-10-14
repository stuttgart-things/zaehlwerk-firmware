#include "fob.h"

#include <MFRC522.h>
#include <SPI.h>

#include <string.h>

#include "checkin.h"
#include "config.h"
#include "diag.h"
#include "ingest.h"

static_assert(ZW_FOB_CORE != ZW_SENSOR_CORE,
              "the fob reader must not share the sampler's core (ADR-0003)");

namespace fob {
namespace {

// How often the reader is asked whether a card is there. A fob is held up for
// a second at least; a tenth of that is plenty and costs a few SPI transfers.
const uint32_t POLL_MS = 50;

// Longer than zaehlwerk's own five seconds for asking Schmetterpause
// (ADR-0010), or every slow resolve would read here as no answer.
const uint32_t HTTP_TIMEOUT_MS = 8000;

// Only when nobody answered at all, and quickly: somebody is standing at the
// table waiting for the LED. The same event_id each time, so zaehlwerk claims
// once however many arrive (ADR-0002).
const int ATTEMPTS = 3;
const uint32_t BACKOFF_MS = 300;

// How often a missing reader is looked for again.
const uint32_t REINIT_MS = 5000;

MFRC522 reader(ZW_PIN_RC522_SS, ZW_PIN_RC522_RST);
checkin::Window window(ZW_CHECKIN_WINDOW_MS);
SemaphoreHandle_t lock_ = nullptr;
TaskHandle_t task_ = nullptr;
volatile bool busy_ = false;

// The version register, 0 while no reader answers. Written by the task, read by
// tick() to say so once rather than every poll.
volatile uint8_t readerVersion_ = 0;

// What the task wants said. Static tokens and numbers, never the UID: there is
// no field it could go in.
struct Record {
  char half;            // 'A', 'B' or ' '
  const char *outcome;  // a protocol token
  uint32_t eventId;     // 0 when no request was made
  int status;           // HTTP, checkin::NO_*, or 0 when nothing was sent
  uint8_t attempts;
};
QueueHandle_t records_ = nullptr;

// The last outcome, for /checkin.
Record last_{' ', "", 0, 0, 0};

struct Guard {
  Guard() { xSemaphoreTake(lock_, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(lock_); }
};

void say(char half, const char *outcome, uint32_t eventId = 0, int status = 0,
         uint8_t attempts = 0) {
  const Record r{half, outcome, eventId, status, attempts};
  // A full queue loses a line of log, never a check-in.
  if (records_) xQueueSend(records_, &r, 0);
}

void led(int pin, bool on) {
  if (pin >= 0) digitalWrite(pin, on ? HIGH : LOW);
}

bool readerAnswers() {
  const uint8_t v = reader.PCD_ReadRegister(MFRC522::VersionReg);
  // 0x00 and 0xFF are a bus with nothing on it. 0x91/0x92 are NXP's, clones
  // answer 0x88 or 0xB2 — anything else on the bus is taken as a reader.
  readerVersion_ = (v == 0x00 || v == 0xFF) ? 0 : v;
  return readerVersion_ != 0;
}

// Copies the UID out of the driver and clears the driver's copy, so the only
// place the card number lives is the one this code controls.
bool readCard(checkin::Uid &out) {
  if (!reader.PICC_IsNewCardPresent() || !reader.PICC_ReadCardSerial()) return false;
  const uint8_t n = reader.uid.size;
  const bool ok = n == 4 || n == 7 || n == 10;
  if (ok) {
    memcpy(out.bytes, reader.uid.uidByte, n);
    out.size = n;
  }
  // Halted, the card stays quiet until it leaves the field and comes back, so
  // a fob resting on the reader is one read and not twenty a second.
  reader.PICC_HaltA();
  memset(&reader.uid, 0, sizeof(reader.uid));
  return ok;
}

void send(checkin::Request r) {
  // One source, one counter: the points of #17 count on the same one, so the
  // id comes from ingest rather than from the window. Stored before the
  // request goes, so a reboot in the middle cannot reuse it; if NVS cannot be
  // written, nothing goes.
  r.eventId = ingest::nextEventId();
  const bool stored = ingest::persistThrough(r.eventId);

  std::string body = checkin::requestBody(ingest::source(), r);
  r.uid.clear();
  std::string answer;
  int status = checkin::NO_ANSWER;
  uint8_t attempts = 0;
  while (stored && attempts < ATTEMPTS) {
    attempts++;
    status = ingest::post("/ingest/checkin", body, answer, HTTP_TIMEOUT_MS);
    if (!checkin::worthRetrying(status)) break;
    vTaskDelay(pdMS_TO_TICKS(BACKOFF_MS * attempts));
  }
  // The request was the UID's last stop.
  memset(&body[0], 0, body.size());
  body.clear();

  const checkin::Verdict v = checkin::judge(status, answer);
  {
    Guard g;
    window.answered(v.signal, millis());
  }
  say(r.half, v.outcome, r.eventId, status, attempts);
}

void lights(uint32_t now) {
  checkin::Signal s;
  uint32_t since;
  {
    Guard g;
    s = window.signal(now);
    since = window.since();
  }
  const checkin::Lights l = checkin::lights(s, now - since);
  led(ZW_PIN_LED_GREEN, l.green);
  led(ZW_PIN_LED_RED, l.red);
}

void task(void *) {
  SPI.begin(ZW_PIN_RC522_SCK, ZW_PIN_RC522_MISO, ZW_PIN_RC522_MOSI, ZW_PIN_RC522_SS);
  reader.PCD_Init();
  bool present = readerAnswers();
  uint32_t lastInit = millis();

  for (;;) {
    // Woken early by a press, so the request goes out without waiting a poll.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_MS));
    uint32_t now = millis();

    bool ready = false;
    checkin::Request r{};
    {
      Guard g;
      if (window.ready()) {
        r = window.take();
        ready = true;
      }
    }
    if (ready) {
      // Both LEDs on before the request blocks this task for up to seconds.
      lights(now);
      send(r);
      r.uid.clear();
      now = millis();
    }

    bool expired;
    {
      Guard g;
      expired = window.expire(now);
    }
    if (expired) say(' ', "expired");

    if (!present) {
      if (now - lastInit >= REINIT_MS) {
        reader.PCD_Init();
        present = readerAnswers();
        lastInit = now;
      }
    } else {
      checkin::Uid uid{};
      if (readCard(uid)) {
        checkin::Signal s;
        {
          Guard g;
          window.fob(uid, busy_, ingest::configured(), now);
          s = window.signal(now);
        }
        uid.clear();
        if (s == checkin::Signal::Waiting)          say(' ', "waiting");
        else if (s == checkin::Signal::Rejected)    say(' ', "busy");
        else if (s == checkin::Signal::Unavailable) say(' ', "off");
        else                                        say(' ', "ignored");
      } else if (now - lastInit >= REINIT_MS) {
        // A reader that was there can go: a loose wire, a brown-out on the
        // module. Looked at now and then, not every poll.
        present = readerAnswers();
        lastInit = now;
      }
    }

    lights(now);
  }
}

}  // namespace

void begin() {
  if (ZW_PIN_LED_GREEN >= 0) pinMode(ZW_PIN_LED_GREEN, OUTPUT);
  if (ZW_PIN_LED_RED >= 0) pinMode(ZW_PIN_LED_RED, OUTPUT);
  led(ZW_PIN_LED_GREEN, false);
  led(ZW_PIN_LED_RED, false);

  lock_ = xSemaphoreCreateMutex();
  records_ = xQueueCreate(8, sizeof(Record));

  // Priority 1, like loop(): the reader is a person holding a fob up, nothing
  // here is in a hurry. The stack is for the HTTP client.
  xTaskCreatePinnedToCore(task, "fob", 6144, nullptr, 1, &task_, ZW_FOB_CORE);
  Serial.printf("[checkin] reader task on core %d, window %u ms\n", ZW_FOB_CORE,
                (unsigned)ZW_CHECKIN_WINDOW_MS);
}

bool press(char half) {
  if (!lock_) return false;
  bool taken;
  {
    Guard g;
    taken = window.press(half, millis());
  }
  if (taken && task_) xTaskNotifyGive(task_);
  return taken;
}

void setBusy(bool busy) { busy_ = busy; }

void tick() {
  static uint8_t reported = 0xFF;  // nothing said yet
  const uint8_t v = readerVersion_;
  if (v != reported) {
    if (v) diag::note("info", String("fob reader answers, version 0x") + String(v, HEX));
    else   diag::note("warn", "no fob reader answers on SPI — check-in by fob is off");
    Serial.printf("[checkin] reader: %s 0x%02X\n", v ? "version" : "none", v);
    reported = v;
  }

  if (!records_) return;
  Record r;
  while (xQueueReceive(records_, &r, 0) == pdTRUE) {
    Serial.println(checkin::logLine(r.half, r.outcome, r.eventId, r.status).c_str());
    diag::checkin(r.half, r.outcome, ingest::source(), r.eventId, r.status, r.attempts);
    last_ = r;
  }
}

String stateJson() {
  checkin::Signal s;
  {
    Guard g;
    s = window.signal(millis());
  }
  const char *half = checkin::halfToken(last_.half);
  String j = String("{\"on\":") + (ingest::configured() ? "true" : "false");
  j += String(",\"reader\":") + (readerVersion_ ? "true" : "false");
  j += String(",\"signal\":\"") + checkin::signalName(s) + "\"";
  j += String(",\"window_ms\":") + ZW_CHECKIN_WINDOW_MS;
  j += String(",\"source\":\"") + ingest::source() + "\"";
  j += String(",\"last\":{\"outcome\":\"") + last_.outcome + "\"";
  if (half) j += String(",\"half\":\"") + last_.half + "\"";
  j += "}}";
  return j;
}

}  // namespace fob
