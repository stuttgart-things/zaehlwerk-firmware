#include "diag.h"

#include <Preferences.h>
#include <WiFiUdp.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_system.h>
#include <string.h>

#include "config.h"
#include "version.h"

namespace diag {
namespace {

const char *NVS_NAMESPACE = "zaehlwerk";
const char *KEY_HOST = "sink_host";
const char *KEY_PORT = "sink_port";
const char *KEY_ON = "sink_on";

// Stay under a plain ethernet MTU so nothing fragments on the way. A hit with
// its samples is several times this, which is what chunking is for.
const size_t MAX_PAYLOAD = 1200;
const size_t CHUNK_BODY = 900;  // room for the envelope around each fragment

// Four is enough: hits arrive a few times a second and the drain runs every
// loop. Each one is the better part of two kilobytes, so depth is not free.
const int QUEUE_DEPTH = 4;

WiFiUDP udp;
QueueHandle_t queue_ = nullptr;

// Held events, for when no sink is listening yet. Curves may be lost, points
// must not (ADR-0004). The rule is the datagram, not the event type: anything
// that fits in one is cheap enough to keep, and that needs no table to stay
// correct when a new kind of event is added.
//
// A linear arena rather than a ring: dropping the oldest happens only when
// nobody has listened for a very long time, and a memmove then is cheaper than
// wrap-around arithmetic on every write.
const size_t PUFFER_BYTES = 16384;
const int PUFFER_MAX = 96;
uint8_t arena_[PUFFER_BYTES];
uint16_t laengen_[PUFFER_MAX];
size_t genutzt_ = 0;
int gehalten_ = 0;
uint32_t haltenSeitMs_ = 0;
uint32_t verworfen_ = 0;   // too large to hold, or dropped to make room

// UDP tells a sender nothing, so a sink that crashed and one that is listening
// are indistinguishable — and a board that cannot tell fires a whole game into
// nothing. So the board says hello and the sink answers; no answer for this long
// and it holds instead of sending.
const uint32_t LEBEN_MS = 6000;
const uint32_t HALLO_MS = 2000;
uint32_t zuletztGehoert_ = 0;
uint32_t zuletztGefragt_ = 0;

char sessionId_[9] = "00000000";
String matchId_;
uint32_t seq_ = 0;
uint32_t dropped_ = 0;
String host_;
uint16_t port_ = 9000;
bool on_ = false;
String deviceId_;

String esc(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if ((uint8_t)c < 0x20) continue;
    else out += c;
  }
  return out;
}

bool lebt() {
  return zuletztGehoert_ && (millis() - zuletztGehoert_) < LEBEN_MS;
}

bool sendable() { return on_ && host_.length() > 0 && lebt(); }

void aeltestenVerwerfen() {
  if (gehalten_ == 0) return;
  const uint16_t weg = laengen_[0];
  memmove(arena_, arena_ + weg, genutzt_ - weg);
  memmove(laengen_, laengen_ + 1, sizeof(uint16_t) * (gehalten_ - 1));
  genutzt_ -= weg;
  gehalten_--;
  verworfen_++;
}

void halten(const String &payload) {
  const size_t n = payload.length();
  if (n > MAX_PAYLOAD) { verworfen_++; return; }   // a curve; let it go
  while (gehalten_ >= PUFFER_MAX || genutzt_ + n > PUFFER_BYTES) {
    if (gehalten_ == 0) { verworfen_++; return; }
    aeltestenVerwerfen();
  }
  memcpy(arena_ + genutzt_, payload.c_str(), n);
  laengen_[gehalten_++] = (uint16_t)n;
  genutzt_ += n;
  if (!haltenSeitMs_) haltenSeitMs_ = millis();
}

void transmit(const String &payload) {
  IPAddress ip;
  if (!ip.fromString(host_)) return;  // a name would need a lookup; not here
  udp.beginPacket(ip, port_);
  udp.write((const uint8_t *)payload.c_str(), payload.length());
  udp.endPacket();
}

// One event out. Small ones go whole; large ones are split as a *string* and
// the sink joins the parts before parsing — chunks are not documents of their
// own, which keeps reassembly from needing a parser that tolerates halves.
void emit(uint32_t seq, const String &body) {
  if (!sendable()) {
    halten(body);
    return;
  }

  if (body.length() <= MAX_PAYLOAD) {
    transmit(body);
    return;
  }
  const size_t n = (body.length() + CHUNK_BODY - 1) / CHUNK_BODY;
  for (size_t i = 0; i < n; i++) {
    String part = body.substring(i * CHUNK_BODY,
                                 min(body.length(), (i + 1) * CHUNK_BODY));
    String wrap = String("{\"v\":1,\"session_id\":\"") + sessionId_ +
                  "\",\"seq\":" + seq + ",\"chunk\":{\"i\":" + i + ",\"n\":" + n +
                  "},\"part\":\"" + esc(part) + "\"}";
    transmit(wrap);
  }
}

// Every event carries the same head. seq is gapless within a session and is the
// only way loss is detectable, since UDP has no retry here.
String head(const char *type, uint32_t seq, uint32_t tUs) {
  return String("{\"v\":1,\"session_id\":\"") + sessionId_ + "\",\"seq\":" + seq +
         ",\"id\":\"" + sessionId_ + "-" + seq + "\",\"t_us\":" + tUs +
         ",\"type\":\"" + type + "\"";
}

void emitNow(const char *type, const String &fields) {
  uint32_t s = seq_++;
  emit(s, head(type, s, (uint32_t)micros()) + fields + "}");
}

String samplesJson(const Hit &h) {
  String a = "[", b = "[", t = "[";
  for (uint16_t i = 0; i < h.sampleCount; i++) {
    if (i) { a += ','; b += ','; t += ','; }
    a += h.samples[i].a;
    b += h.samples[i].b;
    t += h.samples[i].dtUs;
  }
  a += ']'; b += ']'; t += ']';
  return String(",\"samples\":{\"pre_us\":") + h.preUs + ",\"n\":" + h.sampleCount +
         ",\"t_us\":" + t + ",\"a\":" + a + ",\"b\":" + b + "}";
}

void emitHit(const Hit &h) {
  uint32_t s = seq_++;
  String f = String(",\"rally_id\":\"") + sessionId_ + "-r" + h.rallyId + "\"";
  f += ",\"side\":";
  if (h.side == 'A' || h.side == 'B') f += String("\"") + h.side + "\"";
  else f += "null";
  f += String(",\"decision\":\"") + decisionName(h.decision) + "\"";
  f += ",\"peak_a\":" + String(h.peakA) + ",\"peak_b\":" + String(h.peakB);
  f += ",\"baseline_a\":" + String(h.baselineA) +
       ",\"baseline_b\":" + String(h.baselineB);
  f += ",\"cross_a_us\":" + String(h.crossAUs) +
       ",\"cross_b_us\":" + String(h.crossBUs);
  f += ",\"ratio\":" + String(h.ratio, 3);
  f += ",\"counted\":" + String(h.counted ? "true" : "false");
  if (h.intendedSide && h.intendedType) {
    f += String(",\"intended\":{\"side\":\"") + h.intendedSide +
         "\",\"type\":\"" + h.intendedType + "\"}";
  }
  f += samplesJson(h);
  emit(s, head("hit", s, h.tUs) + f + "}");
}

}  // namespace

