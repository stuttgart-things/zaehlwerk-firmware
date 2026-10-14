#include "checkin.h"

#include <stdio.h>
#include <string.h>

namespace checkin {

void Uid::clear() {
  // Zeroed, not only marked empty: a freed UID should not sit in RAM waiting
  // for whatever reads the stack next.
  memset(bytes, 0, sizeof(bytes));
  size = 0;
}

const char *halfToken(char half) {
  if (half == 'A') return "left";
  if (half == 'B') return "right";
  return nullptr;
}

const char *signalName(Signal s) {
  switch (s) {
    case Signal::Off:         return "off";
    case Signal::Waiting:     return "waiting";
    case Signal::Sending:     return "sending";
    case Signal::CheckedIn:   return "checked_in";
    case Signal::Unknown:     return "unknown";
    case Signal::Rejected:    return "rejected";
    case Signal::Unavailable: return "unavailable";
    case Signal::Expired:     return "expired";
  }
  return "off";
}

namespace {
bool blink(uint32_t ms, uint32_t period) { return (ms / period) % 2 == 0; }
}  // namespace

// Green says go on, red says it did not work. One colour per answer, and a
// steady light for the two that end the matter either way, so somebody who
// only glances at it still reads it right.
Lights lights(Signal s, uint32_t ms) {
  switch (s) {
    case Signal::Off:         return {false, false};
    case Signal::Waiting:     return {blink(ms, 250), false};
    case Signal::Sending:     return {true, true};
    case Signal::CheckedIn:   return {true, false};
    case Signal::Unknown:     return {false, true};
    case Signal::Rejected:    return {false, blink(ms, 250)};
    case Signal::Unavailable: return {false, blink(ms, 100)};
    // Two short red blinks and dark: nothing happened, and nothing went wrong.
    case Signal::Expired:     return {false, ms < 450 && blink(ms, 150)};
  }
  return {false, false};
}

std::string requestBody(const char *source, const Request &r) {
  char uid[3 * sizeof(r.uid.bytes)] = {0};
  size_t n = 0;
  for (uint8_t i = 0; i < r.uid.size && i < sizeof(r.uid.bytes); i++) {
    n += snprintf(uid + n, sizeof(uid) - n, i ? ":%02X" : "%02X", r.uid.bytes[i]);
  }
  const char *half = halfToken(r.half);
  std::string body = "{\"source\":\"";
  body += source;
  body += "\",\"event_id\":";
  body += std::to_string(r.eventId);
  body += ",\"half\":\"";
  body += half ? half : "";
  body += "\",\"uid\":\"";
  body += uid;
  body += "\"}";
  memset(uid, 0, sizeof(uid));
  return body;
}

bool worthRetrying(int status) { return status == NO_ANSWER; }

bool tokenUsable(const std::string &token) {
  if (token.empty() || token.size() > TOKEN_MAX) return false;
  for (const char c : token)
    if (c < 0x21 || c > 0x7E) return false;
  return true;
}

std::string authorization(const std::string &token) { return "Bearer " + token; }

Setup setupOf(bool haveApi, bool haveToken) {
  if (!haveApi) return Setup::NoApi;
  if (!haveToken) return Setup::NoToken;
  return Setup::Ready;
}

namespace {

// The outcomes zaehlwerk answers with (its README, "Check-in per half"). Read
// against this list so the token logged is always one of ours.
const char *const OUTCOMES[] = {"claimed", "unchanged", "replaced", "moved", "started",
                                "duplicate", "unknown",   "refused",  "unavailable",
                                "failed"};

const char *outcomeIn(const std::string &body) {
  const std::string key = "\"outcome\":\"";
  const size_t at = body.find(key);
  if (at == std::string::npos) return nullptr;
  const size_t from = at + key.size();
  const size_t to = body.find('"', from);
  if (to == std::string::npos) return nullptr;
  const std::string word = body.substr(from, to - from);
  for (const char *o : OUTCOMES)
    if (word == o) return o;
  return nullptr;
}

}  // namespace

Verdict judge(int status, const std::string &body) {
  // Not sent at all: the token was cleared between the press and the request.
  if (status == NO_TOKEN) return {"no_token", Signal::Rejected};
  if (status == NO_WIFI) return {"no_wifi", Signal::Unavailable};
  if (status < 100) return {"no_answer", Signal::Unavailable};

  const char *said = outcomeIn(body);
  switch (status) {
    case 200:
      // A duplicate is an event id below the last one zaehlwerk saw from this
      // source: nothing changed, so the LED must not say checked in. A retry of
      // the same id is not a duplicate; it gets the first answer again.
      if (said && strcmp(said, "duplicate") == 0) return {said, Signal::Rejected};
      return {said ? said : "ok", Signal::CheckedIn};
    case 401:
    case 403:
      // The token is missing or not zaehlwerk's (#65). zaehlwerk answers before
      // it reads the body, so this says nothing about the fob, and the LED must
      // not say "unknown".
      return {"unauthorized", Signal::Rejected};
    case 404:
      return {"unknown", Signal::Unknown};
    case 409:
      return {"refused", Signal::Rejected};
    case 503:
      return {"unavailable", Signal::Unavailable};
    case 400:
      // A request this firmware built and zaehlwerk would not read. A bug here,
      // not something the player can fix by holding the fob up again.
      return {"bad_request", Signal::Unavailable};
    case 500:
      return {"failed", Signal::Unavailable};
    default:
      return {"http_error", Signal::Unavailable};
  }
}

std::string logLine(char half, const char *outcome, uint32_t eventId, int status) {
  std::string line = "[checkin] ";
  const char *token = halfToken(half);
  if (token) {
    line += "half ";
    line += half;
    line += " (";
    line += token;
    line += ") ";
  }
  line += outcome ? outcome : "?";
  if (eventId) line += " event_id " + std::to_string(eventId);
  if (status) line += " http " + std::to_string(status);
  return line;
}

std::string statusJson(const Status &s) {
  const char *half = halfToken(s.lastHalf);
  std::string j = "{\"on\":";
  j += s.api && s.token ? "true" : "false";
  j += ",\"api\":";
  j += s.api ? "true" : "false";
  j += ",\"token\":";
  j += s.token ? "true" : "false";
  j += ",\"reader\":";
  j += s.reader ? "true" : "false";
  j += ",\"signal\":\"";
  j += signalName(s.signal);
  j += "\",\"window_ms\":" + std::to_string(s.windowMs);
  j += ",\"source\":\"";
  j += s.source ? s.source : "";
  j += "\",\"last\":{\"outcome\":\"";
  j += s.lastOutcome ? s.lastOutcome : "";
  j += "\"";
  if (half) {
    j += ",\"half\":\"";
    j += s.lastHalf;
    j += "\"";
  }
  j += "}}";
  return j;
}

Window::Window(uint32_t windowMs, uint32_t showMs) : windowMs_(windowMs), showMs_(showMs) {}

void Window::show(Signal s, uint32_t now) {
  state_ = State::Showing;
  shown_ = s;
  since_ = now;
}

const char *Window::fob(const Uid &uid, bool busy, Setup setup, uint32_t now) {
  if (state_ == State::Ready || state_ == State::Sending || uid.empty()) return "ignored";
  uid_.clear();
  // Refused before anything is kept: a fob during play is not held for a
  // window, so the press after it is a point like any other.
  if (busy) {
    show(Signal::Rejected, now);
    return "busy";
  }
  if (setup == Setup::NoApi) {
    show(Signal::Unavailable, now);
    return "off";
  }
  // Nothing would be sent, so nothing is held (#65). Rejected, not unknown:
  // the fob may be fine, the board is not allowed to ask.
  if (setup == Setup::NoToken) {
    show(Signal::Rejected, now);
    return "no_token";
  }
  uid_ = uid;
  state_ = State::Waiting;
  since_ = now;
  return "waiting";
}

bool Window::press(char half, uint32_t now) {
  if (state_ != State::Waiting || !halfToken(half)) return false;
  // The window is checked here as well as in expire(): a press that comes in
  // before the next expire() but after the window is not a check-in.
  if (now - since_ >= windowMs_) {
    expire(now);
    return false;
  }
  request_.eventId = ++lastEventId_;
  request_.half = half;
  request_.uid = uid_;
  uid_.clear();
  state_ = State::Ready;
  since_ = now;
  return true;
}

bool Window::expire(uint32_t now) {
  if (state_ != State::Waiting || now - since_ < windowMs_) return false;
  uid_.clear();
  show(Signal::Expired, now);
  return true;
}

Request Window::take() {
  Request r = request_;
  request_.uid.clear();
  state_ = State::Sending;
  return r;
}

void Window::answered(Signal s, uint32_t now) {
  request_.uid.clear();
  show(s, now);
}

Signal Window::signal(uint32_t now) const {
  switch (state_) {
    case State::Idle:    return Signal::Off;
    case State::Waiting: return Signal::Waiting;
    case State::Ready:
    case State::Sending: return Signal::Sending;
    case State::Showing: return now - since_ < showMs_ ? shown_ : Signal::Off;
  }
  return Signal::Off;
}

}  // namespace checkin
