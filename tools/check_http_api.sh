#!/usr/bin/env bash
# HTTP API checks for audit 2026-10-15 2-H2/2-M5/2-M7/2-M4 and the V4 desk API
# gate against a node or tools/simulate_node.py. Only harmless writes: channel
# 64 set to 0, master re-set to its current value, Art OUT re-set to its current
# state, an empty /api/master body; /reboot and the other dangerous routes are
# only probed in ways that are refused.
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
# V2 desk UI served from flash: 8 files (200, gzip, Content-Type, ETag), 304, 301s
for spec in "/|text/html" "/app.js|application/javascript" "/style.css|text/css" "/qr.js|application/javascript" \
            "/from.js|application/javascript" "/show.js|application/javascript" "/caps.js|application/javascript" "/dmxfeed.js|application/javascript"; do
  path="${spec%%|*}"; mime="${spec##*|}"
  h="$(hdr "$base$path")"
  if grep -q '^HTTP/[0-9.]* 200' <<<"$h" && grep -qi '^content-encoding: gzip' <<<"$h" \
     && grep -qiE "^content-type: $mime" <<<"$h" && grep -qE '^[Ee][Tt]ag: "[0-9a-f]+"' <<<"$h" \
     && grep -qi '^cache-control: no-cache' <<<"$h"; then
    pass=$((pass+1)); echo "ok   GET $path 200 + gzip + $mime + ETag + no-cache"
  else fail=$((fail+1)); echo "FAIL GET $path (want 200, gzip, $mime, ETag, no-cache)"; echo "$h" | sed 's/^/     /'; fi
done
etag="$(hdr "$base/app.js" | sed -n 's/^[Ee][Tt]ag: //p')"
h="$(hdr -H "If-None-Match: $etag" "$base/app.js")"
body="$(curl -s --max-time 4 -H "If-None-Match: $etag" "$base/app.js" | wc -c)"
if grep -q '^HTTP/[0-9.]* 304' <<<"$h" && grep -qF "$etag" <<<"$h" && [[ "$body" == 0 ]]; then
  pass=$((pass+1)); echo "ok   GET /app.js If-None-Match $etag -> 304, no body"
else fail=$((fail+1)); echo "FAIL If-None-Match $etag on /app.js (want 304, no body; body bytes: $body)"; fi
expect 200 "GET /app.js If-None-Match of another tag" -H 'If-None-Match: "0000000000000000"' "$base/app.js"
redir() {  # redir <path> <Location>
  local h; h="$(hdr "$base$1")"
  if grep -q '^HTTP/[0-9.]* 301' <<<"$h" && grep -qiE "^location: $2\s*$" <<<"$h"; then
    pass=$((pass+1)); echo "ok   GET $1 -> 301 $2"
  else fail=$((fail+1)); echo "FAIL GET $1 (want 301 Location: $2)"; fi
}
redir /control /#control
redir /patch /
redir /scenes /
redir /vj /
redir /network /
redir /system /
redir /performance /
# V4 desk API (/api/*): the gate, the body rules and the class-c 404, all JSON
J='Content-Type: application/json'
jsonbody() {  # jsonbody <want-code> <python-expr on d> <label> <curl args...>
  local want="$1" expr="$2" label="$3"; shift 3
  local out; out="$(curl -s --max-time 4 -w '\n%{http_code}' "$@")"
  local got="${out##*$'\n'}" body="${out%$'\n'*}"
  if [[ "$got" == "$want" ]] && python3 -c "import json,sys; d=json.loads(sys.argv[1]); assert $expr" "$body" 2>/dev/null; then
    pass=$((pass+1)); echo "ok   $got $label"
  else fail=$((fail+1)); echo "FAIL $got (want $want) $label: ${body:0:120}"; fi
}
jsonbody 200 '"master" in d and "fixtures" in d and "status" in d' "GET /api/state is the desk state" "$base/api/state"
jsonbody 200 'len(d["dmx"]["0"]) == 512' "GET /api/dmx has 512 channels" "$base/api/dmx"
has no 'access-control-allow-origin' "no ACAO on GET /api/state" "$base/api/state"
jsonbody 200 'd == {"ok": True}' "POST /api/master {} (changes nothing)" -X POST -H "$J" -d '{}' "$base/api/master"
jsonbody 415 '"error" in d' "POST /api/master as text/plain" -X POST -H 'Content-Type: text/plain' -d '{}' "$base/api/master"
jsonbody 415 '"error" in d' "POST /api/master as a form" -X POST --data-urlencode master=0 "$base/api/master"
big="$(python3 -c 'import json; print(json.dumps({"channels": [{"channel": 64, "value": 0}] * 120}))')"
jsonbody 413 '"error" in d' "POST /api/raw with ${#big} bytes" -X POST -H "$J" -d "$big" "$base/api/raw"
jsonbody 400 '"error" in d' "POST /api/master invalid JSON" -X POST -H "$J" -d '{"master":' "$base/api/master"
jsonbody 400 '"error" in d' "POST /api/master a JSON array" -X POST -H "$J" -d '[1]' "$base/api/master"
jsonbody 405 'd["allow"] == "POST"' "GET /api/master" "$base/api/master"
has yes '^allow: POST' "GET /api/master carries Allow: POST" "$base/api/master"
jsonbody 405 'd["allow"] == "GET"' "POST /api/state" -X POST -H "$J" -d '{}' "$base/api/state"
expect 403 "OPTIONS /api/master (preflight)"   -X OPTIONS -H 'Origin: http://evil.example' -H 'Access-Control-Request-Method: POST' -H 'Access-Control-Request-Headers: content-type' "$base/api/master"
has no 'access-control-allow-origin' "no ACAO on /api/master preflight" -X OPTIONS -H 'Origin: http://evil.example' "$base/api/master"
jsonbody 404 'd["error"] == "not on this node"' "GET /api/library (class c)" "$base/api/library"
jsonbody 404 'd["error"] == "not on this node"' "POST /api/midi (class c)" -X POST -H "$J" -d '{}' "$base/api/midi"
jsonbody 200 'd == {"error": "not on this node"}' "POST /api/looks/add (class b)" -X POST -H "$J" -d '{}' "$base/api/looks/add"
jsonbody 200 'd["cues"]["n"] == 0' "GET /api/cues (class b, empty list)" "$base/api/cues"
jsonbody 400 '"error" in d' "POST /api/fixture on:false refused" -X POST -H "$J" -d '{"id":"f1","on":false}' "$base/api/fixture"
jsonbody 400 '"error" in d' "POST /api/raw universe 1 refused" -X POST -H "$J" -d '{"universe":1,"channel":64,"value":0}' "$base/api/raw"
jsonbody 200 'isinstance(d["api_replies"], int) and isinstance(d["api_busy"], int)' "/status has api_replies, api_busy" "$base/status"
# V5 GET /api/dmx/stream: 200 text/event-stream, one event in dmxstream.js's shape, the cap (4) answers 503.
jsonbody 200 'd["caps"]["stream"] is True' "/api/state says caps.stream" "$base/api/state"
jsonbody 200 'isinstance(d["sse_clients"], int)' "/status has sse_clients" "$base/status"
jsonbody 405 'd["allow"] == "GET"' "POST /api/dmx/stream" -X POST -H "$J" -d '{}' "$base/api/dmx/stream"
has no 'access-control-allow-origin' "no ACAO on the stream" -m 1 "$base/api/dmx/stream"
stream_out="$(python3 - "$base" <<'PY'
import http.client, json, sys, time, urllib.parse
u = urllib.parse.urlparse(sys.argv[1]); host, port = u.hostname, u.port or 80
def open_stream():
    c = http.client.HTTPConnection(host, port, timeout=5)
    c.request("GET", "/api/dmx/stream")
    return c, c.getresponse()