const char *decisionName(Decision d) {
  switch (d) {
    case Decision::Counted:        return "counted";
    case Decision::BelowThreshold: return "below_threshold";
    case Decision::Deadtime:       return "deadtime";
    default:                       return "ambiguous";
  }
}

// Kept so the session event can be stated again later, when somebody points the
// sink at a different machine mid-session.
Config cfg_{};
String (*parameters_)() = nullptr;

// Why the chip last started, as a token. A loop of sessions that each open with
// "boot" looks the same in the sink whether it is a panic, a watchdog or the
// supply sagging, and those have nothing in common but the symptom (#82).
const char *resetToken(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

void sayHello() {
  String f = String(",\"device_id\":\"") + esc(deviceId_) + "\"";
  f += String(",\"fw_version\":\"") + ZW_FW_VERSION + "\"";
  f += String(",\"git_hash\":\"") + ZW_GIT_HASH + "\"";
  f += String(",\"build_date\":\"") + ZW_BUILD_DATE + "\"";
  f += String(",\"repo\":\"") + ZW_GIT_REPO + "\"";
  // How long this image had been running when the session started. There is no
  // clock on the board, so this is the closest it can come to saying when it
  // was written: a session that starts at nought was a fresh boot.
  f += ",\"uptime_s\":" + String((uint32_t)(millis() / 1000));
  f += String(",\"sensor\":\"") + cfg_.sensor + "\"";
  f += String(",\"reason\":\"") + cfg_.reason + "\"";
  // Only on the session a boot started: the chip's reset reason stays what it
  // was until the next reset, and on a mode switch it would read as news.
  if (cfg_.reason && strcmp(cfg_.reason, "boot") == 0)
    f += String(",\"reset\":\"") + resetToken(esp_reset_reason()) + "\"";
  f += ",\"params\":" + (parameters_ ? parameters_() : cfg_.paramsJson);
  uint32_t s = seq_++;
  emit(s, head("session", s, (uint32_t)micros()) + f + "}");
}

void begin(const Config &cfg) {
  deviceId_ = cfg.deviceId;
  cfg_ = cfg;
  parameters_ = cfg.parameters;

  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  host_ = prefs.isKey(KEY_HOST) ? prefs.getString(KEY_HOST, "") : String();
  port_ = prefs.isKey(KEY_PORT) ? prefs.getUShort(KEY_PORT, 9000) : 9000;
  on_ = prefs.isKey(KEY_ON) ? prefs.getBool(KEY_ON, true) : true;
  prefs.end();

  // Built from the chip and the boot time, so two sessions from one board are
  // still distinguishable after a restart.
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(sessionId_, sizeof(sessionId_), "%02x%02x%04x", mac[4], mac[5],
           (uint16_t)(esp_random() & 0xffff));

  queue_ = xQueueCreate(QUEUE_DEPTH, sizeof(Hit));
  // A fixed local port, so the sink can answer without being told one.
  udp.begin(ZW_DIAG_PORT);

  // Three separate things, said separately. Printing sendable() as "on"/"off"
  // read as "recording is disabled" when all it meant was that the sink had not
  // answered a ping yet — which it cannot have, a few milliseconds after boot. It
  // cost two wrong conclusions about settings not surviving a restart, when they
  // do: host, port and the switch are all in NVS.
  Serial.printf("[diag] session %s, sink %s:%u, recording %s, sink %s\n",
                sessionId_, host_.length() ? host_.c_str() : "(none)", port_,
                enabled() ? "on" : "off",
                !host_.length() ? "not configured"
                                : (sinkAlive() ? "answering" : "not answering yet"));

  // The only place the build and the parameters are stated in full. Everything
  // downstream is read against it.
  sayHello();
}

void restate() { sayHello(); }

void newSession(const char *sensor, const char *reason) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(sessionId_, sizeof(sessionId_), "%02x%02x%04x", mac[4], mac[5],
           (uint16_t)(esp_random() & 0xffff));
  seq_ = 0;
  cfg_.sensor = sensor;
  cfg_.reason = reason;
  Serial.printf("[diag] new session %s (%s, %s)\n", sessionId_, sensor, reason);
  sayHello();
}

