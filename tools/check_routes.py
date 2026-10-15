#!/usr/bin/env python3
"""Route-table drift check (audit 2026-10-15 2-M4, 2-H2, 2-M7).

vizzz::kRoutes in include/vizzz_core.h is the one list of HTTP routes. This
fails if
  - main.cpp registers a path with route() that is not in the table, or the
    table has a path main.cpp never registers;
  - main.cpp still calls server.on() for anything but the gate itself;
  - README.md's HTTP API table disagrees with a route's method, header or CORS;
  - (V4) the DeskApi rows of kRoutes, vizzz::desk::kDeskApi (methods, class),
    README's desk API table and the vendored api-contract.json disagree;
  - (with --di) a route di.iiii's dmxRigClient.js calls is not plain-GET
    reachable (Operator or GetShared).
Usage: tools/check_routes.py [--di ../di.iiii]
"""
import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXPECT = {  # kind -> (README method, header, cors)
    "GetLocal": ("GET", "-", "-"),
    "GetShared": ("GET", "-", "yes"),
    "Operator": ("GET/POST", "-", "-"),
    "Console": ("POST", "yes", "-"),
    "Static": ("GET", "-", "-"),
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
    # ESPAsyncWebServer also matches "<uri>/..." to a handler for <uri>
    # (AsyncCallbackWebHandler::canHandle), so /x would swallow /x/y.
    # DeskApi routes are registered with an exact-URL filter (route() in
    # main.cpp), so /api/input may sit beside /api/input/release.
    for p in table:
        for q in table:
            if p != "/" and table[p] != "DeskApi" and q.startswith(p + "/"):
                errors.append(f"{q} would be served by the {p} handler (prefix match); rename one")
    # Static kind (V2): one entry per file in desk_ui_gz.h ("/" = index.html) plus
    # every kDeskRedirects path, and nothing else.
    gz = (ROOT / "include" / "desk_ui_gz.h").read_text()
    files = {"/" if p == "/index.html" else p
             for p in re.findall(r'^\s*\{"(/[^"]+)", "[^"]+", \w+_gz, ', gz, re.M)}
    redirects = set(re.findall(r'\{"(/[^"]*)", "/[^"]*"\}', core[core.index("kDeskRedirects"):]))
    static = {p for p, k in table.items() if k == "Static"}
    if not files:
        errors.append("no kDeskUiFiles entries found in include/desk_ui_gz.h")
    for p in sorted(static - files - redirects):
        errors.append(f"{p} is Static but neither a desk file nor a redirect")
    for p in sorted((files | redirects) - static):
        errors.append(f"{p} is a desk file or redirect but not a Static route")
    if len(re.findall(r"\bserver\.on\(", main_cpp)) != 1:
        errors.append("server.on() used outside route(): register through the table")

    # V4 desk API: kRoutes DeskApi rows = kDeskApi rows = the vendored contract.
    desk_h = (ROOT / "include" / "vizzz_desk.h").read_text()
    mask = {"kGet": {"GET"}, "kPost": {"POST"}, "kGet | kPost": {"GET", "POST"}}
    row = r'\{"(/api/[^"]*)",\s*(kGet \| kPost|kGet|kPost),\s*ApiClass::(\w)\}'
    # V5: kDeskExtra holds the desk paths that are not in the contract (the stream).
    extra_m = re.search(r"kDeskExtra\[\] = \{(.*?)\};", desk_h, re.S)
    extra = {p: (mask[m], c.lower()) for p, m, c in re.findall(row, extra_m.group(1) if extra_m else "")}
    if extra_m:
        desk_h = desk_h.replace(extra_m.group(0), "")
    desk = {p: (mask[m], c.lower()) for p, m, c in re.findall(row, desk_h)}
    desk_kind = {p for p, k in table.items() if k == "DeskApi"}
    for p in sorted(desk_kind ^ (desk.keys() | extra.keys())):
        errors.append(f"{p}: DeskApi in kRoutes and vizzz::desk::kDeskApi/kDeskExtra disagree")
    for p in sorted(desk.keys() & extra.keys()):
        errors.append(f"{p}: in both kDeskApi and kDeskExtra")
    desk_all = {**desk, **extra}
    if "h.setFilter(" not in main_cpp:
        errors.append("route() no longer adds the exact-URL filter DeskApi routes rely on")

    def contract_paths(path):
        c = json.loads(path.read_text())
        out = {}
        for e in c["entries"]:
            m, cl = out.get("/" + e["path"], (set(), e["class"]))
            m.add(e["method"])
            out["/" + e["path"]] = (m, cl)
        return out
    contracts = [ROOT / "test" / "fixtures" / "api-contract.v1.json"]
    if a.di:
        contracts.append(pathlib.Path(a.di) / "serverXR" / "src" / "lighting" / "api-contract.json")
    for cpath in contracts:
        if not cpath.exists():
            errors.append(f"{cpath} not found")
            continue
        want = contract_paths(cpath)
        for p in sorted(want.keys() ^ desk.keys()):
            errors.append(f"{p}: in {'the contract' if p in want else 'kDeskApi'} only ({cpath})")
        for p in sorted(want.keys() & desk.keys()):
            if want[p] != desk[p]:
                errors.append(f"{p}: kDeskApi {desk[p]} but {cpath.name} {want[p]}")

    readme = (ROOT / "README.md").read_text()
    seen = set()
    for line in readme.splitlines():
        cells = [c.strip() for c in re.split(r"(?<!\\)\|", line.strip().strip("|"))]
        if len(cells) == 4 and cells[1] in ("a", "b", "c") and cells[0] in ("GET", "POST", "GET/POST"):
            for p in re.findall(r"`(/api/[^`]*)`", cells[2]):
                seen.add(p)
                if p not in desk_all:
                    errors.append(f"README desk table lists {p}, not in kDeskApi")
                elif (set(cells[0].split("/")), cells[1]) != desk_all[p]:
                    errors.append(f"README {p}: {cells[0]} class {cells[1]} but kDeskApi {desk_all[p]}")
            continue
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
    print(f"static: {len(files)} files + {len(redirects)} redirects = {len(static)} Static routes")
    counts = {c: sum(len(m) for m, k in desk.values() if k == c) for c in "abc"}
    print(f"desk api: {len(desk)} paths, method+path calls: class a {counts['a']}, b {counts['b']}, c {counts['c']} "
          f"+ {len(extra)} outside the contract ({' '.join(sorted(extra))}) = kRoutes DeskApi {len(desk_kind)}; "
          f"the {len(desk)} = {len(contracts)} contract file(s)")
    print(f"routes: {len(table)} in table, {len(registered)} registered, "
          f"{len(seen)} in README -> {'FAIL' if errors else 'PASS'}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
