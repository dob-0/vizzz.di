#!/usr/bin/env bash
# provision_sta.sh — give a vizzz.di node its WiFi client (STA) credentials over USB,
# without the console: writes ONLY the "cfg" NVS keys sta_ssid / sta_pass into the
# node's NVS partition. Everything else in NVS is erased (scenes, patch, mode) — the
# node boots on firmware defaults + these credentials. Run from the repo root.
#
#   tools/provision_sta.sh <ssid> <password> [port]
#
# Method: ESP-IDF NVS partition generator (esp-idf-nvs-partition-gen, pinned) +
# esptool write_flash at the esp32dev default NVS offset 0x9000, size 0x5000
# (same table the firmware is built against — see .pio/build/esp32dev/partitions.bin).
# First used 2026-10-15 on node BA6FCC; see CURRENT.md.
set -euo pipefail
SSID=${1:?ssid}; PASS=${2:?password}; PORT=${3:-/dev/ttyUSB0}
ESPT=.platformio-core/packages/tool-esptoolpy/esptool.py
[ -f "$ESPT" ] || { echo "esptool not found: $ESPT (run 'pio run' once)"; exit 1; }
VENV=.platformio-core/nvsgen-venv
[ -x "$VENV/bin/python" ] || { python3 -m venv "$VENV"; "$VENV/bin/pip" -q install esp-idf-nvs-partition-gen==0.3.0; }
GEN=$(find "$VENV" -name nvs_partition_gen.py | head -1)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
printf 'key,type,encoding,value\ncfg,namespace,,\nsta_ssid,data,string,%s\nsta_pass,data,string,%s\n' "$SSID" "$PASS" > "$T/nvs.csv"
"$VENV/bin/python" "$GEN" generate "$T/nvs.csv" "$T/nvs.bin" 0x5000 >/dev/null
python3 "$ESPT" --chip esp32 --port "$PORT" --baud 460800 write_flash 0x9000 "$T/nvs.bin" | grep -E "Wrote|verified|rror"
echo "done: node reboots; its boot log prints 'STA SSID : $SSID' and the STA IP (115200 baud)."
