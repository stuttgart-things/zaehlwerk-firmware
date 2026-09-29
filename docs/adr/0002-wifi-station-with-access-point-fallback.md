# 2. Wifi station with an access point fallback

Status: Accepted
Date: 2026-09-29

## Context

The stage 2 rig opens its own access point and serves the scoreboard on
`192.168.4.1`. That was the right call on the bench: no router, no
provisioning, a phone connects and the page is there.

It stops being right the moment the unit has to reach `zaehlwerk-api`. The
piezo board is mains-powered and hosts the hub alongside the sensing
([ADR-0001](0001-esp-now-to-a-hub.md)), and the hub is the one device that
holds wifi and speaks HTTP. A device that only ever carries its own access
point has no route to the API at all.

It also costs the laptop. Labelling a session means the sink runs on a laptop
that has to see the ESP32's log packets, and a phone joined to the ESP32's own
access point cannot be on the office wifi at the same time.

But the table moves. It goes to a tournament, to somebody's cellar, to a room
where the wifi password is on a sticker nobody can find. A unit that only joins
a configured network is a unit that is dead in a new room.

## Decision

Station mode is the normal case. The unit joins the configured network at boot
and registers as `zaehlwerk.local` over mDNS.

If the association does not come up within a timeout, the unit opens its own
access point named `Zaehlwerk-<chipid>` and serves the same web UI there. The
fallback is not a failure state — it is the tournament case, and the UI says
which mode is running, on which address and on which wifi channel.

The channel is on the page because ESP-NOW peers have to sit on the channel the
access point put the radio on. A button reporting a successful send into
nothing is the failure this prevents, and it is not diagnosable from the button.

## Consequences

- Credentials have to come from somewhere. They are set in the web UI and kept
  in NVS, and a build flag from `secrets.ini` seeds the first one; nothing goes
  in the repository.
- Two addresses means two ways to reach the same page, which is confusing
  exactly once. mDNS makes `zaehlwerk.local` work in both modes.
- The timeout is a tuning knob with a real cost either way: too short and a slow
  router loses the unit to its own access point, too long and the tournament
  case waits. It starts at 15 seconds and sits in the settings.
- The sink and the API both become reachable from the unit, which is what makes
  diagnostic logging and idempotent ingest possible at the same time.
