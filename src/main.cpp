#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <freertos/semphr.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArtnetWifi.h>
#include <Preferences.h>

#include "esp_dmx.h"
#include "vidili_core.h"

// ── Hardware ──────────────────────────────────────────────────────────────────
static constexpr int DMX_TX  = 25;
static constexpr int DMX_DIR = 21;
static constexpr int MAX_CH  = 512;

// ── Timing ────────────────────────────────────────────────────────────────────
static constexpr uint32_t   DMX_PERIOD_MS     = 23;
static constexpr TickType_t DMX_SEND_WAIT     = pdMS_TO_TICKS(30);
static constexpr uint32_t   ARTNET_TIMEOUT_MS = 3000;
static constexpr uint16_t   ARTNET_PORT       = 6454;

// ── Paging / Scenes ───────────────────────────────────────────────────────────
static constexpr uint16_t PAGE_SIZE   = 32;
static constexpr uint16_t PAGE_COUNT  = 16;
static constexpr uint8_t  SCENE_COUNT = 8;

// ── Mode ──────────────────────────────────────────────────────────────────────
enum Mode : uint8_t { MODE_WEB = 0, MODE_ARTNET = 1, MODE_HTP = 2 };
static const char* modeName(Mode m) {
  switch (m) {
    case MODE_WEB:    return "WEB_ONLY";
    case MODE_ARTNET: return "ARTNET_ONLY";
    default:          return "MERGE_HTP";
  }
}

// ── Config ────────────────────────────────────────────────────────────────────
static Preferences prefs;
static String  nodeName = "vi_di_li";
static String  apSsid   = "vi_di_li";
static String  apPass   = "Poghka888$";
static String  staSSID, staPass;
static uint8_t artNet = 0, artSubnet = 0, artUni = 0;
static Mode    mode         = MODE_HTP;
static bool    artOutEnabled = false;   // broadcast webVals as Art-Net to slaves
static bool    webEnabled    = true;    // when false, web server and websocket stay disabled
static bool    needSaveConfig = false;  // set when auto-generated values must be persisted
static constexpr const char* FW_TAG = __DATE__ " " __TIME__;

static uint16_t universe() {
  return vidili::packUniverse(artNet, artSubnet, artUni);
}

static void saveConfig() {
  prefs.begin("cfg", false);
  prefs.putString("name",     nodeName);
  prefs.putString("ap_ssid",  apSsid);
  prefs.putString("ap_pass",  apPass);
  prefs.putString("sta_ssid", staSSID);
  prefs.putString("sta_pass", staPass);
  prefs.putUChar("anet_net",  artNet);
  prefs.putUChar("anet_sub",  artSubnet);
  prefs.putUChar("anet_uni",  artUni);
  prefs.putUChar("mode",      uint8_t(mode));
  prefs.putBool("ao_en",      artOutEnabled);
  prefs.putBool("web_en",     webEnabled);
  prefs.putString("fw_tag",   FW_TAG);
  prefs.end();
}

static void loadConfig() {
  prefs.begin("cfg", true);
  bool hasApSsid = prefs.isKey("ap_ssid");  // false on first boot
  nodeName     = prefs.getString("name",     nodeName);
  apSsid       = prefs.getString("ap_ssid",  apSsid);
  if (!hasApSsid) apSsid = "";  // sentinel → startWiFi will generate from MAC
  apPass       = prefs.getString("ap_pass",  apPass);
  staSSID      = prefs.getString("sta_ssid", "");
  staPass      = prefs.getString("sta_pass", "");
  artNet       = prefs.getUChar("anet_net",  artNet);
  artSubnet    = prefs.getUChar("anet_sub",  artSubnet);
  artUni       = prefs.getUChar("anet_uni",  artUni);
  mode         = Mode(prefs.getUChar("mode", uint8_t(mode)));
  artOutEnabled = prefs.getBool("ao_en",     artOutEnabled);
  webEnabled   = prefs.getBool("web_en",     webEnabled);
  String savedFwTag = prefs.getString("fw_tag", "");
  prefs.end();
  if (mode > MODE_HTP) mode = MODE_HTP;

  // New firmware image flashed: rotate default SSID once, then persist.
  if (savedFwTag != FW_TAG) {
    if (apSsid.isEmpty() || apSsid.startsWith("vi_di_li")) apSsid = "";
    needSaveConfig = true;
  }
}

// ── Runtime state ─────────────────────────────────────────────────────────────
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static ArtnetWifi     artnet;
static WiFiUDP        artOutUdp;
static dmx_port_t     dmxPort = DMX_NUM_1;

static uint8_t dmxFrame [MAX_CH + 1];
static uint8_t webVals  [MAX_CH];   // web layer — protected by gLock
static uint8_t artVals  [MAX_CH];   // Art-Net IN — written from UDP task, race accepted
static uint8_t outVals  [MAX_CH];   // final computed output
static uint8_t holdVals [MAX_CH];   // last Art-Net values for hold-on-timeout
static uint8_t sceneBuf [MAX_CH];

static volatile uint32_t lastArtnetMs = 0;
static uint8_t masterDimmer = 255;  // 0=off, 255=full — resets to full on boot

// Fade — access only while holding gLock
static bool     fadeActive  = false;
static uint32_t fadeStartMs = 0;
static uint32_t fadeTimeMs  = 1000;
static uint8_t  fadeFrom[MAX_CH];
static uint8_t  fadeTo  [MAX_CH];

// Mutex guards webVals and all fade state across cores
static SemaphoreHandle_t gLock;
static volatile bool     pendingReboot = false;

