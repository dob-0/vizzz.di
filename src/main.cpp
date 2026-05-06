#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <freertos/semphr.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

#include "esp_dmx.h"
#include "vizzz_core.h"

// ── Hardware ──────────────────────────────────────────────────────────────────
static constexpr int DMX_TX  = 25;
static constexpr int DMX_DIR = 21;
static constexpr int MAX_CH  = 512;

// ── Timing ────────────────────────────────────────────────────────────────────
static constexpr uint32_t   DMX_PERIOD_MS     = 23;
static constexpr TickType_t DMX_SEND_WAIT     = pdMS_TO_TICKS(30);
static constexpr uint32_t   ARTNET_TIMEOUT_MS = 3000;
static constexpr uint16_t   ARTNET_PORT       = 6454;
static constexpr uint32_t   SACN_TIMEOUT_MS   = 3000;
static constexpr uint16_t   SACN_PORT         = 5568;

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

enum NetMode : uint8_t { NET_AP_STA = 0, NET_STA_ONLY = 1, NET_AP_ONLY = 2 };
static const char* netModeName(NetMode m) {
  switch (m) {
    case NET_STA_ONLY: return "STA_ONLY";
    case NET_AP_ONLY:  return "AP_ONLY";
    default:           return "AP_STA";
  }
}

// ── Config ────────────────────────────────────────────────────────────────────
static Preferences prefs;
static constexpr const char* PRODUCT_NAME = "vizzz.di";
static constexpr const char* AP_SSID_PREFIX = "vizzz.di";
static constexpr const char* LEGACY_AP_SSID_PREFIX = "vi_di_li";
static String  nodeName = PRODUCT_NAME;
static String  apSsid   = AP_SSID_PREFIX;
static String  apPass   = "Poghka888$";
static String  staSSID, staPass;
static uint8_t artNet = 0, artSubnet = 0, artUni = 0;
static Mode    mode         = MODE_HTP;
static NetMode netMode      = NET_AP_STA;
static bool    artOutEnabled = false;   // broadcast webVals as Art-Net to slaves
static bool    webEnabled    = true;    // when false, web server and websocket stay disabled
static bool    needSaveConfig = false;  // set when auto-generated values must be persisted
static constexpr const char* FW_TAG = __DATE__ " " __TIME__;
static constexpr uint32_t WIFI_SCAN_TIMEOUT_MS = 15000;
static constexpr uint16_t DISCOVERY_PORT       = 47777;
static bool wifiScanActive = false;
static uint32_t wifiScanStartMs = 0;

static uint16_t universe() {
  return vizzz::packUniverse(artNet, artSubnet, artUni);
}

static bool isGeneratedApSsid(const String& ssid) {
  return ssid.startsWith(AP_SSID_PREFIX) || ssid.startsWith(LEGACY_AP_SSID_PREFIX);
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
  prefs.putUChar("net_mode",  uint8_t(netMode));
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
  netMode      = NetMode(prefs.getUChar("net_mode", uint8_t(netMode)));
  artOutEnabled = prefs.getBool("ao_en",     artOutEnabled);
  webEnabled   = prefs.getBool("web_en",     webEnabled);
  String savedFwTag = prefs.getString("fw_tag", "");
  prefs.end();
  if (mode > MODE_HTP) mode = MODE_HTP;
  if (netMode > NET_AP_ONLY) netMode = NET_AP_STA;

  // New firmware image flashed: rotate default SSID once, then persist.
  if (savedFwTag != FW_TAG) {
    if (apSsid.isEmpty() || isGeneratedApSsid(apSsid)) apSsid = "";
    needSaveConfig = true;
  }
}

// ── Runtime state ─────────────────────────────────────────────────────────────
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static WiFiUDP        artInUdp;
static WiFiUDP        artOutUdp;
static WiFiUDP        sacnUdp;
static WiFiUDP        discoverUdp;
static dmx_port_t     dmxPort = DMX_NUM_1;

static uint8_t dmxFrame [MAX_CH + 1];
static uint8_t webVals  [MAX_CH];   // web layer — protected by gLock
static uint8_t artVals  [MAX_CH];   // Art-Net IN — written from loop UDP poll
static uint8_t sacnVals [MAX_CH];   // sACN IN
static uint8_t outVals  [MAX_CH];   // final computed output

static volatile uint32_t lastArtnetMs = 0;
static volatile uint32_t lastSacnMs   = 0;
static uint8_t masterDimmer = 255;  // 0=off, 255=full — resets to full on boot

// Fade — access only while holding gLock
static bool     fadeActive  = false;
static uint32_t fadeStartMs = 0;
static uint32_t fadeTimeMs  = 1000;
static uint8_t  fadeFrom[MAX_CH];
static uint8_t  fadeTo  [MAX_CH];

// Mutex guards webVals and all fade state across cores
static SemaphoreHandle_t gLock;
static volatile bool     pendingReboot        = false;
static volatile bool     pendingWifiReconnect = false;
static volatile bool     pendingWifiForget    = false;

