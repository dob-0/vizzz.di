#!/usr/bin/env bash
# Cross-checks the firmware's Art-Net/sACN parsers and ArtPollReply against
# di.iiii's own packet code (serverXR/src/lighting), with no hardware and no
# network beyond 127.0.0.1. Usage: tools/check_di_compat.sh [path/to/di.iiii]
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
di="$(cd "${1:-$here/../di.iiii}" && pwd)"   # absolute: di_side.js require()s from its own dir
lighting="$di/serverXR/src/lighting"
[[ -f "$lighting/artnet.js" ]] || { echo "di.iiii lighting code not found at $lighting" >&2; exit 2; }
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
g++ -std=c++17 -Wall -I "$here/include" "$here/tools/di_compat/harness.cpp" -o "$tmp/harness"
node "$here/tools/di_compat/di_side.js" gen "$lighting" "$tmp"
"$tmp/harness" poll   "$tmp/poll.bin"
"$tmp/harness" artdmx "$tmp/artdmx.bin"
"$tmp/harness" sacn   "$tmp/sacn.bin"
"$tmp/harness" reply  "$tmp/reply.bin"
node "$here/tools/di_compat/di_side.js" list "$lighting" "$tmp/reply.bin"
g++ -std=c++17 -Wall -I "$here/include" "$here/tools/di_compat/stream_harness.cpp" -o "$tmp/stream_harness"   # V5
"$tmp/stream_harness" > "$tmp/stream.jsonl"
node "$here/tools/di_compat/stream_side.js" "$lighting" "$tmp/stream.jsonl"
python3 "$here/tools/check_routes.py" --di "$di"   # HTTP: di's GET routes stay plain-GET (2-H2)
echo "di compat: PASS"
