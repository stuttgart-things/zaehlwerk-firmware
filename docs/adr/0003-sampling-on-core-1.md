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

## Measured, 2026-09-30

Taken at the table with the piezos attached and the circuit from
`docs/piezo-stufe-1-2.md`, same board, same taps, same thresholds, ten minutes
apart:

| | `vTaskDelay(1)`, core 0 | continuous, core 1 |
| --- | --- | --- |
| Readings per second | 769 | **5794** |
| Between readings | 1299 µs | **172 µs** |
| Resting level, both channels | 0 | 0 |
| Counted, of ten finger taps | 4 | **10** |
| Samples per recorded curve | 164 | ~240 |

The question this decision was held on — whether the sample-and-hold settles at
the 1 MΩ source impedance of the piezo front end without the millisecond of slack
— is answered. **The levels do not collapse; the same tap reads roughly twice as
high.** The slow loop was catching the transient on its flank and recording that
as the peak.

What it buys is not mainly more bounces over the line. Ball bounces saturate the
ADC either way: 30 of 30 were counted on both builds. What it buys is a peak
worth comparing. At 1.3 ms per reading the same bounce reads 1104, or 3122, or
4095, depending where the sample falls, and **no threshold can be set against a
number that moves by four times** — a threshold of 1200 derived from saturated
measurements dropped eighteen readings above 1000 in one game, all of them real
bounces.

One caution learned the same evening: this makes peaks comparable, it does not by
itself make counting better. It was switched on together with four other changes
and the result was worse than the sketch. It belongs on its own, measured against
[the baseline game](../baselines/2026-09-30-first-clean-game.json).

## Tried, 2026-10-01, and it does not run

Twice, over the air, on the board at the table:

| Attempt | What happened |
| ------- | ------------- |
| `taskYIELD()` on every reading | Booted, satisfied the health check within a fifth of a second, **confirmed itself valid at ten seconds**, then crash-looped every thirty-five. Confirmation is irreversible, so the rollback could no longer help and the baseline had to be pushed back into a thirty-second window of reachability |
| One tick of sleep every 64 readings | Crashed inside fifteen seconds. The probation window had meanwhile been raised to forty-five seconds, so **the bootloader restored the baseline on its own** |

So the idle task starving on core 1 is not the whole story, and this decision
cannot be implemented as written. What has not been separated yet:

- `ARDUINO_RUNNING_CORE=0` moves `loop()` and the web server onto core 0, where
  the wifi and lwIP tasks already live. That may be what breaks, rather than the
  sampling.
- Sampling faster **without** moving cores, leaving the sketch's arrangement
  intact.

Both want a board on a cable, with the serial line readable, rather than a board
across the room. The panic message is the missing piece; neither attempt produced
one that anybody saw.

The measurement above still stands: continuously sampled peaks are consistent and
slowly sampled ones are not. What is not established is a way to get them that
stays up.

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
