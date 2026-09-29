# Zählwerk Firmware

ESP32 firmware for [zaehlwerk](https://github.com/stuttgart-things/zaehlwerk),
a table tennis scoreboard. Two piezo discs under the table halves hear the ball
bounce, the firmware decides whose point it was, and the result reaches
`zaehlwerk-api`.

```
button (C3, battery, deep sleep) ──┐
                                   ├──ESP-NOW──> hub (mains, wifi) ──HTTP──> zaehlwerk-api
piezo (mains) ─────────────────────┘
```

Buttons and piezo units report over ESP-NOW to a hub; only the hub holds wifi
and speaks HTTP ([ADR-0001](adr/0001-esp-now-to-a-hub.md)). The piezo board is
mains-powered, so it hosts the hub alongside the sensing — that is where the
firmware runs.

## Where it stands

Counting accuracy at the table is **around half**, and the interesting question
is why. Missed bounces, phantom hits, the wrong side and a mistake in the
scoring logic all look identical from the scoreboard: a number that is wrong.

Three candidates are known and none is yet proven at the table:

- Between hits the sampler reads **once per millisecond**, against a bounce
  that rises in tens of microseconds.
- After every detected hit it does **not sample at all for 90 ms**.
- Both channels hear every bounce on a single-piece table, and nothing in the
  firmware yet says how much louder one has to be to count as the side.

Answering it is what the diagnostic log
([ADR-0004](adr/0004-diagnostic-log-over-udp-to-a-sink.md)) and the
[event schema](event-schema.md) exist for: every threshold crossing is
recorded, **including the ones that were thrown away**, with the raw signal
around it and the reason it was discarded.

## The pages here

| | |
| --- | --- |
| [Flashing and updates](flashing-and-ota.md) | USB once, then over the air, with a rollback that puts a bad build back |
| [Event schema](event-schema.md) | what the firmware emits and what the sink stores |
| [Bench bring-up](piezo-stufe-1-2.md) | building the piezo circuit and the two measurement gates it had to pass |

## Decisions

| | |
| --- | --- |
| [0001](adr/0001-esp-now-to-a-hub.md) | ESP-NOW to a hub instead of wifi per device |
| [0002](adr/0002-wifi-station-with-access-point-fallback.md) | Wifi station with an access point fallback |
| [0003](adr/0003-sampling-on-core-1.md) | Sampling on core 1, everything else on core 0 |
| [0004](adr/0004-diagnostic-log-over-udp-to-a-sink.md) | Diagnostic log over UDP to a sink, beside the API path |
| [0005](adr/0005-labels-are-append-only-records.md) | Labels are append-only records that point at events |
| [0006](adr/0006-the-firmware-owns-the-half-to-player-mapping.md) | The firmware owns the table half to player mapping |

## Environments

One PlatformIO project. `piezo` is the firmware; `piezo-test` is the same
firmware starting in mock mode; `piezo-ota` and `piezo-ota-crash` differ only
in how the binary gets onto the board; `native` runs the rule tests on a
laptop. `button` and `hub` are planned and unwritten.
