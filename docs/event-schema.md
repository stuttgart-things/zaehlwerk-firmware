# Event schema

What the firmware emits, what the sink stores, and what a person adds on top.

One line of JSON per event. The firmware sends them over UDP
([ADR-0004](adr/0004-diagnostic-log-over-udp-to-a-sink.md)); the sink writes
them to `sessions/<date>-<time>.jsonl` in the order they arrive, adding its own
receive time to every record. Labels are written by the sink only
([ADR-0005](adr/0005-labels-are-append-only-records.md)).

This is a diagnostic stream, not the ingest path. Points go to `zaehlwerk-api`
in the four fields its contract defines and nowhere near this file.

---

## The envelope

Every event carries the same head:

```json
{
  "v": 1,
  "session_id": "8f3a2c11",
  "seq": 1247,
  "id": "8f3a2c11-1247",
  "t_us": 91234567,
  "type": "hit"
}
```

| Field | |
| --- | --- |
| `v` | schema version; bumped when a field changes meaning, not when one is added |
| `session_id` | one session, from chip id and boot time |
| `seq` | monotonic within the session, no gaps. **The only way loss is detectable** |
| `id` | `session_id` + `seq`; what everything else points at |
| `t_us` | µs since boot, from the ESP32 |
| `type` | one of the types below |

There is no wall clock in the firmware. `t_us` is monotonic and enough to order
and measure; the sink stamps `recv_at` on arrival and that is what a person
reads.

`seq` counts every event the firmware produced, including those it did not send
because no sink was configured yet. So the first `seq` a sink sees is not
normally 0, and a sink must take the first one it sees as its baseline rather
than reporting everything before it as lost.

The sink adds, on every record it writes:

```json
{ "recv_at": "2026-09-29T18:22:10.481Z", "recv_seq": 1531 }
```

---

## `session` — always `seq: 0`

The first event of every session, and the only place the parameters and the
build are stated in full. A session ends and a new one starts on boot, on a
switch between mock and real play, and before an OTA update.

```json
{
  "v": 1, "session_id": "8f3a2c11", "seq": 0, "id": "8f3a2c11-0",
  "t_us": 812004, "type": "session",
  "device_id": "piezo-tisch-1",
  "fw_version": "0.3.0",
  "git_hash": "4b9557e",
  "sensor": "adc",
  "reason": "boot",
  "params": {
    "threshold_a": 300, "threshold_b": 300,
    "log_threshold_a": 120, "log_threshold_b": 120,
    "peak_window_us": 30000,
    "deadtime_us": 60000,
    "clear_ratio": 1.8,
    "rally_timeout_ms": 1500,
    "pre_trigger_us": 20000,
    "sample_rate_hz": 20000,
    "profile": "tisch-buero"
  }
}
```

`sensor` is `adc` or `mock`. `reason` is `boot`, `mode_switch`, `ota` or
`manual`. Nothing downstream may assume a session ended cleanly — a power cut
looks like a session that simply stops.

`sample_rate_hz` above is a placeholder, not a decided number. What the ADC
sustains on two channels has to be measured before anything relies on it; the
field is in the schema because the rate has to be recorded, whatever it turns
out to be.

`v` is bumped when a field changes meaning. Adding a field does not bump it, so
a reader has to tolerate fields it does not know.

## `param` — a knob moved

Every change to any value in `params`, whoever made it. A tuning session is
reconstructible from the session event plus the `param` events after it.

```json
{ "type": "param", "name": "threshold_b", "from": 300, "to": 420,
  "by": "web", "profile": "tisch-buero" }
```

`by` is `web`, `profile` (a stored profile was loaded) or `calibration` (the
baseline was re-measured).

## `hit` — every crossing of `log_threshold`, counted or not

The core record. Emitted for **every** crossing, including the ones that were
thrown away — those are the interesting half.

