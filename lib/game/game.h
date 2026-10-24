#pragma once

#include <string>

// The rules of the Zählwerk, with no Arduino underneath.
//
// Everything here is arithmetic on numbers and a bounce sequence, so it can be
// checked with `pio test -e native` on a laptop. Changing something here
// changes the score — not the display.
//
// The identifiers stay German, and so do the hint strings: those are shown in
// the web UI, which a player reads.
namespace game {

// The other table half.
char andere(char seite);

// Who serves at this score. From 10:10 the serve changes after every point,
// before that after every second one.
char aufschlagFuer(int a, int b, char erster);

// A set is over at eleven points with two clear.
bool beendet(int a, int b);

// The verdict on a finished rally.
struct Urteil {
  char gewinner;
  std::string hinweis;  // empty when the sequence was unambiguous; German, shown in the UI
  // Which rule decided. A protocol token for the log, not prose: the hint says
  // what to tell a person, this says what to count.
  const char *grund;    // "last_bounce" | "single_bounce" | "double_bounce" |
                        // "serve_unclear" | "serve_fault" | "none"
  // gewinner is ' ' when no point is awarded: the sequence was recorded but the
  // table could not tell what happened. That is a verdict, not a failure.
};

// Reads a bounce sequence such as "ABAB": who gets the point, and what makes
// the decision uncertain?
Urteil rallyBewerten(const std::string &folge, char aufschlag);

// The two faulty serves of #83 that the sequence and the server can tell apart,
// asked separately and deciding nothing yet. rallyBewerten() does not call this:
// both rules were switched off once already, bundled with other changes, and
// they come into the score only once a labelled round at the table says they
// are better. Until then the board logs what this would have said, next to what
// it did.
//
//   - One bounce, on the server's half, and nothing after it: the serve did not
//     cross. Only on the serve, because a lone bounce at any other time is the
//     ball settling or being picked up (see single_bounce).
//   - First bounce on the receiver's half: the server missed their own half,
//     whatever came after.
//
// aufschlagRally says the rally is the serve: the first since the score last
// moved. grund is "serve_fault" and gewinner the receiver when one of them
// applies; otherwise gewinner is ' ' and grund "none". The let, a serve that
// clips the net and lands right, is two bounces like an ace and stays a
// correction.
Urteil aufschlagPruefen(const std::string &folge, char aufschlag, bool aufschlagRally);

}  // namespace game
