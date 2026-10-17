# 7. A fob turns the next press into a check-in

Status: Accepted
Date: 2026-10-14

## Context

Players are to check in at a half of the table with a key fob
([#63](https://github.com/stuttgart-things/zaehlwerk-firmware/issues/63)). The
hardware decision is in
[#61](https://github.com/stuttgart-things/zaehlwerk-firmware/issues/61): one
RC522 per table, at the hub, because the hub is mains powered and a reader that
draws a hundred milliamps while it looks for a card does not belong in a battery
button. zaehlwerk decided its side in its
[ADR-0010](https://github.com/stuttgart-things/zaehlwerk/blob/main/docs/adr/0010-check-in-per-table-half.md):
`POST /ingest/checkin` with a source, an event id, a half and the UID.

A reader in the middle of the table knows a card was held up. It does not know
for which half. Two readers would know, at twice the parts and two cables, and
the buttons already know: each one is paired to a half
([#59](https://github.com/stuttgart-things/zaehlwerk-firmware/issues/59)).

A UID is not harmless. A company badge held to the reader by mistake produces
one, and so does any card in a wallet.

## Decision

**A fob opens a short window, and the next press of a paired button in it is a
check-in at that button's half, not a point.** One press, one request. The
window is five seconds by default (`ZW_CHECKIN_WINDOW_MS`). No press in it and
the fob is forgotten; the LED says so. A second fob in the window replaces the
first and starts it again.

**The half is the physical half.** Half A is `left`, which zaehlwerk makes
player `a`. This is not the mapping of ADR-0006 applied to a check-in: there is
no match yet, and zaehlwerk resolves the halves itself when the second one is
claimed. The firmware sends what it knows, which is where the button is.

**While play is going on, a fob does nothing**, and the LED shows it was
refused. "Going on" is what this board can see for itself: a rally open, or a
game between its first point and its end. The press after it then stays a point,
which is the whole reason to decide this on the board rather than leave it to
zaehlwerk's 409 — by the time that arrives, a press meant as a point has already
been spent on a check-in. Whether a match is running at zaehlwerk is
zaehlwerk's to say, and it does.

**The request follows the ingest contract**: `source` is `ZW_INGEST_SOURCE`,
and `event_id` is a counter kept in NVS and written before the request goes, so
neither a retry nor a reboot reuses one for a different check-in. A request
nobody answered is sent again with the same id, at most three times. One that
was answered is not: an answer, whatever it says, is what a retry would get back
again.

**The UID lives in RAM, for the window and the one request, and goes nowhere
else.** Not to serial, not to the diagnostic log or the sink (ADR-0004), not to
NVS. The driver's copy is cleared as soon as it is read, the window's copy is
zeroed when it is handed over, and the request body is zeroed once it is sent.
The log says `checkin`, the half and the outcome. The functions that build log
lines take no UID, so there is nothing to forget to leave out. Whether a fob is
known is decided by Schmetterpause; the board holds no list of fobs.

**Only with zaehlwerk's token**
([#65](https://github.com/stuttgart-things/zaehlwerk-firmware/issues/65)).
zaehlwerk takes a check-in only with its `CHECKIN_TOKEN` as a bearer, because a
UID is no secret and a check-in names a player. The board keeps the token the
way it keeps the wifi credentials: in NVS, set on its page, seeded from
`secrets.ini` only while NVS has none, so a released image can be given one. It
follows the UID's rule: never shown back, never logged. Without one the board
sends no check-in, and a fob is rejected at once rather than held for a press.
A 401 or 403 is the token, not the fob, and reads as rejected, not unknown.

**The reader is polled on the core that does not sample** (ADR-0003). Which core
that is follows `ZW_SENSOR_CORE`, and a build that puts the two together does not
compile.

## Consequences

- A press means two things now, and which one depends on something that
  happened up to five seconds earlier at the other end of the table. That is the
  price of one reader instead of two. The LED blinking green is what tells the
  player which one it is.
- A stray fob between rallies, with no point on the board yet, takes the next
  press. The window is short, the LED blinks while it is open, and zaehlwerk
  refuses the check-in while a match is running — but the press is spent. A
  game that has scored at least one point is protected; the first serve of one
  is not.
- The board's own score decides "play is going on". A game abandoned at 3:2 and
  never reset refuses every fob until somebody starts a new one.
- The press has to come from a paired button. Until the ESP-NOW receive path
  (#58) and pairing (#59) exist, `/checkin?press=A` stands in for one, and does
  nothing else: with no fob waiting it is not a point.
- Retrying only an unanswered request means a check-in that zaehlwerk could not
  resolve (503) is not tried again by the board. The player holds the fob up
  again, which is what zaehlwerk's ADR-0010 expects.
