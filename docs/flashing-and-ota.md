# Flashing and updates

Getting firmware onto the board, and getting the next version on there without
a cable. Everything here is also available as a `task`, which adds the checks
that are easy to forget — see [the task list](#doing-it-with-task) at the end.

## Building and flashing

Once, to get a place for wifi and OTA credentials that is not the repository:

```bash
task setup
```

That prompts for both passwords without echoing them and writes `secrets.ini`,
which is gitignored. By hand it is `cp secrets.ini.example secrets.ini` and an
editor.

The build works without that file — it falls back to the defaults in
`include/config.h` — but the OTA password is then empty, and the firmware
refuses to offer an update path at all rather than offering an open one.

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

## Two images, not one

The web UI is `data/index.html`, flashed into the LittleFS partition. It is not
compiled into the firmware, so there are two things to write:

```bash
pio run -e piezo -t upload     # the firmware
pio run -e piezo -t uploadfs   # the web UI
```

`task flash` does both. Over the air the same split applies: `task ota` for the
firmware, `task ota:fs` for the page.

Forgetting the second one is the obvious mistake, so it is caught rather than
punished: a board with no `/index.html` answers `/` with **HTTP 503** and a
recovery page that can upload either image itself. Verified on hardware — a
firmware-only OTA produced exactly that page, and `/updatefs` with
`littlefs.bin` (1.4 MB, 5.5 s) brought the real one back byte for byte.

One asymmetry worth knowing: **the filesystem has no second slot and no
rollback.** Firmware lives in `app0`/`app1` and a bad image is reverted by the
bootloader; the filesystem partition is written in place. A broken page therefore
stays broken until a new one is uploaded — which is what the recovery page is
for. LittleFS is unmounted before the write, because overwriting the partition
under a mounted filesystem means serving from blocks that no longer hold what it
thinks they do.

## Updating over the air

The USB flash happens once. After that the board can be updated over wifi, and
the USB cable is only needed for power and for the serial monitor.

1. Flash `piezo` over USB once, with an OTA password set in `secrets.ini`. The
   serial line has to say `[ota] ready`. If it says `[ota] off: no password`,
   there is no `secrets.ini` and nothing below will work.
2. Nothing, if the board joined your network — it answers to
   `zaehlwerk.local`. If it fell back to its own access point, join that; its
   name carries the chip id (`Zaehlwerk-c4e32c`) and `task boot` prints it.
3. *Upload* under **`piezo-ota`**, or:

```bash
pio run -e piezo-ota -t upload
```

`upload_port` in `platformio.ini` is `zaehlwerk.local`, which resolves over
Bonjour once the board has joined a network. While it carries its own access
point instead, point the tasks at it:

```bash
task ota ESP_HOST=192.168.4.1
```

## Which network is it on

Station mode is the normal case: the board joins the configured wifi and the
laptop stays where it is — no switching, which also lets the log sink listen at
the same time. If the network does not come up within the timeout (fifteen
seconds by default) the board carries its own access point instead, and the web
UI says which of the two happened, on what address, and **on which channel**.

The channel is on the page because ESP-NOW peers have to sit on the channel the
radio ended up on. A peer on the wrong channel reports a successful send into
nothing, and that is not diagnosable from the peer.

Credentials are seeded once from `secrets.ini` and can be changed in the web UI
afterwards, which stores them in NVS and restarts. A wrong password is not a
lockout: after the timeout the board is back on its own access point.

```bash
task board      # which network, address and channel, over HTTP
```

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

Watch the monitor. **The two slots alternate**, so do not look for a fixed
name: whichever slot is running now, the crash build goes into the other one and
the rollback brings the current one back. Running from `app1`, it reads:

```
[ota] running from app0, image on probation
[ota] OTA_TEST_CRASH: crashing on purpose, the bootloader will roll back
...
[ota] running from app1, image confirmed
```

What matters is the pair: a slot **on probation** that is replaced by a slot
**confirmed**, with no hand on the board.

The two app slots in `default.csv` are **labelled** `app0` and `app1`;
`ota_0` and `ota_1` are their subtypes, which is what the partition table lists
and what nothing ever prints. A fresh USB flash lands in `app0` and reports
`Image ohne Kennzeichnung`, because a flash over the wire sets no OTA state —
only an update does.

The board is back on the previous firmware without anybody touching it.

`OTA_TEST_CRASH` only fires on an image that is on probation, so a build
carrying it that is flashed over USB runs normally instead of hanging in a boot
loop. That is deliberate — the test is meant to be survivable.

The version and the short git hash are baked in on every build by
`scripts/version.py` — never typed into a header. They show on the serial line
at boot, at `/version`, at the foot of the web UI, and in the `session` event
once [#15](https://github.com/stuttgart-things/zaehlwerk-firmware/issues/15) lands, which is what ties an exported session to a commit months
later.

A build from a working tree with uncommitted changes to tracked files is marked
`-dirty`, in the version and in the hash. Untracked files do not count; they are
not in the build. There are no tags yet, so the version reads `0.0.0-dev` until
something is released.

All ESP32 environments use `default.csv`, the standard partition table with two
app slots. Two slots are what makes an over-the-air update and a rollback
possible at all — do not swap it for a single-slot table to win flash.


## The OTA password is printed in the clear

PlatformIO calls espota with `--debug`, and espota prints its options — the
password among them — on **every** upload:

```
[DEBUG]: Options: {'esp_ip': '192.168.4.1', ..., 'auth': 'the-password', ...}
```

It therefore lands in terminal scrollback, in any CI log, and in `ps` while the
upload runs. That is espota's behaviour, not something the firmware chooses.
Treat the OTA password as visible to anyone who can read your terminal, and do
not reuse a password from anywhere else for it.

## Released binaries have OTA switched off

The binaries attached to a GitHub release are built by the `release` workflow, in
a checkout that has no `secrets.ini`. Since the OTA password is a compile-time
flag, those images fall back to the empty default — and an empty password means
`ota::enabled()` is false, so `/update` answers `503 OTA ist aus`.

That is on purpose. The alternative was what happened to `v0.1.0`: a binary built
on a laptop, published publicly, carrying that laptop's wifi SSID, wifi password,
access point password and OTA password as plain strings. `strings` on the
download was enough to read them. The asset was deleted and the passwords have to
be treated as public.

To get OTA back, write your own `secrets.ini` (`task setup`) and build. Making the
OTA password settable at runtime — the way wifi credentials already are, in NVS —
would remove the trade-off entirely.

## Doing it with task

`task` wraps all of the above with the checks that are easy to skip:

| | |
| --- | --- |
| `task setup` | write `secrets.ini`, passwords prompted without echo |
| `task status` | where everything stands: board, wifi and channel, laptop, sink, play |
| `task check` | tools, board, serial port, passwords, wifi, git state |
| `task flash` | the USB flash, refusing if a monitor holds the port |
| `task boot` | reset and print the boot lines, so the slot and image state are visible |
| `task ota` | checks the board answers first, then uploads |
| `task rollback` | the crash build, listening on the serial line from before the upload so the crash and the rollback are visible as they happen |
| `task board` | ask a running board over HTTP what it is |
| `task board` | which network, address and channel it is on |
| `task wifi:esp` / `task wifi:back` | join the board's own access point and come back |

`task status` and `task check` answer different questions. `check` asks whether
anything is in the way before you start; `status` answers "what am I looking at",
which is what somebody asks after a break, a move to another room, or a reflash:

```
  Board    192.168.10.109   git 4cba79b   built 2026-09-30T05:24Z
           wifi "sthings", channel 9, zaehlwerk.local
  Laptop   192.168.10.136   same network
  Log      192.168.10.136:9000 answers
           session e32c868a
  Sink     running here, pid 27707
  Play     mock, A vs B, 0:0, set 1
```

Whether the laptop is on the same network as the board is the line that decides
whether an upload or the log can reach anywhere, so it is stated rather than left
to be worked out from two addresses.

The reachability check in `task ota` exists because an upload to a board that
is not there fails as `No response from the ESP`, which reads like dead hardware
rather than a name that did not resolve or a laptop on another network.
