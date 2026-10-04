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

## Tooling

Three things, and what each is for:

| | |
| --- | --- |
| [**Task**](https://taskfile.dev) | Every command anybody types is a task. `Taskfile.yaml` wraps `pio` with the checks that otherwise get forgotten — is a board plugged in, is a monitor holding the serial port, is the laptop even on the same network as the board |
| [**gum**](https://github.com/charmbracelet/gum) | The asking. Passwords are prompted without echo, a flash that overwrites asks first, and a status line reads as a status line |
| [**VS Code**](https://code.visualstudio.com) + [**PlatformIO IDE**](https://platformio.org/install/ide?install=vscode) | How it is built and flashed. The Arduino IDE is out of the loop; `sketches/` stays only as the record of the bench measurement |

```bash
brew install go-task charmbracelet/tap/gum
```

`pio` works on its own just as well — every task says which `pio` command it
runs. The full walkthrough, including the VS Code extension and the rollback
test, is in [docs/flashing-and-ota.md](docs/flashing-and-ota.md).

## The tasks

`task` on its own prints the short path from nothing to a running board.
`task --list` prints all of them.

### Before touching anything

| | |
| --- | --- |
| `task status` | Where everything stands: board, wifi and channel, laptop, sink, play. The question after a break, a move to another room, or a reflash |
| `task check` | Whether anything is **in the way**: tools, board, serial port, passwords, network, a dirty tree |

Different questions. `check` asks whether you can start; `status` says what you
are looking at.

### Onto the board

| | |
| --- | --- |
| `task flash` | The first flash, over USB. Writes **both** images — firmware and the web UI |
| `task ota` | Send new firmware over wifi. Checks the board answers first |
| `task ota:fs` | Send the web UI over wifi. Needed whenever `data/index.html` changed |
| `task boot` | Reset the board and read the boot lines: which slot, which image state, which network |
| `task monitor` | Serial monitor, 115200 |
| `task board` | Ask a running board over HTTP what it is |
| `task rollback` | Upload a deliberately broken build and watch the previous one come back |

### The diagnostic log

| | |
| --- | --- |
| `task sink` | Receive the log, write a day folder with one JSON per game, serve the viewer and the labelling pages on `:9001` |
| `task sink:games` | Split a session into one JSON per game. `SESSION=…/sessions/<file>.jsonl` |
| `task sink:export` | Zip one session with a summary beside it |
| `task sink:mock` | Send a made-up session at the sink — no board needed |

### Building and testing

| | |
| --- | --- |
| `task verify` | Lint, build and test — what has to be green before a commit |
| `task lint` | Exactly what CI lints: cppcheck, gofmt, go vet, python syntax |
| `task build` | Build `piezo` and `piezo-test` |
| `task test` | The rule tests, on the laptop |
| `task sink:test` | The sink's tests |

### Setup and one-offs

| | |
| --- | --- |
| `task setup` | Write `secrets.ini`: wifi and OTA passwords, prompted rather than echoed, never in the repository |
| `task wifi:esp` | Join the access point the board carries when its network is not there. Its name holds the chip id |
| `task wifi:back` | Rejoin the usual wifi |
| `task docs:serve` | Preview the TechDocs the way Backstage renders them |

## The web UI is a second image

The page is `data/index.html` and it is flashed into LittleFS, not compiled into
the firmware. So `pio run -t upload` alone leaves a board with no page — which is
why `task flash` runs `uploadfs` as well, and why there are two over-the-air
tasks rather than one.

A board with no filesystem is not bricked. It answers `/` with **HTTP 503** and a
recovery page that can upload either image itself, so the way back does not need
a cable.

## Continuous integration

| Workflow | When | What |
| -------- | ---- | ---- |
| [`ci`](.github/workflows/ci.yml) | push to `main`, every pull request | cppcheck, gofmt, `go vet`, python syntax, YAML parses, the rule tests, both firmware builds and the filesystem image |
| [`release`](.github/workflows/release.yml) | a `v*` tag | runs `ci` first, then attaches the artefacts **that run built** to the release |

`task lint` runs the same lint locally.

Released binaries are built in CI, where there is no `secrets.ini`. That keeps
wifi and OTA passwords out of a public download — and means the attached images
have OTA disabled, because the OTA password is a compile-time flag. Build your
own to get it back.

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

What the events look like: [docs/event-schema.md](docs/event-schema.md).

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

Sensing runs as its own task, so the web server cannot swallow bounces. Stage 2
pins it to core 0 and that is the wrong core — the wifi and lwIP tasks live
there too, so the split protects the sampler from the web server and hands it to
the radio. [ADR-0003](docs/adr/0003-sampling-on-core-1.md) moves it to core 1.
The firmware still does the old thing until [#10](../../issues/10) lands; keep
the split, swap the sides.

## Status

Early. `button` and `hub` are the path to a working scoreboard and are still
unwritten. `piezo` carries the stage 2 bench firmware and is where the work is:
counting accuracy at the table is around half, and whether that is missed
bounces, phantom hits, the wrong side or the scoring logic is not yet known.
Diagnostic logging is what answers it.