#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prints the development status computed from status/progress.json.

Percentages are weighted by work-package size and by verifiable state only
(planned 0, in_progress 0.25, implemented 0.5, tested 0.8, released 1.0).
"""
import json
import sys
from collections import OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCORE = {"planned": 0.0, "in_progress": 0.25, "implemented": 0.5, "tested": 0.8, "released": 1.0}


def main() -> int:
    data = json.loads((ROOT / "status" / "progress.json").read_text(encoding="utf-8"))
    areas = OrderedDict()
    total_w = total_s = 0.0
    counts = {k: 0 for k in SCORE}
    for p in data["packages"]:
        w, s = p["weight"], SCORE[p["state"]]
        a = areas.setdefault(p["area"], [0.0, 0.0])
        a[0] += w
        a[1] += w * s
        total_w += w
        total_s += w * s
        counts[p["state"]] += 1
    print("WParted Development Status")
    print(f"(computed from status/progress.json, {data['updated']})\n")
    print(f"Overall progress towards v1.0: {100 * total_s / total_w:.0f} %\n")
    for name, (w, s) in areas.items():
        print(f"{name + ':':<20}{100 * s / w:>4.0f} %")
    print("\nWork packages: " + ", ".join(f"{v} {k}" for k, v in counts.items()))
    print(f"Current milestone: {data['milestone']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
