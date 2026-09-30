#!/usr/bin/env python3
"""Decide whether `pio check --json-output` findings should fail the build.

PlatformIO's own --fail-on-defect silently stops working as soon as check_flags
is set: it reports HIGH 1 and still exits 0. That was verified by injecting a
null pointer dereference. So the decision is made here, from the JSON, where it
can be read and tested.

    pio check -e piezo --skip-packages --json-output > check.json
    python scripts/check-gate.py check.json --fail-on high medium
"""

import argparse
import json
import pathlib
import sys

ORDER = ["high", "medium", "low"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("report", type=pathlib.Path)
    ap.add_argument("--fail-on", nargs="+", default=["high", "medium"],
                    choices=ORDER, metavar="SEVERITY")
    args = ap.parse_args()

    try:
        envs = json.loads(args.report.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print(f"cannot read {args.report}: {exc}", file=sys.stderr)
        return 2

    defects = [(env.get("env", "?"), d)
               for env in envs for d in env.get("defects", [])]

    counts = {s: 0 for s in ORDER}
    for _, d in defects:
        counts[d.get("severity", "low")] = counts.get(d.get("severity", "low"), 0) + 1
    print("  ".join(f"{s}: {counts.get(s, 0)}" for s in ORDER))

    blocking = [(e, d) for e, d in defects if d.get("severity") in args.fail_on]
    if not blocking:
        return 0

    print(f"\n{len(blocking)} finding(s) at or above the threshold "
          f"({', '.join(args.fail_on)}):\n", file=sys.stderr)
    for env, d in blocking:
        print(f"  {d.get('file')}:{d.get('line')} [{d.get('severity')}] "
              f"{d.get('message')} [{d.get('id')}]  ({env})", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
