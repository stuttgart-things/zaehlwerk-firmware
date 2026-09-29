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

When corrections were made by hand, the summary adds what they had in common:

```
  points by reason
    last_bounce      13
    manual           12
  corrections by tag
    edge              4
    ghost             1
    missed            2
    net               2
    other             2
    wrong_side        1
```

That is the point of asking for a tag on the board rather than a note
afterwards. Four corrections tagged `edge` in one game is a finding; four notes
reading "Kante", "von der kante" and "Kantenball" are not.

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

## Playing without a table

The board has a mock sensor: the same detection path fed from a generator
instead of the ADC, so the scoreboard, the counting logic, the log and the sink
all run at a desk with no piezo and no ball.

Switch it on under **Mock** in the board's web UI. A large banner says so, and
switching ends the session and starts a new one — mixing readings from a
generator and from a table in one file would make every number in it
unreadable. Sessions carry `sensor: "mock"`.

The triggers are single hits per side, a weak one astride the threshold, a
phantom hit, a net ball with both halves equally loud, one rally, or continuous
autoplay. Every generated hit says what it was meant to be, so the sink scores
the side with nothing labelled:

```
crossings by decision          points by reason
  ambiguous         1            double_bounce   1
  counted          24            last_bounce     5
mock: side right on 25 of 25 (100.0%)
```

The `ambiguous` there is the net ball. Both halves equally loud is the case the
ratio cannot separate, and a detector claiming a side for it is guessing.

**Autoplay is not a stress test yet.** Its bounces are clean and well spaced, so
the detector gets them all right — useful for exercising the chain, not for
finding where it breaks. The hard cases are the manual triggers, and making
autoplay harsh (more crosstalk, tighter timing) is
[#11](https://github.com/stuttgart-things/zaehlwerk-firmware/issues/11)'s
remaining half along with CSV replay.

## Testing the sink alone

```bash
task sink                       # one terminal
task sink:mock                  # another
```

`log-sink mock` is the other half: it sends the same wire format the firmware
does — chunked events,
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
task sink:mock CORRECTIONS=0.5  # half the points end as a tagged correction
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
