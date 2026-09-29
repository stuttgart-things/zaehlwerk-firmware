# 3. Sampling on core 1, everything else on core 0

Status: Accepted
Date: 2026-09-29

## Context

Stage 2 pins the sensor task to core 0 and leaves the web server on core 1,
and `docs/piezo-stufe-1-2.md` says to keep that split. The reason given is
right — a web server in the same loop as the sampling swallows bounces — but
the sides are the wrong way round.

On the ESP32 the wifi and lwIP tasks run on core 0, the PRO_CPU. Arduino's
`loopTask` runs on core 1. Pinning the sampler to core 0 therefore puts it on
the same core as the radio, which is the one thing on the board that takes the
CPU away at unpredictable moments and for hundreds of microseconds at a time.
The split exists, but it protects the sampler from the web server while
exposing it to something worse.

Two further things in the same task make it harder than the core choice:

The idle loop reads one sample per channel and then calls `vTaskDelay(1)`. At
a 1000 Hz tick that is **one sample per channel per millisecond**. A bounce
measured in stage 1 rises in tens of microseconds. Most of the impulse falls
between two samples, and whether it is seen at all is luck.

Once a channel does cross, the task spins for 30 ms collecting peaks and then
sits out a 60 ms dead time. For 90 ms after every detected hit there is no
sampling at all. A net cord or an edge inside that window does not exist.

## Decision

Sampling runs as a FreeRTOS task pinned to **core 1**. Wifi, the web server,
the API client and the log sender run on **core 0**.

The sampler never blocks on anything that is not the ADC. It hands raw events
to the other core through queues and does not wait for them to be taken.
Nothing in the network, logging, web UI or OTA path is allowed to delay
detection — a full queue drops the oldest log entry and records that it did,
rather than stalling the sampler.

The sampler reads continuously rather than idling between single reads, so the
sample rate is set by the ADC and not by the scheduler tick. The dead time
stops being a hole in the record: sampling continues through it, and the dead
time only governs whether a crossing is *counted*, not whether it is *seen*.

## Consequences

- The README and `docs/piezo-stufe-1-2.md` both told the reader to keep the
  sampler on core 0. Both now point here instead, and both say that the
  firmware still does the old thing until the change lands — a document that
  described the intent as if it were the code would be its own kind of wrong.
- A continuously sampling task on core 1 leaves that core to it. Anything else
  that wants core 1 has to justify itself.
- Sampling through the dead time is what makes the pre-trigger ring buffer and
  the `deadtime` decision reason possible: a hit that is suppressed is still
  recorded, with its samples, so the log can show what was thrown away.
- This changes detection behaviour, which means it changes the score. It does
  not belong in the same pull request as anything else, and the accuracy before
  and after has to be measured on the same table.
