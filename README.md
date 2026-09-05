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

## Status

Early. Start with `button` and `hub` — that is the path to a working scoreboard.
`piezo` comes once the sensing actually works.