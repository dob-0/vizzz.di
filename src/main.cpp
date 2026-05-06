#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArtnetWifi.h>
#include <Preferences.h>

#include "esp_dmx.h"

// ======================= Hardware (ESP32 DevKit) =======================
#define DMX_TX   25   // GPIO25 -> MAX485 DI
#define DMX_DIR  21   // GPIO21 -> MAX485 DE+RE tied
static const int MAX_CH = 512;

// ======================= Timing =======================
static const uint32_t DMX_PERIOD_MS     = 20;    // ~50Hz
static const uint32_t ARTNET_TIMEOUT_MS = 3000;

// ======================= Web UI paging =======================
static const uint16_t PAGE_SIZE  = 32;
static const uint16_t PAGE_COUNT = 16;

// ======================= Scenes =======================
static const uint8_t SCENE_COUNT = 8;

// ======================= Mode engine =======================
enum Mode : uint8_t {
  MODE_WEB_ONLY    = 0,
  MODE_ARTNET_ONLY = 1,
  MODE_MERGE_HTP   = 2
};

static const char* modeName(Mode m) {
  switch (m) {
    case MODE_WEB_ONLY:    return "WEB_ONLY";
    case MODE_ARTNET_ONLY: return "ARTNET_ONLY";
    default:               return "MERGE_HTP";
  }
}

// ======================= Persistent config =======================
Preferences prefs;

String nodeName = "vi_di_li";
String apSsid   = "vi_di_li";
String apPass   = "Poghchka666#";
String staSSID  = "";
String staPass  = "";

uint8_t artNet    = 0;
uint8_t artSubnet = 0;
uint8_t artUni    = 0;

Mode mode = MODE_MERGE_HTP;

static inline uint16_t combinedUniverse() {
  return ((uint16_t(artNet) << 8) | (uint16_t(artSubnet) << 4) | (uint16_t(artUni) & 0x0F));
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
  prefs.putUChar("mode",      (uint8_t)mode);
  prefs.end();
}

static void loadConfig() {
  prefs.begin("cfg", true);
  nodeName  = prefs.getString("name",     nodeName);
  apSsid    = prefs.getString("ap_ssid",  apSsid);
  apPass    = prefs.getString("ap_pass",  apPass);
  staSSID   = prefs.getString("sta_ssid", "");
  staPass   = prefs.getString("sta_pass", "");
  artNet    = prefs.getUChar("anet_net",  artNet);
  artSubnet = prefs.getUChar("anet_sub",  artSubnet);
  artUni    = prefs.getUChar("anet_uni",  artUni);
  mode      = (Mode)prefs.getUChar("mode", (uint8_t)mode);
  prefs.end();
  if (mode > MODE_MERGE_HTP) mode = MODE_MERGE_HTP;
}

// ======================= Runtime =======================
AsyncWebServer server(80);
ArtnetWifi artnet;

dmx_port_t dmxPort = DMX_NUM_0;
uint8_t dmxFrame[MAX_CH + 1];

uint8_t webVals[MAX_CH];
uint8_t artVals[MAX_CH];
uint8_t outVals[MAX_CH];
uint8_t holdVals[MAX_CH];

volatile uint32_t lastArtnetMs = 0;

uint8_t sceneBuf[MAX_CH];

bool     fadeActive  = false;
uint32_t fadeStartMs = 0;
uint32_t fadeTimeMs  = 1000;
uint8_t  fadeFrom[MAX_CH];
uint8_t  fadeTo[MAX_CH];

// ======================= Helpers =======================
static String ipToStr(IPAddress ip) {
  return String(ip[0]) + "." + ip[1] + "." + ip[2] + "." + ip[3];
}

static String mdnsHostname() {
  String h = nodeName;
  for (int i = 0; i < (int)h.length(); i++) {
    if (!isAlphaNumeric(h[i]) && h[i] != '-') h[i] = '-';
  }
  h.toLowerCase();
  while (h.length() > 0 && h[0] == '-')            h = h.substring(1);
  while (h.length() > 0 && h[h.length()-1] == '-') h = h.substring(0, h.length()-1);
  if (h.length() == 0) h = "dilights";
  return h;
}

