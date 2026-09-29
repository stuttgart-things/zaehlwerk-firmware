# 4. Diagnostic log over UDP to a sink, beside the API path

Status: Accepted
Date: 2026-09-29

## Context

Counting accuracy at the table is around half, and nobody knows why. Missed
bounces, phantom hits, the wrong side and a mistake in the scoring logic all
look identical from the scoreboard: a number that is wrong. Guessing between
them is what the last weeks were.

Answering it needs the raw signal around every threshold crossing, the decision
that was taken on it and the reason — including for the crossings that were
thrown away, which are exactly the ones the scoreboard never mentions.

The ingest contract cannot carry that, and should not.
[zaehlwerk ADR-0002](https://github.com/stuttgart-things/zaehlwerk/blob/main/docs/adr/0002-idempotent-ingest-contract.md)
is four fields wide on purpose: a source, a player, a delta and a counter. Its
whole value is that the scorer never sees source-specific quirks. Peaks,
crossing times and twenty milliseconds of pre-trigger samples are the most
source-specific thing there is, and a scorer that grew fields for them would
have to grow another set for the next sensor.

## Decision

Two paths out of the device, for two different purposes.

**Points** go to `zaehlwerk-api` over HTTP in exactly the shape ADR-0002
defines, and nothing else. Offline they queue in a ring buffer and are re-sent
in order; the `event_id` counter is what makes that safe.

**Diagnostics** go to a sink over UDP, as JSON, one event per datagram, split
into chunks when an event does not fit. Sending is non-blocking and lossy by
design: a sink that is not listening, a full socket or a slow laptop must never
delay detection. The address and port are configured, not fixed — broadcast for
the bench, unicast for a laptop on the office wifi.

The sink is `tools/log-sink`, a Go program that writes one append-only JSONL
per session and serves the labelling UI.

## Amendment: curves may be lost, points must not

Added after a game was played through and lost. No sink was listening, logging
was off, and nothing said so — and because this path holds nothing, the game did
not exist the moment it ended.

"Lossy by design" was right about why, and too broad about what. Two kinds of
event are mixed under it:

A **hit** carries hundreds of raw samples. It is large, it arrives hundreds of
times a game, and it is only worth anything while somebody is working on
detection. Holding those would cost the memory the sampler needs and buy little.

A **point**, a **rally**, a **correction**, a **change of ends** — these are a
few hundred bytes each, perhaps fifty in a game, and they are the record of what
happened. They cannot be reconstructed from anywhere else once the board is
switched off.

So: **an event that fits in a single datagram is held when no sink is listening,
and sent when one appears.** Anything larger is dropped as before. The rule is
the datagram rather than the event type, because it is the same rule the
chunking already uses and it needs no table to stay correct when a new event is
added.

This does not weaken the first rule. Holding an event is a memcpy on the network
core; the sampler still never blocks, and a full buffer drops the oldest rather
than waiting. What arrives late is marked as such — a `note` says how many
events were held and for how long, so nobody reads a flush as a burst of play.

**Holding needs knowing, and UDP does not tell.** The first version of this held
only when the board knew there was no sink: logging off, or no address set. A
sink that had crashed looks exactly like one that is listening, so the board
went on firing into it — and half a game was lost that way while this was being
tested. So the board asks. It sends a `ping` to the sink every two seconds, the
sink answers whoever asked, and six seconds without an answer means hold rather
than send. The ping goes out whatever the buffer is doing, because it is how the
sink learns where to answer and holding it would deadlock.

That is also what the scoreboard shows. "Logging is switched on" and "this game
is being kept" are different claims, and only the second one is worth a line
under the score.

## Consequences

- UDP loses packets and this one has no retry, so loss has to be visible rather
  than silent. Since the amendment above, an event small enough to fit a
  datagram survives a sink that is not there; a hit with its samples still does
  not. Every event carries a monotonic sequence number, and the sink
  reports the gaps. A session with holes is still usable; a session that
  silently dropped a third of its hits would not be.
- The firmware needs no ack path, no buffering for the sink and no back
  pressure, which is what keeps the sampler clean.
- The same event can be seen from both sides: the sink knows what the firmware
  decided, the API knows what it was told. Where those disagree is a bug worth
  finding.
- A second protocol is a second thing that can be misconfigured. The web UI
  shows the sink address and whether anything has been sent.
- Diagnostic logging is a development instrument, not a permanent load. It is
  switchable, and the switch is itself a logged event.