// ── Helpers ───────────────────────────────────────────────────────────────────
static String ipStr(IPAddress ip) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return buf;
}

static void jsonEsc(char* dst, size_t n, const String& src) {
  size_t d = 0;
  for (size_t i = 0; i < src.length() && d + 3 < n; i++) {
    uint8_t c = uint8_t(src[i]);
    switch (c) {
      case '"':  dst[d++] = '\\'; dst[d++] = '"';  break;
      case '\\': dst[d++] = '\\'; dst[d++] = '\\'; break;
      case '\n': dst[d++] = '\\'; dst[d++] = 'n';  break;
      case '\r': dst[d++] = '\\'; dst[d++] = 'r';  break;
      case '\t': dst[d++] = '\\'; dst[d++] = 't';  break;
      default:
        if (c < 0x20) {
          if (d + 6 >= n) break;
          snprintf(dst + d, n - d, "\\u%04x", c);
          d += 6;
        } else {
          dst[d++] = char(c);
        }
    }
  }
  dst[d] = '\0';
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
// Returns the Art-Net broadcast target: STA subnet when on a router, AP subnet otherwise
static IPAddress artOutTarget() {
  if (WiFi.status() == WL_CONNECTED) {
    uint32_t ip   = (uint32_t)WiFi.localIP();
    uint32_t mask = (uint32_t)WiFi.subnetMask();
    return IPAddress((ip & mask) | (~mask));
  }
  return IPAddress(10, 0, 0, 255);
}

static void startWiFi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  // Empty AP SSID means first boot or new firmware tag: generate one random name.
  if (apSsid.isEmpty()) {
    char suffix[7];
    snprintf(suffix, sizeof(suffix), "%06lX", (unsigned long)(esp_random() & 0xFFFFFF));
    apSsid = String("vi_di_li_") + suffix;
    needSaveConfig = true;
  }

  WiFi.softAPConfig(IPAddress(10,0,0,1), IPAddress(10,0,0,1), IPAddress(255,255,255,0));
  WiFi.softAP(apSsid.c_str(), apPass.c_str());
  if (staSSID.length()) {
    WiFi.begin(staSSID.c_str(), staPass.c_str());
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) delay(100);
  }
}

// ── DMX ───────────────────────────────────────────────────────────────────────
static void setupDMX() {
  dmx_config_t cfg = DMX_CONFIG_DEFAULT;
  dmx_driver_install(dmxPort, &cfg, nullptr, 0);
  dmx_set_pin(dmxPort, DMX_TX, DMX_PIN_NO_CHANGE, DMX_DIR);
  memset(dmxFrame, 0, sizeof(dmxFrame));
}

static bool artnetActive() {
  return lastArtnetMs && (millis() - lastArtnetMs < ARTNET_TIMEOUT_MS);
}

// ── Scenes ────────────────────────────────────────────────────────────────────
static void loadScene(uint8_t n, uint8_t* dst) {
  prefs.begin("scenes", true);
  size_t got = prefs.getBytes((String("s") + n).c_str(), dst, MAX_CH);
  prefs.end();
  if (got != MAX_CH) memset(dst, 0, MAX_CH);
}

static void saveScene(uint8_t n, const uint8_t* src) {
  prefs.begin("scenes", false);
  prefs.putBytes((String("s") + n).c_str(), src, MAX_CH);
  prefs.end();
}

// ── Fade (call with gLock held) ───────────────────────────────────────────────
static void startFade_locked(const uint8_t* target, uint32_t ms) {
  memcpy(fadeFrom, webVals, MAX_CH);
  memcpy(fadeTo,   target,  MAX_CH);
  fadeStartMs = millis();
  fadeTimeMs  = ms < 1 ? 1 : ms;
  fadeActive  = true;
}

static void updateFade_locked() {
  if (!fadeActive) return;
  uint32_t elapsed = millis() - fadeStartMs;
  if (elapsed >= fadeTimeMs) {
    memcpy(webVals, fadeTo, MAX_CH);
    fadeActive = false;
    return;
  }
  uint32_t k = (elapsed << 8) / fadeTimeMs;
  for (int i = 0; i < MAX_CH; i++) {
    int delta = int(fadeTo[i]) - int(fadeFrom[i]);
    webVals[i] = uint8_t(int(fadeFrom[i]) + ((delta * int(k)) >> 8));
  }
}

// ── Output ────────────────────────────────────────────────────────────────────
static void computeOutput(const uint8_t* web, const uint8_t* art, bool aActive) {
  switch (mode) {
    case MODE_WEB:
      memcpy(outVals, web, MAX_CH);
      break;
    case MODE_ARTNET:
      memcpy(outVals, aActive ? art : holdVals, MAX_CH);
      break;
    default: // MODE_HTP
      if (aActive)
        for (int i = 0; i < MAX_CH; i++) outVals[i] = max(web[i], art[i]);
      else
        memcpy(outVals, web, MAX_CH);
  }
  // Apply master dimmer
  if (masterDimmer < 255)
    for (int i = 0; i < MAX_CH; i++)
      outVals[i] = vidili::applyMaster(outVals[i], masterDimmer);
}

static void sendDMX() {
  dmx_wait_sent(dmxPort, DMX_SEND_WAIT);
  memcpy(dmxFrame + 1, outVals, MAX_CH);
  dmx_write(dmxPort, dmxFrame, MAX_CH + 1);
  dmx_send(dmxPort);
  memcpy(holdVals, outVals, MAX_CH);
}

