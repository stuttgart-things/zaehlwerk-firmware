#pragma once

#include <Arduino.h>

// The diagnostic log: every threshold crossing, every scoring decision, and the
// reason for each — sent to a sink over UDP as JSON. See ADR-0004 and
// docs/event-schema.md.
//
// Two rules hold everything else up:
//
//   Detection is never delayed by this. Emitters only enqueue; serialising and
//   sending happen on the other core. A full queue drops the oldest and says so
//   rather than blocking the sampler.
//
//   Discarded crossings are logged too. They are the half the scoreboard never
//   mentions and the reason any of this exists.
namespace diag {

// Why a crossing did or did not become a hit. Protocol tokens, matched by the
// sink — not prose.
enum class Decision { Counted, BelowThreshold, Deadtime, Ambiguous };
const char *decisionName(Decision d);

// One raw sample pair. dtUs is microseconds since the start of the capture, so
// an irregular sample rate stays readable — which it is until #10 lands.
struct Sample {
  uint16_t dtUs;
  int16_t a;
  int16_t b;
};

// Capacities. The pre-trigger holds what the sampler managed to take before the
// crossing; at one sample per millisecond that is 64 milliseconds of very
// little, and with #10 it becomes 64 samples of something.
const size_t PRE_SAMPLES = 64;
const size_t CAPTURE_SAMPLES = 256;

struct Hit {
  uint32_t rallyId;
  char side;  // 'A', 'B', or ' ' when undecided
  Decision decision;
  int peakA, peakB;
  int baselineA, baselineB;
  int32_t crossAUs, crossBUs;  // relative to the first crossing of either
  float ratio;
  // Whether the crossing actually scored. Separate from `decision`, because an
  // ambiguous one still counts today the way it always has — changing that
  // belongs in the change that fixes detection, not in the one that measures it.
  bool counted;
  // What a generated hit was meant to be. Empty on a real one. This is what
  // lets the sink score detection without anybody labelling anything.
  char intendedSide;      // 'A', 'B' or 0
  const char *intendedType;  // "bounce" | "weak" | "ghost" | "net" | 0
  uint32_t tUs;
  uint16_t sampleCount;
  uint32_t preUs;
  Sample samples[CAPTURE_SAMPLES];
};

struct Config {
  const char *deviceId;
  const char *sensor;    // "adc" | "mock"
  String paramsJson;     // the whole parameter set, as the session event carries it
  const char *reason;    // "boot" | "mode_switch" | "ota" | "manual"
  // Read again whenever the session is restated, so a file opened after a knob
  // was turned carries the values as they stand rather than as they booted.
  String (*parameters)();
};

void begin(const Config &cfg);

// Emits a fresh session event. Called when the sink is pointed somewhere new or
// logging is switched on: a file that starts mid-stream would otherwise carry
// no parameters at all, and a session with no parameters cannot be compared
// with another one.
void restate();

// A new session id and a fresh session event. Switching the sensor source ends
// the run that was going and starts another: mixing readings from a generator
// and from a table in one file would make every number in it unreadable.
void newSession(const char *sensor, const char *reason);

// Drains the queue and sends. Belongs next to the web server, never in the
// sampler.
void tick();

// Safe from the sampler: copies into a queue and returns.
void hit(const Hit &h);

// These run on the scoring side, which is already off the sampler.
void rallyStart(uint32_t rallyId);
void rallyEnd(uint32_t rallyId, const String &sequence, const char *closedBy);
// tag and note are what a person said at the moment of a correction. The tag
// comes from a fixed vocabulary so the sink can group by it — free text does
// not cluster, and finding what a game had in common is the whole reason for
// asking. The note is for the one case the vocabulary does not cover.
void point(uint32_t pointId, uint32_t rallyId, const String &reason,
           const String &hint, char side, int fromA, int fromB, char fromServe,
           int toA, int toB, char toServe, bool over,
           const String &tag, const String &note,
           const String &player, const String &playerName);

// Players, ends and sets. phase is "start", "players", "ends_swapped" or "end".
// `sides` is the mapping in force from this event onwards, which is what makes
// every later point resolvable.
void match(const char *phase, const String &nameA, const String &nameB,
           int sideAPlayer, int sideBPlayer, int setNumber);
void param(const char *name, const String &from, const String &to, const char *by);
void note(const char *level, const String &text);

// Settings, stored in NVS.
void setSink(const String &host, uint16_t port);
void setEnabled(bool on);
bool enabled();
const String &sinkHost();
uint16_t sinkPort();

const char *sessionId();
uint32_t droppedEvents();

}  // namespace diag
