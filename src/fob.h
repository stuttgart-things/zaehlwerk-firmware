#pragma once

#include <Arduino.h>

// The fob reader at the hub: an RC522, two LEDs, and the check-in it starts
// (#63, ADR-0007). The rules are in lib/checkin; this is the hardware around
// them and the task that runs it.
//
// The task polls the reader, sends the check-in and drives the LEDs, pinned to
// the core that does not sample (ADR-0003). Everything else here is safe to call
// from loop() or from a web handler.
namespace fob {

// Starts the task. After net::begin() and ingest::begin().
void begin();

// A press of a paired button at half 'A' or 'B'. true: a fob was waiting and
// this press is its check-in, so the caller must not score it. false: no fob,
// or the window is over — the press is whatever it would have been.
//
// The ESP-NOW receive path of #58 is meant to call this before it scores a
// press, and only for paired senders. Until it exists, /checkin?press=A on the
// board's page stands in for the button.
bool press(char half);

// Whether play is going on. A fob is then refused at once, rather than held
// for a press that is meant as a point. Set from loop(), read by the task.
void setBusy(bool busy);

// Drains what the task has to say into the diagnostic log and serial. The diag
// emitters are not safe from another task, so the task queues and loop() sends.
// Belongs next to diag::tick().
void tick();

// For /checkin: whether check-in is on, whether there is an API and a token,
// whether a reader answers, what the LED shows and what the last check-in came
// to. Never the UID, and never the token, only whether one is set.
String stateJson();

}  // namespace fob