// ── Art-Net output (master → slaves broadcast) ────────────────────────────────
static void sendArtNetOut() {
  if (!artOutEnabled) return;
  static uint8_t  seq = 1;
  static uint8_t  pkt[18 + MAX_CH];
  static const uint8_t hdr[12] = {
    'A','r','t','-','N','e','t',0,   // ID
    0x00, 0x50,                       // OpDmx (little-endian)
    0x00, 0x0e                        // Protocol v14
  };
  memcpy(pkt, hdr, 12);
  pkt[12] = seq++;
  pkt[13] = 0;
  pkt[14] = uint8_t((artSubnet << 4) | artUni);
  pkt[15] = artNet;
  pkt[16] = 0x02;   // length hi (512)
  pkt[17] = 0x00;   // length lo
  memcpy(pkt + 18, outVals, MAX_CH);
  artOutUdp.beginPacket(artOutTarget(), ARTNET_PORT);
  artOutUdp.write(pkt, sizeof(pkt));
  artOutUdp.endPacket();
}

// ── Art-Net IN callback ───────────────────────────────────────────────────────
static void onDmxFrame(uint16_t uni, uint16_t len, uint8_t /*seq*/, uint8_t* data) {
  if (uni != universe()) return;
  uint16_t n = min(uint16_t(MAX_CH), len);
  memcpy(artVals, data, n);
  if (n < MAX_CH) memset(artVals + n, 0, MAX_CH - n);
  lastArtnetMs = millis();
}