```json
{
  "type": "hit",
  "rally_id": "8f3a2c11-r14",
  "side": "A",
  "decision": "counted",
  "peak_a": 1820, "peak_b": 640,
  "baseline_a": 112, "baseline_b": 118,
  "cross_a_us": 0, "cross_b_us": 740,
  "ratio": 2.84,
  "counted": true,
  "samples": {
    "pre_us": 0,
    "n": 242,
    "t_us": [0, 96, 191, "…"],
    "a": [110, 113, 109, "…"],
    "b": [117, 118, 116, "…"]
  }
}
```

| | |
| --- | --- |
| `side` | `A`, `B`, or `null` when no side was decided |
| `decision` | `counted`, `below_threshold`, `deadtime`, `ambiguous` |
| `cross_*_us` | relative to the first crossing of either channel; the one that crossed first is `0` |
| `ratio` | the two peaks normalised against their thresholds, the number `clear_ratio` is compared against |
| `counted` | whether the crossing actually scored |
| `samples` | pre-trigger ring buffer plus the peak window, both channels, raw ADC counts |

`samples.t_us` holds one offset per sample, in microseconds from the event's
`t_us`. There is no single rate to state: the sampler reads as fast as the ADC
allows inside the peak window and once per millisecond outside it, so a fixed
`rate_hz` would be a fiction. `n` is how many triples the three arrays hold.

`counted` is separate from `decision` on purpose. An `ambiguous` crossing still
counts today exactly as it always has — the louder channel wins — because the
change that measures detection must not be the change that alters it. When
`clear_ratio` starts deciding, `counted` is what shows the difference between
before and after in a recorded session.

Two limits worth knowing before reading a curve:

- The **pre-trigger holds what was sampled**, and outside the peak window that
  is one sample per millisecond. Until [#10](https://github.com/stuttgart-things/zaehlwerk-firmware/issues/10)
  a pre-trigger is a handful of points, not a shape.
- A crossing that does **not** count carries no window at all — `n` is 0 and
  only the peaks are recorded. Running a peak window for it would spend thirty
  milliseconds of blindness the detector did not previously spend, which would
  contaminate the very measurement this exists for.

`ambiguous` means both channels crossed and `ratio` stayed under `clear_ratio`
— the firmware saw a bounce and does not claim to know whose. That is the case
that becomes `delta: 0` at the API rather than a guessed point.

`samples` is what makes this event large. See [chunking](#chunking).

## `rally` — a ball exchange opened or closed

```json
{ "type": "rally", "rally_id": "8f3a2c11-r14", "phase": "end",
  "sequence": "ABAB", "hit_ids": ["8f3a2c11-1240", "8f3a2c11-1243"],
  "closed_by": "timeout" }
```

`phase` is `start` or `end`; `closed_by` is `timeout` or `manual`.

## `point` — the scoring logic changed state

One per awarded or withdrawn point, with the reason and both sides of the
transition.

```json
{
  "type": "point",
  "point_id": "8f3a2c11-p23",
  "rally_id": "8f3a2c11-r14",
  "cause_event_id": "8f3a2c11-1243",
  "reason": "double_bounce",
  "hint": "Doppelaufsetzer auf B — Ball nicht zurueckgespielt.",
  "side": "A", "player": "a", "player_name": "Pat",
  "from": { "a": 3, "b": 5, "serve": "A", "over": false },
  "to":   { "a": 4, "b": 5, "serve": "B", "over": false }
}
```

A correction made by hand carries two more fields, `tag` and `note`:

```json
{ "type": "point", "reason": "manual", "side": "A",
  "tag": "edge", "note": "Ball kam von der Kante zurueck" }
```

`tag` comes from a fixed vocabulary — `missed`, `wrong_side`, `ghost`, `net`,
`edge`, `bat_or_body`, `let`, `other` — and `note` is free text. The vocabulary
exists because free text does not cluster: "Netzroller", "netz roller" and
"Netz" are three different things to a summary, and finding what a game's
corrections had in common is the whole reason for asking. Both are asked for at
the moment of the correction, on the board's own page, because that is when the
answer is known.

Neither is a label. Labels are the sink's, they point at ids, and they can be
superseded ([ADR-0005](adr/0005-labels-are-append-only-records.md)). These two
are what the person at the table said while pressing the button.

`reason` is `last_bounce`, `double_bounce`, `single_bounce`, `manual`, `undo`
or `none`. It comes out of the rules as a token rather than being read back out
of the German hint, which would be guessing. `side` is the table half; `player` is that half resolved
through the current mapping, and `player_name` the name it carried at that
moment ([ADR-0006](adr/0006-the-firmware-owns-the-half-to-player-mapping.md)).
All three are stored, so a mapping that turns out to be wrong is correctable in
the export instead of invalidating the session.

## `match` — players, ends, sets

```json
{ "type": "match", "phase": "ends_swapped",
  "set_number": 2,
  "players": { "a": "Pat", "b": "Ana" },
  "sides":   { "A": "b", "B": "a" } }
```

`phase` is `start`, `players` (a name was set or changed), `ends_swapped` or
`end`. `sides` is the mapping in force from this event onwards; it is what
makes every later `point` resolvable.

`match_id` is not on this event yet
([#42](https://github.com/stuttgart-things/zaehlwerk-firmware/issues/42)); the
match the board joined at zaehlwerk is on the `link` events below.

**Names are optional.** Without them everything keeps saying `A` and `B`, and
`players` carries empty strings. For doubles both names go in one field —
`"Pat & Chris"` — because a side is what gets a point, and splitting it would
be a data model for something only the display cares about.

## `ingest` — what was sent to the API, and what came back

Not a duplicate of `point`: this is the delivery record, and the place an
offline queue becomes visible
([#17](https://github.com/stuttgart-things/zaehlwerk-firmware/issues/17),
[ADR-0008](adr/0008-reporting-to-zaehlwerk-is-opt-in-and-zaehlwerk-owns-the-score.md)).
Only written when the board has a zaehlwerk address; a board without one never
emits it.

```json
{ "type": "ingest", "status": "retry", "reason": "unavailable",
  "action": "point", "point_id": "8f3a2c11-p23", "rally_id": "8f3a2c11-r41",
  "match_id": "c462e62c", "source": "piezo-1",
  "side": "A", "half": "left", "delta": 1, "event_id": 42,
  "http": 503, "attempt": 1, "queued_ms": 4, "retry_in_ms": 1384 }
```

| Field | |
| --- | --- |
| `status` | `queued` when it enters the queue; then, per attempt, exactly one of `acked`, `retry` or `dropped`. There is no separate `sent`: every send ends in one of the three |
| `reason` | for `retry`: `no_wifi`, `no_answer`, `bad_gateway`, `unavailable`, `gateway_timeout`, `server_error` (any other 5xx), `tls` (the certificate did not verify against the edge root; nothing was sent), `no_time` (https waits for SNTP; nothing was sent), `nvs` (the id could not be stored, so it was not sent). For `dropped`: `match_over` (404/409), `refused` (any other answer), `queue_full`, `phase` (a detected rally during warm-up or a change of ends, not sent), `address_changed`, `address_cleared` |
| `action` | `point` or `undo` ([zaehlwerk ADR-0012](https://github.com/stuttgart-things/zaehlwerk/blob/main/docs/adr/0012-taking-a-point-back-through-the-ingest-contract.md)) |
| `point_id`, `rally_id` | the board's own point and rally this came from. Absent for a rally that awarded nothing (`delta: 0`, only `rally_id`) and for an undo that took nothing back locally |
| `match_id`, `source`, `half`, `delta`, `event_id` | exactly what went over the wire. An undo has no `half` and no `delta`. `side` is the same half in the log's own spelling |
| `http` | the status zaehlwerk or its gateway answered; absent when nobody answered |
| `attempt` | how many times this event has been sent so far |
| `queued_ms` | how long it had been waiting |
| `retry_in_ms` | for `retry`: when it goes again |

The `event_id` of a `retry` and of the `acked` after it are the same: a resend
is never renumbered (zaehlwerk ADR-0002). A `dropped` with `reason` `phase`,
`match_over` or `queue_full` and no `event_id` was never given one.

## `link` — where the board stands with zaehlwerk

Also only with a zaehlwerk address.

```json
{ "type": "link", "phase": "joined", "match_id": "c462e62c",
  "http": 200, "players": { "a": "Pat", "b": "Ana" }, "set_number": 1 }
```

| `phase` | |
| --- | --- |
| `address` | an address is in force: at boot (`reason` says where it came from: `NVS`, `secrets.ini`), or `set` / `cleared` on the page. Carries `url`, and `source` at boot |
| `joined` | the board joined a running match. Carries the players and the set |
| `left` | it is out of that match: `reason` `no_match` (ended on the page), `match_over`, or `demo` (a demo became current) |
| `waiting`, `demo` | no match, or a demo, while the board was not in one |
| `unreachable` | the first failed request in a row. `reason` as for `ingest` `retry`, `retry_in_ms` |
| `back` | the first answer after that. `tries` failed requests, `after_ms` how long it took. A rollout reads as one `unreachable` and one `back` |
| `clock` | SNTP set the clock, once per boot, with `unix`. https waits for it |
| `adopted` | the board took zaehlwerk's score in place of its own count, because they differed. `from` and `to` carry `a`, `b` (points by half), `serve` and `set_number`; `to` also `over` |

`adopted` is what explains a score that changed with no `point` before it:
an undo on the phone, a set won at zaehlwerk, a point the board did not hear.

## `checkin` — a fob, a press, and what zaehlwerk said

One event per step of a check-in by fob
([ADR-0007](adr/0007-a-fob-turns-the-next-press-into-a-check-in.md)).
**There is no field for the card.** The UID never reaches this log, in any
spelling; a fob that does not check in is debugged by its outcome.

```json
{ "type": "checkin", "outcome": "claimed", "side": "A", "half": "left",
  "source": "piezo-1", "event_id": 43, "http": 200, "attempts": 1 }
```

| `outcome` | |
| --- | --- |
| `waiting` | a fob was read, the window is open. No half yet |
| `busy` | a fob during play, refused on the board; nothing was sent |
| `off` | a fob with no `ZW_API_URL`; nothing was sent |
| `no_token` | a fob with no check-in token on the board; nothing was sent (#65) |
| `ignored` | a fob while a request was out |
| `expired` | nobody pressed within the window; the UID is gone |
| `claimed` `unchanged` `replaced` `moved` `started` | zaehlwerk took it (200) |
| `duplicate` | zaehlwerk had a higher `event_id` from this source; nothing changed |
| `unauthorized` | zaehlwerk refused the token, or got none (401 or 403). Says nothing about the fob |
| `unknown` | the fob belongs to nobody (404) |
| `refused` | a match is running at zaehlwerk (409) |
| `unavailable` | zaehlwerk could not ask Schmetterpause, or check-in is off there (503) |
| `failed` | 500 |
| `bad_request` | 400: a request this firmware built and zaehlwerk would not read |
| `http_error` | any other status |
| `no_wifi` | the board is not on a network; nothing was sent |
| `no_answer` | on a network, but nobody answered, after `attempts` tries |
| `ok` | 200 with an outcome this firmware does not know |

Neither field nor outcome ever carries the check-in token; the log says
whether one is set (a `note`), never what it is.

`side` and `half` are there once a press named one: `side` the half as every
other event writes it, `half` the token zaehlwerk was sent. `source`,
`event_id`, `http` and `attempts` are there once a request was made.

## `sync` — a marker for lining up a phone video

```json
{ "type": "sync", "marker": "video", "note": "3x geklopft" }
```

## `note` — the firmware has something to say

Dropped queue entries, a wifi reconnect, a failed NVS write. Anything that
would otherwise only exist on the serial line.

```json
{ "type": "note", "level": "warn", "text": "log queue full, 3 events dropped" }
```

---

## Mock sessions

The session event carries `"sensor": "mock"`, and every generated `hit` also
carries what it was supposed to be:

```json
"intended": { "side": "A", "type": "bounce" }
```

`type` is `bounce`, `weak`, `ghost`, `net`, `crosstalk` or `replay`. That is
what lets the export compare what the detector decided against what was sent
in, with no labelling by hand — the accuracy number for the scoring logic falls
out of it.

A replayed CSV carries `"type": "replay"` and no `side`, since a recording
knows no ground truth unless somebody labels it.

---

## Chunking

A `hit` with 20 ms of pre-trigger plus a 30 ms window at 20 kHz is roughly a
thousand samples per channel and does not fit in a datagram. The event is
serialised to JSON once and the **string** is split; chunks are not structured
documents of their own.

```json
{ "v": 1, "session_id": "8f3a2c11", "seq": 1247,
  "chunk": { "i": 0, "n": 4 },
  "part": "{\"v\":1,\"session_id\":\"8f3a2c11\"," }
```

The sink joins the `part` values by `(session_id, seq)` in `i` order and parses
the result. An event with a missing chunk is reported as incomplete and kept as
a stub with the `seq` that was lost — never silently dropped, and never
half-parsed.

Events that fit in one datagram are sent whole, with no `chunk` field.

---

## Labels — written by the sink, never by the firmware

```json
{
  "type": "label",
  "label_id": "l-19",
  "recv_at": "2026-09-29T18:22:41.207Z",
  "session_id": "8f3a2c11",
  "kind": "event_correction",
  "point_id": "8f3a2c11-p23",
  "rally_id": "8f3a2c11-r14",
  "event_ids": ["8f3a2c11-1243"],
  "value": "crosstalk",
  "note": "Ball war lang auf A, B hat nur mitgeschwungen",
  "author": "handy-pat",
  "supersedes": null
}
```

`kind` and the `value` it allows:

| `kind` | `value` |
| --- | --- |
| `quick_mark` | `wrong`, `right` — set during play, `point_id` may be empty |
| `point_correction` | `correct`, `belongs_a`, `belongs_b`, `no_point`, `rally_not_over` |
| `event_correction` | `bounce_a`, `bounce_b`, `net`, `edge`, `bat_or_body`, `ghost`, `crosstalk` |
| `missed_hit` | `a`, `b` — with `between: ["<event_id>", "<event_id>"]` and `approx_t_us` |

`between` names the event before and the event after the missing bounce; one
of them may be `""` when the bounce came first or last in its rally.
`approx_t_us` is set by the sink, halfway between the two.

An edit or a deletion is a **new** record with `supersedes` naming the earlier
`label_id`; a deletion carries `value: null`. Nothing in the file is ever
rewritten.

`author` is whoever labelled, as typed on the page, and optional. A label the
sink derived from a correction at the table carries `author:
"firmware:<source>"` and `derived_from: "<event id>"` instead — the rule is in
[the sink's documentation](log-sink.md#labels-derived-from-corrections-at-the-table).

Labels say which half the ball was on, never a player name. The name is what
the UI shows, resolved through the `sides` mapping of the surrounding `match`
event — so a label written before a change of ends still reads correctly
afterwards.

---

## Ids at a glance

| | Shape | Issued by |
| --- | --- | --- |
| `session_id` | `8f3a2c11` | firmware, at boot |
| event `id` | `<session_id>-<seq>` | firmware |
| `rally_id` | `<session_id>-r<n>` | firmware |
| `point_id` | `<session_id>-p<n>` | firmware |
| `event_id` (ingest) | integer, monotonic per source | firmware, survives reflash in NVS |
| `match_id` | `c462e62c` | zaehlwerk-api, on `POST /matches` |
| `label_id` | `l-<n>` | sink |

The ingest `event_id` is the one that is not like the others: it is a plain
counter the API deduplicates on, it is per source rather than per session, and
it has to survive a restart or every reboot replays point one
([zaehlwerk ADR-0002](https://github.com/stuttgart-things/zaehlwerk/blob/main/docs/adr/0002-idempotent-ingest-contract.md)).
Check-in by fob is the first thing that sends one. The counter is in NVS
(namespace `ingest`, key `event_id`) and is written before each request goes.