// AP always on; STA connects if credentials set (10s timeout)
static void startWiFi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP(apSsid.c_str(), apPass.c_str());

  if (staSSID.length() > 0) {
    WiFi.begin(staSSID.c_str(), staPass.c_str());
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) delay(100);
  }
}

static void setupMDNS() {
  String h = mdnsHostname();
  if (MDNS.begin(h.c_str())) {
    MDNS.addService("http",   "tcp", 80);
    MDNS.addService("artnet", "udp", 6454);
  }
}

static void setupDMX() {
  dmx_config_t cfg = DMX_CONFIG_DEFAULT;
  dmx_driver_install(dmxPort, &cfg, nullptr, 0);
  dmx_set_pin(dmxPort, DMX_TX, DMX_PIN_NO_CHANGE, DMX_DIR);
  dmxFrame[0] = 0;
}

static inline bool artnetActive() {
  return (lastArtnetMs != 0) && (millis() - lastArtnetMs < ARTNET_TIMEOUT_MS);
}

// ======================= Scenes =======================
static bool loadScene(uint8_t n, uint8_t* dst) {
  if (n >= SCENE_COUNT) return false;
  prefs.begin("scenes", true);
  size_t got = prefs.getBytes((String("s") + n).c_str(), dst, MAX_CH);
  prefs.end();
  if (got != MAX_CH) memset(dst, 0, MAX_CH);
  return true;
}

static bool saveScene(uint8_t n, const uint8_t* src) {
  if (n >= SCENE_COUNT) return false;
  prefs.begin("scenes", false);
  prefs.putBytes((String("s") + n).c_str(), src, MAX_CH);
  prefs.end();
  return true;
}

// ======================= Fade Engine =======================
static void startFadeTo(const uint8_t* target, uint32_t ms) {
  memcpy(fadeFrom, webVals, MAX_CH);
  memcpy(fadeTo,   target,  MAX_CH);
  fadeStartMs = millis();
  fadeTimeMs  = ms < 1 ? 1 : ms;
  fadeActive  = true;
}

static void updateFade() {
  if (!fadeActive) return;
  uint32_t t = millis() - fadeStartMs;
  if (t >= fadeTimeMs) {
    memcpy(webVals, fadeTo, MAX_CH);
    fadeActive = false;
    return;
  }
  float k = (float)t / (float)fadeTimeMs;
  for (int i = 0; i < MAX_CH; i++) {
    int v = fadeFrom[i] + (int)((fadeTo[i] - fadeFrom[i]) * k);
    webVals[i] = (uint8_t)constrain(v, 0, 255);
  }
}

// ======================= Output Engine =======================
static void computeOutput() {
  const bool aActive = artnetActive();
  switch (mode) {
    case MODE_WEB_ONLY:
      memcpy(outVals, webVals, MAX_CH);
      break;
    case MODE_ARTNET_ONLY:
      memcpy(outVals, aActive ? artVals : holdVals, MAX_CH);
      break;
    case MODE_MERGE_HTP:
    default:
      if (aActive) {
        for (int i = 0; i < MAX_CH; i++)
          outVals[i] = max(webVals[i], artVals[i]);
      } else {
        memcpy(outVals, webVals, MAX_CH);
      }
      break;
  }
}

static void sendDMX() {
  memcpy(dmxFrame + 1, outVals, MAX_CH);
  dmx_write(dmxPort, dmxFrame, MAX_CH + 1);
  dmx_send(dmxPort);
  memcpy(holdVals, outVals, MAX_CH);
}

// ======================= Art-Net callback =======================
static void onDmxFrame(uint16_t uni, uint16_t length, uint8_t /*seq*/, uint8_t* data) {
  if (uni != combinedUniverse()) return;
  uint16_t n = min((uint16_t)MAX_CH, length);
  memcpy(artVals, data, n);
  if (n < MAX_CH) memset(artVals + n, 0, MAX_CH - n);
  lastArtnetMs = millis();
}

