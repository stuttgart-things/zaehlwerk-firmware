#pragma once

#include <Arduino.h>

// A sensor that is not there: the same detection path fed from a generator
// instead of the ADC, so the counting logic, the log and the sink can all be
// exercised at a desk with no piezo, no table and no ball.
//
// Every generated hit says what it was meant to be, which is what lets the sink
// score detection against it without anybody labelling anything.
namespace mock {

// What to make. weak sits near the threshold on purpose, ghost is noise on one
// channel that nothing caused, net is both halves equally loud — the case the
// ratio exists for and the one that gets the side wrong.
enum class Kind { Bounce, Weak, Ghost, Net };
const char *kindName(Kind k);

void begin(bool on);

bool on();
void setOn(bool on);

// Reading. Returns what the ADC would have returned, had there been one.
int read(bool channelA, uint32_t nowUs);

// Fire one now.
void trigger(Kind k, char side);

// Queue a rule-abiding rally, or keep queueing them.
void playRally();
void setAutoplay(bool on);
bool autoplay();

// Drives the queue. Belongs next to the web server, not in the sampler.
void tick();

// What the hit being generated right now was meant to be, for the log. Side is
// 0 when nothing is in flight.
char intendedSide();
const char *intendedType();

}  // namespace mock