// Sends a few of the held events per call rather than all of them at once: a
// hundred datagrams into a socket buffer in one go is how a flush turns into
// the loss it was meant to prevent.
void nachschicken() {
  if (!sendable() || gehalten_ == 0) return;

  if (haltenSeitMs_) {
    const uint32_t wie_lange = (millis() - haltenSeitMs_) / 1000;
    haltenSeitMs_ = 0;
    // Said in the file, so nobody reads a flush as a burst of play. Sent first,
    // before the events it describes.
    note("info", String("sending ") + gehalten_ + " events held for " +
                     wie_lange + "s with no sink listening");
  }

  int wie_viele = 4;
  size_t ab = 0;
  int i = 0;
  for (; i < gehalten_ && wie_viele > 0; i++, wie_viele--) {
    String p;
    p.concat((const char *)(arena_ + ab), laengen_[i]);
    transmit(p);
    ab += laengen_[i];
  }
  memmove(arena_, arena_ + ab, genutzt_ - ab);
  memmove(laengen_, laengen_ + i, sizeof(uint16_t) * (gehalten_ - i));
  genutzt_ -= ab;
  gehalten_ -= i;
}

// The hello goes out whatever the buffer is doing: it is how the sink learns
// where to answer, and holding it would be a deadlock.
void hallo() {
  if (!on_ || host_.length() == 0) return;
  if (zuletztGefragt_ && millis() - zuletztGefragt_ < HALLO_MS) return;
  zuletztGefragt_ = millis();
  transmit(String("{\"v\":1,\"type\":\"ping\",\"session_id\":\"") + sessionId_ +
           "\",\"held\":" + gehalten_ + "}");
}

void lauschen() {
  // Anything arriving on our port counts as an answer. There is nothing else
  // that would send to it, and parsing it would buy nothing.
  int n = udp.parsePacket();
  while (n > 0) {
    uint8_t weg[64];
    while (udp.available() > 0) udp.read(weg, sizeof(weg));
    zuletztGehoert_ = millis();
    n = udp.parsePacket();
  }
}