// ======================= Web UI HTML =======================
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>vi_di_li</title>
  <style>
    body{font-family:system-ui,Segoe UI,Roboto,sans-serif;margin:14px;max-width:980px}
    .card{border:1px solid #ddd;border-radius:14px;padding:12px;margin:10px 0}
    .row{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin:8px 0}
    input,select,button{padding:8px 10px;border-radius:10px;border:1px solid #ccc}
    button{cursor:pointer}
    .grid{display:grid;grid-template-columns:78px 1fr 52px;gap:8px 10px;align-items:center}
    .val{text-align:right;font-variant-numeric:tabular-nums;color:#444}
    input[type=range]{width:100%}
    pre{background:#f6f6f6;border:1px solid #eee;border-radius:12px;padding:10px;overflow:auto}
    small{color:#666}
    .net-item{padding:6px 10px;border:1px solid #eee;border-radius:8px;cursor:pointer;margin:3px 0;display:inline-block}
    .net-item:hover{background:#f0f0f0}
    .sta-on{color:#2a7a2a;font-weight:600}
    .sta-off{color:#999}
  </style>
</head>
<body>
  <h2>vi_di_li - DMX Controller</h2>

  <div class="card">
    <div class="row">
      <b>Status</b>
      <span id="st"><small>Loading...</small></span>
    </div>
    <div class="row">
      <b>Mode</b>
      <select id="mode" onchange="setMode(this.value)">
        <option value="0">WEB_ONLY</option>
        <option value="1">ARTNET_ONLY</option>
        <option value="2">MERGE_HTP</option>
      </select>
      <button onclick="blackout()">Blackout</button>
      <button onclick="fullOn()">Full</button>
      <small>Art-Net: <span id="an">...</span></small>
    </div>
  </div>

  <div class="card">
    <div class="row"><b>Network</b></div>
    <div class="row">
      <small>AP: <span id="apip">192.168.4.1</span></small>
      <small>STA: <span id="staip" class="sta-off">not connected</span></small>
      <small>mDNS: <span id="mdns">...</span>.local</small>
    </div>
    <div class="row">
      SSID&nbsp;<input id="wssid" placeholder="Network SSID" style="width:200px">
      Pass&nbsp;<input id="wpass" type="password" placeholder="Password" style="width:160px">
      <button onclick="wifiSet()">Connect</button>
      <button onclick="wifiForget()">Forget</button>
    </div>
    <div class="row">
      <button onclick="wifiScan()">Scan networks</button>
      <span id="scanst"></span>
    </div>
    <div id="nets" style="display:flex;flex-wrap:wrap;gap:6px;margin-top:4px"></div>
  </div>

  <div class="card">
    <div class="row">
      <b>Art-Net Universe</b>
      Net <input id="net" type="number" min="0" max="127" style="width:86px">
      Sub <input id="sub" type="number" min="0" max="15"  style="width:70px">
      Uni <input id="uni" type="number" min="0" max="15"  style="width:70px">
      <button onclick="setUni()">Apply</button>
      <small id="u15"></small>
    </div>
  </div>

  <div class="card">
    <div class="row">
      <b>Channels</b>
      Page
      <select id="page" onchange="loadPage()"></select>
      <small id="pinfo"></small>
    </div>
    <div id="sl" class="grid"></div>
  </div>

  <div class="card">
    <div class="row">
      <b>Scenes</b>
      Fade (ms)
      <input id="fade" value="1000" style="width:110px">
      <small>(fade applies to web layer)</small>
    </div>
    <div class="row">
      <b>Recall</b>
      <button onclick="rec(0)">1</button><button onclick="rec(1)">2</button>
      <button onclick="rec(2)">3</button><button onclick="rec(3)">4</button>
      <button onclick="rec(4)">5</button><button onclick="rec(5)">6</button>
      <button onclick="rec(6)">7</button><button onclick="rec(7)">8</button>
    </div>
    <div class="row">
      <b>Save</b>
      <button onclick="sav(0)">S1</button><button onclick="sav(1)">S2</button>
      <button onclick="sav(2)">S3</button><button onclick="sav(3)">S4</button>
      <button onclick="sav(4)">S5</button><button onclick="sav(5)">S6</button>
      <button onclick="sav(6)">S7</button><button onclick="sav(7)">S8</button>
      <small>(saves current WEB values)</small>
    </div>
  </div>

  <div class="card">
    <div class="row">
      <b>Monitor</b> <small>(first 64 OUTPUT channels)</small>
      <button onclick="refreshMon()">Refresh</button>
    </div>
    <pre id="mon">[]</pre>
  </div>

<script>
const $=id=>document.getElementById(id);

function mkPages(){
  $('page').innerHTML=[...Array(16)].map((_,i)=>`<option value="${i}">${i+1}</option>`).join('');
  $('page').value='0';
}

async function status(){
  const s=await(await fetch('/status',{cache:'no-store'})).json();
  $('apip').textContent=s.ip;
  $('staip').textContent=s.sta_connected?s.sta_ip:s.sta_ssid?'connecting...':'not connected';
  $('staip').className=s.sta_connected?'sta-on':'sta-off';
  $('mdns').textContent=s.hostname;
  $('st').innerHTML=`<small>${s.name} &nbsp;|&nbsp; AP: ${s.ip}${s.sta_connected?' &nbsp;|&nbsp; STA: '+s.sta_ip:''}</small>`;
  $('mode').value=String(s.mode);
  $('net').value=s.net;$('sub').value=s.subnet;$('uni').value=s.uni;
  $('u15').innerHTML=`<small>uni15=${s.uni15}</small>`;
  $('an').textContent=s.artnet_active?'active':'idle';
  if(s.sta_ssid&&!$('wssid').value)$('wssid').placeholder='Current: '+s.sta_ssid;
}

async function loadPage(){
  const p=parseInt($('page').value)||0;
  const s=await(await fetch('/page?i='+p,{cache:'no-store'})).json();
  $('pinfo').innerHTML=`<small>Channels ${s.base_ch}-${s.base_ch+31}</small>`;
  $('sl').innerHTML=[...Array(32)].map((_,i)=>{
    const ch=s.base_ch+i,v=s.vals[i]||0;
    return `<div>Ch ${ch}</div>
      <input type="range" min="0" max="255" value="${v}"
        oninput="setCh(${ch},this.value);this.nextElementSibling.textContent=this.value;">
      <div class="val">${v}</div>`;
  }).join('');
}

let tSend=0;
function setCh(ch,v){const n=Date.now();if(n-tSend>35){tSend=n;fetch(`/set?ch=${ch}&v=${v}`);}}
function setMode(m){fetch(`/mode/set?m=${m}`);}
function setUni(){fetch(`/artnet/set?net=${$('net').value}&subnet=${$('sub').value}&uni=${$('uni').value}`);}
function blackout(){fetch('/blackout');}
function fullOn(){fetch('/full');}
function rec(i){fetch(`/scene/recall?n=${i}&fade=${parseInt($('fade').value)||0}`);}
function sav(i){fetch(`/scene/save?n=${i}`);}
async function refreshMon(){
  const s=await(await fetch('/monitor',{cache:'no-store'})).json();
  $('mon').textContent=JSON.stringify(s.out);
}

let scanTimer=null;
async function wifiScan(){
  if(scanTimer){clearInterval(scanTimer);scanTimer=null;}
  $('scanst').innerHTML='<small>Scanning...</small>';
  $('nets').innerHTML='';
  await fetch('/wifi/scan');
  scanTimer=setInterval(async()=>{
    const r=await(await fetch('/wifi/scan',{cache:'no-store'})).json();
    if(!r.scanning){clearInterval(scanTimer);scanTimer=null;$('scanst').innerHTML='';showNets(r.networks||[]);}
  },1500);
}
function showNets(nets){
  if(!nets.length){$('nets').innerHTML='<small>No networks found.</small>';return;}
  $('nets').innerHTML=nets.map(n=>`<div class="net-item" onclick="selNet('${n.ssid.replace(/\\/g,'\\\\').replace(/'/g,"\\'")}')">
    ${n.ssid}${n.secure?' [lock]':''} <small>${n.rssi}dBm</small></div>`).join('');
}
function selNet(s){$('wssid').value=s;}
function wifiSet(){
  const s=$('wssid').value.trim();
  if(!s){alert('Enter SSID');return;}
  fetch(`/wifi/set?ssid=${encodeURIComponent(s)}&pass=${encodeURIComponent($('wpass').value)}`);
  $('scanst').innerHTML='<small>Connecting... STA IP will appear in status when ready.</small>';
}
function wifiForget(){
  fetch('/wifi/forget');
  $('wssid').value='';$('wpass').value='';$('wssid').placeholder='Network SSID';
  $('scanst').innerHTML='<small>STA credentials cleared.</small>';
}

mkPages();status();loadPage();refreshMon();
setInterval(status,700);setInterval(loadPage,900);
</script>
</body>
</html>
)HTML";

// ======================= Web JSON helpers =======================
static void sendJSON(AsyncWebServerRequest* req, const String& body) {
  AsyncWebServerResponse* res = req->beginResponse(200, "application/json", body);
  res->addHeader("Access-Control-Allow-Origin", "*");
  res->addHeader("Cache-Control", "no-store");
  req->send(res);
}

static String statusJSON() {
  bool staCon = (WiFi.status() == WL_CONNECTED);
  String s = "{";
  s += "\"ip\":\""          + ipToStr(WiFi.softAPIP()) + "\",";
  s += "\"sta_ip\":\""      + (staCon ? ipToStr(WiFi.localIP()) : "") + "\",";
  s += "\"sta_connected\":" + String(staCon ? "true" : "false") + ",";
  s += "\"sta_ssid\":\""    + staSSID + "\",";
  s += "\"hostname\":\""    + mdnsHostname() + "\",";
  s += "\"ssid\":\""        + apSsid + "\",";
  s += "\"name\":\""        + nodeName + "\",";
  s += "\"net\":"            + String(artNet) + ",";
  s += "\"subnet\":"         + String(artSubnet) + ",";
  s += "\"uni\":"            + String(artUni) + ",";
  s += "\"uni15\":"          + String(combinedUniverse()) + ",";
  s += "\"mode\":"           + String((uint8_t)mode) + ",";
  s += "\"mode_name\":\""   + String(modeName(mode)) + "\",";
  s += "\"artnet_active\":"  + String(artnetActive() ? "true" : "false");
  s += "}";
  return s;
}

static String pageJSON(uint16_t page) {
  if (page >= PAGE_COUNT) page = PAGE_COUNT - 1;
  uint16_t base = page * PAGE_SIZE;
  String s = "{\"page\":" + String(page) + ",\"base_ch\":" + String(base + 1) + ",\"vals\":[";
  for (int i = 0; i < PAGE_SIZE; i++) {
    s += String(outVals[base + i]);
    if (i < PAGE_SIZE - 1) s += ",";
  }
  return s + "]}";
}

static String monitorJSON() {
  String s = "{\"out\":[";
  for (int i = 0; i < 64; i++) { s += String(outVals[i]); if (i < 63) s += ","; }
  return s + "]}";
}

static String wifiScanJSON() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return "{\"scanning\":true}";
  if (n < 0) { WiFi.scanNetworks(true); return "{\"scanning\":true}"; }
  String s = "{\"scanning\":false,\"networks\":[";
  for (int i = 0; i < n; i++) {
    if (i > 0) s += ",";
    String ssid = WiFi.SSID(i);
    ssid.replace("\\", "\\\\"); ssid.replace("\"", "\\\"");
    s += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
         ",\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  WiFi.scanDelete();
  return s + "]}";
}

// ======================= Web routes =======================
static void setupWeb() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200, "text/html", INDEX_HTML);
  });
  server.on("/status",  HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, statusJSON()); });
  server.on("/monitor", HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, monitorJSON()); });

  server.on("/page", HTTP_GET, [](AsyncWebServerRequest* r){
    uint16_t p = r->hasArg("i") ? (uint16_t)constrain(r->arg("i").toInt(), 0, PAGE_COUNT-1) : 0;
    sendJSON(r, pageJSON(p));
  });
  server.on("/set", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!r->hasArg("ch") || !r->hasArg("v")) { r->send(400, "text/plain", "missing"); return; }
    webVals[constrain(r->arg("ch").toInt(), 1, 512) - 1] = (uint8_t)constrain(r->arg("v").toInt(), 0, 255);
    r->send(204);
  });
  server.on("/blackout", HTTP_GET, [](AsyncWebServerRequest* r){ memset(webVals,   0, MAX_CH); fadeActive=false; r->send(204); });
  server.on("/full",     HTTP_GET, [](AsyncWebServerRequest* r){ memset(webVals, 255, MAX_CH); fadeActive=false; r->send(204); });

  server.on("/artnet/set", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasArg("net"))    artNet    = (uint8_t)constrain(r->arg("net").toInt(),    0, 127);
    if (r->hasArg("subnet")) artSubnet = (uint8_t)constrain(r->arg("subnet").toInt(), 0, 15);
    if (r->hasArg("uni"))    artUni    = (uint8_t)constrain(r->arg("uni").toInt(),    0, 15);
    saveConfig(); r->send(204);
  });
  server.on("/mode/set", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!r->hasArg("m")) { r->send(400, "text/plain", "missing"); return; }
    mode = (Mode)constrain(r->arg("m").toInt(), 0, 2);
    saveConfig(); r->send(204);
  });
  server.on("/scene/save", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!r->hasArg("n")) { r->send(400, "text/plain", "missing"); return; }
    saveScene((uint8_t)constrain(r->arg("n").toInt(), 0, SCENE_COUNT-1), webVals);
    r->send(204);
  });
  server.on("/scene/recall", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!r->hasArg("n")) { r->send(400, "text/plain", "missing"); return; }
    uint32_t ft = r->hasArg("fade") ? (uint32_t)max(0L, r->arg("fade").toInt()) : 0;
    loadScene((uint8_t)constrain(r->arg("n").toInt(), 0, SCENE_COUNT-1), sceneBuf);
    startFadeTo(sceneBuf, ft);
    r->send(204);
  });

  server.on("/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, wifiScanJSON()); });
  server.on("/wifi/set",  HTTP_GET, [](AsyncWebServerRequest* r){
    if (!r->hasArg("ssid")) { r->send(400, "text/plain", "missing ssid"); return; }
    staSSID = r->arg("ssid");
    staPass = r->hasArg("pass") ? r->arg("pass") : "";
    saveConfig();
    WiFi.disconnect(false);
    WiFi.begin(staSSID.c_str(), staPass.c_str());
    r->send(204);
  });
  server.on("/wifi/forget", HTTP_GET, [](AsyncWebServerRequest* r){
    staSSID = ""; staPass = ""; saveConfig();
    WiFi.disconnect(true); r->send(204);
  });

  server.begin();
}