// ── Editor HTML ───────────────────────────────────────────────────────────────
static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html><html lang="en">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>vi_di_li</title>
<style>
:root{--a:#6366f1;--bg:#f4f4f5;--card:#fff;--bd:#e4e4e7;--tx:#18181b;--sub:#71717a;--r:#ef4444}
*{box-sizing:border-box;margin:0;padding:0}
body{font:14px/1.5 system-ui,sans-serif;background:var(--bg);color:var(--tx);padding:16px;max-width:1020px;margin:0 auto}
h1{font-size:1.2rem;font-weight:700;margin-bottom:14px;display:flex;align-items:center;gap:8px}
.card{background:var(--card);border:1px solid var(--bd);border-radius:14px;padding:14px;margin-bottom:10px}
.ttl{font-size:.73rem;font-weight:700;text-transform:uppercase;letter-spacing:.07em;color:var(--sub);margin-bottom:10px}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:8px}
.row:last-child{margin-bottom:0}
input,select,button{font-size:.85rem;padding:6px 11px;border-radius:9px;border:1px solid var(--bd);background:var(--card);color:var(--tx);outline:none}
input:focus,select:focus{border-color:var(--a)}
button{cursor:pointer;background:var(--a);color:#fff;border:none;font-weight:600;transition:opacity .12s}
button:hover{opacity:.82}
button.g{background:transparent;border:1px solid var(--bd);color:var(--tx)}
button.r{background:var(--r);color:#fff;border:none}
.pill{font-size:.73rem;font-weight:600;padding:3px 9px;border-radius:99px;background:var(--bg);border:1px solid var(--bd)}
.on{background:#dcfce7;border-color:#86efac;color:#166534}
.off{color:var(--sub)}
.warn{background:#fef9c3;border-color:#fde047;color:#713f12}
.ao{background:#ede9fe;border-color:#a5b4fc;color:#3730a3}
.ch{display:grid;grid-template-columns:52px 1fr 42px 42px;gap:5px 8px;align-items:center}
.cl{font-size:.75rem;color:var(--sub)}
.cv,.co{font-size:.78rem;text-align:right;font-variant-numeric:tabular-nums}
.co{color:var(--sub)}.co.hi{color:var(--a);font-weight:700}
input[type=range]{width:100%;accent-color:var(--a);cursor:pointer}
pre{background:var(--bg);border:1px solid var(--bd);border-radius:9px;padding:10px;font-size:.78rem;overflow:auto;white-space:pre-wrap;word-break:break-all}
.nets{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}
.ni{font-size:.8rem;padding:4px 10px;border:1px solid var(--bd);border-radius:8px;cursor:pointer}
.ni:hover{border-color:var(--a);color:var(--a)}
.hint{font-size:.78rem;color:var(--sub)}
hr{border:none;border-top:1px solid var(--bd);margin:10px 0}
a{color:var(--a);font-size:.85rem;text-decoration:none}
</style>
</head>
<body>
<h1>vi_di_li <span class="pill off" id="modePill">…</span> <a href="/vj" style="margin-left:auto">VJ →</a></h1>

<div class="card">
  <div class="ttl">Status</div>
  <div class="row">
    <span class="pill off" id="anPill">Art-Net –</span>
    <span class="pill off" id="aoPill">OUT –</span>
    <span class="pill off" id="staPill">STA –</span>
    <span class="hint">AP: <b id="apip">10.0.0.1</b></span>
  </div>
  <div class="row">
    <select id="mSel" onchange="setMode(this.value)">
      <option value="0">WEB_ONLY</option>
      <option value="1">ARTNET_ONLY</option>
      <option value="2">MERGE_HTP</option>
    </select>
    <button class="g" onclick="blackout()">Blackout</button>
    <button class="g" onclick="fullOn()">Full on</button>
    <button class="r" style="margin-left:auto" onclick="if(confirm('Reboot?'))fetch('/reboot')">Reboot</button>
  </div>
</div>

<div class="card">
  <div class="ttl">Network – STA</div>
  <div class="row">
    <input id="wssid" placeholder="SSID" style="width:180px">
    <input id="wpass" type="password" placeholder="Password" style="width:140px">
    <button onclick="wifiSet()">Connect</button>
    <button class="g" onclick="wifiForget()">Forget</button>
    <button class="g" onclick="wifiScan()">Scan</button>
    <span class="hint" id="scanSt"></span>
  </div>
  <div class="nets" id="nets"></div>
  <hr>
  <div class="ttl">Node / AP</div>
  <div class="row">
    <span class="hint">Name</span><input id="cfgName" placeholder="vi_di_li" style="width:130px">
    <span class="hint">AP SSID</span><input id="cfgApSsid" placeholder="vi_di_li" style="width:130px">
    <span class="hint">AP Pass</span><input id="cfgApPass" type="password" placeholder="password" style="width:120px">
    <button onclick="saveNode()">Save &amp; Reboot</button>
  </div>
</div>

<div class="card">
  <div class="ttl">Art-Net Universe</div>
  <div class="row">
    Net <input id="net" type="number" min="0" max="127" style="width:76px">
    Sub <input id="sub" type="number" min="0" max="15"  style="width:62px">
    Uni <input id="uni" type="number" min="0" max="15"  style="width:62px">
    <button onclick="setUni()">Apply</button>
    <span class="hint" id="u15"></span>
  </div>
</div>

<div class="card">
  <div class="ttl">Channels</div>
  <div class="row">
    Page <select id="page" onchange="changePage()"></select>
    <span class="hint" id="pinfo"></span>
    <span class="hint" style="margin-left:auto">WEB &nbsp;›&nbsp; <span style="color:var(--a)">OUT</span></span>
  </div>
  <div id="sl" class="ch"></div>
</div>

<div class="card">
  <div class="ttl">Scenes</div>
  <div class="row">Fade ms <input id="fade" value="1000" style="width:90px"> <span class="hint">web layer</span></div>
  <div class="row"><span class="hint" style="min-width:50px">Recall</span>
    <button class="g" onclick="rec(0)">1</button><button class="g" onclick="rec(1)">2</button>
    <button class="g" onclick="rec(2)">3</button><button class="g" onclick="rec(3)">4</button>
    <button class="g" onclick="rec(4)">5</button><button class="g" onclick="rec(5)">6</button>
    <button class="g" onclick="rec(6)">7</button><button class="g" onclick="rec(7)">8</button>
  </div>
  <div class="row"><span class="hint" style="min-width:50px">Save</span>
    <button class="g" onclick="sav(0)">S1</button><button class="g" onclick="sav(1)">S2</button>
    <button class="g" onclick="sav(2)">S3</button><button class="g" onclick="sav(3)">S4</button>
    <button class="g" onclick="sav(4)">S5</button><button class="g" onclick="sav(5)">S6</button>
    <button class="g" onclick="sav(6)">S7</button><button class="g" onclick="sav(7)">S8</button>
  </div>
</div>

<div class="card">
  <div class="ttl">Output Monitor – ch 1–64
    <button class="g" style="float:right;font-size:.72rem;margin-top:-3px" onclick="refreshMon()">Refresh</button>
  </div>
  <pre id="mon">–</pre>
</div>

<script>
const $=id=>document.getElementById(id);
let curPage=0,pageWeb=[],pageOut=[];

function escHtml(s){
  return s.replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

function escJsSq(s){
  return s.replace(/\\/g,'\\\\').replace(/'/g,"\\'").replace(/\n/g,'\\n').replace(/\r/g,'\\r');
}

function mkPages(){
  $('page').innerHTML=[...Array(16)].map((_,i)=>
    `<option value="${i}">Page ${i+1} &nbsp;ch ${i*32+1}–${i*32+32}</option>`).join('');
}

// WebSocket for real-time status push
const ws=new WebSocket('ws://'+location.host+'/ws');
ws.onmessage=e=>{
  let s;try{s=JSON.parse(e.data);}catch{return;}
  $('apip').textContent=s.ip;
  $('mSel').value=String(s.mode);
  $('modePill').textContent=s.mode_name;
  $('modePill').className='pill '+(s.artnet_active?'on':'off');
  $('net').value=s.net;$('sub').value=s.subnet;$('uni').value=s.uni;
  $('u15').textContent='15-bit: '+s.uni15;
  $('anPill').textContent='AN: '+(s.artnet_active?'active':'idle');
  $('anPill').className='pill '+(s.artnet_active?'on':'off');
  $('aoPill').textContent='OUT: '+(s.ao?'ON':'off');
  $('aoPill').className='pill '+(s.ao?'ao':'off');
  const c=s.sta_connected;
  $('staPill').textContent=c?'STA: '+s.sta_ip:s.sta_ssid?'STA: connecting…':'STA: –';
  $('staPill').className='pill '+(c?'on':s.sta_ssid?'warn':'off');
  if(s.sta_ssid&&!$('wssid').value)$('wssid').placeholder=s.sta_ssid;
  if(!$('cfgName').value)$('cfgName').placeholder=s.name;
  if(!$('cfgApSsid').value)$('cfgApSsid').placeholder=s.ssid;
};
ws.onclose=()=>{setTimeout(()=>location.reload(),3000);};

async function api(url){
  try{return await(await fetch(url,{cache:'no-store'})).json();}catch{return null;}
}

function renderSliders(base){
  $('sl').innerHTML=[...Array(32)].map((_,i)=>{
    const ch=base+i,w=pageWeb[i]??0,o=pageOut[i]??0;
    return `<div class="cl">Ch ${ch}</div>
<input type="range" min="0" max="255" value="${w}"
  oninput="setCh(${ch},+this.value);this.nextElementSibling.textContent=this.value">
<div class="cv">${w}</div><div class="co${o>w?' hi':''}">${o}</div>`;
  }).join('');
}

function refreshOut(){
  $('sl').querySelectorAll('.co').forEach((el,i)=>{
    const o=pageOut[i]??0,w=+(el.previousElementSibling.previousElementSibling.value);
    el.textContent=o;el.className='co'+(o>w?' hi':'');
  });
}

async function loadPage(){
  const s=await api('/page?i='+curPage);if(!s)return;
  pageWeb=s.web??[];pageOut=s.out??[];
  $('pinfo').textContent=`ch ${s.base_ch}–${s.base_ch+31}`;
  renderSliders(s.base_ch);
}
function changePage(){curPage=+$('page').value;loadPage();}

let tSend=0;
function setCh(ch,v){const n=Date.now();if(n-tSend>35){tSend=n;fetch(`/set?ch=${ch}&v=${v}`);}}
function setMode(m){fetch('/mode/set?m='+m);}
function setUni(){fetch(`/artnet/set?net=${$('net').value}&subnet=${$('sub').value}&uni=${$('uni').value}`);}
function blackout(){fetch('/blackout');}
function fullOn(){fetch('/full');}
function rec(i){fetch(`/scene/recall?n=${i}&fade=${+$('fade').value||0}`);}
function sav(i){fetch('/scene/save?n='+i);}

async function refreshMon(){
  const s=await api('/monitor');if(!s)return;
  $('mon').textContent=JSON.stringify(s.out);
}

let scanTimer=null;
async function wifiScan(){
  if(scanTimer){clearInterval(scanTimer);scanTimer=null;}
  $('scanSt').textContent='Scanning…';$('nets').innerHTML='';
  await fetch('/wifi/scan');
  scanTimer=setInterval(async()=>{
    const r=await api('/wifi/scan');
    if(!r||!r.scanning){clearInterval(scanTimer);scanTimer=null;$('scanSt').textContent='';showNets(r?.networks??[]);}
  },1500);
}
function showNets(nets){
  $('nets').innerHTML=nets.length
    ?nets.map(n=>`<div class="ni" onclick="$('wssid').value='${escJsSq(n.ssid)}'">${escHtml(n.ssid)}${n.secure?' 🔒':''} <span class="hint">${n.rssi}dBm</span></div>`).join('')
    :'<span class="hint">No networks found</span>';
}
function wifiSet(){
  const s=$('wssid').value.trim();if(!s){alert('Enter SSID');return;}
  fetch(`/wifi/set?ssid=${encodeURIComponent(s)}&pass=${encodeURIComponent($('wpass').value)}`);
  $('scanSt').textContent='Connecting…';
}
function wifiForget(){
  fetch('/wifi/forget');$('wssid').value='';$('wpass').value='';$('scanSt').textContent='';
}
function saveNode(){
  const p=new URLSearchParams();
  const name=$('cfgName').value.trim(),ssid=$('cfgApSsid').value.trim(),pass=$('cfgApPass').value;
  if(name)p.append('name',name);if(ssid)p.append('ap_ssid',ssid);if(pass)p.append('ap_pass',pass);
  if(!p.toString()){alert('Nothing to save');return;}
  fetch('/node/set?'+p).then(()=>setTimeout(()=>fetch('/reboot'),300));
}

mkPages();loadPage();refreshMon();
setInterval(async()=>{
  const s=await api('/page?i='+curPage);if(!s)return;
  pageWeb=s.web??[];pageOut=s.out??[];
  $('sl').querySelectorAll('input[type=range]').forEach((sl,i)=>{
    sl.value=pageWeb[i]??0;sl.nextElementSibling.textContent=pageWeb[i]??0;
  });
  refreshOut();
},1000);
</script>
</body></html>)HTML";

// ── VJ UI HTML ────────────────────────────────────────────────────────────────
static const char VJ_HTML[] PROGMEM = R"HTML(<!doctype html><html lang="en">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>vi_di_li VJ</title>
<style>
:root{--a:#6366f1;--bg:#09090b;--card:#18181b;--bd:#27272a;--tx:#fafafa;--sub:#52525b;--r:#ef4444}
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
body{font:14px/1.5 system-ui,sans-serif;background:var(--bg);color:var(--tx);padding:12px;min-height:100dvh;touch-action:manipulation;user-select:none}
h1{font-size:1rem;font-weight:700;margin-bottom:10px;display:flex;align-items:center;gap:8px}
.pill{font-size:.72rem;font-weight:600;padding:2px 8px;border-radius:99px;border:1px solid var(--bd)}
.on{background:#14532d;border-color:#16a34a;color:#4ade80}
.off{color:var(--sub)}
.ao{background:#1e1b4b;border-color:#6366f1;color:#a5b4fc}
a{color:var(--a);text-decoration:none;font-size:.8rem;margin-left:auto}
.g2{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:8px}
.g4{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-bottom:8px}
.btn{height:70px;font-size:1rem;font-weight:700;border-radius:14px;border:2px solid var(--bd);background:var(--card);color:var(--tx);cursor:pointer;transition:border-color .1s,background .1s,color .1s}
.btn:active,.btn.act{border-color:var(--a);background:#1e1b4b;color:#a5b4fc}
.bk{background:var(--r)!important;border-color:var(--r)!important;color:#fff;font-size:1.1rem}
.bk:active{opacity:.75}
.fo{background:#fff!important;border-color:#fff!important;color:#000;font-size:1.1rem}
.fo:active{opacity:.75}
.sec{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:12px;margin-bottom:8px}
.lbl{font-size:.7rem;text-transform:uppercase;letter-spacing:.07em;color:var(--sub);margin-bottom:8px;display:flex;align-items:center;gap:8px}
input[type=range]{width:100%;accent-color:var(--a);height:32px;cursor:pointer}
.pct{font-size:2.4rem;font-weight:800;text-align:center;font-variant-numeric:tabular-nums;letter-spacing:-.02em;padding:4px 0}
select,input[type=number]{font-size:.85rem;padding:6px 10px;border-radius:8px;border:1px solid var(--bd);background:#27272a;color:var(--tx);outline:none}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.togbtn{height:36px;font-size:.82rem;padding:0 14px;border-radius:9px;border:2px solid var(--bd);background:transparent;color:var(--sub);cursor:pointer;font-weight:600;transition:all .15s}
.togbtn.on{border-color:var(--a);color:#a5b4fc}
</style>
</head>
<body>
<h1>vi_di_li VJ
  <span class="pill off" id="anPill">AN –</span>
  <span class="pill off" id="aoPill">OUT –</span>
  <a href="/">Editor →</a>
</h1>

<div class="g2">
  <button class="btn bk" onclick="blackout()">■ BLACKOUT</button>
  <button class="btn fo" onclick="fullOn()">● FULL ON</button>
</div>

<div class="sec">
  <div class="lbl">Master Dimmer</div>
  <input type="range" id="dim" min="0" max="255" value="255" oninput="onDim(+this.value)">
  <div class="pct" id="pct">100%</div>
</div>

<div class="sec">
  <div class="lbl">Scenes
    <span style="margin-left:auto;display:flex;align-items:center;gap:6px;font-size:.82rem;font-weight:400;text-transform:none;letter-spacing:0">
      Fade ms <input type="number" id="fade" value="1000" style="width:80px">
    </span>
  </div>
  <div class="g4">
    <button class="btn" id="s0" onclick="rec(0)">1</button>
    <button class="btn" id="s1" onclick="rec(1)">2</button>
    <button class="btn" id="s2" onclick="rec(2)">3</button>
    <button class="btn" id="s3" onclick="rec(3)">4</button>
    <button class="btn" id="s4" onclick="rec(4)">5</button>
    <button class="btn" id="s5" onclick="rec(5)">6</button>
    <button class="btn" id="s6" onclick="rec(6)">7</button>
    <button class="btn" id="s7" onclick="rec(7)">8</button>
  </div>
</div>

<div class="sec">
  <div class="lbl">Control</div>
  <div class="row">
    <select id="mSel" onchange="fetch('/mode/set?m='+this.value)">
      <option value="0">WEB_ONLY</option>
      <option value="1">ARTNET_ONLY</option>
      <option value="2">MERGE_HTP</option>
    </select>
    <button class="togbtn" id="aoBtn" onclick="toggleAo()">Art-Net OUT: OFF</button>
  </div>
</div>

<script>
const $=id=>document.getElementById(id);
let aoEnabled=false;

let dimT=null;
function onDim(v){
  $('pct').textContent=Math.round(v/255*100)+'%';
  clearTimeout(dimT);dimT=setTimeout(()=>fetch('/master?v='+v),40);
}
function blackout(){fetch('/blackout');}
function fullOn(){fetch('/full');}
function rec(i){
  document.querySelectorAll('[id^=s]').forEach(b=>b.classList.remove('act'));
  $('s'+i).classList.add('act');
  fetch('/scene/recall?n='+i+'&fade='+(+$('fade').value||0));
}
function toggleAo(){
  aoEnabled=!aoEnabled;
  fetch('/artout/set?en='+(aoEnabled?1:0));
  updateAoBtn();
}
function updateAoBtn(){
  $('aoBtn').textContent='Art-Net OUT: '+(aoEnabled?'ON':'OFF');
  $('aoBtn').className='togbtn'+(aoEnabled?' on':'');
}

const ws=new WebSocket('ws://'+location.host+'/ws');
ws.onmessage=e=>{
  let s;try{s=JSON.parse(e.data);}catch{return;}
  $('anPill').textContent='AN: '+(s.artnet_active?'active':'idle');
  $('anPill').className='pill '+(s.artnet_active?'on':'off');
  $('aoPill').textContent='OUT: '+(s.ao?'ON':'off');
  $('aoPill').className='pill '+(s.ao?'ao':'off');
  $('mSel').value=String(s.mode);
  if(s.ao!==aoEnabled){aoEnabled=s.ao;updateAoBtn();}
  if(document.activeElement!==$('dim')){
    $('dim').value=s.dim;
    $('pct').textContent=Math.round(s.dim/255*100)+'%';
  }
};
ws.onclose=()=>{setTimeout(()=>location.reload(),2000);};
</script>
</body></html>)HTML";

// ── JSON helpers ──────────────────────────────────────────────────────────────
static void sendJSON(AsyncWebServerRequest* req, const String& body) {
  AsyncWebServerResponse* res = req->beginResponse(200, "application/json", body);
  res->addHeader("Access-Control-Allow-Origin", "*");
  res->addHeader("Cache-Control", "no-store");
  req->send(res);
}

static String statusJSON() {
  bool staCon = (WiFi.status() == WL_CONNECTED);
  char eName[80], eSsid[80], eStaSsid[80];
  jsonEsc(eName,    sizeof(eName),    nodeName);
  jsonEsc(eSsid,    sizeof(eSsid),    apSsid);
  jsonEsc(eStaSsid, sizeof(eStaSsid), staSSID);

  char buf[640];
  snprintf(buf, sizeof(buf),
    "{"
    "\"ip\":\"%s\","
    "\"sta_ip\":\"%s\","
    "\"sta_connected\":%s,"
    "\"sta_ssid\":\"%s\","
    "\"ssid\":\"%s\","
    "\"name\":\"%s\","
    "\"net\":%u,\"subnet\":%u,\"uni\":%u,"
    "\"uni15\":%u,"
    "\"mode\":%u,"
    "\"mode_name\":\"%s\","
    "\"artnet_active\":%s,"
    "\"ao\":%s,"
    "\"web\":%s,"
    "\"dim\":%u,"
    "\"ao_target\":\"%s\""
    "}",
    ipStr(WiFi.softAPIP()).c_str(),
    staCon ? ipStr(WiFi.localIP()).c_str() : "",
    staCon ? "true" : "false",
    eStaSsid, eSsid, eName,
    artNet, artSubnet, artUni,
    universe(),
    uint8_t(mode), modeName(mode),
    artnetActive()  ? "true" : "false",
    artOutEnabled   ? "true" : "false",
    webEnabled      ? "true" : "false",
    masterDimmer,
    ipStr(artOutTarget()).c_str()
  );
  return buf;
}

static String pageJSON(uint16_t page) {
  if (page >= PAGE_COUNT) page = PAGE_COUNT - 1;
  uint16_t base = page * PAGE_SIZE;

  static uint8_t snapWeb[PAGE_SIZE];
  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) == pdTRUE) {
    memcpy(snapWeb, webVals + base, PAGE_SIZE);
    xSemaphoreGive(gLock);
  } else {
    memcpy(snapWeb, webVals + base, PAGE_SIZE);
  }

  char buf[440];
  int pos = snprintf(buf, sizeof(buf),
    "{\"page\":%u,\"base_ch\":%u,\"web\":[", page, base + 1);
  for (int i = 0; i < PAGE_SIZE; i++)
    pos += snprintf(buf + pos, sizeof(buf) - pos, i < PAGE_SIZE-1 ? "%u," : "%u", snapWeb[i]);
  pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"out\":[");
  for (int i = 0; i < PAGE_SIZE; i++)
    pos += snprintf(buf + pos, sizeof(buf) - pos, i < PAGE_SIZE-1 ? "%u," : "%u", outVals[base + i]);
  snprintf(buf + pos, sizeof(buf) - pos, "]}");
  return buf;
}

static String monitorJSON() {
  char buf[360];
  int pos = snprintf(buf, sizeof(buf), "{\"out\":[");
  for (int i = 0; i < 64; i++)
    pos += snprintf(buf + pos, sizeof(buf) - pos, i < 63 ? "%u," : "%u", outVals[i]);
  snprintf(buf + pos, sizeof(buf) - pos, "]}");
  return buf;
}

static String wifiScanJSON() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING)  return "{\"scanning\":true}";
  if (n < 0) { WiFi.scanNetworks(true); return "{\"scanning\":true}"; }

  String s = "{\"scanning\":false,\"networks\":[";
  for (int i = 0; i < n; i++) {
    if (i) s += ',';
    char ssidEsc[70];
    jsonEsc(ssidEsc, sizeof(ssidEsc), WiFi.SSID(i));
    char entry[120];
    snprintf(entry, sizeof(entry),
      "{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
      ssidEsc, WiFi.RSSI(i),
      WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false");
    s += entry;
  }
  WiFi.scanDelete();
  return s + "]}";
}

// ── Web routes ────────────────────────────────────────────────────────────────
static void setupWeb() {
  // WebSocket
  ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType, void*, uint8_t*, size_t){});
  server.addHandler(&ws);

  server.on("/",   HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200, "text/html", INDEX_HTML); });
  server.on("/vj", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200, "text/html", VJ_HTML); });

  server.on("/status",  HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, statusJSON()); });
  server.on("/monitor", HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, monitorJSON()); });

  server.on("/page", HTTP_GET, [](AsyncWebServerRequest* r){
    uint16_t p = r->hasArg("i")
      ? uint16_t(constrain(r->arg("i").toInt(), 0, PAGE_COUNT - 1)) : 0;
    sendJSON(r, pageJSON(p));
  });

  server.on("/set", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!r->hasArg("ch") || !r->hasArg("v")) { r->send(400, "text/plain", "missing arg"); return; }
    uint16_t ch = uint16_t(constrain(r->arg("ch").toInt(), 1, MAX_CH)) - 1;
    uint8_t  v  = uint8_t(constrain(r->arg("v").toInt(), 0, 255));
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    webVals[ch] = v;
    xSemaphoreGive(gLock);
    r->send(204);
  });

  server.on("/blackout", HTTP_GET, [](AsyncWebServerRequest* r){
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    memset(webVals, 0, MAX_CH);
    fadeActive = false;
    xSemaphoreGive(gLock);
    r->send(204);
  });

  server.on("/full", HTTP_GET, [](AsyncWebServerRequest* r){
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    memset(webVals, 255, MAX_CH);
    fadeActive = false;
    xSemaphoreGive(gLock);
    r->send(204);
  });

  server.on("/master", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasArg("v"))
      masterDimmer = uint8_t(constrain(r->arg("v").toInt(), 0, 255));
    r->send(204);
  });

  server.on("/artnet/set", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasArg("net"))    artNet    = uint8_t(constrain(r->arg("net").toInt(),    0, 127));
    if (r->hasArg("subnet")) artSubnet = uint8_t(constrain(r->arg("subnet").toInt(), 0, 15));
    if (r->hasArg("uni"))    artUni    = uint8_t(constrain(r->arg("uni").toInt(),    0, 15));
    saveConfig(); r->send(204);
  });

  server.on("/artout/set", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasArg("en")) artOutEnabled = r->arg("en").toInt() != 0;
    saveConfig(); r->send(204);
  });

  server.on("/mode/set", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!r->hasArg("m")) { r->send(400, "text/plain", "missing arg"); return; }
    mode = Mode(constrain(r->arg("m").toInt(), 0, int(MODE_HTP)));
    saveConfig(); r->send(204);
  });

  server.on("/web/set", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!r->hasArg("en")) { r->send(400, "text/plain", "missing arg"); return; }
    webEnabled = r->arg("en").toInt() != 0;
    saveConfig();
    r->send(200, "text/plain", "ok,rebooting");
    pendingReboot = true;
  });

  server.on("/scene/save", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!r->hasArg("n")) { r->send(400, "text/plain", "missing arg"); return; }
    uint8_t n = uint8_t(constrain(r->arg("n").toInt(), 0, SCENE_COUNT - 1));
    static uint8_t snap[MAX_CH];
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    memcpy(snap, webVals, MAX_CH);
    xSemaphoreGive(gLock);
    saveScene(n, snap);
    r->send(204);
  });

  server.on("/scene/recall", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!r->hasArg("n")) { r->send(400, "text/plain", "missing arg"); return; }
    uint8_t  n  = uint8_t(constrain(r->arg("n").toInt(), 0, SCENE_COUNT - 1));
    uint32_t ft = r->hasArg("fade") ? uint32_t(max(0L, r->arg("fade").toInt())) : 0;
    loadScene(n, sceneBuf);
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    startFade_locked(sceneBuf, ft);
    xSemaphoreGive(gLock);
    r->send(204);
  });

  server.on("/wifi/scan",   HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, wifiScanJSON()); });

  server.on("/wifi/set", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!r->hasArg("ssid")) { r->send(400, "text/plain", "missing ssid"); return; }
    staSSID = r->arg("ssid");
    staPass = r->hasArg("pass") ? r->arg("pass") : "";
    saveConfig();
    WiFi.disconnect(false);
    WiFi.begin(staSSID.c_str(), staPass.c_str());
    r->send(204);
  });

  server.on("/wifi/forget", HTTP_GET, [](AsyncWebServerRequest* r){
    staSSID = ""; staPass = "";
    saveConfig(); WiFi.disconnect(true); r->send(204);
  });

  server.on("/node/set", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasArg("name"))    nodeName = r->arg("name");
    if (r->hasArg("ap_ssid")) apSsid   = r->arg("ap_ssid");
    if (r->hasArg("ap_pass")) apPass   = r->arg("ap_pass");
    saveConfig(); r->send(204);
  });

  server.on("/reboot", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200, "text/plain", "Rebooting…");
    pendingReboot = true;
  });

  server.onNotFound([](AsyncWebServerRequest* r){ r->send(404, "text/plain", "Not found"); });
  server.begin();
}

