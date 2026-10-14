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

Per-unit configuration — source id, hub MAC, wifi — is set by build flag, not
by editing source. The wifi and the zaehlwerk address can also be set on the
board's page and are then kept in NVS.

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

## Reporting to zaehlwerk-api

**Off unless the board is given an address.** Without one the board counts on
its own, shows the score on its page and sends the diagnostic log to the sink,
exactly as v0.2.2 does: no task is started, nothing is sent to zaehlwerk and
nothing new goes into the log. Released images carry no address, and CI checks
that. Why it is built this way, and who owns the score once the board is
connected:
[ADR-0008](docs/adr/0008-reporting-to-zaehlwerk-is-opt-in-and-zaehlwerk-owns-the-score.md).

### Setting the address

On the board's page, card **Zaehlwerk-Server**: enter the address and press
*Speichern*. On the table's box and in the lab that is
`https://zaehlwerk.<edge domain>`, zaehlwerk behind the Gateway; for a
zaehlwerk on a laptop `http://<laptop IP>:8080`. It applies at once, no restart,
and is kept in NVS. *Trennen* clears it, and it stays cleared across a reboot.
Or from a laptop on the same network:

```bash
curl -4 -s -X POST --data-urlencode "url=https://zaehlwerk.edge.sthings.lab" http://zaehlwerk.local/zaehlwerk/url
curl -4 -s -X POST --data-urlencode "url=" http://zaehlwerk.local/zaehlwerk/url   # clear
curl -4 -s http://zaehlwerk.local/zaehlwerk                                        # state as JSON
```

