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

Without that file the OTA password is empty, and the firmware then refuses to
offer an update path at all rather than offering an open one.

```bash
pio run                          # build piezo and piezo-test
pio run -e piezo -t upload       # first flash, over USB
pio device monitor               # 115200 baud
pio test -e native               # run the rule tests on the laptop
```

| Env | What it is for |
| --- | --- |
| `piezo` | the firmware, flashed over USB |
| `piezo-test` | the same firmware, starting in mock mode |
| `piezo-ota` | the same firmware, sent over the air instead of over USB |
| `piezo-ota-crash` | a build that crashes on purpose, to test the rollback |
| `native` | the rule tests; `pio test -e native`, it has no program to build |

A bare `pio run` builds `piezo` and `piezo-test`. The two OTA environments
produce the same binary and differ only in how it gets onto the board.

## The PlatformIO extension in VS Code

Install **PlatformIO IDE** from the Extensions panel, then open this folder. The
first open downloads the toolchain and takes a few minutes; the little house
icon in the blue bar at the bottom opens the PlatformIO home.

Everything is in the **PlatformIO panel** in the left sidebar, under *Project
Tasks*. Each environment is its own group, and each group has *Build*, *Upload*
and *Monitor*. There is no environment picker to get wrong — you click the task
under the environment you mean.

The icons in the blue bar at the bottom act on the **default environments**,
which are `piezo` and `piezo-test`. For anything else, use the task under its
environment in the panel.

## Updating over the air

The USB flash happens once. After that the board can be updated over wifi, and
the USB cable is only needed for power and for the serial monitor.

1. Flash `piezo` over USB once, with an OTA password set in `secrets.ini`. The
   serial line has to say `[ota] bereit`. If it says `[ota] aus: kein
   Passwort`, there is no `secrets.ini` and nothing below will work.
2. Join the board's wifi — `Zaehlwerk`, password from `secrets.ini`.
3. *Upload* under **`piezo-ota`**, or:

```bash
pio run -e piezo-ota -t upload
```

The board is at `192.168.4.1` as long as it carries its own access point. That
is what `upload_port` in `platformio.ini` says; change it once the board joins a
router (#14).

**The first upload will make macOS ask whether Python may accept incoming
connections. Say yes.** espota does not push the firmware — it tells the board
to connect back to the laptop, so the laptop has to listen. That port is pinned
to 45678 so a firewall rule for it keeps working. A refused prompt looks exactly
like a board that is not answering.

The web UI has the same thing under *Firmware einspielen*: pick `firmware.bin`
from `.pio/build/piezo/`, type the OTA password, upload. That path needs no
PlatformIO at all, which is the point of having it.

Sampling stops before either kind of update starts, and the running rally is
closed rather than cut in half.

## Testing the rollback

New firmware is not trusted just because it arrived. It boots **on probation**:
if it does not run cleanly for ten seconds — wifi up, web server listening, the
sensor task actually turning — it is never confirmed, and the next reset puts
the previous version back. The web UI says so while it is happening.

The confirmation is ours to give, not the core's. The Arduino core would mark a
fresh image valid before `setup()` even runs, which would make the rollback
decoration; `verifyRollbackLater()` in `src/ota.cpp` takes that decision back.

To see it work, with USB plugged in for the serial monitor:

```bash
pio run -e piezo -t upload           # a good build, over USB
pio device monitor                   # leave this open
pio run -e piezo-ota-crash -t upload # over wifi, from a second terminal
```

Watch the monitor. The versions are identical on both slots, so the partition
name is what tells them apart:

```
[ota] laeuft aus ota_1, Image auf Probe
[ota] OTA_TEST_CRASH: Absturz mit Absicht, der Bootloader rollt zurueck
...
[ota] laeuft aus ota_0, Image bestaetigt
```

The board is back on the previous firmware without anybody touching it.

`OTA_TEST_CRASH` only fires on an image that is on probation, so a build
carrying it that is flashed over USB runs normally instead of hanging in a boot
loop. That is deliberate — the test is meant to be survivable.

The version and the short git hash are baked in on every build by
`scripts/version.py` — never typed into a header. They show on the serial line
at boot, at `/version`, at the foot of the web UI, and in the `session` event
once #15 lands, which is what ties an exported session to a commit months
later.

A build from a working tree with uncommitted changes to tracked files is marked
`-dirty`, in the version and in the hash. Untracked files do not count; they are
not in the build. There are no tags yet, so the version reads `0.0.0-dev` until
something is released.

All ESP32 environments use `default.csv`, the standard partition table with two
app slots. Two slots are what makes an over-the-air update and a rollback
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