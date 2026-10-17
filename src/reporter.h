#pragma once

#include <WString.h>
#include <stdint.h>

#include "report.h"

// The board's link to zaehlwerk-api (#17, #41, #66, #67, #69, ADR-0008): joins
// the running match, sends every rally verdict, point given by hand and undo
// through POST /ingest/piezo, and hands zaehlwerk's score back to the board.
//
// Opt-in. Without an address none of this does anything: no task is started,
// nothing is sent, nothing is logged, and every call below returns at once.
//
// The rules are lib/report. This file is the task that does the HTTP, on the
// core that does not sample (ADR-0003), and the hand-over to loop(): the diag
// log and the board's game variables are only ever touched from there.
namespace reporter {

// Reads the address and the counter, and starts the task if there is an
// address. After net::begin().
void begin();

// From loop(). Writes what the link has to say into the diag log, and hands a
// score zaehlwerk answered to `apply`, which sets the board's variables. Not
// in mock mode: a simulated game keeps its own score, as it sends nothing.
void tick(void (*apply)(const report::State &), bool mock);

// A finished rally, as lib/game judged it. `winner` is ' ' for a rally that
// awarded nothing, which goes as delta 0. `pointId` and `rallyId` are the
// board's own, for the log. Nothing is sent in mock mode: made-up hits do not
// belong in somebody's match.
void rally(char winner, const String &sequence, uint32_t pointId, uint32_t rallyId, bool mock);
// A point given by hand on the page.
void manualPoint(char half, uint32_t pointId, bool mock);
// "Letzten zurueck": an undo event with the next id, never POST
// /matches/{id}/undo (#67).
void undo(uint32_t pointId, bool mock);

// GET /zaehlwerk.
String statusJson(bool mock);

// POST /zaehlwerk/url. True if stored (or unchanged); `message` is what the
// page shows either way.
bool setUrl(const String &url, String &message);

}  // namespace reporter
