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
};

// Reads a bounce sequence such as "ABAB": who gets the point, and what makes
// the decision uncertain?
Urteil rallyBewerten(const std::string &folge, char aufschlag);

}  // namespace game