The board refuses, and says why: anything but `https://` or `http://`, a
`.local` name (its HTTP client asks DNS, not mDNS: use the IP or a name
dnsmasq or the router's DNS knows), a path after the port, spaces or umlauts.

**https** is verified against the stuttgart-things edge root CA and nothing
else (`include/tls_trust_anchors.h`, a public certificate copied from
harvester's `clusters/edge/edge-root-ca.crt`), hostname included. There is no
insecure fallback: a certificate that does not chain to the edge root means no
request, and the card says *Zertifikat passt nicht zur Edge-Root-CA*. The board
sets its clock over SNTP (the DHCP gateway, then `pool.ntp.org`), but https
does not wait for it: this core's mbedTLS never checks validity dates
(ADR-0008, `ZW_TLS_WAIT_FOR_TIME`). The card shows whether the clock is set. To check the embedded root against the source, and the Gateway against
it:

```bash
sed -n '/^-----BEGIN/,/^-----END/p' include/tls_trust_anchors.h > /tmp/edge-root.pem
cmp /tmp/edge-root.pem ../harvester/clusters/edge/edge-root-ca.crt
openssl s_client -connect <gateway IP>:443 -servername zaehlwerk.<edge domain> \
  -CAfile /tmp/edge-root.pem -verify_hostname zaehlwerk.<edge domain> </dev/null | grep Verify
```

`/zaehlwerk` also reports `heap_free`, `heap_min` and `heap_block`: a TLS
connection needs about 40 KB, and is kept open between requests.

A board built from source can be seeded from `secrets.ini`
(`-DZW_API_URL='"https://zaehlwerk.edge.sthings.lab"'`, see
`secrets.ini.example`). Like the
wifi credentials, the seed is used only while NVS holds no address; once one is
set or cleared on the page, that wins. `-DZW_INGEST_SOURCE` names the board in
the ingest contract (default: `ZW_DEVICE_ID`, `piezo-1`); two boards need two
names.

### What the page shows

Under the score, once an address is set: `⇄ Zaehlwerk-Match <id>` while the
board is in a match (with the phase, *Einspielen*, *um die Angabe*,
*Seitenwechsel*, and how many events wait), or *kein Match*, *Demo laeuft*,
*nicht erreichbar*. The card says the same in full: not configured / waiting
for a match / a demo running / in match `<id>` with the players / match over /
server unreachable with the reason and when it tries again, and below it the
address, the source, how many events were delivered, are waiting and were
dropped, the last `event_id` and the last error.

### What the board does with an address

- It asks `GET /matches/current` every two seconds. No match, or a demo, and it
  waits and keeps counting on its own; nothing is sent. A real match, started
  on zaehlwerk's page, and it joins it. The board never creates a match.
- Joined, every rally `lib/game` awards goes to `POST /ingest/piezo` as the
  board's half (`left`/`right`) with `delta: 1`; a rally it awards to nobody as
  `delta: 0`; *Punkt A/B* on the page as `delta: 1`; *Letzten zurück* as
  `"action": "undo"`. Never `POST /matches/{id}/undo` (#67).
- zaehlwerk owns the score. The board still counts at once, then takes points,
  sets, serve, names and ends from zaehlwerk's answer, so an undo on the phone
  shows on the board within two seconds, and the next rally is judged against
  zaehlwerk's server. *Seiten wechseln* is hidden while joined: zaehlwerk keeps
  the ends.
- During warm-up and a change of ends a detected rally is not sent, because
  there a press confirms a side; a point given by hand is.
- Nothing is sent in mock mode.
- No answer, no wifi, or any 5xx (a rollout's 502/503/504, or a 500): the same
  event again with the same `event_id`, after 1 s doubling to 30 s. Events wait
  in order in a queue of 64. A 404/409 (match over) drops what was queued for
  that match; any other refusal drops that one event. Every step is in the
  diagnostic log as `ingest` and `link` events
  ([docs/event-schema.md](docs/event-schema.md)).
- The `event_id` counter is in NVS and stored before an event first goes out,
  so a reboot never sends an id twice. The queue itself is RAM: a reboot loses
  what was not yet delivered.

### Trying it

**Against a zaehlwerk on the laptop.** In a checkout of
[zaehlwerk](https://github.com/stuttgart-things/zaehlwerk):

```bash
go build -o /tmp/zaehlwerk-api ./cmd/zaehlwerk-api
HTTP_ADDR=0.0.0.0:8080 /tmp/zaehlwerk-api
```

Give the board `http://<laptop IP>:8080` (the laptop's address on the board's
wifi, not `localhost`). Start a match on `http://localhost:8080/` or with
`curl -s -X POST -d '{"players":["Pat","Ana"],"best_of":3}' localhost:8080/matches`.
Within two seconds the card says *Im Match*, and a tap on the table or *Punkt A*
on the board shows up on zaehlwerk's page. An undo there shows up on the board.
A firewall on the laptop has to let port 8080 in.

**Against the lab's zaehlwerk.** `https://zaehlwerk.<edge domain>` on the
card, a name the board's DNS resolves to the Gateway. The card should show
*TLS, Uhrzeit gesetzt* and then *Im Match* once a match runs. Only a match started there is joined. To check #69, the rollout: with a
match running that nobody is really playing, `kubectl -n zaehlwerk rollout
restart deploy/zaehlwerk`, keep scoring, and compare zaehlwerk's events with
the sink's `ingest` records afterwards: no point missing, none twice. The log
shows one `link`/`unreachable` and one `link`/`back` around the gap.

**Upgrading a board that runs v0.2.2.** Flash this build without an address
first; it behaves as v0.2.2 did. Then set the address on the page. Clearing it
puts the board back.

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

## Check-in by fob

A key fob held to a reader at the hub, then a press on your own button, checks
you in at that half ([#63](../../issues/63),
[ADR-0007](docs/adr/0007-a-fob-turns-the-next-press-into-a-check-in.md)). Two
halves checked in and zaehlwerk starts the match
([zaehlwerk ADR-0010](https://github.com/stuttgart-things/zaehlwerk/blob/main/docs/adr/0010-check-in-per-table-half.md)).

1. A fob is read. The green LED blinks: press your button, within five seconds.
2. The next press of a paired button names the half. That press is a check-in,
   not a point.
3. The board sends `POST /ingest/checkin` once and shows the answer.
4. No press in time: the fob is forgotten, two short red blinks.

While a rally is open, or a game stands between its first point and its end, a
fob is refused at once and the press after it stays a point.

**Only with zaehlwerk's token.** zaehlwerk takes a check-in only with its
`CHECKIN_TOKEN` as a bearer ([#65](../../issues/65), zaehlwerk#113), so the
board sends `Authorization: Bearer <token>` on every check-in. Points carry no
token. Without one on the board no check-in is sent at all: startup and the page
say so, and a fob gets "rejected".

**The UID is never logged or stored.** Not on serial, not in the diagnostic log,
not in NVS. It lives in RAM for the window and the one request. The log says
`checkin`, the half and the outcome. Our own fobs only: MIFARE Classic 1K or NTAG.

### Wiring

An RC522 on VSPI, beside the piezo inputs on 34 and 35, which it does not touch:

| RC522 | ESP32 | |
| --- | --- | --- |
| SDA | GPIO 5 | chip select |
| SCK | GPIO 18 | |
| MOSI | GPIO 23 | |
| MISO | GPIO 19 | |
| RST | GPIO 22 | |
| IRQ | — | not connected, the reader is polled |
| 3.3V | 3V3 | **not 5 V** |
| GND | GND | |
| green LED | GPIO 25 | through a resistor (220–330 Ω) to GND |
| red LED | GPIO 26 | the same |

Every pin is a build flag in `include/config.h`. The range is one to three
centimetres, so the reader goes at the table edge by the net, on a short cable,
not under the top.

### What the LED says

| | Green | Red |
| --- | --- | --- |
| Press your button | blinks | |
| Sending | on | on |
| Checked in (`claimed`, `unchanged`, `replaced`, `moved`, `started`) | on, 2 s | |
| Unknown fob (404) | | on, 2 s |
| Refused: a match is running (409), play is going on here, a `duplicate`, a wrong token (401/403) or none on the board | | blinks, 2 s |
| Unavailable: zaehlwerk or Schmetterpause not answering (503, 5xx, no wifi), check-in off | | blinks fast, 2 s |
| Nobody pressed in time | | two short blinks |

### Configuration

| Build flag | Default | |
| --- | --- | --- |
| `ZW_API_URL` | empty | zaehlwerk-api, `http://10.0.0.5:8080`. Empty: check-in is off and a fob gets "unavailable". An IP or a DNS name; `.local` is not resolved |
| `ZW_CHECKIN_TOKEN` | empty | zaehlwerk's `CHECKIN_TOKEN`. Seeds the board only while NVS holds none; see below |
| `ZW_INGEST_SOURCE` | `ZW_DEVICE_ID` | the `source` of the ingest contract. The `event_id` behind it is kept in NVS |
| `ZW_CHECKIN_WINDOW_MS` | `5000` | how long a fob waits for a press |
| `ZW_FOB_CORE` | the core that does not sample | never `ZW_SENSOR_CORE`; the build fails if it is |

### The token

The token is kept like the wifi credentials, not like the OTA password: in NVS,
set on the board's page. A released image is built in CI without `secrets.ini`
and carries none, so this is how it gets one.

- **On the page:** card "Check-in per Fob", paste the token, "Token speichern".
  It applies at once, no restart. "Token loeschen" clears it.
- **With curl**, from a variable so it stays out of the shell history:

  ```bash
  curl -4 -s -X POST --data-urlencode "token=$CHECKIN_TOKEN" http://zaehlwerk.local/checkin/token
  curl -4 -s -X POST --data-urlencode "token=" http://zaehlwerk.local/checkin/token   # clear
  ```

- **From `secrets.ini`:** `-DZW_CHECKIN_TOKEN='"…"'` seeds a board whose NVS has
  no token key. Once a token is set or cleared on the page, that wins; a cleared
  one stays cleared until NVS is erased.

**The token is never shown back and never logged.** The page and `/checkin` say
`"token": true` or `false`, never the value, and the field is empty after
saving. Serial and the diagnostic log say whether one is set and where it came
from, never what it is. A token with a space, a line break or a character
outside printable ASCII is refused rather than stored; at most 256 characters.

The board's page has no login, as for the wifi credentials: anybody on the
network can set or clear the token, but nobody can read it back.

### Until the buttons exist

The press is meant to come from a paired ESP-NOW button (#58, #59), and neither
exists yet. Until then the board's page stands in for one:

```bash
curl -4 -s 'http://zaehlwerk.local/checkin'           # on, api, token set, reader, LED, last outcome
curl -4 -s 'http://zaehlwerk.local/checkin?press=A'   # a press at half A
```

`?press=` only ever checks in. With no fob waiting it answers `"taken": false`
and is not a point.

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
| [0007](docs/adr/0007-a-fob-turns-the-next-press-into-a-check-in.md) | A fob turns the next press into a check-in |
| [0008](docs/adr/0008-reporting-to-zaehlwerk-is-opt-in-and-zaehlwerk-owns-the-score.md) | Reporting to zaehlwerk is opt-in, and once joined zaehlwerk owns the score |

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