#!/usr/bin/env python3
"""Das Board zurücksetzen und mitlesen, was es beim Start sagt.

Die Bootzeilen sind die einzige Stelle, an der steht, aus welchem Slot die
Firmware läuft und ob ihr Image bestätigt ist — und sie sind vorbei, bevor ein
Monitor offen ist. Also: Reset auslösen und von der ersten Zeile an mitlesen.

    python3 scripts/boot-log.py /dev/cu.usbserial-0001 14

Mit --listen wird nicht zurückgesetzt, sondern nur zugehört. Das ist der Fall
beim Rückroll-Test: dort startet das Board von selbst neu, und ein eigener
Reset würde die Geschichte zerschneiden, die man gerade sehen will.

Die Voreinstellung von 14 Sekunden ist kein Zufall: die Bewährungsfrist für ein
frisch eingespieltes Image liegt bei zehn, die Bestätigung kommt also erst
danach.
"""

import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial fehlt. Aufruf ueber `task boot`, das nimmt das Python "
             "von PlatformIO, in dem es steckt.")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    nur_lauschen = "--listen" in sys.argv[1:]

    port = args[0] if args else "/dev/cu.usbserial-0001"
    seconds = float(args[1]) if len(args) > 1 else 14.0

    try:
        s = serial.Serial(port, 115200, timeout=0.2)
    except Exception as e:
        sys.exit("Port %s nicht zu oeffnen: %s\nLaeuft noch ein Monitor darauf?"
                 % (port, e))

    if nur_lauschen:
        print("Hoere %g s zu, ohne zurueckzusetzen ...\n" % seconds)
    else:
        # DTR niedrig heisst IO0 hoch: normal starten, nicht in den Bootloader.
        # RTS kurz hoch zieht EN auf Masse — das ist der Reset.
        s.dtr = False
        s.rts = True
        time.sleep(0.12)
        s.rts = False
        print("Reset ausgeloest, lese %g s mit ...\n" % seconds)
    raw = b""
    ende = time.time() + seconds
    while time.time() < ende:
        raw += s.read(4096)
    s.close()

    text = raw.decode("utf-8", "replace")

    # Der CP2102-Treiber spuelt beim Reset Puffermuell aus. Der ist nicht vom
    # Board und verdeckt sonst die zwei Zeilen, auf die es ankommt.
    zeilen = []
    for zeile in text.splitlines():
        sauber = re.sub(r"[^\x20-\x7e]", "", zeile).strip()
        if len(sauber) > 3 and re.search(r"[A-Za-z]{3}", sauber) \
                and not re.fullmatch(r"[x ]+", sauber):
            zeilen.append(sauber)

    # Gleiche Zeile mehrfach hintereinander: einmal zeigen, mit Zaehler.
    vorige, wie_oft = None, 0
    gefiltert = []
    for z in zeilen:
        if z == vorige:
            wie_oft += 1
            continue
        if wie_oft:
            gefiltert.append("  ... %dx wiederholt" % wie_oft)
            wie_oft = 0
        gefiltert.append(z)
        vorige = z
    if wie_oft:
        gefiltert.append("  ... %dx wiederholt" % wie_oft)

    for z in gefiltert:
        print(z)

    if not raw:
        if nur_lauschen:
            print("(still geblieben — das Board hat in der Zeit nicht neu gestartet)")
        else:
            print("(nichts empfangen — haengt das Board am USB?)")
        return

    # Beim Zuhoeren sind zwei Starts erwartet: der neue Stand und, nach dem
    # Absturz, der alte. Erst darueber hinaus ist es eine Schleife.
    grenze = 4 if nur_lauschen else 3
    starts = text.count("ets Jul")
    if starts > grenze:
        print("\nACHTUNG: %d Neustarts in %g s — das ist eine Bootschleife."
              % (starts, seconds))


if __name__ == "__main__":
    main()
