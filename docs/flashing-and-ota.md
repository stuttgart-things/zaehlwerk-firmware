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
router ([#14](https://github.com/stuttgart-things/zaehlwerk-firmware/issues/14)).

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
[ota] laeuft aus app0, Image auf Probe
[ota] OTA_TEST_CRASH: Absturz mit Absicht, der Bootloader rollt zurueck
...
[ota] laeuft aus app1, Image bestaetigt
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

## Doing it with task

`task` wraps all of the above with the checks that are easy to skip:

| | |
| --- | --- |
| `task setup` | write `secrets.ini`, passwords prompted without echo |
| `task doctor` | tools, board, serial port, passwords, wifi, git state |
| `task flash` | the USB flash, refusing if a monitor holds the port |
| `task boot` | reset and print the boot lines, so the slot and image state are visible |
| `task ota` | the wifi check first, then the upload |
| `task rollback` | the crash build, with the boot log straight afterwards |
| `task board` | ask a running board over HTTP what it is |
| `task wifi:esp` / `task wifi:back` | switch to the board's access point and back |

The wifi check in `task ota` exists because an upload from the wrong network
fails as `No response from the ESP`, which reads like a dead board rather than
a laptop on the wrong wifi.
