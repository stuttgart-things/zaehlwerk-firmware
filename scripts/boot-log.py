#!/usr/bin/env python3
"""Reset the board and read what it says on the way up.

The boot lines are the only place that names the slot the firmware is running
from and whether its image is confirmed, and they are over before a monitor can
be opened. So: trigger the reset, and read from the first byte.

    python3 scripts/boot-log.py /dev/cu.usbserial-0001 14

The default of fourteen seconds is not arbitrary. A freshly uploaded image is
on probation for ten, so the line confirming it arrives after that.

With --listen nothing is reset, it only listens. That is the rollback case:
there the board restarts on its own, and a reset of ours would cut in half the
sequence the test exists to show.
"""

import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing. Run it through `task boot`, which uses the "
             "python PlatformIO ships, where it is installed.")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    listen_only = "--listen" in sys.argv[1:]

    port = args[0] if args else "/dev/cu.usbserial-0001"
    seconds = float(args[1]) if len(args) > 1 else 14.0

    try:
        s = serial.Serial(port, 115200, timeout=0.2)
    except Exception as e:
        sys.exit("Cannot open %s: %s\nIs a monitor still holding it?" % (port, e))

    if listen_only:
        print("Listening for %gs, resetting nothing ...\n" % seconds)
    else:
        # DTR low means IO0 high: boot normally, not into the bootloader.
        # RTS high briefly pulls EN to ground — that is the reset.
        s.dtr = False
        s.rts = True
        time.sleep(0.12)
        s.rts = False
        print("Reset sent, reading for %gs ...\n" % seconds)

    raw = b""
    until = time.time() + seconds
    while time.time() < until:
        raw += s.read(4096)
    s.close()

    text = raw.decode("utf-8", "replace")

    # The CP2102 driver flushes buffer noise on reset. It is not from the board
    # and it buries the two lines that matter.
    lines = []
    for line in text.splitlines():
        clean = re.sub(r"[^\x20-\x7e]", "", line).strip()
        # The noise arrives as a long run of x and spaces, and the first real
        # line is often glued to the end of it. Cut the run off rather than
        # dropping the line with it.
        clean = re.sub(r"^[x ]{8,}", "", clean)
        if len(clean) > 3 and re.search(r"[A-Za-z]{3}", clean) \
                and not re.fullmatch(r"[x ]+", clean):
            lines.append(clean)

    # We triggered the reset, so everything before the last ROM banner is the
    # driver flushing what it still held from the previous run. Drop it. While
    # listening we keep all of it: there the restarts are the story.
    if not listen_only:
        starts = [i for i, l in enumerate(lines) if "ets Jul" in l]
        if starts:
            lines = lines[starts[-1]:]

    # The same line many times over: show it once, with a count. Only from three
    # repeats — for one or two the note is longer than the lines it replaces,
    # and four identical hits in a row are four events worth seeing.
    FOLD_FROM = 3

    def flush(out, line, repeats):
        if repeats >= FOLD_FROM:
            out.append("  ... repeated %dx" % repeats)
        else:
            out.extend([line] * repeats)

    previous, repeats = None, 0
    folded = []
    for line in lines:
        if line == previous:
            repeats += 1
            continue
        if previous is not None:
            flush(folded, previous, repeats)
        repeats = 0
        folded.append(line)
        previous = line
    if previous is not None:
        flush(folded, previous, repeats)

    for line in folded:
        print(line)

    if not raw:
        if listen_only:
            print("(stayed quiet — the board did not restart in that time)")
        else:
            print("(nothing received — is the board plugged in?)")
        return

    # While listening, two starts are expected: the new image, and the one that
    # replaces it. Only beyond that is it a loop.
    limit = 4 if listen_only else 3
    starts = text.count("ets Jul")
    if starts > limit:
        print("\nWARNING: %d restarts in %gs — that is a boot loop."
              % (starts, seconds))


if __name__ == "__main__":
    main()
