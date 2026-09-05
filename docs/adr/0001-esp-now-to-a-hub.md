# 1. ESP-NOW to a hub instead of wifi per device

Status: Accepted
Date: 2026-09-05

## Context

Buttons sit on the table barrier and report a point. The obvious approach is
wifi plus an HTTP POST from each button.

Associating with an access point takes two to four seconds and draws around
100mA while it happens. At the table that is a button that appears not to have
worked, so it gets pressed again. It also assumes there is a wifi the button is
provisioned for, which fails as soon as the table moves to another location.

## Decision

Buttons and piezo units send over ESP-NOW to a hub. Only the hub holds wifi and
speaks HTTP to zaehlwerk-api.

A button wakes from deep sleep on the GPIO, sends, and sleeps again.

## Consequences

- A press reaches the hub in roughly ten milliseconds instead of seconds.
- Deep sleep at tens of microamps means a battery lasts a season rather than a
  weekend.
- No access point needed at the table. The hub can carry its own.
- The hub is a single point of failure and must be powered. Where the piezo unit
  is built its board is mains-powered anyway, so it may host the hub alongside
  the sensing. That is a choice about where the hub runs, not a second path to
  the API: every field device, the piezo unit included, reports over ESP-NOW,
  and only the hub holds wifi.
- Devices must be paired to the hub, so there is a provisioning step that plain
  wifi would not have needed.
- ESP-NOW gives no delivery guarantee, so senders retry. That is what makes the
  idempotent ingest contract in zaehlwerk ADR-0002 load-bearing rather than
  nice to have.