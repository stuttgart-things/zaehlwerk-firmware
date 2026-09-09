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

One PlatformIO project, three environments:

| Env | Board | Power | Role |
| --- | ----- | ----- | ---- |
| `button` | ESP32-C3 Super Mini | Battery, deep sleep | Wake on press, send, sleep |
| `hub` | ESP32 | Mains | Receive, forward over HTTP |
| `piezo` | ESP32 | Mains | Hit detection, send over ESP-NOW; the board may also host the hub |

```bash
pio run -e button -t upload
pio run -e hub -t upload
```

Per-unit configuration — source id, hub MAC, wifi, API URL — is set by build
flag, not by editing source.

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

## Piezo bring-up — Stufe 1 and 2

The piezo path is in bench testing, ahead of the PlatformIO firmware. Two stages,
each with a gate that decides whether the next one happens:

| Stage | Question | Effort | Gate |
| ----- | -------- | ------ | ---- |
| 1 | Does the piezo hear the ball, and does it separate from bat clatter? | An evening | Weakest real bounce ≥ 2× the strongest disturbance (peak, or rise time) |
| 2 | Does the counting logic get a real game right enough? | A week | Under ~1 correction per game |

Both stages run as standalone Arduino IDE sketches, not as PlatformIO
environments — they are a test rig, deliberately outside the ESP-NOW
architecture. Stage 2 in particular opens its own access point and serves the
scoreboard itself, so it can be tuned mid-game without reflashing. That is a
measurement shortcut, not a second path to the API: the field devices still
report over ESP-NOW to the hub as in [ADR-0001](docs/adr/0001-esp-now-to-a-hub.md).

- Step-by-step build and measurement guide, macOS and Ubuntu:
  [docs/piezo-stufe-1-2.md](docs/piezo-stufe-1-2.md) — with breadboard drawings
  for [channel A](docs/images/piezo-channel-a-breadboard.svg) and
  [channel B](docs/images/piezo-channel-b-breadboard.svg)
- [`sketches/stufe1-piezo-test`](sketches/stufe1-piezo-test) — one piezo on
  GPIO 34, serial plotter plus a CSV event mode (`nr,spitze,anstieg_us,dauer_us,pause_ms`)
- [`sketches/stufe2-zaehlwerk-mvp`](sketches/stufe2-zaehlwerk-mvp) — two piezos
  (GPIO 34/35), full counting logic, scoreboard on `http://192.168.4.1`

Sensing runs as its own task pinned to core 0 in stage 2; the web server on core
1 would otherwise swallow bounces. Keep that split.

## Status

Early. `button` and `hub` are the path to a working scoreboard and are still
unwritten. `piezo` is being characterised on the bench first — see the stage
gates above; the firmware environment follows once the sensing is proven.