void tick() {
  lauschen();
  hallo();
  nachschicken();
  if (!queue_) return;
  static Hit h;
  while (xQueueReceive(queue_, &h, 0) == pdTRUE) emitHit(h);

  static uint32_t reported = 0;
  if (dropped_ != reported) {
    uint32_t lost = dropped_ - reported;
    reported = dropped_;
    note("warn", String("log queue full, ") + lost + " events dropped");
  }
}

void hit(const Hit &h) {
  if (!queue_) return;
  // Never blocks. A full queue costs the event, not the detection.
  if (xQueueSend(queue_, &h, 0) != pdTRUE) dropped_++;
}

void rallyStart(uint32_t rallyId) {
  emitNow("rally", String(",\"rally_id\":\"") + sessionId_ + "-r" + rallyId +
                       "\",\"phase\":\"start\"");
}

void rallyEnd(uint32_t rallyId, const String &sequence, const char *closedBy, char serve,
              bool onServe, char serveFault) {
  String f = String(",\"rally_id\":\"") + sessionId_ + "-r" + rallyId +
             "\",\"phase\":\"end\",\"sequence\":\"" + esc(sequence) +
             "\",\"closed_by\":\"" + closedBy + "\"";
  if (serve == 'A' || serve == 'B') {
    f += String(",\"serve\":\"") + serve + "\",\"on_serve\":" + (onServe ? "true" : "false");
    if (serveFault == 'A' || serveFault == 'B') f += String(",\"serve_fault\":\"") + serveFault + "\"";
  }
  emitNow("rally", f);
}

void point(uint32_t pointId, uint32_t rallyId, const String &reason,
           const String &hint, char side, int fromA, int fromB, char fromServe,
           int toA, int toB, char toServe, bool over,
           const String &tag, const String &note,
           const String &player, const String &playerName) {
  String f = String(",\"point_id\":\"") + sessionId_ + "-p" + pointId + "\"";
  f += String(",\"rally_id\":\"") + sessionId_ + "-r" + rallyId + "\"";
  f += ",\"reason\":\"" + esc(reason) + "\"";
  f += ",\"hint\":\"" + esc(hint) + "\"";
  f += String(",\"side\":\"") + side + "\"";
  f += ",\"from\":{\"a\":" + String(fromA) + ",\"b\":" + String(fromB) +
       ",\"serve\":\"" + String(fromServe) + "\"}";
  f += ",\"to\":{\"a\":" + String(toA) + ",\"b\":" + String(toB) + ",\"serve\":\"" +
       String(toServe) + "\",\"over\":" + (over ? "true" : "false") + "}";
  if (player.length()) f += ",\"player\":\"" + esc(player) + "\"";
  if (playerName.length()) f += ",\"player_name\":\"" + esc(playerName) + "\"";
  if (tag.length()) f += ",\"tag\":\"" + esc(tag) + "\"";
  if (note.length()) f += ",\"note\":\"" + esc(note) + "\"";
  emitNow("point", f);
}

void match(const char *phase, const String &nameA, const String &nameB,
           int sideAPlayer, int sideBPlayer, int setNumber) {
  String f = String(",\"phase\":\"") + phase + "\"";
  f += ",\"players\":{\"a\":\"" + esc(nameA) + "\",\"b\":\"" + esc(nameB) + "\"}";
  f += String(",\"sides\":{\"A\":\"") + (sideAPlayer == 0 ? "a" : "b") +
       "\",\"B\":\"" + (sideBPlayer == 0 ? "a" : "b") + "\"}";
  f += ",\"set_number\":" + String(setNumber);
  if (matchId_.length()) f += ",\"match_id\":\"" + esc(matchId_) + "\"";
  emitNow("match", f);
}

void setMatchId(const char *matchId) { matchId_ = matchId ? matchId : ""; }

