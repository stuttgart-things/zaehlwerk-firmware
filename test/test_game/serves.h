#pragma once

// Serves with what happened at the table, for the replay in test_game.cpp
// (#83). Each row is one rally: its sequence, who served, whether it was the
// serve (the first rally since the score last moved), and the point it should
// have given, ' ' for none (a let, a ball picked up).
//
// The rows below are the shapes from session 143424-e32c611b, written out by
// hand, not a labelled round. The labelled round of #83 (10-15 of each kind,
// marked on /mark with a note) replaces them: every rally_end since #83 carries
// `serve` and `on_serve`, so a row is the sequence, those two and the label.
struct Aufschlag {
  const char *folge;
  char aufschlag;
  bool aufschlagRally;
  char richtig;
  const char *was;  // what happened, as the note on /mark would say it
};

static const Aufschlag AUFSCHLAEGE[] = {
    // Into the net or off the table: one bounce on the server's half.
    {"A", 'A', true, 'B', "serve into the net"},
    {"B", 'B', true, 'A', "serve into the net"},
    {"A", 'A', true, 'B', "serve off the table"},
    // Straight onto the other half.
    {"B", 'A', true, 'B', "serve straight across"},
    {"BA", 'A', true, 'B', "serve straight across, returned"},
    {"ABAB", 'B', true, 'A', "serve straight across, played on"},
    // The let: the same two bounces as an ace. Neither rule may touch it.
    {"AB", 'A', true, ' ', "let"},
    {"AB", 'A', true, 'A', "ace"},
    // A lone bounce that is not the serve: the ball settling or picked up after
    // a rally that awarded nothing. The reason single_bounce stopped awarding.
    {"A", 'A', false, ' ', "ball picked up"},
    {"B", 'A', false, ' ', "ball picked up"},
    // An ordinary rally from a good serve.
    {"ABAB", 'A', true, 'A', "rally"},
};