// ── Helpers ───────────────────────────────────────────────────────────────────
static void ipToCStr(char* dst, size_t n, IPAddress ip) {
  snprintf(dst, n, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static String ipStr(IPAddress ip) {
  char buf[16];
  ipToCStr(buf, sizeof(buf), ip);
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

static void startSoftAP() {
  WiFi.softAPConfig(IPAddress(10,0,0,1), IPAddress(10,0,0,1), IPAddress(255,255,255,0));
  WiFi.softAP(apSsid.c_str(), apPass.c_str());
}

static void ensureStaInterface() {
  wifi_mode_t wm = WiFi.getMode();
  if (wm == WIFI_OFF || wm == WIFI_AP) {
    wifi_mode_t target = (netMode == NET_STA_ONLY) ? WIFI_STA : WIFI_AP_STA;
    WiFi.mode(target);
    if (target == WIFI_AP_STA) startSoftAP();
    delay(200);
  }
  WiFi.setSleep(false);
}

static void startWiFi() {
  wifi_mode_t wm = WIFI_AP_STA;
  if (netMode == NET_AP_ONLY) wm = WIFI_AP;
  if (netMode == NET_STA_ONLY) wm = WIFI_STA;
  WiFi.mode(wm);
  WiFi.setSleep(false);

  // Empty AP SSID means first boot or new firmware tag: generate one random name.
  if (apSsid.isEmpty()) {
    char suffix[7];
    snprintf(suffix, sizeof(suffix), "%06lX", (unsigned long)(esp_random() & 0xFFFFFF));
    apSsid = String(AP_SSID_PREFIX) + "_" + suffix;
    needSaveConfig = true;
  }

  if (netMode != NET_STA_ONLY) {
    startSoftAP();
  }

  if (netMode != NET_AP_ONLY && staSSID.length()) {
    WiFi.begin(staSSID.c_str(), staPass.c_str());
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) delay(100);

    // Safety net: in STA_ONLY without a successful join, enable temporary AP.
    if (netMode == NET_STA_ONLY && WiFi.status() != WL_CONNECTED) {
      WiFi.mode(WIFI_AP_STA);
      startSoftAP();
    }
  } else if (netMode == NET_STA_ONLY) {
    // No STA credentials in STA_ONLY: expose AP so the node is still recoverable.
    WiFi.mode(WIFI_AP_STA);
    startSoftAP();
  }
}

// ── mDNS ──────────────────────────────────────────────────────────────────────
static String mdnsName;

static void buildMdnsName() {
  mdnsName = nodeName;
  for (size_t i = 0; i < mdnsName.length(); i++) {
    char c = mdnsName[i];
    if (!isalnum((unsigned char)c) && c != '-') mdnsName[i] = '-';
  }
  while (mdnsName.startsWith("-")) mdnsName = mdnsName.substring(1);
  while (mdnsName.endsWith("-"))   mdnsName = mdnsName.substring(0, mdnsName.length() - 1);
  if (mdnsName.isEmpty()) mdnsName = "vizzz-di";
}

static void startMdns() {
  buildMdnsName();
  MDNS.end();
  if (MDNS.begin(mdnsName.c_str())) {
    MDNS.addService("http", "tcp", 80);
  }
}

static void sendBeacon() {
  if (WiFi.status() != WL_CONNECTED) return;
  uint32_t ip   = (uint32_t)WiFi.localIP();
  uint32_t mask = (uint32_t)WiFi.subnetMask();
  IPAddress bcast((ip & mask) | ~mask);
  char buf[160];
  char eName[64]; jsonEsc(eName, sizeof(eName), nodeName);
  snprintf(buf, sizeof(buf),
    "{\"name\":\"%s\",\"ip\":\"%s\",\"ap_ip\":\"10.0.0.1\",\"mdns\":\"%s.local\",\"product\":\"vizzz.di\"}",
    eName, ipStr(WiFi.localIP()).c_str(), mdnsName.c_str());
  discoverUdp.beginPacket(bcast, DISCOVERY_PORT);
  discoverUdp.write((const uint8_t*)buf, strlen(buf));
  discoverUdp.endPacket();
  // Also respond to any incoming discovery requests
  discoverUdp.begin(DISCOVERY_PORT);
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

static bool sacnActive() {
  return lastSacnMs && (millis() - lastSacnMs < SACN_TIMEOUT_MS);
}

static void refreshArtNetSocket() {
  artInUdp.stop();
  artInUdp.begin(ARTNET_PORT);
}

static void refreshSacnSocket() {
  sacnUdp.stop();
  // Unicast/broadcast fallback
  sacnUdp.begin(SACN_PORT);

  // Join universe multicast group when STA is connected.
  if (WiFi.status() == WL_CONNECTED) {
    uint16_t u = universe();
    IPAddress group(239, 255, uint8_t(u >> 8), uint8_t(u & 0xFF));
    (void)sacnUdp.beginMulticast(group, SACN_PORT);
  }
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
static void computeOutput_locked(bool aActive, bool sActive) {
  bool netActive = aActive || sActive;

  for (int i = 0; i < MAX_CH; i++) {
    uint8_t netIn = 0;
    if (aActive && sActive) netIn = max(artVals[i], sacnVals[i]);
    else if (aActive)       netIn = artVals[i];
    else if (sActive)       netIn = sacnVals[i];

    uint8_t v;
    switch (mode) {
      case MODE_WEB:
        v = webVals[i];
        break;
      case MODE_ARTNET:
        v = netActive ? netIn : outVals[i];
        break;
      default: // MODE_HTP
        v = netActive ? max(webVals[i], netIn) : webVals[i];
        break;
    }

    outVals[i] = masterDimmer < 255 ? vizzz::applyMaster(v, masterDimmer) : v;
  }
  memcpy(dmxFrame + 1, outVals, MAX_CH);
}

static void sendDMX() {
  dmx_wait_sent(dmxPort, DMX_SEND_WAIT);
  dmx_write(dmxPort, dmxFrame, MAX_CH + 1);
  dmx_send(dmxPort);
}

// ── Art-Net output (master → slaves broadcast) ────────────────────────────────
static void sendArtNetOut() {
  if (!artOutEnabled) return;
  static uint8_t  seq = 1;
  static const uint8_t hdr[12] = {
    'A','r','t','-','N','e','t',0,   // ID
    0x00, 0x50,                       // OpDmx (little-endian)
    0x00, 0x0e                        // Protocol v14
  };
  uint8_t pkt[18 + MAX_CH];
  memcpy(pkt, hdr, 12);
  pkt[12] = seq++;
  pkt[13] = 0;
  pkt[14] = uint8_t((artSubnet << 4) | artUni);
  pkt[15] = artNet;
  pkt[16] = 0x02;   // length hi (512)
  pkt[17] = 0x00;   // length lo
  memcpy(pkt + 18, dmxFrame + 1, MAX_CH);
  artOutUdp.beginPacket(artOutTarget(), ARTNET_PORT);
  artOutUdp.write(pkt, sizeof(pkt));
  artOutUdp.endPacket();
}

// ── Art-Net IN ────────────────────────────────────────────────────────────────
static void pollArtNet() {
  uint8_t pkt[18 + MAX_CH];
  static const uint8_t artNetId[8] = {'A','r','t','-','N','e','t',0};

  for (int size = artInUdp.parsePacket(); size > 0; size = artInUdp.parsePacket()) {
    int n = artInUdp.read(pkt, min(int(sizeof(pkt)), size));
    if (n < 18) continue;
    if (memcmp(pkt, artNetId, sizeof(artNetId)) != 0) continue;
    if (!(pkt[8] == 0x00 && pkt[9] == 0x50)) continue; // OpDmx, little-endian

    uint16_t uni = uint16_t(pkt[14]) | (uint16_t(pkt[15] & 0x7F) << 8);
    if (uni != universe()) continue;

    uint16_t len = (uint16_t(pkt[16]) << 8) | pkt[17];
    if (len == 0 || 18 + len > uint16_t(n)) continue;

    uint16_t dmxLen = min<uint16_t>(MAX_CH, len);
    memcpy(artVals, pkt + 18, dmxLen);
    if (dmxLen < MAX_CH) memset(artVals + dmxLen, 0, MAX_CH - dmxLen);
    lastArtnetMs = millis();
  }
}

// ── sACN (E1.31) IN ─────────────────────────────────────────────────────────
static void pollSacn() {
  uint8_t pkt[638]; // enough for E1.31 packet with full universe

  for (int size = sacnUdp.parsePacket(); size > 0; size = sacnUdp.parsePacket()) {
    int n = sacnUdp.read(pkt, min(int(sizeof(pkt)), size));
    if (n < 126) continue;

    // ACN Packet Identifier
    static const uint8_t acnPid[12] = {
      0x41, 0x53, 0x43, 0x2D, 0x45, 0x31, 0x2E, 0x31, 0x37, 0x00, 0x00, 0x00
    };
    if (memcmp(pkt + 4, acnPid, sizeof(acnPid)) != 0) continue;

    // Root Vector (Data Packet = 0x00000004)
    if (!(pkt[18] == 0x00 && pkt[19] == 0x00 && pkt[20] == 0x00 && pkt[21] == 0x04)) continue;
    // Framing Vector (Data Packet = 0x00000002)
    if (!(pkt[40] == 0x00 && pkt[41] == 0x00 && pkt[42] == 0x00 && pkt[43] == 0x02)) continue;
    // DMP Vector (set property)
    if (pkt[117] != 0x02) continue;

    uint16_t uni = (uint16_t(pkt[113]) << 8) | pkt[114];
    if (uni != universe()) continue;

    uint16_t propCount = (uint16_t(pkt[123]) << 8) | pkt[124];
    if (propCount < 2) continue; // includes start code
    if (125 + propCount > uint16_t(n)) continue;

    const uint8_t* props = pkt + 125;
    if (props[0] != 0x00) continue; // only DMX data start code

    uint16_t dmxLen = min<uint16_t>(MAX_CH, propCount - 1);
    memcpy(sacnVals, props + 1, dmxLen);
    if (dmxLen < MAX_CH) memset(sacnVals + dmxLen, 0, MAX_CH - dmxLen);
    lastSacnMs = millis();
  }
}

// ── Unified App UI ───────────────────────────────────────────────────────────
static const char APP_HTML[] PROGMEM = R"HTML(<!doctype html><html lang="en">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>vizzz.di console</title>
<style>
:root{--di-cyan:#4df9ff;--di-cyan-dim:rgba(77,249,255,.1);--di-cyan-border:rgba(77,249,255,.3);--di-black:#000;--di-surface:#0a0a0a;--di-panel:#050505;--di-text:#fff;--di-muted:rgba(255,255,255,.45);--di-danger:#f25f5c;--di-mono:"JetBrains Mono","Fira Code","Consolas",monospace}
*{box-sizing:border-box;margin:0;padding:0}
body{font:14px/1.45 "Inter","Segoe UI",sans-serif;background:var(--di-black);color:var(--di-text);min-height:100vh}
.shell{max-width:1080px;margin:0 auto;padding:10px}
.hero{background:var(--di-surface);border:1px solid var(--di-cyan-border);border-radius:0;padding:10px;margin-bottom:10px}
.brand{display:flex;gap:10px;align-items:flex-start;justify-content:space-between;flex-wrap:wrap}
.title{font:700 1.45rem/1 var(--di-mono);letter-spacing:0;text-transform:lowercase}.title:before{content:"□ ";color:var(--di-cyan)}
.sub{display:none}
.pillbar{display:flex;gap:6px;flex-wrap:wrap;margin-top:10px}
.pill{padding:5px 7px;border-radius:0;border:1px solid var(--di-cyan-border);background:var(--di-panel);font:600 .66rem/1 var(--di-mono);letter-spacing:.04em;color:var(--di-muted);text-transform:uppercase}
.pill.on,.pill.hot{background:var(--di-cyan-dim);border-color:var(--di-cyan);color:var(--di-cyan)}.pill.warn{background:var(--di-panel);border-color:var(--di-cyan);color:var(--di-text)}
.tabs{display:flex;gap:6px;overflow-x:auto;margin-top:10px;padding-bottom:2px;-webkit-overflow-scrolling:touch}
.tab{display:inline-flex;align-items:center;justify-content:center;flex:0 0 auto;min-height:42px;padding:10px 12px;border-radius:0;border:1px solid var(--di-cyan-border);background:var(--di-black);color:var(--di-muted);text-decoration:none;font:700 .74rem/1 var(--di-mono);text-transform:uppercase;letter-spacing:.04em}
.tab:hover,.tab.active{background:var(--di-cyan-dim);border-color:var(--di-cyan);color:var(--di-cyan)}
.grid{display:grid;grid-template-columns:1.1fr .9fr;gap:10px}
.stack{display:grid;gap:10px}.section{display:none}.section.active{display:grid;gap:10px}
.card{background:var(--di-surface);border:1px solid var(--di-cyan-border);border-radius:0;padding:10px}
.card h2{font:700 .82rem/1.1 var(--di-mono);letter-spacing:.05em;text-transform:uppercase;margin-bottom:10px;color:var(--di-text)}.card h2:before{content:"□ ";color:var(--di-cyan)}.meta{display:none}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.row+.row{margin-top:8px}
.grow{flex:1 1 150px}.split{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}.triple{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px}
label{display:grid;gap:5px;font:700 .66rem/1 var(--di-mono);text-transform:uppercase;letter-spacing:.06em;color:var(--di-muted)}
input,select,button,textarea{border-radius:0;border:1px solid var(--di-cyan-border);min-height:44px;padding:10px;background:var(--di-black);color:var(--di-text);font:600 .9rem/1.2 "Inter","Segoe UI",sans-serif;outline:none}
input:focus,select:focus,textarea:focus{border-color:var(--di-cyan);box-shadow:0 0 0 1px var(--di-cyan)}
button{cursor:pointer;background:var(--di-cyan-dim);border-color:var(--di-cyan);color:var(--di-cyan);font-weight:800;text-transform:uppercase;letter-spacing:.04em}
button:hover{background:rgba(77,249,255,.16)}button.alt{background:var(--di-black);border-color:var(--di-cyan-border);color:var(--di-text)}button.good,button.bad{background:var(--di-cyan-dim);border-color:var(--di-cyan);color:var(--di-cyan)}
button.wide{width:100%}.actions{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}
.heroMeter{padding:12px;border-radius:0;background:var(--di-black);border:1px solid var(--di-cyan-border);color:var(--di-text);text-align:center}.heroMeter .big{font:700 2rem/1 var(--di-mono);color:var(--di-cyan)}
.sceneGrid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px}.sceneBtn{height:68px;font-size:.9rem}.miniScene{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px}
.patchHead{display:flex;justify-content:space-between;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:10px}.channels{display:grid;grid-template-columns:52px 1fr 38px 38px;gap:7px;align-items:center}
.chLabel{font:700 .68rem/1 var(--di-mono);color:var(--di-muted);letter-spacing:.02em}.outVal{font:700 .72rem/1 var(--di-mono);text-align:right;color:var(--di-muted)}.outVal.hot{color:var(--di-cyan)}.webVal{font:700 .72rem/1 var(--di-mono);text-align:right}
input[type=range]{width:100%;accent-color:var(--di-cyan)}
.nets{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:8px}.net{padding:9px;border-radius:0;border:1px solid var(--di-cyan-border);background:var(--di-black);cursor:pointer;font-size:.8rem;text-align:left;overflow:hidden;text-overflow:ellipsis}
pre{white-space:pre-wrap;word-break:break-word;background:var(--di-black);border:1px solid var(--di-cyan-border);border-radius:0;padding:10px;font:11px/1.4 var(--di-mono);max-height:260px;overflow:auto;color:var(--di-text)}
.footerNote{font:600 .7rem/1.35 var(--di-mono);color:var(--di-muted);letter-spacing:.02em}
@media (max-width:760px){body{font-size:13px}.shell{padding:6px}.hero{padding:8px;margin-bottom:8px}.brand{display:block}.pillbar{overflow-x:auto;flex-wrap:nowrap;padding-bottom:2px}.pill{flex:0 0 auto}.grid,.split,.triple{grid-template-columns:1fr}.section.active,.stack{gap:8px}.card{padding:9px}.actions{grid-template-columns:1fr}.sceneGrid,.miniScene{grid-template-columns:repeat(2,minmax(0,1fr))}.channels{grid-template-columns:42px 1fr 32px 32px;gap:6px}.chLabel{font-size:.62rem}.footerNote{margin-top:6px}.heroMeter .big{font-size:1.7rem}}
</style>
</head>
<body>
<div class="shell">
  <div class="hero">
    <div class="brand">
      <div>
        <div class="title">vizzz.di</div>
        <div class="sub">physical dmx node / art-net + sacn bridge / live browser surface</div>
        <div class="pillbar">
          <span class="pill" id="modePill">mode</span>
          <span class="pill" id="netModePill">network</span>
          <span class="pill" id="artPill">artnet</span>
          <span class="pill" id="sacnPill">sacn</span>
          <span class="pill" id="aoPill">out</span>
          <span class="pill" id="webPill">web</span>
          <span class="pill" id="staPill">sta</span>
        </div>
      </div>
      <div class="footerNote">AP <b id="apIp">10.0.0.1</b><br>Target <b id="aoTarget">-</b><br>mDNS <b id="mdnsLabel">-</b></div>
    </div>
    <nav class="tabs" id="tabs">
      <a class="tab" data-route="/control" href="/control">Control</a>
      <a class="tab" data-route="/patch" href="/patch">Patch</a>
      <a class="tab" data-route="/scenes" href="/scenes">Scenes</a>
      <a class="tab" data-route="/network" href="/network">WiFi</a>
      <a class="tab" data-route="/system" href="/system">System</a>
      <a class="tab" data-route="/performance" href="/performance">Show</a>
    </nav>
  </div>

  <section class="section" data-route="/control">
    <div class="grid">
      <div class="stack">
        <div class="card">
          <h2>Core Control</h2>
          <div class="meta">Direct mode, dimmer, outputs, and network behavior.</div>
          <div class="split">
            <label>Mode<select id="modeSel"><option value="0">WEB_ONLY</option><option value="1">ARTNET_ONLY</option><option value="2">MERGE_HTP</option></select></label>
            <label>WiFi Mode<select id="netModeSel"><option value="0">AP_STA</option><option value="1">STA_ONLY</option><option value="2">AP_ONLY</option></select></label>
          </div>
          <div class="row" style="margin-top:12px">
            <label class="grow">Master Dimmer<input id="master" type="range" min="0" max="255" value="255"></label>
            <div class="heroMeter"><div class="big" id="masterPct">100%</div><div>master</div></div>
          </div>
          <div class="actions" style="margin-top:12px">
            <button class="bad" onclick="blackout()">Blackout</button>
            <button class="good" onclick="fullOn()">Full</button>
            <button class="alt" id="aoBtn" onclick="toggleArtOut()">Art-Net OUT</button>
            <button class="alt" id="webBtn" onclick="toggleWeb()">Web</button>
          </div>
        </div>
      </div>
      <div class="stack">
        <div class="card">
          <h2>Live Snapshot</h2>
          <div class="meta">Route-aware UI keeps the same live state everywhere.</div>
          <pre id="statusDump">Loading…</pre>
        </div>
      </div>
    </div>
  </section>

  <section class="section" data-route="/patch">
    <div class="card">
      <div class="patchHead">
        <div>
          <h2>Channel Patch</h2>
          <div class="meta">Paged editor for the web layer with live output compare.</div>
        </div>
        <div class="row">
          <label>Page<select id="pageSel"></select></label>
          <div class="footerNote" id="pageInfo">ch 1-32</div>
        </div>
      </div>
      <div class="channels" id="channels"></div>
    </div>
  </section>

  <section class="section" data-route="/scenes">
    <div class="grid">
      <div class="card">
        <h2>Recall</h2>
        <div class="meta">Performance-safe scene recalls with fade time.</div>
        <div class="row"><label class="grow">Fade ms<input id="fadeInput" type="number" value="1000"></label></div>
        <div class="sceneGrid" id="sceneRecall"></div>
      </div>
      <div class="card">
        <h2>Store</h2>
        <div class="meta">Write current web layer into scene memory.</div>
        <div class="miniScene" id="sceneSave"></div>
      </div>
    </div>
  </section>

  <section class="section" data-route="/network">
    <div class="grid">
      <div class="stack">
        <div class="card">
          <h2>WiFi</h2>
          <div class="meta">Join a local network and scan available SSIDs.</div>
          <div class="split">
            <label>SSID<input id="staSsid" placeholder="Studio WiFi"></label>
            <label>Password<input id="staPass" type="password" placeholder="Password"></label>
          </div>
          <div class="row" style="margin-top:8px"><button onclick="wifiSet()">Join</button><button class="alt" onclick="wifiScan()">Scan</button><button class="alt" onclick="wifiForget()">Forget</button><div class="footerNote" id="scanState"></div></div>
          <div class="nets" id="networks" style="margin-top:8px"></div>
        </div>
        <div class="card">
          <h2>Universe</h2>
          <div class="meta">Universe, routing, and protocol-facing configuration.</div>
          <div class="triple">
            <label>Net<input id="netInput" type="number" min="0" max="127"></label>
            <label>Subnet<input id="subInput" type="number" min="0" max="15"></label>
            <label>Universe<input id="uniInput" type="number" min="0" max="15"></label>
          </div>
          <div class="row" style="margin-top:8px"><button onclick="saveUniverse()">Save</button><div class="footerNote" id="uni15">15-bit: 0</div></div>
        </div>
      </div>
      <div class="stack">
        <div class="card">
          <h2>Node Identity</h2>
          <div class="meta">AP and node naming live here now.</div>
          <div class="split">
            <label>Node Name<input id="nodeName" placeholder="vizzz.di"></label>
            <label>AP SSID<input id="apSsid" placeholder="vizzz.di"></label>
          </div>
          <div class="row" style="margin-top:8px"><label class="grow">AP Password<input id="apPass" type="password" placeholder="Password"></label></div>
          <div class="row" style="margin-top:8px"><button onclick="saveNode()">Save</button></div>
        </div>
      </div>
    </div>
  </section>

  <section class="section" data-route="/system">
    <div class="grid">
      <div class="card">
        <h2>Output Monitor</h2>
        <div class="meta">First 64 channels from the final DMX output.</div>
        <div class="row" style="margin-bottom:12px"><button onclick="refreshMonitor()">Refresh</button></div>
        <pre id="monitorDump">[]</pre>
      </div>
      <div class="card">
        <h2>System Actions</h2>
        <div class="meta">Fast safe operations.</div>
        <div class="actions">
          <button class="bad" onclick="rebootNow()">Reboot Device</button>
          <button class="alt" onclick="refreshStatusDump()">Refresh Status</button>
        </div>
        <pre id="diagDump" style="margin-top:12px">No diagnostics yet.</pre>
      </div>
      <div class="card">
        <h2>Node Manifest</h2>
        <div class="meta">Machine-readable firmware node contract.</div>
        <div class="actions">
          <button onclick="loadManifest()">Load Manifest</button>
          <button class="alt" onclick="openManifest()">Open JSON</button>
        </div>
        <pre id="manifestDump" style="margin-top:12px">manifest pending</pre>
      </div>
    </div>
  </section>

  <section class="section" data-route="/performance">
    <div class="grid">
      <div class="card">
        <h2>Show</h2>
        <div class="meta">Big actions only: scenes, master, blackout, full on.</div>
        <div class="actions"><button class="bad" onclick="blackout()">Blackout</button><button class="good" onclick="fullOn()">Full</button></div>
        <div class="row" style="margin-top:8px"><label class="grow">Master<input id="perfMaster" type="range" min="0" max="255" value="255"></label></div>
        <div class="heroMeter" style="margin-top:8px"><div class="big" id="perfPct">100%</div><div>level</div></div>
      </div>
      <div class="card">
        <h2>Scenes</h2>
        <div class="meta">Single-tap recalls with the shared fade setting.</div>
        <div class="row"><label class="grow">Fade ms<input id="perfFade" type="number" value="1000"></label></div>
        <div class="sceneGrid" id="perfScenes" style="margin-top:12px"></div>
      </div>
    </div>
  </section>
</div>

<script>
const $=id=>document.getElementById(id);
const route=location.pathname==='/vj'?'/performance':(location.pathname==='/'?'/control':location.pathname);
let state=null,pageWeb=[],pageOut=[],curPage=0,masterTimer=null,scanTimer=null,artOutEnabled=false,webEnabled=true;

function escHtml(s){return String(s||'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
function escJsSq(s){return String(s||'').replace(/\\/g,'\\\\').replace(/'/g,"\\'").replace(/\n/g,'\\n').replace(/\r/g,'\\r');}
function qs(url){return fetch(url,{cache:'no-store'}).then(r=>r.json()).catch(()=>null);}
function hit(url){return fetch(url,{cache:'no-store'});}
function setActiveRoute(){document.querySelectorAll('.section').forEach(x=>x.classList.toggle('active',x.dataset.route===route));document.querySelectorAll('.tab').forEach(x=>x.classList.toggle('active',x.dataset.route===route));}
function makeScenes(){
  $('sceneRecall').innerHTML=[...Array(8)].map((_,i)=>`<button class="sceneBtn" onclick="recallScene(${i})">Recall ${i+1}</button>`).join('');
  $('sceneSave').innerHTML=[...Array(8)].map((_,i)=>`<button onclick="saveScene(${i})">Save ${i+1}</button>`).join('');
  $('perfScenes').innerHTML=[...Array(8)].map((_,i)=>`<button class="sceneBtn" onclick="recallPerf(${i})">Scene ${i+1}</button>`).join('');
}
function makePages(){ $('pageSel').innerHTML=[...Array(16)].map((_,i)=>`<option value="${i}">Page ${i+1}</option>`).join(''); }
function updateMasterDisplays(v){const pct=Math.round((v/255)*100);$('masterPct').textContent=pct+'%';$('perfPct').textContent=pct+'%';if(document.activeElement!==$('master'))$('master').value=v;if(document.activeElement!==$('perfMaster'))$('perfMaster').value=v;}
function updatePills(s){
  $('apIp').textContent=s.ip||'-'; $('aoTarget').textContent=s.ao_target||'-'; if(s.mdns) $('mdnsLabel').textContent=s.mdns+'.local';
  const set=(id,text,on,hot)=>{const el=$(id);el.textContent=text;el.className='pill'+(hot?' hot':on?' on':'');};
  set('modePill',s.mode_name,false,true); set('netModePill',s.net_mode_name,false,false); set('artPill','ART '+(s.artnet_active?'active':'idle'),s.artnet_active,false); set('sacnPill','sACN '+(s.sacn_active?'active':'idle'),s.sacn_active,false); set('aoPill','OUT '+(s.ao?'on':'off'),s.ao,s.ao); set('webPill','WEB '+(s.web?'on':'off'),s.web,false);
  const wls=s.wl_status; const staLabel=s.sta_connected?('STA '+s.sta_ip):s.sta_ssid?(wls===4?'STA auth fail':wls===1?'STA no SSID':'STA joining'):'STA idle'; const staErr=s.sta_ssid&&!s.sta_connected&&(wls===4||wls===1); $('staPill').textContent=staLabel; $('staPill').className='pill'+(s.sta_connected?' on':staErr?' warn':s.sta_ssid?' warn':'');
}
function syncFields(s){
  $('modeSel').value=String(s.mode); $('netModeSel').value=String(s.net_mode); $('netInput').value=s.net; $('subInput').value=s.subnet; $('uniInput').value=s.uni; $('uni15').textContent='15-bit: '+s.uni15; $('statusDump').textContent=JSON.stringify(s,null,2); $('diagDump').textContent=`mode=${s.mode_name}\nnetwork=${s.net_mode_name}\nartnet=${s.artnet_active}\nsacn=${s.sacn_active}\nweb=${s.web}\nout_target=${s.ao_target}`;
  artOutEnabled=!!s.ao; webEnabled=!!s.web; $('aoBtn').textContent='Art OUT '+(artOutEnabled?'ON':'OFF'); $('webBtn').textContent='Web '+(webEnabled?'ON':'OFF');
  if(!$('staSsid').value && s.sta_ssid) $('staSsid').placeholder=s.sta_ssid; if(!$('nodeName').value) $('nodeName').placeholder=s.name; if(!$('apSsid').value) $('apSsid').placeholder=s.ssid; updateMasterDisplays(s.dim||255);
}
function renderChannels(base){$('channels').innerHTML=[...Array(32)].map((_,i)=>{const ch=base+i,w=pageWeb[i]??0,o=pageOut[i]??0;return `<div class="chLabel">Ch ${ch}</div><input type="range" min="0" max="255" value="${w}" oninput="setChannel(${ch},+this.value);this.nextElementSibling.textContent=this.value"><div class="webVal">${w}</div><div class="outVal${o>w?' hot':''}">${o}</div>`;}).join('');}
async function loadPage(){const s=await qs('/page?i='+curPage);if(!s)return;pageWeb=s.web||[];pageOut=s.out||[];$('pageInfo').textContent='ch '+s.base_ch+'-'+(s.base_ch+31);renderChannels(s.base_ch);}
function refreshOutGrid(){const rows=$('channels'); if(!rows) return; rows.querySelectorAll('input[type=range]').forEach((sl,i)=>{sl.value=pageWeb[i]??0; sl.nextElementSibling.textContent=pageWeb[i]??0; const out=sl.nextElementSibling.nextElementSibling; const o=pageOut[i]??0; out.textContent=o; out.className='outVal'+(o>(pageWeb[i]??0)?' hot':'');});}
function setChannel(ch,v){hit('/set?ch='+ch+'&v='+v);}
function blackout(){hit('/blackout');}
function fullOn(){hit('/full');}
function queueMaster(v){updateMasterDisplays(v); clearTimeout(masterTimer); masterTimer=setTimeout(()=>hit('/master?v='+v),40);}
function recallScene(i){hit('/scene/recall?n='+i+'&fade='+(+$('fadeInput').value||0));}
function recallPerf(i){hit('/scene/recall?n='+i+'&fade='+(+$('perfFade').value||0));}
function saveScene(i){hit('/scene/save?n='+i);}
function saveUniverse(){hit(`/artnet/set?net=${$('netInput').value}&subnet=${$('subInput').value}&uni=${$('uniInput').value}`);}
function toggleArtOut(){artOutEnabled=!artOutEnabled; hit('/artout/set?en='+(artOutEnabled?1:0)); $('aoBtn').textContent='Art OUT '+(artOutEnabled?'ON':'OFF');}
function toggleWeb(){const next=webEnabled?0:1; if(confirm('This will reboot the device. Continue?')) hit('/web/set?en='+next);}
function rebootNow(){if(confirm('Reboot device now?')) hit('/reboot');}
function refreshStatusDump(){if(state) $('statusDump').textContent=JSON.stringify(state,null,2);}
function saveNode(){const p=new URLSearchParams(); const n=$('nodeName').value.trim(),s=$('apSsid').value.trim(),pw=$('apPass').value; if(n) p.append('name',n); if(s) p.append('ap_ssid',s); if(pw) p.append('ap_pass',pw); if(!p.toString()) return alert('Nothing to save'); hit('/node/set?'+p).then(()=>setTimeout(()=>hit('/reboot'),250));}
function wifiSet(){const s=$('staSsid').value.trim(); if(!s) return alert('Enter SSID'); $('scanState').textContent='Connecting…'; hit('/wifi/set?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent($('staPass').value));}
function wifiForget(){hit('/wifi/forget'); $('staSsid').value=''; $('staPass').value=''; $('scanState').textContent='';}
async function wifiScan(){if(scanTimer){clearInterval(scanTimer); scanTimer=null;} $('scanState').textContent='Scanning…'; $('networks').innerHTML=''; let fails=0; const poll=async()=>{const r=await qs('/wifi/scan'); if(!r){if(++fails<4) return false; $('scanState').textContent='Scan failed'; showNetworks([]); return true;} fails=0; if(r.scanning) return false; $('scanState').textContent=r.error?('Scan '+r.error):''; showNetworks(r.networks||[]); return true;}; if(await poll()) return; scanTimer=setInterval(async()=>{if(await poll()){clearInterval(scanTimer); scanTimer=null;}},1200);}
function showNetworks(nets){$('networks').innerHTML=nets.length?nets.map(n=>`<button class="net" onclick="$('staSsid').value='${escJsSq(n.ssid)}'">${escHtml(n.ssid||'(hidden)')} ${n.secure?'LOCK':''} ${n.rssi}dBm</button>`).join(''):'<span class="footerNote">No 2.4GHz networks found</span>';}
async function refreshMonitor(){const r=await qs('/monitor'); if(r) $('monitorDump').textContent=JSON.stringify(r.out);}
async function loadManifest(){const r=await qs('/node/manifest'); if(r) $('manifestDump').textContent=JSON.stringify(r,null,2);}
function openManifest(){window.open('/node/manifest','_blank');}
function applyMode(){hit('/mode/set?m='+$('modeSel').value);} function applyNetMode(){if(confirm('Switch network profile and reboot?')) hit('/netmode/set?m='+$('netModeSel').value);}

$('modeSel').addEventListener('change',applyMode); $('netModeSel').addEventListener('change',applyNetMode); $('master').addEventListener('input',e=>queueMaster(+e.target.value)); $('perfMaster').addEventListener('input',e=>queueMaster(+e.target.value)); $('pageSel').addEventListener('change',e=>{curPage=+e.target.value;loadPage();});

const ws=new WebSocket('ws://'+location.host+'/ws');
ws.onmessage=e=>{let s; try{s=JSON.parse(e.data);}catch{return;} state=s; updatePills(s); syncFields(s);};
ws.onclose=()=>setTimeout(()=>location.reload(),2000);

setActiveRoute(); makeScenes(); makePages(); loadPage(); refreshMonitor(); loadManifest();
setInterval(async()=>{const s=await qs('/page?i='+curPage); if(!s) return; pageWeb=s.web||[]; pageOut=s.out||[]; refreshOutGrid();},1000);
</script>
</body></html>)HTML";

// ── JSON helpers ──────────────────────────────────────────────────────────────
static void sendJSON(AsyncWebServerRequest* req, const String& body) {
  AsyncWebServerResponse* res = req->beginResponse(200, "application/json", body);
  res->addHeader("Access-Control-Allow-Origin", "*");
  res->addHeader("Cache-Control", "no-store");
  req->send(res);
}

static size_t fillStatusJSON(char* buf, size_t n) {
  if (!n) return 0;

  wl_status_t wlSt = WiFi.status();
  bool staCon = (wlSt == WL_CONNECTED);
  char apIp[16], staIp[16], outTarget[16];
  char eName[80], eSsid[80], eStaSsid[80], eMdns[60];
  ipToCStr(apIp, sizeof(apIp), WiFi.softAPIP());
  staIp[0] = '\0';
  if (staCon) ipToCStr(staIp, sizeof(staIp), WiFi.localIP());
  ipToCStr(outTarget, sizeof(outTarget), artOutTarget());
  jsonEsc(eName,    sizeof(eName),    nodeName);
  jsonEsc(eSsid,    sizeof(eSsid),    apSsid);
  jsonEsc(eStaSsid, sizeof(eStaSsid), staSSID);
  jsonEsc(eMdns,    sizeof(eMdns),    mdnsName);

  int written = snprintf(buf, n,
    "{"
    "\"ip\":\"%s\","
    "\"sta_ip\":\"%s\","
    "\"sta_connected\":%s,"
    "\"sta_ssid\":\"%s\","
    "\"wl_status\":%u,"
    "\"ssid\":\"%s\","
    "\"name\":\"%s\","
    "\"mdns\":\"%s\","
    "\"net_mode\":%u,"
    "\"net_mode_name\":\"%s\","
    "\"net\":%u,\"subnet\":%u,\"uni\":%u,"
    "\"uni15\":%u,"
    "\"mode\":%u,"
    "\"mode_name\":\"%s\","
    "\"artnet_active\":%s,"
    "\"sacn_active\":%s,"
    "\"ao\":%s,"
    "\"web\":%s,"
    "\"dim\":%u,"
    "\"ao_target\":\"%s\""
    "}",
    apIp,
    staIp,
    staCon ? "true" : "false",
    eStaSsid,
    uint8_t(wlSt),
    eSsid, eName, eMdns,
    uint8_t(netMode), netModeName(netMode),
    artNet, artSubnet, artUni,
    universe(),
    uint8_t(mode), modeName(mode),
    artnetActive()  ? "true" : "false",
    sacnActive()    ? "true" : "false",
    artOutEnabled   ? "true" : "false",
    webEnabled      ? "true" : "false",
    masterDimmer,
    outTarget
  );
  if (written < 0) {
    buf[0] = '\0';
    return 0;
  }
  if (size_t(written) >= n) return n - 1;
  return size_t(written);
}

static String statusJSON() {
  char buf[720];
  fillStatusJSON(buf, sizeof(buf));
  return buf;
}

static String pageJSON(uint16_t page) {
  if (page >= PAGE_COUNT) page = PAGE_COUNT - 1;
  uint16_t base = page * PAGE_SIZE;

  uint8_t snapWeb[PAGE_SIZE];
  uint8_t snapOut[PAGE_SIZE];
  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) == pdTRUE) {
    memcpy(snapWeb, webVals + base, PAGE_SIZE);
    memcpy(snapOut, outVals + base, PAGE_SIZE);
    xSemaphoreGive(gLock);
  } else {
    memset(snapWeb, 0, PAGE_SIZE);
    memset(snapOut, 0, PAGE_SIZE);
  }

  char buf[440];
  int pos = snprintf(buf, sizeof(buf),
    "{\"page\":%u,\"base_ch\":%u,\"web\":[", page, base + 1);
  for (int i = 0; i < PAGE_SIZE; i++)
    pos += snprintf(buf + pos, sizeof(buf) - pos, i < PAGE_SIZE-1 ? "%u," : "%u", snapWeb[i]);
  pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"out\":[");
  for (int i = 0; i < PAGE_SIZE; i++)
    pos += snprintf(buf + pos, sizeof(buf) - pos, i < PAGE_SIZE-1 ? "%u," : "%u", snapOut[i]);
  snprintf(buf + pos, sizeof(buf) - pos, "]}");
  return buf;
}

static String monitorJSON() {
  uint8_t snapOut[64];
  bool busy = xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE;
  if (busy) {
    memset(snapOut, 0, sizeof(snapOut));
  } else {
    memcpy(snapOut, outVals, sizeof(snapOut));
    xSemaphoreGive(gLock);
  }

  char buf[360];
  int pos = snprintf(buf, sizeof(buf), "{\"busy\":%s,\"out\":[", busy ? "true" : "false");
  for (int i = 0; i < 64; i++)
    pos += snprintf(buf + pos, sizeof(buf) - pos, i < 63 ? "%u," : "%u", snapOut[i]);
  snprintf(buf + pos, sizeof(buf) - pos, "]}");
  return buf;
}

static String nodeManifestJSON() {
  bool staCon = (WiFi.status() == WL_CONNECTED);
  char eName[80], eSsid[80], eStaSsid[80], eFw[80];
  jsonEsc(eName,    sizeof(eName),    nodeName);
  jsonEsc(eSsid,    sizeof(eSsid),    apSsid);
  jsonEsc(eStaSsid, sizeof(eStaSsid), staSSID);
  jsonEsc(eFw,      sizeof(eFw),      FW_TAG);

  String apIp = ipStr(WiFi.softAPIP());
  String staIp = staCon ? ipStr(WiFi.localIP()) : "";
  String mac = WiFi.macAddress();
  String outTarget = ipStr(artOutTarget());

  char buf[2600];
  snprintf(buf, sizeof(buf),
    "{"
    "\"schema\":\"vizzz.di.node.manifest.v1\","
    "\"kind\":\"firmware-node\","
    "\"product\":\"%s\","
    "\"name\":\"%s\","
    "\"firmware\":{\"tag\":\"%s\",\"framework\":\"Arduino/PlatformIO\"},"
    "\"identity\":{\"mac\":\"%s\",\"ap_ssid\":\"%s\",\"sta_ssid\":\"%s\"},"
    "\"network\":{\"mode\":\"%s\",\"ap_ip\":\"%s\",\"sta_connected\":%s,\"sta_ip\":\"%s\"},"
    "\"hardware\":{\"board\":\"ESP32 DevKit\",\"dmx_uart\":\"DMX_NUM_1\",\"dmx_tx_gpio\":%d,\"dmx_dir_gpio\":%d,\"max_channels\":%d,\"dmx_period_ms\":%lu},"
    "\"protocols\":["
      "{\"id\":\"http\",\"role\":\"control\",\"routes\":[\"/\",\"/control\",\"/patch\",\"/scenes\",\"/network\",\"/system\",\"/performance\",\"/vj\",\"/status\",\"/page\",\"/monitor\",\"/node/manifest\",\"/manifest.json\"]},"
      "{\"id\":\"websocket\",\"role\":\"live-status\",\"endpoint\":\"/ws\",\"push_ms\":400},"
      "{\"id\":\"artnet\",\"role\":\"input-output\",\"port\":%u,\"universe\":%u,\"output_target\":\"%s\",\"output_enabled\":%s},"
      "{\"id\":\"sacn\",\"role\":\"input\",\"port\":%u,\"universe\":%u},"
      "{\"id\":\"dmx512\",\"role\":\"physical-output\",\"channels\":%d}"
    "],"
    "\"state\":{\"output_mode\":\"%s\",\"master\":%u,\"artnet_active\":%s,\"sacn_active\":%s,\"web_enabled\":%s},"
    "\"sync\":{\"source\":\"firmware\",\"live_status\":\"/ws\",\"durable_config\":\"ESP32 Preferences NVS\",\"git_policy\":\"verify, commit, push origin/main\"}"
    "}",
    PRODUCT_NAME,
    eName,
    eFw,
    mac.c_str(), eSsid, eStaSsid,
    netModeName(netMode), apIp.c_str(), staCon ? "true" : "false", staIp.c_str(),
    DMX_TX, DMX_DIR, MAX_CH, (unsigned long)DMX_PERIOD_MS,
    ARTNET_PORT, universe(), outTarget.c_str(), artOutEnabled ? "true" : "false",
    SACN_PORT, universe(),
    MAX_CH,
    modeName(mode), masterDimmer, artnetActive() ? "true" : "false", sacnActive() ? "true" : "false", webEnabled ? "true" : "false"
  );
  return buf;
}

static String wifiScanJSON() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    if (wifiScanActive && millis() - wifiScanStartMs > WIFI_SCAN_TIMEOUT_MS) {
      WiFi.scanDelete();
      wifiScanActive = false;
      return "{\"scanning\":false,\"error\":\"timeout\",\"networks\":[]}";
    }
    return "{\"scanning\":true}";
  }

  if (wifiScanActive && n == WIFI_SCAN_FAILED) {
    WiFi.scanDelete();
    wifiScanActive = false;
    return "{\"scanning\":false,\"error\":\"failed\",\"networks\":[]}";
  }

  if (n < 0 && !wifiScanActive) {
    WiFi.scanDelete();
    ensureStaInterface();
    wifiScanActive = true;
    wifiScanStartMs = millis();
    int rc = WiFi.scanNetworks(true, false);
    if (rc == WIFI_SCAN_FAILED) {
      wifiScanActive = false;
      return "{\"scanning\":false,\"error\":\"failed\",\"networks\":[]}";
    }
    return "{\"scanning\":true}";
  }

  if (n < 0) {
    return "{\"scanning\":true}";
  }

  wifiScanActive = false;

  String s;
  s.reserve(34 + size_t(n) * 96);
  s = "{\"scanning\":false,\"networks\":[";
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

  auto sendApp = [](AsyncWebServerRequest* r){
    r->send(200, "text/html", reinterpret_cast<const uint8_t*>(APP_HTML), sizeof(APP_HTML) - 1);
  };
  server.on("/",            HTTP_GET, sendApp);
  server.on("/control",     HTTP_GET, sendApp);
  server.on("/patch",       HTTP_GET, sendApp);
  server.on("/scenes",      HTTP_GET, sendApp);
  server.on("/network",     HTTP_GET, sendApp);
  server.on("/system",      HTTP_GET, sendApp);
  server.on("/performance", HTTP_GET, sendApp);
  server.on("/vj",          HTTP_GET, sendApp);

  server.on("/status",  HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, statusJSON()); });
  server.on("/monitor", HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, monitorJSON()); });
  server.on("/node/manifest", HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, nodeManifestJSON()); });
  server.on("/manifest.json", HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, nodeManifestJSON()); });

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
    saveConfig();
    refreshSacnSocket();
    r->send(204);
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

  server.on("/netmode/set", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!r->hasArg("m")) { r->send(400, "text/plain", "missing arg"); return; }
    netMode = NetMode(constrain(r->arg("m").toInt(), 0, int(NET_AP_ONLY)));
    saveConfig();
    r->send(200, "text/plain", "ok,rebooting");
    pendingReboot = true;
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
    uint8_t snap[MAX_CH];
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
    uint8_t target[MAX_CH];
    loadScene(n, target);
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    startFade_locked(target, ft);
    xSemaphoreGive(gLock);
    r->send(204);
  });

  server.on("/wifi/scan",   HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, wifiScanJSON()); });

  server.on("/wifi/set", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!r->hasArg("ssid")) { r->send(400, "text/plain", "missing ssid"); return; }
    staSSID = r->arg("ssid");
    staPass = r->hasArg("pass") ? r->arg("pass") : "";
    if (netMode == NET_AP_ONLY) netMode = NET_AP_STA;
    saveConfig();
    pendingWifiReconnect = true;
    r->send(204);
  });

  server.on("/wifi/forget", HTTP_GET, [](AsyncWebServerRequest* r){
    staSSID = ""; staPass = "";
    saveConfig();
    pendingWifiForget = true;
    r->send(204);
  });

  server.on("/node/set", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasArg("name"))    nodeName = r->arg("name");
    if (r->hasArg("ap_ssid")) apSsid   = r->arg("ap_ssid");
    if (r->hasArg("ap_pass")) apPass   = r->arg("ap_pass");
    saveConfig();
    startMdns();
    r->send(204);
  });

  server.on("/reboot", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200, "text/plain", "Rebooting…");
    pendingReboot = true;
  });

  server.on("/discover", HTTP_GET, [](AsyncWebServerRequest* r){
    char buf[160];
    char eName[64]; jsonEsc(eName, sizeof(eName), nodeName);
    snprintf(buf, sizeof(buf),
      "{\"name\":\"%s\",\"ip\":\"%s\",\"ap_ip\":\"10.0.0.1\",\"mdns\":\"%s.local\",\"product\":\"vizzz.di\"}",
      eName, ipStr(WiFi.localIP()).c_str(), mdnsName.c_str());
    r->send(200, "application/json", buf);
    sendBeacon();
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
  memset(sacnVals, 0, MAX_CH);
  memset(outVals,  0, MAX_CH);

  setupDMX();
  startWiFi();
  if (needSaveConfig) { saveConfig(); needSaveConfig = false; }
  startMdns();
  refreshArtNetSocket();
  artOutUdp.begin(0);
  refreshSacnSocket();
  if (webEnabled) setupWeb();

  lastArtnetMs = 0;
  lastSacnMs = 0;

  Serial.printf("\n=== vizzz.di ===\n");
  Serial.printf("AP SSID  : %s\n", apSsid.c_str());
  Serial.printf("AP IP    : %s\n", ipStr(WiFi.softAPIP()).c_str());
  Serial.printf("mDNS     : http://%s.local\n", mdnsName.c_str());
  if (staSSID.length()) {
    Serial.printf("STA SSID : %s\n", staSSID.c_str());
    if (WiFi.status() == WL_CONNECTED)
      Serial.printf("STA IP   : %s\n", ipStr(WiFi.localIP()).c_str());
  }
  Serial.printf("Mode     : %s\n", modeName(mode));
  Serial.printf("Net Mode : %s\n", netModeName(netMode));
  Serial.printf("Art-Net  : net=%u sub=%u uni=%u (15-bit=%u)\n",
    artNet, artSubnet, artUni, universe());
  Serial.printf("sACN     : universe=%u port=%u\n", universe(), SACN_PORT);
  Serial.printf("Art-Out  : %s\n", artOutEnabled ? "enabled" : "disabled");
  Serial.printf("Web      : %s\n", webEnabled ? "enabled" : "disabled");
}

void loop() {
  if (pendingReboot) { delay(150); ESP.restart(); }

  if (pendingWifiForget) {
    pendingWifiForget = false;
    WiFi.disconnect(false);
  }

  if (pendingWifiReconnect) {
    pendingWifiReconnect = false;
    WiFi.scanDelete();
    wifiScanActive = false;
    ensureStaInterface();
    WiFi.begin(staSSID.c_str(), staPass.c_str());
  }

  pollArtNet();
  pollSacn();

  // Rejoin sACN multicast when WiFi link state changes.
  static wl_status_t prevWiFiStatus = WL_IDLE_STATUS;
  wl_status_t curWiFiStatus = WiFi.status();
  if (curWiFiStatus != prevWiFiStatus) {
    prevWiFiStatus = curWiFiStatus;
    if (curWiFiStatus == WL_CONNECTED) { startMdns(); sendBeacon(); }
    refreshArtNetSocket();
    refreshSacnSocket();
  }

  const uint32_t now = millis();

  // DMX output cycle
  static uint32_t lastDmx = 0;
  if (now - lastDmx >= DMX_PERIOD_MS) {
    lastDmx = now;

    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) == pdTRUE) {
      updateFade_locked();
      computeOutput_locked(artnetActive(), sacnActive());
      xSemaphoreGive(gLock);
    }

    sendDMX();
    sendArtNetOut();
  }

  // Discovery beacon every 30s + respond to incoming discovery requests
  {
    static uint32_t lastBeacon = 0;
    if (now - lastBeacon >= 30000) { lastBeacon = now; sendBeacon(); }
    int sz = discoverUdp.parsePacket();
    if (sz > 0) {
      discoverUdp.flush();
      sendBeacon();  // unicast reply goes to subnet broadcast; good enough
    }
  }

  // WebSocket status push ~400ms (only when web stack is enabled)
  if (webEnabled) {
    static uint32_t lastWs = 0;
    if (now - lastWs >= 400) {
      lastWs = now;
      if (ws.count() > 0) {
        char statusBuf[720];
        size_t len = fillStatusJSON(statusBuf, sizeof(statusBuf));
        ws.textAll(statusBuf, len);
      }
      ws.cleanupClients();
    }
  }
}
