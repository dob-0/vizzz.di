#!/usr/bin/env python3
"""HTTP crash/stress test for vizzz.di firmware.

Safe by default:
- Enables burn-safe mode before running.
- Avoids dangerous routes (reboot/factory reset/network profile changes).

Examples:
  python3 tools/crash_http.py --host 192.168.4.1 --seconds 300 --workers 10
  python3 tools/crash_http.py --host 192.168.4.1 --seconds 120 --allow-dangerous
"""

from __future__ import annotations

import argparse
import random
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass


@dataclass(frozen=True)
class RouteCase:
    path: str
    query: dict[str, str]
    dangerous: bool = False


def http_call(host: str, case: RouteCase, method: str, timeout: float = 1.0) -> tuple[bool, int]:
    qs = urllib.parse.urlencode(case.query)
    url = f"http://{host}{case.path}"
    if qs:
        url = f"{url}?{qs}"
    req = urllib.request.Request(url=url, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return True, int(resp.status)
    except urllib.error.HTTPError as err:
        return False, int(err.code)
    except Exception:
        return False, 0


def cases() -> list[RouteCase]:
    ch = random.randint(1, 512)
    val = random.randint(0, 255)
    mode = random.randint(0, 2)
    net = random.randint(0, 127)
    subnet = random.randint(0, 15)
    uni = random.randint(0, 15)
    scene = random.randint(0, 7)
    group = random.randint(0, 7)
    cue = random.randint(0, 15)

    return [
        RouteCase("/set", {"ch": str(ch), "v": str(val)}),
        RouteCase("/master", {"v": str(val)}),
        RouteCase("/blackout", {}),
        RouteCase("/full", {}),
        RouteCase("/mode/set", {"m": str(mode)}),
        RouteCase("/artnet/set", {"net": str(net), "subnet": str(subnet), "uni": str(uni)}),
        RouteCase("/scene/save", {"n": str(scene)}),
        RouteCase("/scene/recall", {"n": str(scene), "fade": str(random.randint(0, 2000))}),
        RouteCase("/group/apply", {"g": str(group), "v": str(val)}),
        RouteCase("/cue/set", {"i": str(cue), "scene": str(scene), "dwell": "500", "fade": "250"}),
        RouteCase("/cue/run", {"en": str(random.randint(0, 1))}),
        RouteCase("/cue/next", {}),
        RouteCase("/fx/set", {"mode": random.choice(["none", "strobe", "chase", "pulse", "sine", "sparkle", "comet", "bars", "glitch"]), "en": str(random.randint(0, 1)), "bpm": str(random.randint(20, 240)), "depth": str(random.randint(0, 255))}),
        RouteCase("/fx/tap", {}),
        RouteCase("/color/set", {"en": str(random.randint(0, 1)), "r": str(random.randint(0, 255)), "g": str(random.randint(0, 255)), "b": str(random.randint(0, 255))}),
        RouteCase("/net/master", {"v": str(val)}),
        RouteCase("/net/scene/recall", {"n": str(scene), "fade": str(random.randint(0, 2000))}),
        RouteCase("/status", {}),
        RouteCase("/page", {"i": str(random.randint(0, 15))}),
        RouteCase("/monitor", {}),
        RouteCase("/peers", {}),
        RouteCase("/safety/status", {}),
        RouteCase("/reboot", {}, dangerous=True),
        RouteCase("/factory-reset", {}, dangerous=True),
        RouteCase("/netmode/set", {"m": str(random.randint(0, 2))}, dangerous=True),
        RouteCase("/web/set", {"en": str(random.randint(0, 1))}, dangerous=True),
        RouteCase("/wifi/forget", {}, dangerous=True),
    ]


def worker(host: str, seconds: int, allow_dangerous: bool, stats: dict[str, int], lock: threading.Lock, stop_at: float) -> None:
    methods = ["GET", "POST"]
    while time.time() < stop_at:
        case = random.choice(cases())
        if case.dangerous and not allow_dangerous:
            continue
        method = random.choice(methods)
        ok, code = http_call(host, case, method)
        key = "ok" if ok else "fail"
        with lock:
            stats[key] = stats.get(key, 0) + 1
            if code:
                stats[f"code_{code}"] = stats.get(f"code_{code}", 0) + 1


def set_burn_safe(host: str, enabled: bool) -> None:
    case = RouteCase("/safety/set", {"burn": "1" if enabled else "0"})
    for method in ("POST", "GET"):
        ok, _ = http_call(host, case, method, timeout=2.0)
        if ok:
            return


def main() -> int:
    parser = argparse.ArgumentParser(description="HTTP crash/stress test for vizzz.di")
    parser.add_argument("--host", default="192.168.4.1", help="Target host")
    parser.add_argument("--seconds", type=int, default=180, help="Test duration")
    parser.add_argument("--workers", type=int, default=8, help="Parallel workers")
    parser.add_argument("--allow-dangerous", action="store_true", help="Include dangerous reboot/reset/network routes")
    parser.add_argument("--disable-burn-safe", action="store_true", help="Do not enable burn-safe mode before stress")
    args = parser.parse_args()

    if not args.disable_burn_safe:
        set_burn_safe(args.host, True)

    stats: dict[str, int] = {}
    lock = threading.Lock()
    stop_at = time.time() + max(1, args.seconds)

    threads = [
        threading.Thread(
            target=worker,
            args=(args.host, args.seconds, args.allow_dangerous, stats, lock, stop_at),
            daemon=True,
        )
        for _ in range(max(1, args.workers))
    ]

    for t in threads:
        t.start()
    for t in threads:
        t.join()

    print("HTTP stress results")
    for k in sorted(stats):
        print(f"{k}: {stats[k]}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
