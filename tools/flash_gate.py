#!/usr/bin/env python3
"""Free-flash gate (docs/DI_DESK_UI_PLAN.md step V1): fails when the app slot has too little room.

  python3 tools/flash_gate.py [.pio/build/esp32dev/firmware.bin | esp32dev] [--partitions table.csv]

Also a PlatformIO post-build script (extra_scripts = post:tools/flash_gate.py): `pio run` fails on the gate.
The app slot size is the smallest `app` partition of the table in use (--partitions, or the project's
board_build.partitions); with neither, the esp32dev default table: two OTA slots of 1,310,720 B.
"""
import os
import sys

MIN_FREE_BYTES = 128 * 1024          # the gate: fail when free flash in the app slot is below this
DEFAULT_APP_SLOT = 1310720           # esp32dev default.csv: app0 and app1 at 0x140000 each
def _root():
    """Project root. __file__ is not defined when PlatformIO exec()s this as an extra_script."""
    here = globals().get("__file__")
    return os.path.dirname(os.path.dirname(os.path.abspath(here))) if here else os.getcwd()


def _num(s):
    s = s.strip()
    mult = 1
    if s[-1:] in "kKmM":
        mult = 1024 if s[-1] in "kK" else 1024 * 1024
        s = s[:-1]
    return int(s, 0) * mult


def slot_size(csv_path=None):
    """Smallest `app` partition in a PlatformIO/ESP-IDF partition CSV, else the default."""
    if not csv_path:
        return DEFAULT_APP_SLOT
    sizes = []
    with open(csv_path) as f:
        for line in f:
            cols = [c.strip() for c in line.split("#")[0].split(",")]
            if len(cols) >= 5 and cols[1] == "app":
                sizes.append(_num(cols[4]))
    if not sizes:
        raise SystemExit("flash gate: no app partition in %s" % csv_path)
    return min(sizes)


def check(bin_path, slot):
    used = os.path.getsize(bin_path)
    free = slot - used
    print("flash gate: %s used %d of %d B (%.1f %%), free %d B (gate: >= %d B)"
          % (os.path.basename(bin_path), used, slot, 100.0 * used / slot, free, MIN_FREE_BYTES))
    if free < MIN_FREE_BYTES:
        print("flash gate: FAIL, %d B free is under %d B" % (free, MIN_FREE_BYTES), file=sys.stderr)
        return 1
    print("flash gate: PASS")
    return 0


def _cli():
    args, csv = sys.argv[1:], None
    if "--partitions" in args:
        i = args.index("--partitions")
        csv = args[i + 1]
        del args[i:i + 2]
    target = args[0] if args else "esp32dev"
    if not target.endswith(".bin"):
        target = os.path.join(_root(), ".pio", "build", target, "firmware.bin")
    if not os.path.isfile(target):
        raise SystemExit("flash gate: %s not found (build first)" % target)
    return check(target, slot_size(csv))


try:
    Import("env")  # noqa: F821  (provided by PlatformIO/SCons when run as extra_scripts)
except NameError:
    if __name__ == "__main__":
        sys.exit(_cli())
else:
    def _post(source, target, env):  # noqa: F811
        part = env.GetProjectOption("board_build.partitions", "")  # noqa: F821
        csv = os.path.join(env["PROJECT_DIR"], part) if part and os.path.isfile(os.path.join(env["PROJECT_DIR"], part)) else None  # noqa: F821
        return check(str(target[0]), slot_size(csv))

    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _post)  # noqa: F821