def first_event(r):
    """Reads lines until the first 'data:' line of an 'event: frame'; returns (event name, parsed data)."""
    name = data = None
    for _ in range(40):
        line = r.fp.readline().decode().rstrip("\r\n")
        if line.startswith("event:"): name = line[6:].strip()
        elif line.startswith("data:"): data = json.loads(line[5:]); break
    return name, data
out = []
c, r = open_stream()
ctype = r.getheader("Content-Type") or ""
out.append(("200 + text/event-stream", r.status == 200 and ctype.startswith("text/event-stream")))
name, ev = first_event(r)
d = ev and ev.get("d")
out.append(("first event is a frame key frame of 512 slots with meta",
            name == "frame" and ev and ev["k"] == 1 and ev["s"] >= 1 and isinstance(ev["t"], int) and
            len(d) == 1 and d[0][0] == 0 and d[0][1] == 0 and len(d[0][2]) == 512 and
            ev["m"]["blackout"] is False and 0 <= ev["m"]["master"] <= 255 and ev["m"]["looks"] == [] and ev["m"]["cues"] is None))
held = [(c, r)]
for _ in range(3):                      # clients 2..4
    held.append(open_stream())
time.sleep(0.3)
sc = http.client.HTTPConnection(host, port, timeout=5); sc.request("GET", "/status")
sse = json.loads(sc.getresponse().read())["sse_clients"]
out.append(("/status sse_clients = 4 with four streams open (got %s)" % sse, sse == 4))
c5, r5 = open_stream()
body = r5.read()
out.append(("5th client: 503 {error: stream full} (got %s %s)" % (r5.status, body[:60]),
            r5.status == 503 and json.loads(body) == {"error": "stream full"}))
for cc, rr in held: rr.close(); cc.close()   # the response holds the socket's file too
c5.close()
time.sleep(1.5)
c6, r6 = open_stream()
out.append(("a slot is free again after the four close", r6.status == 200))
r6.close(); c6.close()
for label, ok in out: print(("ok   " if ok else "FAIL ") + label)
PY
)"
while IFS= read -r line; do
  echo "$line"
  if [[ "$line" == ok* ]]; then pass=$((pass+1)); else fail=$((fail+1)); fi
done <<<"$stream_out"
[[ -n "$stream_out" ]] || { fail=$((fail+1)); echo "FAIL the stream check did not run"; }
echo "http api: $pass passed, $fail failed"
exit $((fail ? 1 : 0))
