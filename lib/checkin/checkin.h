#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

// Check-in with a key fob, the rules (#63, ADR-0007). A fob is read at the hub,
// the next press of a paired button names the half, and that press is a
// check-in rather than a point.
//
// No Arduino underneath. The reader, the LED pins, the HTTP client and the clock
// belong to src/fob.cpp; what counts, for how long, and what an answer means are
// here, where `pio test -e native` reaches them.
//
// A UID is a card number, and a company badge held up by mistake produces one
// too. So it is a thing this code holds and hands on, never a thing it says:
// the only function that writes it out is requestBody(), and nothing that
// builds a log line takes it as an argument.
namespace checkin {

// A UID as the reader hands it over: 4, 7 or 10 bytes. Lives in RAM, for the
// window and for the one request, and is zeroed when it goes rather than merely
// marked empty.
struct Uid {
  uint8_t bytes[10];
  uint8_t size;  // 0: none

  void clear();
  bool empty() const { return size == 0; }
};

// The table half in the request. Half A is "left", which is player a in
// zaehlwerk (its ADR-0010): the physical half, not the player the firmware's
// own mapping resolves it to today (ADR-0006). nullptr for anything but 'A'/'B'.
const char *halfToken(char half);

// What the LED says. One state at a time; lights() turns it into pins.
enum class Signal {
  Off,
  Waiting,      // a fob was read, press your button
  Sending,      // the press was taken, the request is out
  CheckedIn,
  Unknown,      // the fob belongs to nobody
  Rejected,     // a match is running, here or at zaehlwerk
  Unavailable,  // zaehlwerk or Schmetterpause did not answer, or check-in is off
  Expired,      // nobody pressed within the window
};
const char *signalName(Signal s);

struct Lights {
  bool green;
  bool red;
};

// The pattern of a signal, `ms` after it started. Pure, so the patterns can be
// read in a test rather than at a table with a stopwatch.
Lights lights(Signal s, uint32_t ms);

// What is sent: the ingest contract of zaehlwerk ADR-0002, a half and the UID.
struct Request {
  uint32_t eventId;
  char half;  // 'A' | 'B'
  Uid uid;
};

// The body of POST /ingest/checkin. The one place the UID is spelled out, upper
// case with colons, the way zaehlwerk normalises it.
std::string requestBody(const char *source, const Request &r);

// Statuses that are not HTTP: the request never reached anybody.
const int NO_WIFI = -2;    // the board is not on a network that leads anywhere
const int NO_ANSWER = -1;  // on a network, but no answer: refused, timed out

// Whether to send the same request again. Only when nobody answered: an answer,
// whatever it says, is what a retry would get back again (ADR-0002), and a 503
// from zaehlwerk is already its final word on this event.
bool worthRetrying(int status);

// An answer, read. `outcome` is a protocol token from a fixed list, never text
// copied out of the body, so nothing the server says ends up in a log verbatim.
struct Verdict {
  const char *outcome;
  Signal signal;
};
Verdict judge(int status, const std::string &body);

// The line for serial. Takes no UID, by construction: there is nothing to leak.
std::string logLine(char half, const char *outcome, uint32_t eventId, int status);

// The window between a fob and a press, and what the LED shows around it.
//
// Not thread-safe; the firmware holds a lock around it. Times are millis(),
// compared by difference so the wrap after 49 days is harmless.
class Window {
 public:
  explicit Window(uint32_t windowMs, uint32_t showMs = 2000);

  // The last event id used, from NVS, so a restart carries on above it.
  void seed(uint32_t lastEventId) { lastEventId_ = lastEventId; }
  uint32_t lastEventId() const { return lastEventId_; }

  // A fob was read. `busy`: the firmware knows play is going on, so a fob does
  // nothing and the next press stays a point. `enabled`: there is a zaehlwerk
  // to ask at all. A fob while one is already waiting replaces it and starts
  // the window again; a fob while a request is out is ignored.
  void fob(const Uid &uid, bool busy, bool enabled, uint32_t now);

  // A press of a paired button. true: it is a check-in, the request is ready in
  // take(), and the caller must not score it. false: it is whatever it would
  // have been without a fob.
  bool press(char half, uint32_t now);

  // The window ran out: the UID is dropped. true once, on the call where it
  // happens, so the caller can say so in the log.
  bool expire(uint32_t now);

  bool ready() const { return state_ == State::Ready; }
  // Hands the request over and keeps no copy of the UID.
  Request take();
  // What came back. The UID is already gone by now.
  void answered(Signal s, uint32_t now);

  bool waiting() const { return state_ == State::Waiting; }
  Signal signal(uint32_t now) const;
  // When the current signal started, for lights().
  uint32_t since() const { return since_; }

 private:
  enum class State { Idle, Waiting, Ready, Sending, Showing };

  void show(Signal s, uint32_t now);

  uint32_t windowMs_;
  uint32_t showMs_;
  State state_ = State::Idle;
  Signal shown_ = Signal::Off;
  uint32_t since_ = 0;
  uint32_t lastEventId_ = 0;
  Uid uid_{};
  Request request_{};
};

}  // namespace checkin
