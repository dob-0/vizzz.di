#!/usr/bin/env bash
# HTTP API checks for audit 2026-10-15 2-H2/2-M5/2-M7/2-M4 against a node or
# tools/simulate_node.py. Only harmless writes: channel 64 set to 0, master
# re-set to its current value, Art OUT re-set to its current state; /reboot
# and the other dangerous routes are only probed in ways that are refused.
# Usage: tools/check_http_api.sh http://192.168.4.1     (or http://127.0.0.1:8088)
set -uo pipefail
base="${1:?usage: $0 http://node}"
H='X-Vizzz-Console: 1'
pass=0; fail=0
code() { curl -s -o /dev/null -w '%{http_code}' --max-time 4 "$@"; }
hdr()  { curl -s -D - -o /dev/null --max-time 4 "$@" | tr -d '\r'; }
expect() {  # expect <want> <label> <curl args...>
  local want="$1" label="$2"; shift 2
  local got; got="$(code "$@")"
  if [[ "$got" == "$want" ]]; then pass=$((pass+1)); echo "ok   $got $label"
  else fail=$((fail+1)); echo "FAIL $got (want $want) $label"; fi
}
has() {  # has <yes|no> <header-regex> <label> <curl args...>
  local want="$1" re="$2" label="$3"; shift 3
  if hdr "$@" | grep -qiE "$re"; then got=yes; else got=no; fi
  if [[ "$got" == "$want" ]]; then pass=$((pass+1)); echo "ok   $label"
  else fail=$((fail+1)); echo "FAIL $label (header present: $got)"; fi
}
st="$(curl -s --max-time 4 "$base/status")" || { echo "node not reachable at $base"; exit 2; }
master="$(python3 -c 'import json,sys; s=json.load(sys.stdin); print(s.get("dim", s.get("master", 255)))' <<<"$st")"

# 2-H2 console routes: GET 405 (+Allow: POST), POST w/o header 403, preflight 403 w/o ACAO
expect 405 "GET /reboot"                       "$base/reboot"
has yes '^allow: POST' "GET /reboot carries Allow: POST" "$base/reboot"
expect 405 "GET /wifi/set"                     "$base/wifi/set?ssid=x"
expect 403 "POST /reboot without header"       -X POST "$base/reboot"
expect 403 "POST /netmode/set without header"  -X POST "$base/netmode/set?m=2"
expect 403 "OPTIONS /reboot (preflight)"       -X OPTIONS -H 'Origin: http://evil.example' -H 'Access-Control-Request-Method: POST' -H 'Access-Control-Request-Headers: x-vizzz-console' "$base/reboot"
has no 'access-control-allow-origin' "no ACAO on /reboot preflight" -X OPTIONS -H 'Origin: http://evil.example' "$base/reboot"
art="$(python3 -c 'import json,sys; s=json.load(sys.stdin); print(1 if s.get("ao") else 0)' <<<"$st")"
expect 204 "POST /artout/set with header (unchanged en=$art)" -X POST -H "$H" "$base/artout/set?en=$art"
# Operator routes di uses: plain GET, no header
expect 204 "GET /set ch=64 v=0"                "$base/set?ch=64&v=0"
expect 204 "GET /master v=$master (unchanged)" "$base/master?v=$master"
expect 204 "POST /set ch=64 v=0 no header"     -X POST "$base/set?ch=64&v=0"
# 2-M5 numbers
expect 400 "/set ch=abc"                       "$base/set?ch=abc&v=255"
expect 400 "/set ch=900"                       "$base/set?ch=900&v=0"
expect 400 "/set ch=0"                         "$base/set?ch=0&v=0"
expect 400 "/set v=-1"                         "$base/set?ch=64&v=-1"
expect 400 "/master v=1.5"                     "$base/master?v=1.5"
expect 400 "/artnet/set uni=x (with header)"   -X POST -H "$H" "$base/artnet/set?uni=x"
expect 400 "/page i=99"                        "$base/page?i=99"
# 2-M7 CORS
has yes 'access-control-allow-origin: \*' "ACAO on /status"   "$base/status"
has yes 'access-control-allow-origin: \*' "ACAO on /discover" "$base/discover"
has no  'access-control-allow-origin'     "no ACAO on /wifi/scan" "$base/wifi/scan"
has no  'access-control-allow-origin'     "no ACAO on GET /set"   "$base/set?ch=64&v=0"
expect 405 "POST /status"                      -X POST "$base/status"
# 2-M4 manifest lists every route with methods
if curl -s --max-time 4 "$base/manifest.json" | python3 -c '
import json,sys; m=json.load(sys.stdin); h=[p for p in m["protocols"] if p["id"]=="http"][0]
t={r["path"]:r for r in h["route_table"]}
assert t["/set"]["methods"]=="GET, POST" and t["/reboot"]["methods"]=="POST" and t["/reboot"]["console_header"]
assert set(h["routes"])==set(t) and "/panic" in t and "/fixture/set" in t
a=[p for p in m["protocols"] if p["id"]=="artnet"][0]; s=[p for p in m["protocols"] if p["id"]=="sacn"][0]
assert s["universe"]==a["universe"]+1 and a["universe"]==(a["net"]<<8|a["subnet"]<<4|a["uni"])
print(len(t))' >/dev/null 2>&1; then pass=$((pass+1)); echo "ok   manifest route_table + net/sub/uni + sACN = PA+1"
else fail=$((fail+1)); echo "FAIL manifest route_table / universe fields"; fi
echo "http api: $pass passed, $fail failed"
exit $((fail ? 1 : 0))