// ── Setup / Loop ──────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  gLock = xSemaphoreCreateMutex();

  loadConfig();

  memset(webVals,  0, MAX_CH);
  memset(artVals,  0, MAX_CH);
  memset(outVals,  0, MAX_CH);
  memset(holdVals, 0, MAX_CH);

  setupDMX();
  startWiFi();
  if (needSaveConfig) { saveConfig(); needSaveConfig = false; }
  artOutUdp.begin(0);
  if (webEnabled) setupWeb();

  artnet.begin(nodeName.c_str());
  artnet.setArtDmxCallback(onDmxFrame);
  lastArtnetMs = 0;

  Serial.printf("\n=== vi_di_li ===\n");
  Serial.printf("AP SSID  : %s\n", apSsid.c_str());
  Serial.printf("AP IP    : %s\n", ipStr(WiFi.softAPIP()).c_str());
  if (staSSID.length()) {
    Serial.printf("STA SSID : %s\n", staSSID.c_str());
    if (WiFi.status() == WL_CONNECTED)
      Serial.printf("STA IP   : %s\n", ipStr(WiFi.localIP()).c_str());
  }
  Serial.printf("Mode     : %s\n", modeName(mode));
  Serial.printf("Art-Net  : net=%u sub=%u uni=%u (15-bit=%u)\n",
    artNet, artSubnet, artUni, universe());
  Serial.printf("Art-Out  : %s\n", artOutEnabled ? "enabled" : "disabled");
  Serial.printf("Web      : %s\n", webEnabled ? "enabled" : "disabled");
}

void loop() {
  if (pendingReboot) { delay(150); ESP.restart(); }

  artnet.read();

  const uint32_t now = millis();

  // DMX output cycle
  static uint32_t lastDmx = 0;
  if (now - lastDmx >= DMX_PERIOD_MS) {
    lastDmx = now;

    static uint8_t snapWeb[MAX_CH];
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) == pdTRUE) {
      updateFade_locked();
      memcpy(snapWeb, webVals, MAX_CH);
      xSemaphoreGive(gLock);
    } else {
      memcpy(snapWeb, webVals, MAX_CH);
    }

    computeOutput(snapWeb, artVals, artnetActive());
    sendDMX();
    sendArtNetOut();
  }

  // WebSocket status push ~400ms (only when web stack is enabled)
  if (webEnabled) {
    static uint32_t lastWs = 0;
    if (now - lastWs >= 400) {
      lastWs = now;
      if (ws.count() > 0) ws.textAll(statusJSON());
      ws.cleanupClients();
    }
  }
}
