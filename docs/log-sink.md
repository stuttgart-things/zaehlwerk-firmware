# The log sink

`tools/log-sink` receives what the firmware sends, writes one append-only file
per session, and shows the curve around a crossing next to the thresholds that
judged it.

It exists because counting accuracy at the table is around half and the
scoreboard cannot say why: a missed bounce, a phantom hit, the wrong side and a
mistake in the scoring logic all look the same from there — a number that is
wrong. See [ADR-0004](adr/0004-diagnostic-log-over-udp-to-a-sink.md) and
[the event schema](event-schema.md).

## Running it

```bash
task sink                    # receive on :9000, viewer on http://localhost:9001
```

Then point the board at the laptop, under **Diagnose** in its web UI, or:

```bash
curl "http://zaehlwerk.local/diag?host=192.168.178.42&port=9000&on=1"
```

Switching it on emits a fresh `session` event, so the file that starts at that
moment still carries the firmware version, the git hash and every parameter. A
file without that event is a file that cannot be compared with another one, and
the viewer says so rather than drawing a curve with no thresholds beside it.

Sessions land in `sessions/<date>-<time>-<session id>.jsonl`.

## What it reports

```
session e32c87dd, started 2026-09-29T14:10:10Z
  records      117
  seq          4304..4420
  lost         0 of 117 (0.00%)
  events
    hit              111
    param            4
    rally            1
    session          1
  crossings by decision
    ambiguous        3
    below_threshold  27
    counted          38
    deadtime         43
    of those counted 41
```

**`lost` is the number that matters.** UDP has no retry here, so loss that is
not reported is loss that reads as a quiet session. A gap goes into the file as
a record of its own, not only into this summary — once the file is all anybody
has, a session that dropped packets must not look like one that did not.

The first sequence number is the baseline, and it is never zero: the firmware
counts every event it **produced**, including those it did not send because no
sink was configured yet. The first number to arrive in a bench run was 4140.

`ambiguous` and `counted` overlap on purpose. An ambiguous crossing still counts
today exactly as it always has, so `of those counted` is the two added together.
When `clear_ratio` starts deciding, the difference between the two lines is what
shows the change.

## Testing it without a board

```bash
task sink                       # one terminal
task sink:mock                  # another
```

The generator sends the same wire format the firmware does — chunked events,
rallies, points, crossings that were counted and crossings that were not. Every
generated hit carries `intended`, which is what lets the summary score the side
with nothing labelled by hand:

```
mock: side right on 46 of 51 (90.2%)
```

Knobs, so a run can be repeated and a failure reproduced:

```bash
task sink:mock LOSS=0.05        # drop a twentieth of the datagrams
task sink:mock WRONG=0.3        # decide three in ten on the wrong side
task sink:mock RALLIES=40
```

`LOSS` is how the gap detection and the incomplete-event path get exercised: a
dropped chunk leaves an event that never completes, which has to be written off
rather than waited for.

Only crossings that decided a side are scored. One that was never counted has no
side to be right or wrong about — counting those dragged a run with 15% wrong
sides down to 40% before it was fixed.

## Export

```bash
task sink:export SESSION=sessions/20260929-161010-e32c87dd.jsonl
```

A zip with the raw file, `summary.json`, `summary.txt` and `labels.json` —
the labels with their `supersedes` chains already resolved, while the raw file
keeps every version ([ADR-0005](adr/0005-labels-are-append-only-records.md)).
The summary is rebuilt from the file rather than taken from the running sink, so
it works on a session copied off a laptop weeks later.

## Limits worth knowing

- **Labelling is not here yet** — that is
  [#19](https://github.com/stuttgart-things/zaehlwerk-firmware/issues/19). The
  export already resolves label records if a file has them.
- **The curves are sparse** until
  [#10](https://github.com/stuttgart-things/zaehlwerk-firmware/issues/10):
  outside the peak window the sampler reads once per millisecond, so a
  pre-trigger is a handful of points rather than a shape.
- **Out-of-order datagrams are counted, not repaired.** One sender on a local
  network makes reordering rare, and a reordered datagram is not loss.
- **`log_threshold` has to sit above each channel's noise floor**, and that
  floor has to be measured on the assembled circuit. An early bench run recorded
  channel A resting around 146 counts and B near zero and read it as an
  asymmetry in the hardware. It was not: **nothing was connected to the board**,
  so those were two floating pins. Floating inputs say nothing about a piezo, a
  1 MΩ resistor or a lead. Measure the floor with the circuit attached, on the
  table it will stand on.