// ======================= Setup / Loop =======================
void setup() {
  Serial.begin(115200);
  delay(200);

  loadConfig();
  startWiFi();
  setupMDNS();

  memset(webVals,  0, MAX_CH);
  memset(artVals,  0, MAX_CH);
  memset(outVals,  0, MAX_CH);
  memset(holdVals, 0, MAX_CH);

  setupDMX();
  setupWeb();

  artnet.begin(nodeName.c_str());
  artnet.setArtDmxCallback(onDmxFrame);
  lastArtnetMs = 0;

  Serial.println("\n=== vi_di_li - ESP32 DevKit ===");
  Serial.printf("AP SSID  : %s\n", apSsid.c_str());
  Serial.printf("AP IP    : %s\n", ipToStr(WiFi.softAPIP()).c_str());
  if (staSSID.length() > 0) {
    Serial.printf("STA SSID : %s\n", staSSID.c_str());
    Serial.printf("STA IP   : %s\n",
      WiFi.status() == WL_CONNECTED ? ipToStr(WiFi.localIP()).c_str() : "connecting...");
  }
  Serial.printf("Hostname : %s.local\n", mdnsHostname().c_str());
  Serial.printf("Mode     : %s\n", modeName(mode));
  Serial.printf("Universe : %d/%d/%d\n", artNet, artSubnet, artUni);
}

void loop() {
  artnet.read();
  updateFade();

  static uint32_t lastDmx = 0;
  uint32_t now = millis();
  if (now - lastDmx >= DMX_PERIOD_MS) {
    lastDmx = now;
    computeOutput();
    sendDMX();
  }
}
