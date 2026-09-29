#include "diag.h"

#include <Preferences.h>
#include <WiFiUdp.h>
#include <esp_mac.h>
#include <esp_random.h>

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

char sessionId_[9] = "00000000";
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

bool sendable() { return on_ && host_.length() > 0; }

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

void sayHello() {
  String f = String(",\"device_id\":\"") + esc(deviceId_) + "\"";
  f += String(",\"fw_version\":\"") + ZW_FW_VERSION + "\"";
  f += String(",\"git_hash\":\"") + ZW_GIT_HASH + "\"";
  f += String(",\"sensor\":\"") + cfg_.sensor + "\"";
  f += String(",\"reason\":\"") + cfg_.reason + "\"";
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
  udp.begin(0);

  Serial.printf("[diag] session %s, sink %s:%u, %s\n", sessionId_,
                host_.length() ? host_.c_str() : "(none)", port_,
                sendable() ? "on" : "off");

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

void tick() {
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

void rallyEnd(uint32_t rallyId, const String &sequence, const char *closedBy) {
  emitNow("rally", String(",\"rally_id\":\"") + sessionId_ + "-r" + rallyId +
                       "\",\"phase\":\"end\",\"sequence\":\"" + esc(sequence) +
                       "\",\"closed_by\":\"" + closedBy + "\"");
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
  emitNow("match", f);
}

void param(const char *name, const String &from, const String &to, const char *by) {
  emitNow("param", String(",\"name\":\"") + name + "\",\"from\":\"" + esc(from) +
                       "\",\"to\":\"" + esc(to) + "\",\"by\":\"" + by + "\"");
}

void note(const char *level, const String &text) {
  emitNow("note", String(",\"level\":\"") + level + "\",\"text\":\"" + esc(text) + "\"");
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

bool enabled() { return sendable(); }
const String &sinkHost() { return host_; }
uint16_t sinkPort() { return port_; }
const char *sessionId() { return sessionId_; }
uint32_t droppedEvents() { return dropped_ + verworfen_; }

int heldEvents() { return gehalten_; }

}  // namespace diag
