#include "buttons.h"

namespace buttons {

const char *gestureName(Gesture g) {
  switch (g) {
    case Gesture::Short: return "short";
    case Gesture::Long:  return "long";
    default:             return "very_long";
  }
}

const char *sourceName(Source s) {
  switch (s) {
    case Source::Virtual: return "virtual";
    case Source::EspNow:  return "espnow";
    default:              return "wired";
  }
}

const char *actionName(Action a) {
  switch (a) {
    case Action::Point:     return "point";
    case Action::StartGame: return "start_game";
    case Action::Undo:      return "undo";
    case Action::NewGame:   return "new_game";
    default:                return "refused";
  }
}

// Below 200 ms a long press is a tap held a moment too long, and a very long
// press beyond ten seconds is one nobody keeps a finger on. The gap between the
// two keeps them apart for a person, not just for the comparison.
bool valid(const Timing &t) {
  return t.longMs >= 200 && t.veryLongMs <= 10000 && t.veryLongMs >= t.longMs + 500;
}

Gesture classify(uint32_t heldMs, const Timing &t) {
  if (heldMs >= t.veryLongMs) return Gesture::VeryLong;
  if (heldMs >= t.longMs) return Gesture::Long;
  return Gesture::Short;
}

Decision decide(const Event &e, const Board &b) {
  switch (e.gesture) {
    case Gesture::Short:
      if (e.side != 'A' && e.side != 'B') return {Action::Refused, "no_side"};
      if (!b.gameOver) return {Action::Point, ""};
      // A point after the game is won has nowhere to go. Unlinked it is the
      // natural way to begin the next one; linked, the next game is zaehlwerk's.
      if (b.linked) return {Action::Refused, "next_game_on_zaehlwerk"};
      return {Action::StartGame, ""};

    case Gesture::Long:
      // Also after the game is won: taking back the match point is exactly
      // what somebody who just pressed one point too many reaches for.
      return {Action::Undo, ""};

    default:
      if (b.linked) return {Action::Refused, "next_game_on_zaehlwerk"};
      return {Action::NewGame, ""};
  }
}

}  // namespace buttons
