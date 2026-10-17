#pragma once

#include <stdint.h>

// One way in for every button, whatever it is made of (#56).
//
// A press arrives as an Event — which half, which gesture, from where — and
// decide() says what it does to the game. The scoring code consumes only that
// and never learns whether the press came from the board's page, a radio
// button (#58) or a wire. A fourth kind of button is a new Source and nothing
// else.
//
// No Arduino underneath, so `pio test -e native` reaches all of it.
namespace buttons {

// What a press was. The button that is pressed decides this, because only it
// sees both ends of the press (#2): a gesture rebuilt from two timestamps sent
// over a radio that drops frames would be guessed at the far end. The page is
// a button too and is timed with classify() below.
//
// A new game is a very long press on either half, not both halves held long:
// pairing two long presses would hold every undo back by the pairing window
// (#56). With one button deciding alone, an undo acts the moment it is let go.
enum class Gesture : uint8_t { Short, Long, VeryLong };

enum class Source : uint8_t { Virtual, EspNow, Wired };

const char *gestureName(Gesture g);  // short | long | very_long
const char *sourceName(Source s);    // virtual | espnow | wired

// Where long and very long begin, in milliseconds held. Adjustable on the
// board's page; the defaults are the ones the button mock started from.
struct Timing {
  uint32_t longMs = 1000;
  uint32_t veryLongMs = 3000;
};

// A timing that cannot be pressed is refused rather than stored: long has to be
// longer than a tap and very long has to be reachable after long.
bool valid(const Timing &t);

Gesture classify(uint32_t heldMs, const Timing &t);

struct Event {
  // 'A' or 'B', the table half the button belongs to. Only a point needs one;
  // the page's "take back" and "new game" belong to no half and send ' '.
  char side;
  Gesture gesture;
  Source source;
  uint32_t heldMs;  // as measured, for the log; 0 when the button only sent the gesture
};

enum class Action : uint8_t {
  Point,      // a point for this half
  StartGame,  // a short press after the game is over starts the next one
  Undo,       // take back the last point
  NewGame,    // start over
  Refused,    // nothing happens; `reason` says why
};

const char *actionName(Action a);  // point | start_game | undo | new_game | refused

// What the board is doing, as far as a button is concerned.
struct Board {
  bool gameOver;  // the game is won and no new one has started
  // Joined to a match at zaehlwerk-api (#71). The score is zaehlwerk's then,
  // and zaehlwerk has no door for a table to start a new game — ending the
  // match is the nearest, and that cannot be taken back. So while linked a
  // button never starts a game; the next one starts on zaehlwerk.
  bool linked;
};

struct Decision {
  Action action;
  const char *reason;  // empty unless refused
};

Decision decide(const Event &e, const Board &b);

}  // namespace buttons
