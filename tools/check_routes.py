#!/usr/bin/env python3
"""Route-table drift check (audit 2026-10-15 2-M4, 2-H2, 2-M7).

vizzz::kRoutes in include/vizzz_core.h is the one list of HTTP routes. This
fails if
  - main.cpp registers a path with route() that is not in the table, or the
    table has a path main.cpp never registers;
  - main.cpp still calls server.on() for anything but the gate itself;
  - README.md's HTTP API table disagrees with a route's method, header or CORS;
  - (with --di) a route di.iiii's dmxRigClient.js calls is not plain-GET
    reachable (Operator or GetShared).
Usage: tools/check_routes.py [--di ../di.iiii]
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXPECT = {  # kind -> (README method, header, cors)
    "GetLocal": ("GET", "-", "-"),
    "GetShared": ("GET", "-", "yes"),
    "Operator": ("GET/POST", "-", "-"),
    "Console": ("POST", "yes", "-"),
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--di", help="path to di.iiii checkout")
    a = ap.parse_args()
    core = (ROOT / "include" / "vizzz_core.h").read_text()
    table = dict(re.findall(r'\{"(/[^"]*)",\s*RouteKind::(\w+)\}', core))
    main_cpp = (ROOT / "src" / "main.cpp").read_text()
    registered = set(re.findall(r'\broute\("(/[^"]*)"', main_cpp))
    errors = []
    for p in sorted(registered - table.keys()):
        errors.append(f"main.cpp registers {p}, not in kRoutes")
    for p in sorted(table.keys() - registered):
        errors.append(f"kRoutes has {p}, main.cpp never registers it")
    if len(re.findall(r"\bserver\.on\(", main_cpp)) != 1:
        errors.append("server.on() used outside route(): register through the table")

    readme = (ROOT / "README.md").read_text()
    seen = set()
    for line in readme.splitlines():
        cells = [c.strip() for c in re.split(r"(?<!\\)\|", line.strip().strip("|"))]
        if len(cells) != 6 or cells[0] not in ("GET", "POST", "GET/POST"):
            continue
        for p in re.findall(r"`(/[^`?&]*)`", cells[3]):
            seen.add(p)
            kind = table.get(p)
            if kind is None:
                errors.append(f"README lists {p}, not in kRoutes")
            elif tuple(cells[:3]) != EXPECT[kind]:
                errors.append(f"README {p}: {cells[:3]} but table kind {kind} -> {EXPECT[kind]}")
    for p in sorted(table.keys() - seen):
        errors.append(f"README HTTP API table misses {p}")

    if a.di:
        client = pathlib.Path(a.di) / "src" / "raw" / "utils" / "dmxRigClient.js"
        js = client.read_text()
        used = {p for p in re.findall(r"(/[a-z][a-z/.]*)", js) if p in table}
        for must in ("/status", "/set", "/master", "/blackout"):
            if must not in used:
                errors.append(f"di client parse did not find {must}; check {client}")
        for p in sorted(used):
            if table[p] not in ("Operator", "GetShared"):
                errors.append(f"di.iiii calls {p} as a plain GET but it is {table[p]}")
        print(f"di.iiii routes, all plain-GET reachable: {' '.join(sorted(used))}")

    for e in errors:
        print("FAIL", e)
    print(f"routes: {len(table)} in table, {len(registered)} registered, "
          f"{len(seen)} in README -> {'FAIL' if errors else 'PASS'}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
