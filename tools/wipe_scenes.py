#!/usr/bin/env python3
"""Wipe every stored scene on a vizzz node, leaving config and identity alone.

There is no delete route in the firmware; a scene whose bytes are all zero is
indistinguishable from one that was never saved (loadScene memsets on a short
read). So the wipe is: /blackout (zeroes webVals), then /scene/save for every
slot. Wi-Fi, node name, universe, groups and cues are untouched.

Usage: wipe_scenes.py <host> [--yes]
"""
import sys
import urllib.request

SCENE_COUNT = 8


def hit(base, path):
    with urllib.request.urlopen(base + path, timeout=5) as r:
        return r.status


def main():
    args = [a for a in sys.argv[1:] if a != "--yes"]
    if len(args) != 1:
        sys.exit(__doc__.strip())
    host = args[0]
    base = host if host.startswith("http") else "http://" + host

    status = urllib.request.urlopen(base + "/status", timeout=5).read(200)
    print(f"node answers: {status[:120]!r}")

    if "--yes" not in sys.argv:
        if input(f"wipe all {SCENE_COUNT} scenes on {base}? [y/N] ").lower() != "y":
            sys.exit("aborted")

    print("/blackout ->", hit(base, "/blackout"))
    for n in range(SCENE_COUNT):
        print(f"/scene/save?n={n} ->", hit(base, f"/scene/save?n={n}"))
    print("done: all scenes are zeroed (recall of any slot now fades to black)")


if __name__ == "__main__":
    main()
