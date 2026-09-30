# Baselines

A baseline is one complete game, recorded by the sink, kept as the thing the next
change has to beat. Not a test — a measurement, with the parameters it was taken
under written into the same file.

| File | What it is |
| ---- | ---------- |
| [`2026-09-30-first-clean-game.json`](2026-09-30-first-clean-game.json) | The first game that counted itself correctly. 18 rallies, 19 points, one wrong |

## Why this one matters

Counting at the table had been around half right for weeks, and a whole evening
of changes made it worse rather than better — five parameters moved at once,
every one of them justified by a measurement taken with nobody playing. Finger
taps and deliberate bounces are not a rally.

Going back to exactly what the Arduino sketch did produced this game. So the
sketch's numbers are the baseline, and anything that claims to improve on them
has to show it against a recorded game, one change at a time.

## What it holds

Everything needed to say whether a later game was better, and under what
conditions this one happened:

- `params` — thresholds, rally timeout, deadtime, peak window, clear ratio
- `firmware` — version, git hash, build date, repository, sensor source
- `rallies` — every rally with its bounce sequence
- every crossing with both channels, peaks, crossing times, ratio and decision
- `transport` — how much of it reached the sink (all of it, here)

## Reading one

```bash
task sink                 # the viewer, curves and all, on :9001
jq '.summary' docs/baselines/2026-09-30-first-clean-game.json
```
