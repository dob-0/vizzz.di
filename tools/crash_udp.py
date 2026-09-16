#!/usr/bin/env python3
"""UDP protocol stress sender for vizzz.di.

Sends randomized Art-Net, sACN, and OSC packets to stress parser paths.

Examples:
  python3 tools/crash_udp.py --host 192.168.4.1 --seconds 120 --pps 300
  python3 tools/crash_udp.py --host 192.168.4.1 --seconds 300 --artnet-universe 0
"""

from __future__ import annotations

import argparse
import os
import random
import socket
import struct
import time


ARTNET_PORT = 6454
SACN_PORT = 5568
OSC_PORT = 9000


def artnet_packet(universe: int, channels: int = 512) -> bytes:
    channels = max(2, min(512, channels))
    dmx = os.urandom(channels)
    header = bytearray()
    header.extend(b"Art-Net\x00")
    header.extend(b"\x00\x50")  # OpDmx
    header.extend(b"\x00\x0e")  # v14
    header.append(random.randint(0, 255))
    header.append(0)
    header.append(universe & 0xFF)
    header.append((universe >> 8) & 0x7F)
    header.extend(struct.pack(">H", channels))
    return bytes(header) + dmx


def sacn_packet(universe: int, channels: int = 512) -> bytes:
    channels = max(1, min(512, channels))
    props = b"\x00" + os.urandom(channels)
    prop_count = len(props)

    root_pdu_len = 0x7000 | (22 + 77 + 11 + prop_count)
    framing_pdu_len = 0x7000 | (77 + 11 + prop_count)
    dmp_pdu_len = 0x7000 | (10 + prop_count)

    pkt = bytearray()
    pkt.extend(struct.pack(">H", 0x0010))
    pkt.extend(struct.pack(">H", root_pdu_len))
    pkt.extend(b"ASC-E1.17\x00\x00\x00")
    pkt.extend(struct.pack(">I", 0x00000004))
    pkt.extend(os.urandom(16))

    pkt.extend(struct.pack(">H", framing_pdu_len))
    pkt.extend(struct.pack(">I", 0x00000002))
    source = b"vizzz-crash".ljust(64, b"\x00")
    pkt.extend(source)
    pkt.append(100)  # priority
    pkt.extend(b"\x00\x00")
    pkt.append(0)
    pkt.append(0)
    pkt.extend(struct.pack(">H", universe & 0xFFFF))

    pkt.extend(struct.pack(">H", dmp_pdu_len))
    pkt.append(0x02)
    pkt.append(0xa1)
    pkt.extend(struct.pack(">H", 0x0000))
    pkt.extend(struct.pack(">H", 0x0001))
    pkt.extend(struct.pack(">H", prop_count))
    pkt.extend(props)
    return bytes(pkt)


def osc_packet() -> bytes:
    addr = random.choice(["/ch/1", "/ch/256", "/group/2", "/master", "/fx/bpm", "/color/r"])
    val = random.randint(0, 255)

    def pad4(data: bytes) -> bytes:
        while len(data) % 4:
            data += b"\x00"
        return data

    a = pad4(addr.encode("ascii") + b"\x00")
    t = pad4(b",i\x00")
    v = struct.pack(">i", val)
    return a + t + v


def malformed_packet() -> bytes:
    return os.urandom(random.randint(8, 96))


def main() -> int:
    parser = argparse.ArgumentParser(description="UDP crash/stress sender for vizzz.di")
    parser.add_argument("--host", default="192.168.4.1", help="Target host")
    parser.add_argument("--seconds", type=int, default=120, help="Duration")
    parser.add_argument("--pps", type=int, default=200, help="Approx packets per second")
    parser.add_argument("--artnet-universe", type=int, default=0, help="15-bit universe for Art-Net")
    parser.add_argument("--sacn-universe", type=int, default=0, help="16-bit universe for sACN")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    end = time.time() + max(1, args.seconds)
    sent = 0

    pps = max(10, args.pps)
    sleep_s = 1.0 / pps

    while time.time() < end:
        kind = random.choice(["artnet", "sacn", "osc", "bad"])
        if kind == "artnet":
            payload = artnet_packet(args.artnet_universe)
            port = ARTNET_PORT
        elif kind == "sacn":
            payload = sacn_packet(args.sacn_universe)
            port = SACN_PORT
        elif kind == "osc":
            payload = osc_packet()
            port = OSC_PORT
        else:
            payload = malformed_packet()
            port = random.choice([ARTNET_PORT, SACN_PORT, OSC_PORT])

        sock.sendto(payload, (args.host, port))
        sent += 1
        time.sleep(sleep_s)

    print(f"UDP stress complete: sent {sent} packets in {args.seconds}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
