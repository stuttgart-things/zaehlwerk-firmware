# 4. Diagnostic log over UDP to a sink, beside the API path

Status: Proposed
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

## Consequences

- UDP loses packets and this one has no retry, so loss has to be visible rather
  than silent. Every event carries a monotonic sequence number, and the sink
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
