# 6. The firmware owns the table half to player mapping

Status: Proposed
Date: 2026-09-29

## Context

A piezo knows which half of the table it is glued under. It cannot know who is
standing there, and between sets the players change ends, so the answer moves
while the sensor does not.

`zaehlwerk-api` does not resolve this either, deliberately. Its `player` is a
player: `a` and `b` are the two competitors, the names sit on the match as
`Players[2]` from `POST /matches`, and the scorer stays rule-only — it has no
concept of ends being changed, because which side of a table somebody stands on
is not something set logic can have an opinion about
([zaehlwerk ADR-0001](https://github.com/stuttgart-things/zaehlwerk/blob/main/docs/adr/0001-independent-scorekeeping-api.md)).

So something has to hold the mapping, and there is exactly one device that
knows both the half a bounce was on and that the players just swapped ends.

## Decision

The firmware owns it. It keeps a mapping from table half to player — `A → a`,
`B → b` — and inverts it on a change of ends, which is an explicit action in
the web UI and a logged event.

The firmware also creates the match. Names are entered in its web UI, it calls
`POST /matches` with them and keeps the `match_id` for the session. Recently
used names are offered from a short list in NVS.

Names never travel on an ingest event. `POST /ingest/piezo` carries the four
contract fields, with `player` already resolved through the mapping. Names
reach the API once, at `POST /matches`, which is where the API keeps them.

Internally and in the log, a half stays a half. Points, hit events and labels
record `side: "A"` and additionally the `player` and the name it resolved to at
that moment. The display shows the name; the record keeps both.

## Consequences

- A change of ends that nobody presses silently swaps every point afterwards.
  That is the one manual step this design cannot remove, so it is a large
  control and it is on the main screen, not in settings.
- Storing the side *and* the resolved player on every point means a wrong
  mapping can be corrected afterwards in the export instead of invalidating the
  session.
- The firmware holds a `match_id`, so it has to decide what happens when it
  reboots mid-match. It keeps the id in NVS and carries on; a match that has
  ended is cleared.
- `POST /matches` from a device means a match can be created by a board with no
  player list, so the names are free text. Matches that are reported to
  Schmetterpause are created by the page instead
  ([zaehlwerk ADR-0004](https://github.com/stuttgart-things/zaehlwerk/blob/main/docs/adr/0004-reporting-results-to-schmetterpause.md)),
  and the firmware then joins the running match rather than creating one.
- The `delta: 0` case in the ingest contract now has an obvious user: a hit the
  firmware saw but could not attribute is reported honestly instead of being
  guessed into a point.