void ingest(const char *status, const char *reason, const char *action, uint32_t eventId,
            char half, int delta, const char *matchId, const char *source, int http,
            uint16_t attempt, uint32_t queuedMs, uint32_t waitMs, uint32_t pointId,
            uint32_t rallyId) {
  String f = String(",\"status\":\"") + status + "\"";
  if (reason && reason[0]) f += String(",\"reason\":\"") + reason + "\"";
  f += String(",\"action\":\"") + action + "\"";
  if (pointId) f += String(",\"point_id\":\"") + sessionId_ + "-p" + pointId + "\"";
  if (rallyId) f += String(",\"rally_id\":\"") + sessionId_ + "-r" + rallyId + "\"";
  if (matchId && matchId[0]) f += String(",\"match_id\":\"") + esc(matchId) + "\"";
  f += String(",\"source\":\"") + esc(source) + "\"";
  if (half == 'A' || half == 'B') {
    f += String(",\"side\":\"") + half + "\",\"half\":\"" + (half == 'A' ? "left" : "right") + "\"";
    f += String(",\"delta\":") + delta;
  }
  if (eventId) f += String(",\"event_id\":") + eventId;
  if (http) f += String(",\"http\":") + http;
  if (attempt) f += String(",\"attempt\":") + attempt;
  f += String(",\"queued_ms\":") + queuedMs;
  if (waitMs) f += String(",\"retry_in_ms\":") + waitMs;
  emitNow("ingest", f);
}

void link(const char *phase, const char *reason, const char *matchId, int http, uint32_t tries,
          const String &extra) {
  String f = String(",\"phase\":\"") + phase + "\"";
  if (reason && reason[0]) f += String(",\"reason\":\"") + reason + "\"";
  if (matchId && matchId[0]) f += String(",\"match_id\":\"") + esc(matchId) + "\"";
  if (http) f += String(",\"http\":") + http;
  if (tries) f += String(",\"tries\":") + tries;
  f += extra;
  emitNow("link", f);
}

void button(const char *source, const char *gesture, char side, uint32_t heldMs,
            const char *action, const char *reason, uint32_t pointId, uint32_t rallyId) {
  String f = String(",\"source\":\"") + source + "\",\"gesture\":\"" + gesture + "\"";
  f += ",\"side\":";
  if (side == 'A' || side == 'B') f += String("\"") + side + "\"";
  else f += "null";
  f += ",\"held_ms\":" + String(heldMs);
  f += String(",\"action\":\"") + action + "\"";
  if (reason && *reason) f += String(",\"reason\":\"") + reason + "\"";
  if (pointId) f += String(",\"point_id\":\"") + sessionId_ + "-p" + pointId + "\"";
  f += String(",\"rally_id\":\"") + sessionId_ + "-r" + rallyId + "\"";
  emitNow("button", f);
}

void checkin(char half, const char *outcome, const char *source, uint32_t eventId,
             int http, uint8_t attempts) {
  String f = String(",\"outcome\":\"") + outcome + "\"";
  if (half == 'A' || half == 'B')
    f += String(",\"side\":\"") + half + "\",\"half\":\"" +
         (half == 'A' ? "left" : "right") + "\"";
  if (eventId) {
    f += String(",\"source\":\"") + esc(source) + "\",\"event_id\":" + eventId;
    f += String(",\"http\":") + http + ",\"attempts\":" + attempts;
  }
  emitNow("checkin", f);
}

void param(const char *name, const String &from, const String &to, const char *by) {
  emitNow("param", String(",\"name\":\"") + name + "\",\"from\":\"" + esc(from) +
                       "\",\"to\":\"" + esc(to) + "\",\"by\":\"" + by + "\"");
}

void note(const char *level, const String &text) {
  emitNow("note", String(",\"level\":\"") + level + "\",\"text\":\"" + esc(text) + "\"");
}

void mark(const String &tag, const String &text, uint32_t rallyId) {
  String f = String(",\"level\":\"note\",\"text\":\"") + esc(text) + "\"";
  if (tag.length()) f += ",\"tag\":\"" + esc(tag) + "\"";
  f += String(",\"rally_id\":\"") + sessionId_ + "-r" + rallyId + "\"";
  emitNow("note", f);
}

void setSink(const String &host, uint16_t port) {
  host_ = host;
  port_ = port;
  if (sendable()) restate();
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(KEY_HOST, host);
  prefs.putUShort(KEY_PORT, port);
  prefs.end();
}

void setEnabled(bool on) {
  const bool war = sendable();
  on_ = on;
  if (sendable() && !war) restate();
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putBool(KEY_ON, on);
  prefs.end();
}

bool enabled() { return on_ && host_.length() > 0; }
bool sinkAlive() { return sendable(); }
const String &sinkHost() { return host_; }
uint16_t sinkPort() { return port_; }
const char *sessionId() { return sessionId_; }
uint32_t droppedEvents() { return dropped_ + verworfen_; }

int heldEvents() { return gehalten_; }

}  // namespace diag
