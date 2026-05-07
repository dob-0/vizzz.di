#!/usr/bin/env python3
"""Clean-flash and configure one vizzz.di ESP32 at a time.

Examples:
  python3 onboard_device.py --erase
  python3 onboard_device.py --erase --host 10.0.0.1 --universe 2 --name vizzz.di-u2
  python3 onboard_device.py --skip-serial --skip-upload --host 192.168.88.127 --universe 18 --mode artnet
"""

import argparse
import re
import shlex
import subprocess
import sys
import urllib.parse
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parent
DEFAULT_PIO = Path("/home/nnn/.platformio/penv/bin/pio")
DEFAULT_PYTHON = Path("/home/nnn/.platformio/penv/bin/python")
DEFAULT_ESPTOOL = ROOT / ".platformio-core/packages/tool-esptoolpy/esptool.py"
MODE_MAP = {"web": 0, "artnet": 1, "htp": 2}


def run_cmd(args, use_sg=False):
    if use_sg:
        cmd = ["sg", "dialout", "-c", " ".join(shlex.quote(str(a)) for a in args)]
    else:
        cmd = [str(a) for a in args]
    print("+ " + " ".join(shlex.quote(str(a)) for a in cmd))
    return subprocess.run(cmd, cwd=ROOT, check=True, text=True, capture_output=True)


def print_process_output(proc):
    if proc.stdout:
        print(proc.stdout, end="")
    if proc.stderr:
        print(proc.stderr, end="", file=sys.stderr)


def read_mac(args):
    proc = run_cmd(
        [args.python, args.esptool, "--port", args.port, "read_mac"],
        use_sg=not args.no_sg,
    )
    print_process_output(proc)
    matches = re.findall(r"MAC:\s*([0-9a-fA-F:]{17})", proc.stdout + proc.stderr)
    if not matches:
        raise RuntimeError("Could not parse MAC from esptool output")
    return matches[-1].upper()


def erase_flash(args):
    proc = run_cmd(
        [args.python, args.esptool, "--port", args.port, "erase_flash"],
        use_sg=not args.no_sg,
    )
    print_process_output(proc)


def upload(args):
    proc = run_cmd(
        [args.pio, "run", "-e", "esp32dev", "--target", "upload"],
        use_sg=not args.no_sg,
    )
    print_process_output(proc)


def http_get(host, path, query=None):
    query = query or {}
    url = f"http://{host}{path}"
    if query:
        url += "?" + urllib.parse.urlencode(query)
    print("+ GET " + url)
    with urllib.request.urlopen(url, timeout=4) as response:
        body = response.read().decode("utf-8", errors="replace")
    if body:
        print(body)
    return body


def split_universe(value):
    if value < 0 or value > 0x7FFF:
        raise ValueError("Art-Net universe must be in 0..32767")
    return (value >> 8) & 0x7F, (value >> 4) & 0x0F, value & 0x0F


def configure_http(args):
    if not args.host:
        return

    if args.name:
        http_get(args.host, "/node/set", {"name": args.name})

    if args.universe is not None:
        net, subnet, uni = split_universe(args.universe)
        http_get(args.host, "/artnet/set", {"net": net, "subnet": subnet, "uni": uni})

    if args.mode:
        http_get(args.host, "/mode/set", {"m": MODE_MAP[args.mode]})

    if args.test:
        http_get(args.host, "/mode/set", {"m": MODE_MAP["web"]})
        http_get(args.host, "/master", {"v": 255})
        http_get(args.host, "/blackout")
        for channel in range(1, args.test + 1):
            http_get(args.host, "/set", {"ch": channel, "v": 255})

    http_get(args.host, "/node/manifest")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Read MAC, optionally erase/flash, and configure one vizzz.di node."
    )
    parser.add_argument("--port", default="/dev/ttyUSB0", help="serial port")
    parser.add_argument("--erase", action="store_true", help="chip erase before upload")
    parser.add_argument("--skip-serial", action="store_true", help="do not read USB MAC")
    parser.add_argument("--skip-upload", action="store_true", help="only read/configure")
    parser.add_argument("--host", help="node HTTP host after boot, for example 10.0.0.1")
    parser.add_argument("--name", help="node name to set over HTTP")
    parser.add_argument(
        "--universe",
        type=int,
        help="absolute Art-Net/sACN universe 0..32767; split into net/subnet/uni",
    )
    parser.add_argument(
        "--mode",
        choices=sorted(MODE_MAP),
        help="set output mode after boot",
    )
    parser.add_argument(
        "--test",
        type=int,
        default=0,
        metavar="N",
        help="after configure, set WEB mode and channels 1..N full",
    )
    parser.add_argument("--no-sg", action="store_true", help="do not wrap serial commands in sg dialout")
    parser.add_argument("--pio", type=Path, default=DEFAULT_PIO)
    parser.add_argument("--python", type=Path, default=DEFAULT_PYTHON)
    parser.add_argument("--esptool", type=Path, default=DEFAULT_ESPTOOL)
    return parser.parse_args()


def main():
    args = parse_args()
    if args.skip_serial and (args.erase or not args.skip_upload):
        raise RuntimeError("--skip-serial can only be used with --skip-upload and without --erase")

    if not args.skip_serial:
        mac = read_mac(args)
        print(f"Detected ESP32 MAC: {mac}")

    if args.erase:
        erase_flash(args)

    if not args.skip_upload:
        upload(args)

    configure_http(args)

    print("Done.")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        print_process_output(exc)
        sys.exit(exc.returncode)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
