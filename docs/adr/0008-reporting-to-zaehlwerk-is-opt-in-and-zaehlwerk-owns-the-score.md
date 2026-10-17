# 8. Reporting to zaehlwerk is opt-in, and once joined zaehlwerk owns the score

Status: Proposed
Date: 2026-10-17

Number 0007 is taken by the check-in branch (#64, "a fob turns the next press
into a check-in"), which is not merged yet.

## Context

Up to v0.2.2 the piezo board is an island. It counts every rally itself
(`lib/game`), keeps its own sets, serve and half-to-player mapping
([ADR-0006](0006-the-firmware-owns-the-half-to-player-mapping.md)), shows the
result on its own page and sends a diagnostic log to the sink
([ADR-0004](0004-diagnostic-log-over-udp-to-a-sink.md)). Nothing reaches
`zaehlwerk-api`.

zaehlwerk has moved on in the meantime, and its decisions fix most of what the
board has to do:

- Every point is an ingest event, `source` plus a counter that only goes up,
  and a repeat is a duplicate answered with 200
  ([zaehlwerk ADR-0002](https://github.com/stuttgart-things/zaehlwerk/blob/main/docs/adr/0002-idempotent-ingest-contract.md)).
- zaehlwerk owns the phases and the ends. A device may send `half`
  (`left`/`right`) instead of `player`, and zaehlwerk resolves it against the
  ends as they stand. During warm-up and a change of ends a press from a half
  confirms that side instead of scoring
  ([zaehlwerk ADR-0005](https://github.com/stuttgart-things/zaehlwerk/blob/main/docs/adr/0005-match-phases-and-table-halves.md)).
  That supersedes ADR-0006 on the board's side.
- A correction is an ingest event too, `"action": "undo"` on the same counter;
  `POST /matches/{id}/undo` is not safe to retry
  ([zaehlwerk ADR-0012](https://github.com/stuttgart-things/zaehlwerk/blob/main/docs/adr/0012-taking-a-point-back-through-the-ingest-contract.md)).
- `GET /matches/current` marks a match zaehlwerk-pong started as `demo`
  ([zaehlwerk ADR-0008](https://github.com/stuttgart-things/zaehlwerk/blob/main/docs/adr/0008-demo-matches.md)).
- During a rollout the gateway in front of zaehlwerk answers 503 for a few
  seconds (#69).

The board is about to be flashed in a new office wifi, first with v0.2.2 as it
is, and only then upgraded. Whatever is added must not change a board that is
not told about zaehlwerk.

## Decision

**Reporting is opt-in, by address.** The board talks to zaehlwerk only when it
has an address for it: set on its page, kept in NVS, and seeded from
`ZW_API_URL` in `secrets.ini` only while NVS holds no key (#66). Released
images carry none, and CI asserts it. Without an address no task is started,
nothing is sent, nothing new is written to the diagnostic log, and the board
counts, shows and logs exactly as v0.2.2 does. Clearing the address on the
page returns it to that state without a restart.

**With an address the board joins, it does not create.** It asks
`GET /matches/current` every two seconds. No match, or a demo, and it waits
and keeps counting on its own; nothing it counts then is sent anywhere. A real
match and it joins it. The board never calls `POST /matches`; the match is
started on zaehlwerk's page, where the players come from Schmetterpause.

**Once joined, zaehlwerk owns the score.** Points, sets, serve, the players'
names and which player stands at which half come from zaehlwerk's answers.
The board still counts every rally itself, at once, so its page does not wait
for the network, and then takes zaehlwerk's answer in place of its own count.
An answer is only taken when it already contains every event the board has
queued since; an older one would take back a point the board just counted and
give it back a moment later. A difference between the board's count and
zaehlwerk's (an undo on the phone, a won set, the first answer after joining)
is written to the log as `link`/`adopted`.

**What goes over the wire** is the ingest contract and nothing else:

- a rally `lib/game` awarded: `half` and `delta: 1`;
- a rally it awarded to nobody (the single bounce): `delta: 0`, with the half
  the ball was last heard on, because the contract wants a side;
- a point given by hand on the page: `delta: 1`, in every phase, since it is
  what a button press is;
- "Letzten zurueck": `"action": "undo"`, every press, whatever the board had
  left to take back locally;
- always with `match_id`, so an undo can still reach a won match.

The board sends its **half, never a player**. Its local change of ends and
names stop mattering while it is joined, and the page hides the change-of-ends
button then.

**A detected rally is not sent during warm-up or a change of ends.** There a
press confirms a side, and a stray bounce while the players walk round the
table would confirm for them. A point given by hand is sent, as the
confirmation a person meant. The serve rally takes detected rallies: deciding
it is what it is for.

**Mock mode reports nothing and takes nothing.** Made-up hits do not belong in
somebody's match, and a simulated game keeps its own score.

**Every event waits in a queue until zaehlwerk has answered it**, in order,
with the id it was given when it was queued:

- The id comes from a counter in NVS (`ingest/event_id`, the same key the
  check-in uses on #64). It is stored before an event is first sent, not
  before it is queued: only an id that went out can collide with one sent
  after a reboot. If NVS cannot be written the event waits.
- No answer, no wifi, or any 5xx (the gateway's 502/503/504 during a
  rollout, or zaehlwerk's own 500): the same event again,
  after 1 s doubling to 30 s with up to half again at random. There is no
  limit on tries; a board on the table keeps trying while it is switched on.
  This also covers a join poll: a rollout never makes the board give up the
  match it is in. Retrying every 5xx is what the zaehlwerk piezo mock does,
  and the mock is the reference client; #69 named only 502/503/504.
- 404 or 409: the match is over or gone. That event and everything queued for
  the same match are dropped, each with a log record, and the board waits for
  the next match, or stays with a won one (below).
- Any other answer (400, 422, a 3xx, ...): zaehlwerk refused it and would again. It is
  dropped with a record, and the next event is not held up behind it.
- The queue holds 64 events. When it is full a new event is refused and
  logged, and what is queued stays: zaehlwerk then holds a gapless prefix of
  the match, which is easier to put right on the phone than a match with a
  hole in the middle. Sixty-four is more than a long outage in a game to 11.
- The queue lives in RAM. A reboot loses what was not yet delivered (never
  what was), and the counter carries on above every id that went out.

**A won match stays joined** until another match starts. `GET
/matches/current` no longer returns it, but the undo of the match point has to
name it, and that is the mis-press zaehlwerk keeps undo after a match for.
Points are not sent to it.

**The HTTP client runs as its own task on the core that does not sample**
(ADR-0003), today core 1 beside `loop()`, and a `static_assert` stops a build
that puts it on the sampler's core. Nothing in it touches the diagnostic log or
the game variables: it hands records and answers to `loop()`, which writes and
applies them. The rules (queue, retry, interpretation of answers, join and
demo, the address check) are in `lib/report` with native tests.

**https to the Gateway, trusting the edge root only; http for a laptop.** On
the table's box and in the lab zaehlwerk is only reachable as
`https://zaehlwerk.<edge domain>` through the Gateway; its http listener
redirects. So:

- An `https://` address is verified against one trust store,
  `TLS_TRUST_ANCHORS_PEM` in `include/tls_trust_anchors.h`, which holds the
  stuttgart-things edge root CA (EC P-384, valid to 2046) and nothing else,
  copied from harvester's `clusters/edge/edge-root-ca.crt`, the root the
  zaehlwerk piezo mock trusts. zaehlwerk is never on a public name, so no
  public root is in it. A second root (ISRG Root X1 for a Let's Encrypt name,
  say) would be one more PEM block in that same constant, added only with a
  reason written here, because every root in it can vouch for any address.
- mbedTLS checks the chain and the hostname, wildcards included
  (`*.edge.sthings.lab`, `*.edge-tt-test1.4sthings.tiab.ssc.sva.de`). There is
  no fallback to an unverified connection: a certificate that does not verify
  means no request (`tls` in the log), and the event waits like for no answer.
- The connection is kept open between requests (HTTP/1.1 keep-alive). The
  handshake is the expensive part, about 40 KB of heap and a second or two of
  CPU on core 1; polling every two seconds keeps one connection alive instead
  of paying it per request. A kept connection that the server closed while it
  idled is retried once at once on a fresh one, with the same event.
- `/zaehlwerk` reports free heap, the lowest since boot and the largest free
  block, so the cost can be read off a real board.
- A `.local` name is still refused: the box names things with dnsmasq, and the
  client does not ask mDNS.
- `http://` stays for a zaehlwerk on a laptop. An answer of 301/308 is named
  `redirect` on the page: an http address where the server wants https.

**The clock.** The board has no RTC. With an https address it starts SNTP
(the gateway DHCP handed out first, the Opal router on the box runs one, then
`pool.ntp.org`); the page shows whether the clock is set and the log has
`link`/`clock` once it is. An http address
starts no SNTP, so a board without https keeps the clock it had. The
diagnostic log's `t_us` stays boot-relative; the clock is not used for it.

The mbedTLS of the Arduino core this builds against (2.28.4, sdkconfig
without `MBEDTLS_HAVE_TIME_DATE`) does not check certificate validity dates
at all, so a board at 1970 verifies. https therefore does **not** wait for
the clock (`ZW_TLS_WAIT_FOR_TIME` 0): waiting would only add a way to report
nothing -- an unreachable NTP server -- without making the board any safer.
With a core that checks dates, `ZW_TLS_WAIT_FOR_TIME=1` holds https until the
clock is set (the event waits, nothing is dropped, the page says *Uhrzeit
fehlt*, the log has `link`/`unreachable` with `no_time`). The other side of
the same fact: an expired certificate is not refused by the board; trust
rests on the chain to the edge root and the hostname.

## Consequences

- A board without an address is the v0.2.2 board plus a card on its page that
  says so, and one line on serial.
- With an address and a match the board's number is zaehlwerk's number; the
  phone, the panel and the board agree, and an undo on the phone shows on the
  board within two seconds, together with who serves next. The next rally is
  judged against that serve.
- ADR-0006 is superseded while the board is joined: zaehlwerk keeps the ends,
  the board sends halves. Without a match the local mapping and the
  change-of-ends button still work as before.
- ADR-0006 expected the board to keep a `match_id` across a reboot. It does not
  need to: after a reboot it joins `GET /matches/current` again, which is the
  same running match. Only a won match is lost that way, and with it the undo
  of the match point from the board (the phone can still do it).
- Offline while joined, the board counts on its own and its score may differ
  from zaehlwerk's once it is back; zaehlwerk's wins. With a set length other
  than 11 the board stops counting at 11 until zaehlwerk answers.
- Every delivered event costs one NVS write, right after a rally has ended.
  Flash writes stall the caches of both cores for a moment, as the existing
  NVS writes from the page already do.
- One point is two requests at most: the ingest, and a poll only when nothing
  was sent for two seconds.
- TLS costs about 130 KB of flash (the image goes from 76 % to 86 % of its
  1.25 MB slot) and some 40 KB of heap while a connection is open.
- With https and no reachable NTP server the board reports nothing; its page
  says why.

## Left open

- Whether the board should score into a demo. zaehlwerk's own comment on
  `currentBody` says a board has no use for the flag, because a person hitting
  the ball into a demo ending it is the point. The board's piezos also hear
  things that are not a game, so this decision waits instead, like the
  zaehlwerk piezo mock does.
- A 500 that zaehlwerk keeps answering for one event (a bug on its side)
  holds the queue behind that event until zaehlwerk is fixed. That is the
  price of retrying every 5xx; the log shows it as one `retry` after another.
- The `match` diagnostic event carrying `match_id` (#42).
