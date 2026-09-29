# zaehlwerk-firmware

ESP32 firmware for [zaehlwerk](https://github.com/stuttgart-things/zaehlwerk).

Buttons and piezo units report table tennis points over ESP-NOW to a hub. Only
the hub holds wifi and talks to `zaehlwerk-api`.

```
button (C3, battery, deep sleep) ──┐
                                   ├──ESP-NOW──> hub (mains, wifi) ──HTTP──> zaehlwerk-api
piezo (mains) ─────────────────────┘
```

## Environments

One PlatformIO project. Two of the planned environments are written:

| Env | Board | Power | Role | State |
| --- | ----- | ----- | ---- | ----- |
| `piezo` | ESP32 dev module | Mains | Hit detection and scoreboard | written |
| `piezo-test` | ESP32 dev module | Mains | Same firmware, starts in mock mode | written |
| `native` | — the laptop | — | Unity tests of the rules | written |
| `button` | ESP32-C3 Super Mini | Battery, deep sleep | Wake on press, send, sleep | [#2](../../issues/2) |
| `hub` | ESP32 | Mains | Receive, forward over HTTP | [#3](../../issues/3) |

Per-unit configuration — source id, hub MAC, wifi, API URL — is set by build
flag, not by editing source.

## Building and flashing

Once, to get a place for wifi and OTA credentials that is not the repository:

```bash
cp secrets.ini.example secrets.ini      # then fill it in; it is gitignored
```

Without that file the build still works and falls back to the defaults in
`include/config.h`.

```bash
pio run -e piezo                 # build
pio run -e piezo -t upload       # flash over USB
pio device monitor               # 115200 baud
pio test -e native               # run the rule tests on the laptop
```

Both ESP32 environments use `default.csv`, the standard partition table with
two app slots. Two slots are what makes an over-the-air update and a rollback
possible at all — do not swap it for a single-slot table to win flash.

## Shared protocol

`lib/protocol` holds the ESP-NOW payload struct and the pairing constants.
Sender and receiver must agree on it exactly; the struct is explicitly packed
because `button` builds for RISC-V and `hub` for Xtensa. A mismatch presents as
radio that silently does nothing.

## Two things that look like broken hardware

**Channel mismatch.** Wifi and ESP-NOW share the radio, so peers must sit on the
channel the hub's access point put it on. Wrong channel means the button reports
a successful send and the hub receives nothing.

**Lost event counter.** The counter behind `event_id` lives in RTC memory. In a
plain variable it resets on every wake, every press arrives as id 1, and the API
discards all but the first as duplicates — see
[zaehlwerk ADR-0002](https://github.com/stuttgart-things/zaehlwerk/blob/main/docs/adr/0002-idempotent-ingest-contract.md).

## Decisions

| ADR | Subject |
| --- | ------- |
| [0001](docs/adr/0001-esp-now-to-a-hub.md) | Why ESP-NOW to a hub rather than wifi on every device |
| [0002](docs/adr/0002-wifi-station-with-access-point-fallback.md) | Wifi station with an access point fallback |
| [0003](docs/adr/0003-sampling-on-core-1.md) | Sampling on core 1, everything else on core 0 |
| [0004](docs/adr/0004-diagnostic-log-over-udp-to-a-sink.md) | Diagnostic log over UDP to a sink, beside the API path |
| [0005](docs/adr/0005-labels-are-append-only-records.md) | Labels are append-only records that point at events |
| [0006](docs/adr/0006-the-firmware-owns-the-half-to-player-mapping.md) | The firmware owns the table half to player mapping |

0002 to 0006 are proposed, not accepted. What the events look like once they
are: [docs/event-schema.md](docs/event-schema.md).

## Piezo bring-up — Stufe 1 and 2

The piezo path is in bench testing, ahead of the PlatformIO firmware. Two stages,
each with a gate that decides whether the next one happens:

| Stage | Question | Effort | Gate |
| ----- | -------- | ------ | ---- |
| 1 | Does the piezo hear the ball, and does it separate from bat clatter? | An evening | Weakest real bounce ≥ 2× the strongest disturbance (peak, or rise time) |
| 2 | Does the counting logic get a real game right enough? | A week | Under ~1 correction per game |

Both stages were built as standalone Arduino IDE sketches, deliberately outside
the ESP-NOW architecture. Stage 2 in particular opens its own access point and
serves the scoreboard itself, so it can be tuned mid-game without reflashing.
That is a measurement shortcut, not a second path to the API: the field devices
still report over ESP-NOW to the hub as in
[ADR-0001](docs/adr/0001-esp-now-to-a-hub.md).

Stage 2 now lives in `src/` as the `piezo` environment and is built and flashed
with PlatformIO. The sketches stay where they are, unchanged, as the record of
how the bench measurement was done — but nothing is developed in them any more,
and the Arduino IDE is no longer part of the workflow.

- Step-by-step build and measurement guide, macOS and Ubuntu:
  [docs/piezo-stufe-1-2.md](docs/piezo-stufe-1-2.md) — with breadboard drawings
  for [channel A](docs/images/piezo-channel-a-breadboard.svg) and
  [channel B](docs/images/piezo-channel-b-breadboard.svg)
- [`sketches/stufe1-piezo-test`](sketches/stufe1-piezo-test) — one piezo on
  GPIO 34, serial plotter plus a CSV event mode (`nr,spitze,anstieg_us,dauer_us,pause_ms`)
- [`sketches/stufe2-zaehlwerk-mvp`](sketches/stufe2-zaehlwerk-mvp) — two piezos
  (GPIO 34/35), full counting logic, scoreboard on `http://192.168.4.1`;
  carried over verbatim into `src/main.cpp`, with the rules split out into
  `lib/game` so they can be tested off the board

Sensing runs as its own task pinned to core 0 in stage 2; the web server on core
1 would otherwise swallow bounces. Keep that split.

## Status

Early. `button` and `hub` are the path to a working scoreboard and are still
unwritten. `piezo` carries the stage 2 bench firmware and is where the work is:
counting accuracy at the table is around half, and whether that is missed
bounces, phantom hits, the wrong side or the scoring logic is not yet known.
Diagnostic logging is what answers it.