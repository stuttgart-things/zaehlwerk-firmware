# 5. Labels are append-only records that point at events

Status: Proposed
Date: 2026-09-29

## Context

The log says what the firmware decided. Only a person can say what actually
happened at the table, and that is the other half of every diagnosis: a
phantom hit and a real bounce on the wrong side produce the same log entry and
differ only in what the ball did.

The obvious shape is a correction over a time window — "around 18:22 it counted
wrong". It does not survive contact with a rally. Bounces are a few hundred
milliseconds apart, a person reacts in one or two seconds, and the window that
is wide enough to catch the mistake is wide enough to catch three other hits
with it. A label that cannot be resolved to one specific decision is a label
that cannot be counted.

Corrections also get corrected. Somebody marks a point wrong during the rally,
looks at the curve afterwards and sees it was a net cord rather than a phantom.
Overwriting the first label loses the fact that the first impression was wrong,
which is itself a finding about how usable the labelling is.

## Decision

A label is its own record in the session JSONL, never a field on an existing
one. It carries a `label_id` and points at what it is about by id: a `point_id`,
a `rally_id`, a list of `event_ids`, or a combination.

Every hit event carries the `rally_id` it belongs to, every awarded point a
`point_id`, so there is always something to point at. A quick mark during play
("that is wrong") is a label with a `rally_id` and no `point_id` yet — an open
marker, resolved afterwards against the timeline.

A missing bounce has nothing to point at, so it points between: a
`missed_hit` label names the two `event_ids` it falls between, plus a side and
an approximate time.

Nothing is ever overwritten or deleted. An edit is a new record carrying
`supersedes: <label_id>`. The export resolves the chain and reports the final
state; the file keeps the whole history.

## Consequences

- The firmware never writes labels and never reads them. It only has to emit
  ids that are stable and unique, which it has to do anyway for idempotent
  ingest.
- The session file stays append-only, so it can be copied, streamed or
  truncated without a consistency problem, and a crashed sink loses at most the
  last record.
- Resolving `supersedes` chains is work the export and the viewer both have to
  do. That is worth it: "the first label was wrong" is information about the
  labelling method, and the method is on trial here too.
- Labels stay valid across a firmware change, because they name events rather
  than thresholds or parameters. A session recorded today can be replayed
  against a new detector and the labels still say what was true.
