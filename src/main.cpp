#include <Arduino.h>
#include <stdarg.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

#include <math.h>

#include "esp_dmx.h"
#include "vizzz_core.h"

// The AP password lives in src/secrets.h, which is gitignored. Copy
// src/secrets.example.h over it and set your own. Without that file the build
// still succeeds on the placeholder below and warns about it at boot.
#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef AP_PASS_DEFAULT
  #define AP_PASS_DEFAULT "changeme123"
#endif

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
static constexpr uint8_t    SAFE_MAX_LEVEL    = 245;
static constexpr uint8_t    SAFE_SLEW_STEP    = 18;
static constexpr uint8_t    BURN_SAFE_MAX_LEVEL = 96;
static constexpr uint8_t    BURN_SAFE_SLEW_STEP = 8;
static constexpr uint8_t    ARTNET_PKT_BUDGET = 10;
static constexpr uint8_t    SACN_PKT_BUDGET   = 10;
static constexpr uint8_t    OSC_PKT_BUDGET    = 14;
static constexpr uint8_t    DISC_PKT_BUDGET   = 4;

// ── Paging / Scenes ───────────────────────────────────────────────────────────
static constexpr uint16_t PAGE_SIZE   = 32;
static constexpr uint16_t PAGE_COUNT  = 16;
static constexpr uint8_t  SCENE_COUNT = 8;
static constexpr uint8_t  MAX_FIXTURES = 12;
static constexpr uint32_t FADE_MAX_MS  = 600000;  // 10 min; see startFade_locked
static constexpr size_t   MAX_NAME_LEN = 31;      // keeps the 800-byte WS status JSON valid
static constexpr size_t   MAX_PASS_LEN = 63;
static constexpr uint32_t WEB_GRACE_MS = 90000;   // console stays up this long even when disabled

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
static String  apPass   = AP_PASS_DEFAULT;
static String  staSSID, staPass;
static uint8_t artNet = 0, artSubnet = 0, artUni = 0;
static Mode    mode         = MODE_HTP;
static NetMode netMode      = NET_AP_STA;
static bool    artOutEnabled = false;   // broadcast webVals as Art-Net to slaves
static String  artOutPeerIp = "";       // if non-empty, unicast to this IP instead of broadcast
static bool    webEnabled    = true;    // when false, web server and websocket stay disabled
static bool    burnSafeMode  = false;   // temporary reduced-output profile for stress tests
static bool    artnetFallbackToWeb = true; // use web layer when Art-Net/sACN signal is inactive
static bool    needSaveConfig = false;  // set when auto-generated values must be persisted
static constexpr const char* FW_TAG = __DATE__ " " __TIME__;
static constexpr uint32_t WIFI_SCAN_TIMEOUT_MS = 15000;
static constexpr uint16_t DISCOVERY_PORT       = 47777;
static constexpr uint16_t OSC_PORT             = 9000;
static bool wifiScanActive = false;
static uint32_t wifiScanStartMs = 0;

static uint16_t universe() {
  return vizzz::packUniverse(artNet, artSubnet, artUni);
}

static bool isGeneratedApSsid(const String& ssid) {
  return ssid.startsWith(AP_SSID_PREFIX) || ssid.startsWith(LEGACY_AP_SSID_PREFIX);
}

static bool isGeneratedNodeName(const String& name) {
  return name == PRODUCT_NAME || name.startsWith(String(PRODUCT_NAME) + "-");
}

// Returns the last 3 bytes of the STA MAC as uppercase hex (e.g. "BA6FCC").
// Deterministic per device, survives NVS erase and firmware reflash.
static String macSuffix6() {
  uint8_t m[6];
  WiFi.macAddress(m);
  char s[7];
  snprintf(s, sizeof(s), "%02X%02X%02X", m[3], m[4], m[5]);
  return s;
}

static String makeNodeNameFromMac() {
  return String(PRODUCT_NAME) + "-" + macSuffix6();
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
  prefs.putBool("burn_safe",  burnSafeMode);
  prefs.putBool("anet_fb_web", artnetFallbackToWeb);
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
  burnSafeMode = prefs.getBool("burn_safe",  burnSafeMode);
  artnetFallbackToWeb = prefs.getBool("anet_fb_web", artnetFallbackToWeb);
  String savedFwTag = prefs.getString("fw_tag", "");
  prefs.end();
  if (mode > MODE_HTP) mode = MODE_HTP;
  if (netMode > NET_AP_ONLY) netMode = NET_AP_STA;

  // Update persisted FW_TAG and regenerate node name if still default.
  // Do NOT clear apSsid on firmware updates — suffix is random, would break reconnection.
  if (savedFwTag != FW_TAG) {
    if (nodeName.isEmpty() || isGeneratedNodeName(nodeName)) nodeName = makeNodeNameFromMac();
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
static WiFiUDP        oscUdp;
static dmx_port_t     dmxPort = DMX_NUM_1;

// ── Peer discovery ────────────────────────────────────────────────────────────
struct PeerNode {
  char     name[40];
  char     ip[16];
  char     mdns[48];
  char     mac[18];   // "xx:xx:xx:xx:xx:xx\0" — used for dedup and identity
  uint32_t lastSeenMs;
};
static constexpr uint8_t MAX_PEERS    = 4;
static constexpr uint32_t PEER_EXPIRE_MS = 90000;
static PeerNode  peers[MAX_PEERS];   // guarded by gLock
static uint8_t   peerCount = 0;

static uint8_t dmxFrame [MAX_CH + 1];
static uint8_t webVals  [MAX_CH];   // web layer — protected by gLock
static uint8_t artVals  [MAX_CH];   // Art-Net IN — written from loop UDP poll
static uint8_t sacnVals [MAX_CH];   // sACN IN
static uint8_t outVals  [MAX_CH];   // final computed output

static volatile uint32_t lastArtnetMs = 0;
static volatile uint32_t lastSacnMs   = 0;
static uint8_t masterDimmer = 255;  // 0=off, 255=full — resets to full on boot

// ── VJ controls (groups / cues / FX) ────────────────────────────────────────
struct GroupDef {
  char name[16];
  uint16_t start; // 1-based DMX channel
  uint16_t end;   // 1-based DMX channel
  bool enabled;
};
static constexpr uint8_t MAX_GROUPS = 8;
static GroupDef groups[MAX_GROUPS];

struct FixtureDef {
  char name[16];
  uint16_t start; // 1-based DMX channel
  uint16_t end;   // 1-based DMX channel
  uint8_t x;      // 0..255 stage X position
  uint8_t y;      // 0..255 stage Y position
  bool enabled;
};
static FixtureDef fixtures[MAX_FIXTURES];
static uint8_t fixtureCount = 4;

static void initFixtureDefaults() {
  const uint16_t starts[MAX_FIXTURES] = {1, 4, 7, 10, 64, 67, 70, 73, 128, 131, 134, 137};
  const uint16_t ends[MAX_FIXTURES]   = {3, 6, 9, 12, 66, 69, 72, 75, 130, 133, 136, 139};
  const uint8_t xs[MAX_FIXTURES] = {32, 96, 160, 224, 32, 96, 160, 224, 32, 96, 160, 224};
  const uint8_t ys[MAX_FIXTURES] = {32, 32, 32, 32, 128, 128, 128, 128, 224, 224, 224, 224};
  fixtureCount = 4;
  for (uint8_t i = 0; i < MAX_FIXTURES; i++) {
    snprintf(fixtures[i].name, sizeof(fixtures[i].name), "F%u", i + 1);
    fixtures[i].start = starts[i];
    fixtures[i].end = ends[i];
    fixtures[i].x = xs[i];
    fixtures[i].y = ys[i];
    fixtures[i].enabled = (i < fixtureCount);
  }
}

static void saveFixtures() {
  prefs.begin("fixtures", false);
  prefs.putUChar("count", fixtureCount);
  prefs.putBytes("items", fixtures, sizeof(fixtures));
  prefs.end();
}

static void loadFixtures() {
  initFixtureDefaults();
  prefs.begin("fixtures", true);
  uint8_t count = prefs.getUChar("count", fixtureCount);
  size_t got = prefs.getBytes("items", fixtures, sizeof(fixtures));
  prefs.end();
  if (got == sizeof(fixtures)) {
    fixtureCount = constrain(count, uint8_t(1), MAX_FIXTURES);
    for (uint8_t i = 0; i < MAX_FIXTURES; i++) fixtures[i].enabled = (i < fixtureCount) ? fixtures[i].enabled : false;
  } else {
    initFixtureDefaults();
  }
}

// Groups and cues live in their own namespace. The VJ deck offers "Save Group"
// and a cue "Save"; before this they only ever wrote to RAM and were lost on the
// next reboot. /group/apply deliberately does NOT save — it is a live fader and
// would burn NVS write cycles.
static void saveVj();

struct CueStep {
  uint8_t scene;       // 0..7
  uint32_t dwellMs;    // hold time after fade
  uint32_t fadeMs;     // fade duration
};
static constexpr uint8_t MAX_CUES = 16;
static CueStep cueList[MAX_CUES];
static uint8_t cueCount = 0;
static uint8_t cueIndex = 0;
static bool cueRunning = false;
static uint32_t cueNextMs = 0;      // reported in /cue/status only
static uint32_t cueStartedMs = 0;   // rollover-safe scheduling below
static uint32_t cueStepMs = 0;

enum FxMode : uint8_t {
  FX_NONE = 0,
  FX_STROBE = 1,
  FX_CHASE = 2,
  FX_PULSE = 3,
  FX_SINE = 4,
  FX_SPARKLE = 5,
  FX_COMET = 6,
  FX_BARS = 7,
  FX_GLITCH = 8,
  FX_RADAR = 9,
};
static FxMode fxMode = FX_NONE;
static bool fxEnabled = false;
static uint16_t fxBpm = 120;
static uint8_t fxDepth = 255;
static uint8_t colorR = 255;
static uint8_t colorG = 255;
static uint8_t colorB = 255;
static bool colorEnabled = false;
static uint32_t lastTapMs = 0;
static uint32_t tapIntervals[4] = {0, 0, 0, 0};
static uint8_t tapCount = 0;
static uint8_t tapPos = 0;

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
static volatile bool     pendingFactoryReset  = false;
static volatile bool     pendingPanic         = false;
static volatile bool     pendingBlackout      = false;
static bool              webStopped           = false;

// ── Broadcast queue (web callbacks write, peer task drains) ───────────────────
static constexpr uint8_t BCAST_CAP      = 8;
static constexpr size_t  BCAST_PATH_LEN = 72;
static constexpr size_t  BCAST_IP_LEN   = 16;
struct BcastEntry {
  char path[BCAST_PATH_LEN];
  char targetIp[BCAST_IP_LEN];  // empty → all peers; non-empty → specific peer only
};
static QueueHandle_t bcastQueue = nullptr;
static TaskHandle_t  bcastTaskHandle = nullptr;

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

static uint8_t effectiveSafeMaxLevel() {
  return burnSafeMode ? BURN_SAFE_MAX_LEVEL : SAFE_MAX_LEVEL;
}

static uint8_t effectiveSafeSlewStep() {
  return burnSafeMode ? BURN_SAFE_SLEW_STEP : SAFE_SLEW_STEP;
}

static bool jsonAppend(char* buf, size_t n, int& pos, const char* fmt, ...) {
  if (!buf || !n) return false;
  if (pos < 0) pos = 0;
  if (size_t(pos) >= n) {
    pos = int(n);
    return false;
  }
  va_list ap;
  va_start(ap, fmt);
  int written = vsnprintf(buf + pos, n - size_t(pos), fmt, ap);
  va_end(ap);
  if (written < 0) {
    pos = int(n);
    return false;
  }
  if (size_t(written) >= n - size_t(pos)) {
    pos = int(n);
    return false;
  }
  pos += written;
  return true;
}

// Extract a quoted JSON string value for a given key (simple, no escape handling needed).
static bool jsonExtract(const char* json, const char* key, char* dst, size_t dstLen) {
  char search[48];
  snprintf(search, sizeof(search), "\"%s\":\"", key);
  const char* p = strstr(json, search);
  if (!p) return false;
  p += strlen(search);
  size_t i = 0;
  while (*p && *p != '"' && i < dstLen - 1) dst[i++] = *p++;
  dst[i] = '\0';
  return i > 0;
}

// ── Fleet broadcast helpers ───────────────────────────────────────────────────
static bool startsWithCStr(const char* s, const char* prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool isCleanPeerPath(const char* path) {
  if (!path || path[0] != '/' || path[1] == '/') return false;
  for (size_t i = 0; i < BCAST_PATH_LEN; i++) {
    uint8_t c = uint8_t(path[i]);
    if (c == 0) return i > 1;
    if (c <= 0x20 || c >= 0x7f || c == '#') return false;
  }
  return false;  // too long for the fixed queue entry
}

static bool isAllowedPeerPath(const char* path) {
  if (!isCleanPeerPath(path) || strstr(path, "://")) return false;
  return strcmp(path, "/blackout") == 0 ||
         strcmp(path, "/full") == 0 ||
         startsWithCStr(path, "/master?v=") ||
         startsWithCStr(path, "/scene/recall?n=");
}

static bool sendPeerGet(const char* ip, const char* path) {
  IPAddress addr;
  if (!addr.fromString(ip) || !isAllowedPeerPath(path)) return false;

  WiFiClient client;
  client.setTimeout(120);
  if (!client.connect(addr, 80, 120)) {
    client.stop();
    return false;
  }

  char req[180];
  int len = snprintf(req, sizeof(req),
    "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
    path, ip);
  if (len <= 0 || size_t(len) >= sizeof(req)) {
    client.stop();
    return false;
  }
  client.write(reinterpret_cast<const uint8_t*>(req), size_t(len));
  client.flush();
  client.stop();
  return true;
}

static void bcastTask(void*) {
  BcastEntry entry;
  for (;;) {
    if (xQueueReceive(bcastQueue, &entry, portMAX_DELAY) != pdTRUE) continue;
    if (!isAllowedPeerPath(entry.path)) continue;

    if (entry.targetIp[0]) {
      sendPeerGet(entry.targetIp, entry.path);
      continue;
    }

    char ips[MAX_PEERS][BCAST_IP_LEN];
    uint8_t cnt = 0;
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) == pdTRUE) {
      cnt = peerCount;
      for (uint8_t i = 0; i < cnt; i++) strlcpy(ips[i], peers[i].ip, BCAST_IP_LEN);
      xSemaphoreGive(gLock);
    }
    for (uint8_t i = 0; i < cnt; i++) {
      sendPeerGet(ips[i], entry.path);
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

static bool startBcastTask() {
  if (bcastQueue) return true;
  bcastQueue = xQueueCreate(BCAST_CAP, sizeof(BcastEntry));
  if (!bcastQueue) return false;
  BaseType_t ok = xTaskCreatePinnedToCore(
    bcastTask, "peerBcast", 4096, nullptr, 1, &bcastTaskHandle, 0);
  if (ok != pdPASS) {
    vQueueDelete(bcastQueue);
    bcastQueue = nullptr;
    bcastTaskHandle = nullptr;
    return false;
  }
  return true;
}

// Called from async web callbacks. targetIp = nullptr -> all peers.
static bool enqueueBcast(const char* path, const char* targetIp = nullptr) {
  if (!bcastQueue || !isAllowedPeerPath(path)) return false;
  BcastEntry entry{};
  strlcpy(entry.path, path, sizeof(entry.path));
  if (targetIp) strlcpy(entry.targetIp, targetIp, sizeof(entry.targetIp));
  return xQueueSend(bcastQueue, &entry, 0) == pdTRUE;
}

// Forward declarations needed by startWiFi()
static String mdnsName;
static void buildMdnsName();

// ── WiFi ──────────────────────────────────────────────────────────────────────
// True when a datagram came from one of our own interfaces. Art-Net OUT
// broadcasts the merged frame; without this check a node in MERGE_HTP takes its
// own output back in and the level can only ever ratchet upwards.
static bool isOwnIp(const IPAddress& ip) {
  if (ip == WiFi.softAPIP()) return true;
  if (WiFi.status() == WL_CONNECTED && ip == WiFi.localIP()) return true;
  return false;
}

// Returns the Art-Net target: peer unicast if set, else STA subnet broadcast, else AP broadcast
static IPAddress artOutTarget() {
  if (!artOutPeerIp.isEmpty()) {
    IPAddress peer;
    if (peer.fromString(artOutPeerIp)) return peer;
  }
  if (WiFi.status() == WL_CONNECTED) {
    uint32_t ip   = (uint32_t)WiFi.localIP();
    uint32_t mask = (uint32_t)WiFi.subnetMask();
    return IPAddress((ip & mask) | (~mask));
  }
  return IPAddress(192, 168, 4, 255);
}

static void startSoftAP() {
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
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

  // Ensure the AP SSID is MAC-derived. Migrate any stale NVS value
  // (e.g. old random-suffix firmware) so reflashing fixes identity automatically.
  {
    String expected = String(AP_SSID_PREFIX) + "_" + macSuffix6();
    if (apSsid.isEmpty() || apSsid != expected) {
      // Only overwrite if it looks like a vizzz.di_ SSID (not user-customised).
      // A user-customised SSID won't start with the default prefix.
      if (apSsid.isEmpty() || apSsid.startsWith(AP_SSID_PREFIX)) {
        apSsid = expected;
        needSaveConfig = true;
      }
    }
  }

  // Register DHCP hostname BEFORE connecting so the router's table is correct.
  // Without this both devices appear as generic "espressif" in the DHCP lease list.
  buildMdnsName();
  WiFi.setHostname(mdnsName.c_str());

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
  char buf[240];
  char eName[64];   jsonEsc(eName,   sizeof(eName),   nodeName);
  char eApSsid[64]; jsonEsc(eApSsid, sizeof(eApSsid), apSsid);
  String mac = WiFi.macAddress();
  snprintf(buf, sizeof(buf),
    "{\"name\":\"%s\",\"ip\":\"%s\",\"ap_ssid\":\"%s\",\"mdns\":\"%s.local\",\"mac\":\"%s\",\"product\":\"vizzz.di\"}",
    eName, ipStr(WiFi.localIP()).c_str(), eApSsid, mdnsName.c_str(), mac.c_str());
  discoverUdp.beginPacket(bcast, DISCOVERY_PORT);
  discoverUdp.write((const uint8_t*)buf, strlen(buf));
  discoverUdp.endPacket();
}

// Parse an incoming beacon UDP packet and update the peers table.
static void pollDiscovery() {
  char buf[256];
  for (uint8_t processed = 0; processed < DISC_PKT_BUDGET; processed++) {
    int sz = discoverUdp.parsePacket();
    if (sz <= 0) break;

    int n = discoverUdp.read(buf, min(sz, (int)sizeof(buf) - 1));
    discoverUdp.flush();  // drop any residue; parsePacket() stalls forever otherwise
    if (n <= 0) continue;
    buf[n] = '\0';
    if (!strstr(buf, "vizzz.di")) continue;  // ignore non-vizzz packets

    char pName[40], pIp[16], pIpJson[16], pMdns[48], pMac[18];
    pMdns[0] = '\0';  // optional in the beacon; without this it leaks stack
    if (!jsonExtract(buf, "name", pName, sizeof(pName))) continue;
    if (!jsonExtract(buf, "ip",   pIpJson, sizeof(pIpJson))) continue;
    jsonExtract(buf, "mdns", pMdns, sizeof(pMdns));
    pMac[0] = '\0';
    jsonExtract(buf, "mac",  pMac,  sizeof(pMac));
    ipToCStr(pIp, sizeof(pIp), discoverUdp.remoteIP());

    // Ignore our own beacon — compare by MAC (reliable) with IP fallback.
    String ownMac = WiFi.macAddress();
    if (pMac[0] != '\0') {
      if (strcasecmp(pMac, ownMac.c_str()) == 0) continue;
    } else if (WiFi.status() == WL_CONNECTED) {
      if (ipStr(WiFi.localIP()) == pIp) continue;
    }

    uint32_t now = millis();
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) != pdTRUE) continue;

    bool found = false;
    for (uint8_t i = 0; i < peerCount; i++) {
      // Match by MAC first (IP can change on DHCP rebind), fall back to IP.
      bool match = (pMac[0] != '\0') ? (strcasecmp(peers[i].mac, pMac) == 0)
                                     : (strcmp(peers[i].ip, pIp) == 0);
      if (match) {
        strlcpy(peers[i].name, pName, sizeof(peers[i].name));
        strlcpy(peers[i].ip,   pIp,   sizeof(peers[i].ip));
        strlcpy(peers[i].mdns, pMdns, sizeof(peers[i].mdns));
        strlcpy(peers[i].mac,  pMac,  sizeof(peers[i].mac));
        peers[i].lastSeenMs = now;
        found = true;
        break;
      }
    }
    if (!found && peerCount < MAX_PEERS) {
      strlcpy(peers[peerCount].name, pName, sizeof(peers[peerCount].name));
      strlcpy(peers[peerCount].ip,   pIp,   sizeof(peers[peerCount].ip));
      strlcpy(peers[peerCount].mdns, pMdns, sizeof(peers[peerCount].mdns));
      strlcpy(peers[peerCount].mac,  pMac,  sizeof(peers[peerCount].mac));
      peers[peerCount].lastSeenMs = now;
      peerCount++;
    }
    bool isNewPeer = !found;
    xSemaphoreGive(gLock);

    // Reply only to a peer we did not already know, and no more than once a
    // second. Replying to every beacon makes two nodes trade broadcasts
    // forever, which eats airtime and wrecks Art-Net timing for the whole LAN.
    static uint32_t lastBeaconReplyMs = 0;
    if (isNewPeer && (uint32_t)(now - lastBeaconReplyMs) >= 1000) {
      lastBeaconReplyMs = now;
      sendBeacon();
    }
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
  char key[4];
  snprintf(key, sizeof(key), "s%u", n);
  prefs.begin("scenes", true);
  size_t got = prefs.getBytes(key, dst, MAX_CH);
  prefs.end();
  if (got != MAX_CH) memset(dst, 0, MAX_CH);
}

static void saveScene(uint8_t n, const uint8_t* src) {
  char key[4];
  snprintf(key, sizeof(key), "s%u", n);
  prefs.begin("scenes", false);
  prefs.putBytes(key, src, MAX_CH);
  prefs.end();
}

static void saveVj() {
  prefs.begin("vj", false);
  prefs.putBytes("groups", groups, sizeof(groups));
  prefs.putBytes("cues", cueList, sizeof(cueList));
  prefs.putUChar("cuecount", cueCount);
  prefs.end();
}

static void loadVj() {
  prefs.begin("vj", true);
  size_t g = prefs.getBytes("groups", groups, sizeof(groups));
  size_t c = prefs.getBytes("cues", cueList, sizeof(cueList));
  uint8_t n = prefs.getUChar("cuecount", cueCount);
  prefs.end();
  if (g != sizeof(groups)) {
    for (uint8_t i = 0; i < MAX_GROUPS; i++) {
      snprintf(groups[i].name, sizeof(groups[i].name), "G%u", i + 1);
      groups[i].start = (i * 64) + 1;
      groups[i].end = min<uint16_t>(MAX_CH, groups[i].start + 63);
      groups[i].enabled = true;
    }
  }
  if (c == sizeof(cueList)) cueCount = constrain(n, uint8_t(1), MAX_CUES);
  // Stored values are re-clamped: a struct written by older firmware, or a
  // corrupted read, must not put an out-of-range channel into the output path.
  for (uint8_t i = 0; i < MAX_GROUPS; i++) {
    groups[i].name[sizeof(groups[i].name) - 1] = '\0';
    groups[i].start = constrain(groups[i].start, uint16_t(1), uint16_t(MAX_CH));
    groups[i].end   = constrain(groups[i].end, groups[i].start, uint16_t(MAX_CH));
  }
  for (uint8_t i = 0; i < MAX_CUES; i++) {
    if (cueList[i].scene >= SCENE_COUNT) cueList[i].scene = 0;
    if (cueList[i].fadeMs  > FADE_MAX_MS) cueList[i].fadeMs  = FADE_MAX_MS;
    if (cueList[i].dwellMs > FADE_MAX_MS) cueList[i].dwellMs = FADE_MAX_MS;
  }
  if (cueIndex >= cueCount) cueIndex = 0;
}

// ── Fade (call with gLock held) ───────────────────────────────────────────────
static void startFade_locked(const uint8_t* target, uint32_t ms) {
  memcpy(fadeFrom, webVals, MAX_CH);
  memcpy(fadeTo,   target,  MAX_CH);
  fadeStartMs = millis();
  // updateFade_locked computes (elapsed << 8), which overflows uint32 once
  // elapsed passes 2^24 ms (~4.7 h) and throws the fade backwards. Ten minutes
  // is longer than any real cue and leaves three orders of magnitude of margin.
  if (ms > FADE_MAX_MS) ms = FADE_MAX_MS;
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

static void initVjDefaults() {
  for (uint8_t i = 0; i < MAX_GROUPS; i++) {
    snprintf(groups[i].name, sizeof(groups[i].name), "G%u", i + 1);
    groups[i].start = (i * 64) + 1;
    groups[i].end = min<uint16_t>(MAX_CH, groups[i].start + 63);
    groups[i].enabled = true;
  }
  for (uint8_t i = 0; i < MAX_CUES; i++) {
    cueList[i].scene = i % SCENE_COUNT;
    cueList[i].dwellMs = 1500;
    cueList[i].fadeMs = 500;
  }
  cueCount = 4;
  initFixtureDefaults();
}

static void triggerCueStep(uint8_t idx) {
  uint8_t scene = 0;
  uint32_t fade = 0;
  uint32_t dwell = 0;
  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) return;
  if (idx >= cueCount) {
    xSemaphoreGive(gLock);
    return;
  }
  scene = cueList[idx].scene;
  fade = cueList[idx].fadeMs;
  dwell = cueList[idx].dwellMs;
  xSemaphoreGive(gLock);

  uint8_t target[MAX_CH];
  loadScene(scene, target);

  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) return;
  startFade_locked(target, fade);
  cueStartedMs = millis();
  cueStepMs    = fade + dwell;
  cueNextMs    = cueStartedMs + cueStepMs;  // display only; may wrap
  xSemaphoreGive(gLock);
}

static void tickCueEngine(uint32_t now) {
  uint8_t nextIdx = 0;
  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) != pdTRUE) return;
  // Delta comparison, not `now < cueNextMs`. The absolute form free-runs for
  // ~300 ms every 49.7 days when millis() wraps, firing a scene change per loop.
  if (!cueRunning || cueCount == 0 || (uint32_t)(now - cueStartedMs) < cueStepMs) {
    xSemaphoreGive(gLock);
    return;
  }
  cueIndex = (cueIndex + 1) % cueCount;
  nextIdx = cueIndex;
  xSemaphoreGive(gLock);
  triggerCueStep(nextIdx);
}

static uint8_t tri8(uint8_t phase) {
  return phase < 128 ? uint8_t(phase << 1) : uint8_t((255 - phase) << 1);
}

static uint8_t fxApplyDepth(uint8_t level) {
  uint8_t floorLevel = uint8_t(255 - fxDepth);
  return uint8_t(floorLevel + ((uint16_t(level) * fxDepth + 127) / 255));
}

static uint8_t fxLaneForChannel(uint16_t ch, uint8_t lanes) {
  uint16_t laneSize = (MAX_CH + lanes - 1) / lanes;
  if (laneSize == 0) laneSize = 1;
  return min<uint8_t>(lanes - 1, uint8_t(ch / laneSize));
}

static const FixtureDef* fixtureForChannel(uint16_t ch) {
  for (uint8_t i = 0; i < fixtureCount; i++) {
    if (!fixtures[i].enabled) continue;
    uint16_t s = constrain(fixtures[i].start, 1, MAX_CH);
    uint16_t e = constrain(fixtures[i].end, s, MAX_CH);
    if (ch + 1 >= s && ch + 1 <= e) return &fixtures[i];
  }
  return nullptr;
}

static uint32_t fxHash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dUL;
  x ^= x >> 15;
  x *= 0x846ca68bUL;
  x ^= x >> 16;
  return x;
}

static uint8_t tailLevel(uint8_t dist) {
  switch (dist) {
    case 0: return 255;
    case 1: return 190;
    case 2: return 110;
    case 3: return 52;
    default: return 0;
  }
}

static uint8_t fxLevelForChannel(uint16_t ch, uint32_t now) {
  if (!fxEnabled || fxMode == FX_NONE) return 255;
  uint32_t bpm = max<uint16_t>(20, fxBpm);
  uint32_t beatMs = 60000UL / bpm;
  if (beatMs == 0) beatMs = 1;

  if (fxMode == FX_STROBE) {
    uint32_t slice = max<uint32_t>(18, beatMs / 8);
    bool on = (now % slice) < max<uint32_t>(6, slice / 3);
    return fxApplyDepth(on ? 255 : 0);
  }

  if (fxMode == FX_CHASE) {
    const uint8_t lanes = 8;
    uint8_t activeLane = uint8_t((now / max<uint32_t>(24, beatMs / 2)) % lanes);
    uint8_t lane = fxLaneForChannel(ch, lanes);
    uint8_t dist = uint8_t((lane + lanes - activeLane) % lanes);
    return fxApplyDepth(tailLevel(dist));
  }

  if (fxMode == FX_PULSE) {
    uint8_t phase = uint8_t(((now % beatMs) * 255UL) / beatMs);
    uint8_t level = tri8(phase);
    level = uint8_t((uint16_t(level) * level + 255) >> 8);
    return fxApplyDepth(level);
  }

  if (fxMode == FX_SINE) {
    const uint8_t lanes = 16;
    uint8_t lane = fxLaneForChannel(ch, lanes);
    uint8_t phase = uint8_t((((now % beatMs) * 255UL) / beatMs) + lane * 16);
    uint8_t level = tri8(phase);
    return fxApplyDepth(level);
  }

  if (fxMode == FX_SPARKLE) {
    uint32_t slot = now / max<uint32_t>(18, beatMs / 16);
    uint8_t r = uint8_t(fxHash((uint32_t(ch + 1) << 16) ^ slot) & 0x1F);
    uint8_t level = r < 2 ? 255 : r < 5 ? 150 : r < 9 ? 58 : 0;
    return fxApplyDepth(level);
  }

  if (fxMode == FX_COMET) {
    const uint8_t lanes = 16;
    uint8_t activeLane = uint8_t((now / max<uint32_t>(24, beatMs / 4)) % lanes);
    uint8_t lane = fxLaneForChannel(ch, lanes);
    uint8_t dist = uint8_t((lane + lanes - activeLane) % lanes);
    return fxApplyDepth(tailLevel(dist));
  }

  if (fxMode == FX_BARS) {
    const uint8_t lanes = 16;
    uint8_t lane = fxLaneForChannel(ch, lanes);
    uint8_t flip = uint8_t((now / max<uint32_t>(24, beatMs / 2)) & 1);
    uint8_t level = ((lane & 1) == flip) ? 255 : 18;
    return fxApplyDepth(level);
  }

  if (fxMode == FX_GLITCH) {
    const uint8_t lanes = 16;
    uint8_t lane = fxLaneForChannel(ch, lanes);
    uint32_t slot = now / max<uint32_t>(16, beatMs / 20);
    uint8_t r = uint8_t(fxHash(slot ^ (uint32_t(lane) * 0x45d9f3bUL)) & 0x0F);
    uint8_t level = r < 3 ? 255 : r < 6 ? 0 : r < 9 ? 115 : 32;
    return fxApplyDepth(level);
  }

  if (fxMode == FX_RADAR) {
    const FixtureDef* fx = fixtureForChannel(ch);
    if (!fx) return 255;
    int dx = int(fx->x) - 128;
    int dy = int(fx->y) - 128;
    float ang = atan2f(float(dy), float(dx));
    if (ang < 0.0f) ang += 6.2831853f;
    float sweep = float(now % beatMs) / float(beatMs) * 6.2831853f;
    float diff = fabsf(ang - sweep);
    if (diff > 3.1415926f) diff = 6.2831853f - diff;
    float level = 1.0f - min(1.0f, diff / 0.28f);
    return fxApplyDepth(uint8_t(level * 255.0f));
  }

  return 255;
}

static void applyColorWash_locked() {
  if (!colorEnabled) return;
  for (uint16_t i = 0; i + 2 < MAX_CH; i += 3) {
    webVals[i] = colorR;
    webVals[i + 1] = colorG;
    webVals[i + 2] = colorB;
  }
}

static bool oscReadPaddedString(const uint8_t* pkt, int n, int& off, char* out, size_t outLen) {
  if (off >= n || outLen == 0) return false;
  int i = 0;
  while (off < n && pkt[off] != 0 && i < int(outLen - 1)) out[i++] = char(pkt[off++]);
  out[i] = '\0';
  while (off < n && pkt[off] != 0) off++;
  if (off >= n) return false;
  off++; // consume null terminator
  while (off < n && (off % 4) != 0) off++;
  return i > 0;
}

static bool oscReadInt(const uint8_t* pkt, int n, int& off, int32_t& out) {
  if (off + 4 > n) return false;
  out = (int32_t(pkt[off]) << 24) | (int32_t(pkt[off + 1]) << 16) |
        (int32_t(pkt[off + 2]) << 8) | int32_t(pkt[off + 3]);
  off += 4;
  return true;
}

static bool oscReadFloat(const uint8_t* pkt, int n, int& off, float& out) {
  int32_t raw = 0;
  if (!oscReadInt(pkt, n, off, raw)) return false;
  memcpy(&out, &raw, sizeof(out));
  return true;
}

static void pollOsc() {
  uint8_t pkt[256];
  for (uint8_t processed = 0; processed < OSC_PKT_BUDGET; processed++) {
    int sz = oscUdp.parsePacket();
    if (sz <= 0) break;
    int n = oscUdp.read(pkt, min(int(sizeof(pkt)), sz));
    oscUdp.flush();  // OSC bundles routinely exceed sizeof(pkt); drain the rest
    if (n < 8) continue;

    int off = 0;
    char addr[48], tags[16];
    if (!oscReadPaddedString(pkt, n, off, addr, sizeof(addr))) continue;
    if (!oscReadPaddedString(pkt, n, off, tags, sizeof(tags))) continue;
    if (tags[0] != ',') continue;
    int32_t v = 0;
    if (tags[1] == 'i') {
      if (!oscReadInt(pkt, n, off, v)) continue;
    } else if (tags[1] == 'f') {
      float fv = 0.0f;
      if (!oscReadFloat(pkt, n, off, fv)) continue;
      v = int32_t(fv);
    } else {
      continue;
    }

    if (strncmp(addr, "/ch/", 4) == 0) {
      int ch = atoi(addr + 4);
      if (ch >= 1 && ch <= MAX_CH) {
        if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) == pdTRUE) {
          webVals[ch - 1] = uint8_t(constrain(v, 0, 255));
          xSemaphoreGive(gLock);
        }
      }
    } else if (strncmp(addr, "/group/", 7) == 0) {
      int g = atoi(addr + 7);
      if (g >= 0 && g < MAX_GROUPS) {
        if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) == pdTRUE) {
          if (!groups[g].enabled) {
            xSemaphoreGive(gLock);
            continue;
          }
          uint16_t s = constrain(groups[g].start, 1, MAX_CH);
          uint16_t e = constrain(groups[g].end, s, MAX_CH);
          for (uint16_t ch = s; ch <= e; ch++) webVals[ch - 1] = uint8_t(constrain(v, 0, 255));
          xSemaphoreGive(gLock);
        }
      }
    } else if (strcmp(addr, "/master") == 0) {
      masterDimmer = uint8_t(constrain(v, 0, 255));
    } else if (strcmp(addr, "/scene/recall") == 0) {
      uint8_t idx = uint8_t(constrain(v, 0, SCENE_COUNT - 1));
      uint8_t target[MAX_CH];
      loadScene(idx, target);
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) == pdTRUE) {
        startFade_locked(target, 500);
        xSemaphoreGive(gLock);
      }
    } else if (strcmp(addr, "/cue/run") == 0) {
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) == pdTRUE) {
        cueRunning = (v != 0) && cueCount > 0;
        if (cueRunning) {
          cueIndex = 0;
          xSemaphoreGive(gLock);
          triggerCueStep(0);
          continue;
        }
        xSemaphoreGive(gLock);
      }
    } else if (strcmp(addr, "/fx/mode") == 0) {
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) == pdTRUE) {
        fxMode = FxMode(constrain(v, 0, 9));
        fxEnabled = (fxMode != FX_NONE);
        xSemaphoreGive(gLock);
      }
    } else if (strcmp(addr, "/fx/bpm") == 0) {
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) == pdTRUE) {
        fxBpm = uint16_t(constrain(v, 20, 240));
        xSemaphoreGive(gLock);
      }
    } else if (strcmp(addr, "/color/r") == 0) {
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) == pdTRUE) {
        colorR = uint8_t(constrain(v, 0, 255));
        colorEnabled = true;
        applyColorWash_locked();
        xSemaphoreGive(gLock);
      }
    } else if (strcmp(addr, "/color/g") == 0) {
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) == pdTRUE) {
        colorG = uint8_t(constrain(v, 0, 255));
        colorEnabled = true;
        applyColorWash_locked();
        xSemaphoreGive(gLock);
      }
    } else if (strcmp(addr, "/color/b") == 0) {
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) == pdTRUE) {
        colorB = uint8_t(constrain(v, 0, 255));
        colorEnabled = true;
        applyColorWash_locked();
        xSemaphoreGive(gLock);
      }
    }
  }
}

// ── Output ────────────────────────────────────────────────────────────────────
static void computeOutput_locked(bool aActive, bool sActive, uint32_t nowMs) {
  bool netActive = aActive || sActive;
  uint8_t ceiling = effectiveSafeMaxLevel();
  uint8_t slewStep = effectiveSafeSlewStep();

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
        v = netActive ? netIn : (artnetFallbackToWeb ? webVals[i] : outVals[i]);
        break;
      default: // MODE_HTP
        v = netActive ? max(webVals[i], netIn) : webVals[i];
        break;
    }

    if (masterDimmer < 255) v = vizzz::applyMaster(v, masterDimmer);
    v = vizzz::clampLevel(v, ceiling);
    uint8_t fxLevel = fxLevelForChannel(i, nowMs);
    uint8_t target = (fxLevel < 255) ? vizzz::applyMaster(v, fxLevel) : v;
    outVals[i] = vizzz::slewToward(outVals[i], target, slewStep);
  }
  memcpy(dmxFrame + 1, outVals, MAX_CH);
}

static void sendDMX() {
  dmx_write(dmxPort, dmxFrame, MAX_CH + 1);
  dmx_send(dmxPort);
  dmx_wait_sent(dmxPort, DMX_SEND_WAIT);
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

  for (uint8_t processed = 0; processed < ARTNET_PKT_BUDGET; processed++) {
    int size = artInUdp.parsePacket();
    if (size <= 0) break;
    int n = artInUdp.read(pkt, min(int(sizeof(pkt)), size));
    artInUdp.flush();
    if (n < 18) continue;
    if (isOwnIp(artInUdp.remoteIP())) continue;  // never merge our own Art-Net OUT back in
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

  for (uint8_t processed = 0; processed < SACN_PKT_BUDGET; processed++) {
    int size = sacnUdp.parsePacket();
    if (size <= 0) break;
    int n = sacnUdp.read(pkt, min(int(sizeof(pkt)), size));
    sacnUdp.flush();
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
.grid > *, .stack > *, .section > *, .card > *, .split > *, .triple > *, .sceneActions > *, .vjStrip > *, .sceneSlotGrid > *, .vjPadGrid > *, .vjFaders > *{min-width:0}
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
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:10px}
.stack{display:grid;gap:10px}.section{display:none}.section.active{display:grid;gap:10px}
.card{background:var(--di-surface);border:1px solid var(--di-cyan-border);border-radius:0;padding:10px}
.card h2{font:700 .82rem/1.1 var(--di-mono);letter-spacing:.05em;text-transform:uppercase;margin-bottom:10px;color:var(--di-text)}.card h2:before{content:"□ ";color:var(--di-cyan)}.meta{display:none}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.row+.row{margin-top:8px}
.grow{flex:1 1 150px}.split{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:8px}.triple{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:8px}
label{display:grid;gap:5px;font:700 .66rem/1 var(--di-mono);text-transform:uppercase;letter-spacing:.06em;color:var(--di-muted)}
input,select,button,textarea{border-radius:0;border:1px solid var(--di-cyan-border);min-height:44px;padding:10px;background:var(--di-black);color:var(--di-text);font:600 .9rem/1.2 "Inter","Segoe UI",sans-serif;outline:none}
input:focus,select:focus,textarea:focus{border-color:var(--di-cyan);box-shadow:0 0 0 1px var(--di-cyan)}
button{cursor:pointer;background:var(--di-cyan-dim);border-color:var(--di-cyan);color:var(--di-cyan);font-weight:800;text-transform:uppercase;letter-spacing:.02em;white-space:normal;word-break:break-word;overflow-wrap:anywhere;line-height:1.15;text-align:center}
button:hover,button.active{background:rgba(77,249,255,.16)}button.alt{background:var(--di-black);border-color:var(--di-cyan-border);color:var(--di-text)}button.alt.active{border-color:var(--di-cyan);color:var(--di-cyan);background:var(--di-cyan-dim)}button.good,button.bad{background:var(--di-cyan-dim);border-color:var(--di-cyan);color:var(--di-cyan)}
button.wide{width:100%}.actions{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}
.heroMeter{padding:12px;border-radius:0;background:var(--di-black);border:1px solid var(--di-cyan-border);color:var(--di-text);text-align:center}.heroMeter .big{font:700 2rem/1 var(--di-mono);color:var(--di-cyan)}
.sceneGrid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px}.sceneBtn{height:68px;font-size:.9rem}.miniScene{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px}
.sceneCoach{border:1px solid var(--di-cyan-border);background:var(--di-black);padding:10px;font:700 .72rem/1.35 var(--di-mono);color:var(--di-muted);text-transform:uppercase;letter-spacing:.03em}
.sceneBigRead{margin-top:8px;padding:12px;border:1px solid var(--di-cyan);background:var(--di-cyan-dim);font:800 1.4rem/1 var(--di-mono);color:var(--di-cyan);text-align:center}
.sceneSlotGrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(96px,1fr));gap:8px;margin-top:10px}.sceneSlotBtn{min-height:64px;font:800 1.05rem/1 var(--di-mono)}
.sceneActions{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:8px;margin-top:10px}
.patchHead{display:flex;justify-content:space-between;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:10px}.patchGrid{display:grid;grid-template-columns:minmax(260px,1.3fr) minmax(240px,.7fr);gap:10px}.stageCanvas{position:relative;min-height:320px;border:1px solid var(--di-cyan-border);background:radial-gradient(circle at 50% 50%, rgba(77,249,255,.08), transparent 45%), linear-gradient(90deg, rgba(77,249,255,.06) 1px, transparent 1px), linear-gradient(rgba(77,249,255,.06) 1px, transparent 1px);background-size:100% 100%, 25% 100%, 100% 25%;overflow:hidden}.fixtureDot{position:absolute;transform:translate(-50%,-50%);min-width:72px;min-height:42px;padding:4px 6px;border-radius:0;border:1px solid var(--di-cyan-border);background:rgba(0,0,0,.88);color:var(--di-text);font:700 .62rem/1 var(--di-mono);text-transform:uppercase;letter-spacing:.04em}.fixtureDot.active{border-color:var(--di-cyan);background:var(--di-cyan-dim);color:var(--di-cyan)}.fixtureList{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px;margin-top:10px}.fixtureCard{border:1px solid var(--di-cyan-border);background:var(--di-black);padding:8px;display:grid;gap:8px}.fixtureCard b{font:800 .75rem/1 var(--di-mono)}.fixtureCard span{font:600 .6rem/1.2 var(--di-mono);color:var(--di-muted)}.channels{display:grid;grid-template-columns:52px 1fr 38px 38px;gap:7px;align-items:center}
.chLabel{font:700 .68rem/1 var(--di-mono);color:var(--di-muted);letter-spacing:.02em}.outVal{font:700 .72rem/1 var(--di-mono);text-align:right;color:var(--di-muted)}.outVal.hot{color:var(--di-cyan)}.webVal{font:700 .72rem/1 var(--di-mono);text-align:right}
input[type=range]{width:100%;accent-color:var(--di-cyan)}
.nets{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:8px}.net{padding:9px;border-radius:0;border:1px solid var(--di-cyan-border);background:var(--di-black);cursor:pointer;font-size:.8rem;text-align:left;overflow:hidden;text-overflow:ellipsis}
pre{white-space:pre-wrap;word-break:break-word;background:var(--di-black);border:1px solid var(--di-cyan-border);border-radius:0;padding:10px;font:11px/1.4 var(--di-mono);max-height:260px;overflow:auto;color:var(--di-text)}
.footerNote{font:600 .7rem/1.35 var(--di-mono);color:var(--di-muted);letter-spacing:.02em}
.vjStatus{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:8px}.vjReadout{border:1px solid var(--di-cyan-border);background:var(--di-black);padding:10px;min-height:64px}.vjReadout b{display:block;font:800 1.25rem/1 var(--di-mono);color:var(--di-cyan)}.vjReadout span{display:block;margin-top:6px;font:700 .62rem/1 var(--di-mono);text-transform:uppercase;letter-spacing:.05em;color:var(--di-muted)}
.vjPadGrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(96px,1fr));gap:8px}.vjPad{min-height:66px;text-align:left;display:grid;align-content:center;gap:4px;touch-action:manipulation}.vjPad b{display:block;font:800 1.05rem/1 var(--di-mono);color:inherit}.vjPad span{display:block;font:700 .58rem/1 var(--di-mono);color:var(--di-muted);letter-spacing:.04em;text-transform:uppercase}.vjPad.active{background:var(--di-cyan-dim);border-color:var(--di-cyan);color:var(--di-cyan)}
.vjStrip{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:8px}.vjStrip button{min-height:54px}.vjFaders{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:8px}.groupTile{border:1px solid var(--di-cyan-border);background:var(--di-black);padding:8px;display:grid;gap:8px;min-width:0}.groupTile .top{display:flex;justify-content:space-between;gap:6px;align-items:flex-start}.groupTile b{font:800 .78rem/1 var(--di-mono);color:var(--di-text);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.groupTile span{font:700 .58rem/1 var(--di-mono);color:var(--di-muted)}.groupTile input{min-height:38px;padding:0}.groupTile .miniActions{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:6px}.groupTile button{min-height:34px;padding:6px;font-size:.64rem}
.swatches{display:grid;grid-template-columns:repeat(auto-fit,minmax(96px,1fr));gap:8px}.swatch{min-height:44px;border:1px solid var(--di-cyan-border);color:#fff;text-shadow:0 1px 1px #000;font:800 .62rem/1 var(--di-mono);text-transform:uppercase}.swatch.cyan{background:#00484d}.swatch.mag{background:#4a004d}.swatch.amber{background:#4d2a00}.swatch.red{background:#4d0000}.swatch.green{background:#004d18}.swatch.blue{background:#001f4d}.swatch.white{background:#eee;color:#000;text-shadow:none}.swatch.black{background:#000;color:var(--di-muted)}
@media (max-width:760px){body{font-size:13px}.shell{padding:6px}.hero{padding:8px;margin-bottom:8px}.brand{display:block}.pillbar{overflow-x:auto;flex-wrap:nowrap;padding-bottom:2px}.pill{flex:0 0 auto}.grid,.split,.triple,.vjStatus{grid-template-columns:1fr}.section.active,.stack{gap:8px}.card{padding:9px}.actions,.vjStrip,.sceneActions{grid-template-columns:1fr}.sceneGrid,.miniScene,.sceneSlotGrid,.vjPadGrid,.vjFaders,.swatches{grid-template-columns:repeat(2,minmax(0,1fr))}.channels{grid-template-columns:42px 1fr 32px 32px;gap:6px}.chLabel{font-size:.62rem}.footerNote{margin-top:6px}.heroMeter .big{font-size:1.7rem}.vjPad{min-height:62px}.vjReadout{min-height:54px}}
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
          <span class="pill" id="peersPill">0 peers</span>
        </div>
      </div>
      <div class="footerNote">AP <b id="apIp">192.168.4.1</b><br>Target <b id="aoTarget">-</b><br>mDNS <b id="mdnsLabel">-</b></div>
    </div>
    <nav class="tabs" id="tabs">
      <a class="tab" data-route="/control" href="/control">Control</a>
      <a class="tab" data-route="/patch" href="/patch">Patch</a>
      <a class="tab" data-route="/vj" href="/vj">Performance</a>
      <a class="tab" data-route="/network" href="/network">WiFi</a>
      <a class="tab" data-route="/system" href="/system">System</a>
      <a class="tab" data-route="/vj" href="/vj">VJ</a>
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
    <div class="grid">
      <div class="card">
        <div class="patchHead">
          <div>
            <h2>Stage Patch</h2>
            <div class="meta">Place fixtures on the stage, assign DMX ranges, and save positions for visual effects.</div>
          </div>
          <div class="row">
            <label>Selected<input id="fixtureSel" type="number" min="0" max="11" value="0"></label>
            <button class="alt" onclick="loadFixtures()">Refresh</button>
          </div>
        </div>
        <div class="stageCanvas" id="stageCanvas"></div>
        <div class="fixtureList" id="fixtureList"></div>
      </div>
      <div class="card">
        <h2>Fixture Editor</h2>
        <div class="split">
          <label>Name<input id="fxName" value="F1"></label>
          <label>Count<input id="fxCount" type="number" min="1" max="12" value="4"></label>
        </div>
        <div class="split" style="margin-top:8px">
          <label>Start Ch<input id="fxStart" type="number" min="1" max="512" value="1"></label>
          <label>End Ch<input id="fxEnd" type="number" min="1" max="512" value="3"></label>
        </div>
        <div class="split" style="margin-top:8px">
          <label>X<input id="fxX" type="range" min="0" max="255" value="32" oninput="$('fxXLabel').textContent=this.value"></label>
          <label>Y<input id="fxY" type="range" min="0" max="255" value="32" oninput="$('fxYLabel').textContent=this.value"></label>
        </div>
        <div class="row" style="margin-top:8px"><div class="footerNote">X <span id="fxXLabel">32</span> · Y <span id="fxYLabel">32</span></div></div>
        <div class="row" style="margin-top:8px"><label><input id="fxEn" type="checkbox" checked style="min-height:auto;align-self:center"> Enabled</label></div>
        <div class="vjStrip" style="margin-top:8px">
          <button onclick="saveFixture()">Save Fixture</button>
          <button class="alt" onclick="fixturePrev()">Prev</button>
          <button class="alt" onclick="fixtureNext()">Next</button>
        </div>
        <div class="vjStrip" style="margin-top:8px">
          <button class="alt" onclick="applyRadarPreset()">Radar Preset</button>
          <button class="alt" onclick="applyThunder6Mode()">THUNDER 6</button>
          <button class="alt" onclick="setMode(2)">HTP Mode</button>
          <button class="alt" onclick="setMode(0)">Web Mode</button>
        </div>
      </div>
    </div>
    <div class="card" style="margin-top:10px">
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
        <div class="card">
          <h2>WiFi Status</h2>
          <div id="wsAP" class="footerNote" style="margin-bottom:6px">AP: —</div>
          <div id="wsSTA" class="footerNote">STA: idle</div>
        </div>
        <div class="card">
          <h2>Peers</h2>
          <div class="row" style="margin-bottom:8px"><button onclick="loadPeers()">Refresh</button><span class="footerNote" id="peerCount" style="margin-left:8px">—</span></div>
          <div id="peerList"><span class="footerNote">Join a WiFi network to discover other vizzz.di nodes.</span></div>
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

  <section class="section" data-route="/vj">
    <div class="stack">
      <div class="card">
        <h2>VJ Deck</h2>
        <div class="vjStatus">
          <div class="vjReadout"><b id="vjMasterPct">100%</b><span>master</span></div>
          <div class="vjReadout"><b id="vjModeRead">HTP</b><span>output</span></div>
          <div class="vjReadout"><b id="vjFxRead">none</b><span>fx</span></div>
        </div>
        <div class="vjStrip" style="margin-top:8px">
          <button class="bad" onclick="vjPanic()">Panic</button>
          <button onclick="blackout()">Blackout</button>
          <button class="good" onclick="fullOn()">Full</button>
        </div>
        <div class="vjStrip" style="margin-top:8px">
          <button id="vjModeWeb" class="alt" onclick="setMode(0)">Web</button>
          <button id="vjModeHtp" class="alt" onclick="setMode(2)">HTP</button>
          <button id="vjModeArt" class="alt" onclick="setMode(1)">Art</button>
        </div>
        <div class="row" style="margin-top:8px">
          <label class="grow">Master<input id="perfMaster" type="range" min="0" max="255" value="255"></label>
          <div class="heroMeter"><div class="big" id="perfPct">100%</div><div>level</div></div>
        </div>
      </div>

      <div class="grid">
        <div class="card">
          <h2>Easy Scenes</h2>
          <div class="sceneCoach">1. Tap a scene number.<br>2. Press Save to remember your current lights.<br>3. Press Play to show that scene.</div>
          <div class="sceneBigRead" id="sceneSlotRead">Scene 1 ready</div>
          <div class="sceneSlotGrid" id="vjScenes"></div>
          <div class="row" style="margin-top:10px"><label class="grow">Scene Change Speed (ms)<input id="perfFade" type="number" value="800"></label></div>
          <div class="vjStrip" style="margin-top:8px">
            <button class="alt" onclick="setSceneFade(0)">Instant</button>
            <button class="alt" onclick="setSceneFade(800)">Soft</button>
            <button class="alt" onclick="setSceneFade(1500)">Slow</button>
          </div>
          <div class="sceneActions">
            <button onclick="sceneSaveSelected()">Save This Look</button>
            <button class="good" onclick="scenePlaySelected()">Play This Scene</button>
            <button class="alt" onclick="sceneSelectNext()">Next Scene</button>
            <button class="alt" onclick="sceneSaveAndNext()">Save + Next</button>
          </div>
          <div class="vjStrip" style="margin-top:8px">
            <button class="alt" onclick="applyThunder6Mode()">THUNDER 6 Electro Mode</button>
            <button class="alt" id="ugModeBtn" onclick="toggleUndergroundMode()">Underground Colors ON</button>
            <button class="alt" onclick="applyUndergroundLook()">Apply Underground Look</button>
          </div>
          <div class="footerNote" id="sceneActionText" style="margin-top:8px">Tip: build scenes from 1 to 8.</div>
        </div>

        <div class="card">
          <h2>FX Engine</h2>
          <select id="fxModeSel" style="display:none"><option value="none">none</option><option value="strobe">strobe</option><option value="chase">chase</option><option value="pulse">pulse</option><option value="sine">sine</option><option value="sparkle">sparkle</option><option value="comet">comet</option><option value="bars">bars</option><option value="glitch">glitch</option><option value="radar">radar</option></select>
          <div class="vjPadGrid">
            <button class="vjPad" data-fx="none" onclick="fxMode('none')"><b>OFF</b><span>fx</span></button>
            <button class="vjPad" data-fx="strobe" onclick="fxMode('strobe')"><b>STRB</b><span>fx</span></button>
            <button class="vjPad" data-fx="chase" onclick="fxMode('chase')"><b>CHAS</b><span>fx</span></button>
            <button class="vjPad" data-fx="pulse" onclick="fxMode('pulse')"><b>PULS</b><span>fx</span></button>
            <button class="vjPad" data-fx="sine" onclick="fxMode('sine')"><b>SINE</b><span>fx</span></button>
            <button class="vjPad" data-fx="sparkle" onclick="fxMode('sparkle')"><b>SPRK</b><span>fx</span></button>
            <button class="vjPad" data-fx="comet" onclick="fxMode('comet')"><b>COMT</b><span>fx</span></button>
            <button class="vjPad" data-fx="bars" onclick="fxMode('bars')"><b>BARS</b><span>fx</span></button>
            <button class="vjPad" data-fx="glitch" onclick="fxMode('glitch')"><b>GLCH</b><span>fx</span></button>
            <button class="vjPad" data-fx="radar" onclick="fxMode('radar')"><b>RADR</b><span>fx</span></button>
            <button class="vjPad" onclick="bpmStep(.5)"><b>1/2</b><span>bpm</span></button>
            <button class="vjPad" onclick="bpmStep(2)"><b>2X</b><span>bpm</span></button>
          </div>
          <div class="split" style="margin-top:10px">
            <label>BPM<input id="fxBpm" type="range" min="20" max="240" value="120" oninput="setBpmLabel(+this.value)"></label>
            <label>Depth<input id="fxDepth" type="range" min="0" max="255" value="255" oninput="setDepthLabel(+this.value)"></label>
          </div>
          <div class="vjStrip" style="margin-top:8px">
            <button onclick="applyFx()">Apply</button>
            <button class="alt" onclick="tapFx()">Tap</button>
            <button class="alt" onclick="killFx()">Kill FX</button>
          </div>
          <div class="footerNote" style="margin-top:8px"><span id="bpmLabel">120 bpm</span> · <span id="depthLabel">depth 255</span></div>
        </div>
      </div>

      <div class="grid">
        <div class="card">
          <h2>Color Engine</h2>
          <div class="swatches">
            <button class="swatch white" onclick="colorPreset('white')">White</button>
            <button class="swatch amber" onclick="colorPreset('amber')">Amber</button>
            <button class="swatch red" onclick="colorPreset('red')">Red</button>
            <button class="swatch green" onclick="colorPreset('green')">Green</button>
            <button class="swatch blue" onclick="colorPreset('blue')">Blue</button>
            <button class="swatch cyan" onclick="colorPreset('cyan')">Cyan</button>
            <button class="swatch mag" onclick="colorPreset('magenta')">Mag</button>
            <button class="swatch black" onclick="killColor()">Off</button>
          </div>
          <div class="triple" style="margin-top:10px">
            <label>R<input id="colorR" type="range" min="0" max="255" value="255" oninput="queueColor()"></label>
            <label>G<input id="colorG" type="range" min="0" max="255" value="255" oninput="queueColor()"></label>
            <label>B<input id="colorB" type="range" min="0" max="255" value="255" oninput="queueColor()"></label>
          </div>
          <div class="vjStrip" style="margin-top:8px">
            <button onclick="applyColor()">Apply</button>
            <button class="alt" onclick="$('colorEn').checked=true;applyColor()">Color On</button>
            <button class="alt" onclick="killColor()">Color Off</button>
          </div>
          <input id="colorEn" type="checkbox" checked style="display:none">
          <div class="footerNote" id="colorLabel" style="margin-top:8px">rgb 255 / 255 / 255</div>
        </div>

        <div class="card">
          <h2>Cue Runner</h2>
          <div class="split">
            <label>Cue Count<input id="cueCount" type="number" min="1" max="16" value="4"></label>
            <label>Cue Step<input id="cueStep" type="number" min="0" max="15" value="0"></label>
          </div>
          <div class="split" style="margin-top:8px">
            <label>Scene<input id="cueScene" type="number" min="0" max="7" value="0"></label>
            <label>Dwell ms<input id="cueDwell" type="number" min="0" value="1500"></label>
          </div>
          <div class="row" style="margin-top:8px"><label class="grow">Fade ms<input id="cueFade" type="number" min="0" value="500"></label></div>
          <div class="vjStrip" style="margin-top:8px"><button onclick="setCueStep()">Save</button><button onclick="cueRun(1)">Run</button><button class="alt" onclick="cueNext()">Next</button></div>
          <div class="vjStrip" style="margin-top:8px"><button class="alt" onclick="cueRun(0)">Stop</button><button class="alt" onclick="cueBuildBasic()">Build 1-4</button><button class="alt" onclick="cueBuildAll()">Build 1-8</button></div>
          <pre id="vjDump" style="margin-top:8px">VJ status pending…</pre>
        </div>
      </div>

      <div class="card">
        <h2>Group Mixer</h2>
        <div class="vjFaders" id="groupDeck"></div>
        <div class="split" style="margin-top:10px">
          <label>Group<input id="grpId" type="number" min="0" max="7" value="0"></label>
          <label>Name<input id="grpName" value="G1"></label>
        </div>
        <div class="split" style="margin-top:8px">
          <label>Start Ch<input id="grpStart" type="number" min="1" max="512" value="1"></label>
          <label>End Ch<input id="grpEnd" type="number" min="1" max="512" value="64"></label>
        </div>
        <div class="row" style="margin-top:8px"><label class="grow">Value<input id="grpValue" type="range" min="0" max="255" value="180"></label></div>
        <div class="vjStrip" style="margin-top:8px"><button onclick="setGroup()">Save Group</button><button class="alt" onclick="applyGroup()">Apply</button><button class="alt" onclick="groupAllOff()">All Groups 0</button></div>
      </div>

      <div class="card">
        <h2>Fleet</h2>
        <div id="fleetList" class="footerNote" style="margin-bottom:10px">—</div>
        <div class="vjStrip">
          <button class="bad" onclick="netBlackout()">ALL Blackout</button>
          <button class="good" onclick="netFull()">ALL Full</button>
          <button class="alt" onclick="loadFleet()">Refresh</button>
        </div>
        <div class="row" style="margin-top:10px">
          <label class="grow">ALL Master<input id="netMaster" type="range" min="0" max="255" value="255" oninput="queueNetMaster(+this.value)"></label>
          <div class="heroMeter"><div class="big" id="netMasterPct">100%</div><div>fleet</div></div>
        </div>
        <div class="row" style="margin-top:8px"><label class="grow">Fade ms<input id="netFade" type="number" value="1000"></label></div>
        <div class="sceneGrid" id="netScenes" style="margin-top:8px"></div>
      </div>
    </div>
  </section>
</div>

<script>
const $=id=>document.getElementById(id);
const route=(location.pathname==='/performance'||location.pathname==='/scenes')?'/vj':(location.pathname==='/'?'/control':location.pathname);
let state=null,pageWeb=[],pageOut=[],curPage=0,masterTimer=null,scanTimer=null,artOutEnabled=false,webEnabled=true,vjTimer=null,colorTimer=null,vjGroups=[],groupTimers={},groupSig='',pagePollTimer=null,fleetTimer=null,fixtureTimer=null,fixtures=[],fixtureSel=0;
let quickSceneSlot=0;
let undergroundMode=true;

function escHtml(s){return String(s||'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
function escJsSq(s){return String(s||'').replace(/\\/g,'\\\\').replace(/'/g,"\\'").replace(/\n/g,'\\n').replace(/\r/g,'\\r');}
function qs(url){return fetch(url,{cache:'no-store'}).then(r=>r.json()).catch(()=>null);}
function hit(url){return fetch(url,{cache:'no-store'});}
function hitWait(url){return fetch(url,{cache:'no-store'});}
function setActiveRoute(){document.querySelectorAll('.section').forEach(x=>x.classList.toggle('active',x.dataset.route===route));document.querySelectorAll('.tab').forEach(x=>x.classList.toggle('active',x.dataset.route===route));}
function makeScenes(){
  $('vjScenes').innerHTML=[...Array(8)].map((_,i)=>`<button class="sceneSlotBtn" data-scene-slot="${i}" onclick="scenePick(${i})">${i+1}</button>`).join('');
  $('netScenes').innerHTML=[...Array(8)].map((_,i)=>`<button class="sceneBtn" onclick="netRecall(${i})">Scene ${i+1}</button>`).join('');
  renderQuickSceneState();
}
function makePages(){ $('pageSel').innerHTML=[...Array(16)].map((_,i)=>`<option value="${i}">Page ${i+1}</option>`).join(''); }
function clampFixtureIndex(i){return Math.max(0,Math.min(11,i|0));}
function fixtureRow(f,i){return `<div class="fixtureCard${i===fixtureSel?' active':''}" onclick="selectFixture(${i})"><b>${escHtml(f.name||('F'+(i+1)))}</b><span>Ch ${f.start}-${f.end} · X ${f.x} · Y ${f.y}</span></div>`;}
function syncFixtureEditor(){const f=fixtures[fixtureSel]||{name:'F'+(fixtureSel+1),start:1,end:3,x:32,y:32,enabled:true}; $('fixtureSel').value=fixtureSel; $('fxName').value=f.name||('F'+(fixtureSel+1)); $('fxCount').value=Math.max(1,Math.min(12,fixtures.length||4)); $('fxStart').value=f.start||1; $('fxEnd').value=f.end||1; $('fxX').value=f.x??32; $('fxY').value=f.y??32; $('fxXLabel').textContent=String(f.x??32); $('fxYLabel').textContent=String(f.y??32); $('fxEn').checked=!!f.enabled;}
function renderFixtures(){const stage=$('stageCanvas'); const list=$('fixtureList'); if(!stage||!list) return; const count=Math.max(1,Math.min(12,fixtures.length||4)); stage.innerHTML=fixtures.slice(0,count).map((f,i)=>`<button class="fixtureDot${i===fixtureSel?' active':''}" style="left:${(Math.max(0,Math.min(255,+f.x||0))*100/255).toFixed(1)}%;top:${(Math.max(0,Math.min(255,+f.y||0))*100/255).toFixed(1)}%" onclick="selectFixture(${i})">${escHtml(f.name||('F'+(i+1)))}<br><span>Ch ${f.start}-${f.end}</span></button>`).join('')||'<div class="footerNote" style="padding:10px">No fixtures loaded</div>'; list.innerHTML=fixtures.slice(0,count).map((f,i)=>fixtureRow(f,i)).join('');}
function selectFixture(i){fixtureSel=clampFixtureIndex(i); syncFixtureEditor(); renderFixtures();}
function fixturePrev(){selectFixture(fixtureSel-1);}
function fixtureNext(){selectFixture(fixtureSel+1);}
function applyRadarPreset(){fxMode('radar'); setMode(2);}
function setUndergroundModeUI(){
  if($('ugModeBtn')) $('ugModeBtn').textContent='Underground Colors '+(undergroundMode?'ON':'OFF');
}
function toggleUndergroundMode(){
  undergroundMode=!undergroundMode;
  setUndergroundModeUI();
  if($('sceneActionText')) $('sceneActionText').textContent='Underground color guard is '+(undergroundMode?'ON':'OFF')+'.';
}
async function applyThunder6Mode(){
  const setup=[
    {name:'TW1',start:1,end:3,x:24,y:42,en:1},
    {name:'TW2',start:4,end:6,x:56,y:42,en:1},
    {name:'TW3',start:7,end:9,x:88,y:42,en:1},
    {name:'TW4',start:10,end:12,x:24,y:76,en:1},
    {name:'TW5',start:13,end:15,x:56,y:76,en:1},
    {name:'TW6',start:16,end:18,x:88,y:76,en:1}
  ];
  for(let i=0;i<setup.length;i++){
    const f=setup[i];
    const q=new URLSearchParams({i:String(i),count:'6',name:f.name,start:String(f.start),end:String(f.end),x:String(f.x),y:String(f.y),en:String(f.en)});
    await hitWait('/fixture/set?'+q.toString());
  }
  await loadFixtures();
  undergroundMode=true;
  setUndergroundModeUI();
  setMode(2);
  $('fxBpm').value=124;
  $('fxDepth').value=150;
  fxMode('radar');
  applyUndergroundLook();
  if($('sceneActionText')) $('sceneActionText').textContent='THUNDER 6 mode ready: 6 fixtures patched for dark electro.';
}
async function loadFixtures(){const r=await qs('/fixtures'); if(!r) return; fixtures=r.fixtures||[]; if(fixtures.length===0) fixtures=[{name:'F1',start:1,end:3,x:32,y:32,enabled:true}]; fixtureSel=clampFixtureIndex(Math.min(fixtureSel, fixtures.length-1)); syncFixtureEditor(); renderFixtures();}
function saveFixture(){const i=clampFixtureIndex($('fixtureSel').value); const p=new URLSearchParams(); p.append('i',i); p.append('name',$('fxName').value.trim()||('F'+(i+1))); p.append('count',String(Math.max(1,Math.min(12,+$('fxCount').value||fixtures.length||4)))); p.append('start',String(Math.max(1,+$('fxStart').value||1))); p.append('end',String(Math.max(1,+$('fxEnd').value||1))); p.append('x',String(Math.max(0,Math.min(255,+$('fxX').value||0)))); p.append('y',String(Math.max(0,Math.min(255,+$('fxY').value||0)))); p.append('en',$('fxEn').checked?1:0); hit('/fixture/set?'+p.toString()).then(()=>loadFixtures());}
function updateMasterDisplays(v){const pct=Math.round((v/255)*100);$('masterPct').textContent=pct+'%';$('perfPct').textContent=pct+'%';$('vjMasterPct').textContent=pct+'%';if(document.activeElement!==$('master'))$('master').value=v;if(document.activeElement!==$('perfMaster'))$('perfMaster').value=v;}
function updatePills(s){
  $('apIp').textContent=s.ip||'-'; $('aoTarget').textContent=s.ao_target||'-'; if(s.mdns) $('mdnsLabel').textContent=s.mdns+'.local';
  const set=(id,text,on,hot)=>{const el=$(id);el.textContent=text;el.className='pill'+(hot?' hot':on?' on':'');};
  set('modePill',s.mode_name,false,true); set('netModePill',s.net_mode_name,false,false); set('artPill','ART '+(s.artnet_active?'active':'idle'),s.artnet_active,false); set('sacnPill','sACN '+(s.sacn_active?'active':'idle'),s.sacn_active,false); set('aoPill','OUT '+(s.ao?'on':'off'),s.ao,s.ao); set('webPill','WEB '+(s.web?'on':'off'),s.web,false);
  const wls=s.wl_status; const staLabel=s.sta_connected?('STA '+s.sta_ip):s.sta_ssid?(wls===4?'STA auth fail':wls===1?'STA no SSID':'STA joining'):'STA idle'; const staErr=s.sta_ssid&&!s.sta_connected&&(wls===4||wls===1); $('staPill').textContent=staLabel; $('staPill').className='pill'+(s.sta_connected?' on':staErr?' warn':s.sta_ssid?' warn':'');
  const pc=s.peer_count||0; set('peersPill',pc+(pc===1?' peer':' peers'),pc>0,false);
}
function syncFields(s){
  $('modeSel').value=String(s.mode); $('netModeSel').value=String(s.net_mode); $('netInput').value=s.net; $('subInput').value=s.subnet; $('uniInput').value=s.uni; $('uni15').textContent='15-bit: '+s.uni15;
  if(route==='/control') $('statusDump').textContent=JSON.stringify(s,null,2);
  if(route==='/system') $('diagDump').textContent=`mode=${s.mode_name}\nnetwork=${s.net_mode_name}\nartnet=${s.artnet_active}\nsacn=${s.sacn_active}\nweb=${s.web}\nout_target=${s.ao_target}`;
  artOutEnabled=!!s.ao; webEnabled=!!s.web; $('aoBtn').textContent='Art OUT '+(artOutEnabled?'ON':'OFF'); $('webBtn').textContent='Web '+(webEnabled?'ON':'OFF');
  if(!$('staSsid').value && s.sta_ssid) $('staSsid').placeholder=s.sta_ssid; if(!$('nodeName').value) $('nodeName').placeholder=s.name; if(!$('apSsid').value) $('apSsid').placeholder=s.ssid; updateMasterDisplays(s.dim||255);
  if(route==='/vj'){
    $('vjModeRead').textContent=(s.mode_name||'').replace('_ONLY',''); $('vjFxRead').textContent=(document.querySelector('[data-fx].active')?.dataset.fx)||'none';
    $('vjModeWeb').classList.toggle('active',s.mode===0); $('vjModeArt').classList.toggle('active',s.mode===1); $('vjModeHtp').classList.toggle('active',s.mode===2);
  }
  const rssiLabel=s.sta_connected&&s.sta_rssi?(' \u00b7 '+s.sta_rssi+'dBm'):'';
  if(route==='/network'){
    if($('wsAP'))$('wsAP').textContent='AP: '+escHtml(s.ssid||'?')+' \u00b7 '+s.ip+' \u00b7 '+(s.ap_clients||0)+' client'+(s.ap_clients===1?'':'s');
    if($('wsSTA'))$('wsSTA').textContent=s.sta_connected?('STA \u2713 '+(s.sta_ssid||'')+' \u00b7 '+s.sta_ip+rssiLabel):s.sta_ssid?('STA \u2192 '+(s.sta_ssid||'')+'\u2026'):'STA idle';
  }
}
function renderChannels(base){$('channels').innerHTML=[...Array(32)].map((_,i)=>{const ch=base+i,w=pageWeb[i]??0,o=pageOut[i]??0;return `<div class="chLabel">Ch ${ch}</div><input type="range" min="0" max="255" value="${w}" oninput="setChannel(${ch},+this.value);this.nextElementSibling.textContent=this.value"><div class="webVal">${w}</div><div class="outVal${o>w?' hot':''}">${o}</div>`;}).join('');}
async function loadPage(){const s=await qs('/page?i='+curPage);if(!s)return;pageWeb=s.web||[];pageOut=s.out||[];$('pageInfo').textContent='ch '+s.base_ch+'-'+(s.base_ch+31);renderChannels(s.base_ch);}
function refreshOutGrid(){const rows=$('channels'); if(!rows) return; rows.querySelectorAll('input[type=range]').forEach((sl,i)=>{sl.value=pageWeb[i]??0; sl.nextElementSibling.textContent=pageWeb[i]??0; const out=sl.nextElementSibling.nextElementSibling; const o=pageOut[i]??0; out.textContent=o; out.className='outVal'+(o>(pageWeb[i]??0)?' hot':'');});}
function setChannel(ch,v){hit('/set?ch='+ch+'&v='+v);}
function blackout(){hit('/blackout');}
function fullOn(){hit('/full');}
function queueMaster(v){updateMasterDisplays(v); clearTimeout(masterTimer); masterTimer=setTimeout(()=>hit('/master?v='+v),40);}
function recallScene(i,fade){hit('/scene/recall?n='+i+'&fade='+(+fade||0));}
function saveScene(i){hit('/scene/save?n='+i);}
function renderQuickSceneState(){
  document.querySelectorAll('[data-scene-slot]').forEach(b=>{
    const idx=+(b.dataset.sceneSlot||0);
    b.classList.toggle('active',idx===quickSceneSlot);
  });
  if($('sceneSlotRead')) $('sceneSlotRead').textContent='Scene '+(quickSceneSlot+1)+' ready';
}
function selectQuickScene(i){quickSceneSlot=Math.max(0,Math.min(7,i|0));renderQuickSceneState();}
function scenePick(i){selectQuickScene(i);if($('sceneActionText')) $('sceneActionText').textContent='Scene '+(i+1)+' selected. Press Play to use it.';}
function setSceneFade(ms){$('perfFade').value=ms; if($('sceneActionText')) $('sceneActionText').textContent='Scene change speed set to '+ms+' ms.';}
function scenePlaySelected(){recallScene(quickSceneSlot,+($('perfFade').value||0)); if($('sceneActionText')) $('sceneActionText').textContent='Playing scene '+(quickSceneSlot+1)+'.';}
function sceneSaveSelected(){saveScene(quickSceneSlot); if($('sceneActionText')) $('sceneActionText').textContent='Saved current lights to scene '+(quickSceneSlot+1)+'.';}
function sceneSelectNext(){selectQuickScene((quickSceneSlot+1)%8); if($('sceneActionText')) $('sceneActionText').textContent='Moved to scene '+(quickSceneSlot+1)+'.';}
function sceneSaveAndNext(){sceneSaveSelected(); sceneSelectNext();}
function saveUniverse(){hit(`/artnet/set?net=${$('netInput').value}&subnet=${$('subInput').value}&uni=${$('uniInput').value}`);}
function toggleArtOut(){artOutEnabled=!artOutEnabled; hit('/artout/set?en='+(artOutEnabled?1:0)); $('aoBtn').textContent='Art OUT '+(artOutEnabled?'ON':'OFF');}
function toggleWeb(){const next=webEnabled?0:1; const msg=next?'Re-enable the web console and reboot?':'Turn the web console OFF and reboot?\n\nAfter rebooting you get 90 seconds to reach this page again before it goes quiet. DMX, Art-Net and OSC keep running.'; if(confirm(msg)) hit('/web/set?en='+next);}
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
function peerCmd(ip,path){hit('/peer/cmd?ip='+encodeURIComponent(ip)+'&path='+encodeURIComponent(path));}
async function loadPeers(){const r=await qs('/peers');if(!r){$('peerCount').textContent='error';return;}const c=r.count||0;$('peerCount').textContent=c?c+(c===1?' peer found':' peers found'):'none found';$('peerList').innerHTML=c?r.peers.map(p=>`<div style="padding:8px;border:1px solid var(--di-cyan-border);margin-bottom:6px"><div style="display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:6px"><div><b>${escHtml(p.name)}</b> <span class="footerNote">${escHtml(p.ip)}</span><br><span class="footerNote">${escHtml(p.mdns)||''} &bull; <span style="font-family:monospace;font-size:.65rem">${escHtml(p.mac)||''}</span> &bull; ${p.age_s}s ago</span></div><button onclick="window.open('http://${escJsSq(p.ip)}','_blank')" style="min-height:28px;padding:3px 10px;font-size:.7rem;flex:0 0 auto">Open UI</button></div><div style="display:grid;grid-template-columns:repeat(3,1fr);gap:6px"><button onclick="peerCmd('${escJsSq(p.ip)}','/blackout')" style="min-height:32px;font-size:.72rem" class="bad">Blackout</button><button onclick="peerCmd('${escJsSq(p.ip)}','/full')" style="min-height:32px;font-size:.72rem" class="good">Full</button><button onclick="targetPeer('${escJsSq(p.ip)}')" style="min-height:32px;font-size:.72rem">Link AO</button></div></div>`).join(''):'<span class="footerNote">No vizzz.di nodes found. Devices must share a WiFi network. Retry after 30s.</span>';}
function targetPeer(ip){if(!confirm('Route Art-Net OUT directly to '+ip+'? (enables unicast, overrides broadcast)'))return;fetch('/artout/peer?ip='+encodeURIComponent(ip),{cache:'no-store'}).then(()=>fetch('/artout/set?en=1',{cache:'no-store'}));artOutEnabled=true;$('aoBtn').textContent='Art OUT ON';$('aoTarget').textContent=ip;}
function setMode(m){$('modeSel').value=String(m);hit('/mode/set?m='+m);}
function vjPanic(){hit('/panic');updateMasterDisplays(255);setTimeout(refreshVj,300);}
function setBpmLabel(v){$('bpmLabel').textContent=v+' bpm';}
function setDepthLabel(v){$('depthLabel').textContent='depth '+v;}
function markFx(mode){document.querySelectorAll('[data-fx]').forEach(b=>b.classList.toggle('active',b.dataset.fx===mode));$('vjFxRead').textContent=mode||'none';}
function fxMode(mode){$('fxModeSel').value=mode;markFx(mode);applyFx();}
function applyFx(){const mode=$('fxModeSel').value;markFx(mode);hit('/fx/set?en='+(mode==='none'?0:1)+'&mode='+encodeURIComponent(mode)+'&bpm='+(+$('fxBpm').value||120)+'&depth='+(+$('fxDepth').value||255));}
function tapFx(){hit('/fx/tap');}
function bpmStep(mult){const v=Math.max(20,Math.min(240,Math.round((+$('fxBpm').value||120)*mult)));$('fxBpm').value=v;setBpmLabel(v);applyFx();}
function killFx(){$('fxModeSel').value='none';markFx('none');hit('/fx/set?en=0&mode=none');}
function colorLabel(){const r=+$('colorR').value||0,g=+$('colorG').value||0,b=+$('colorB').value||0;$('colorLabel').textContent='rgb '+r+' / '+g+' / '+b;}
function queueColor(){colorLabel();$('colorEn').checked=true;clearTimeout(colorTimer);colorTimer=setTimeout(applyColor,55);}
function applyColor(){colorLabel();hit('/color/set?en='+(($('colorEn').checked)?1:0)+'&r='+(+$('colorR').value||0)+'&g='+(+$('colorG').value||0)+'&b='+(+$('colorB').value||0));}
function applyUndergroundLook(){
  $('colorR').value=100; $('colorG').value=70; $('colorB').value=48;
  $('colorEn').checked=true;
  applyColor();
}
function colorPreset(name){
  const normal={white:[255,255,255],amber:[255,140,20],red:[255,0,0],green:[0,255,70],blue:[0,70,255],cyan:[0,220,255],magenta:[255,0,220]};
  const underground={white:[180,180,170],amber:[120,80,30],red:[120,10,10],green:[10,70,20],blue:[12,32,85],cyan:[20,70,70],magenta:[75,15,65]};
  const p=((undergroundMode?underground:normal)[name])||[180,180,170];
  $('colorR').value=p[0]; $('colorG').value=p[1]; $('colorB').value=p[2]; $('colorEn').checked=true;
  applyColor();
}
function killColor(){$('colorEn').checked=false;hit('/color/set?en=0');}
function cueRun(en){hit('/cue/run?en='+en);}
function cueNext(){hit('/cue/next');}
function setCueStep(){
  const i=+$('cueStep').value||0;
  const q=+$('cueCount').value||4;
  hit('/cue/count?c='+q);
  hit('/cue/set?i='+i+'&scene='+(+$('cueScene').value||0)+'&dwell='+(+$('cueDwell').value||0)+'&fade='+(+$('cueFade').value||0));
}
function cueBuildBasic(){hit('/cue/count?c=4');for(let i=0;i<4;i++)hit('/cue/set?i='+i+'&scene='+i+'&dwell=1500&fade=500');}
function cueBuildAll(){hit('/cue/count?c=8');for(let i=0;i<8;i++)hit('/cue/set?i='+i+'&scene='+i+'&dwell=1500&fade=500');}
function setGroup(){
  const g=+$('grpId').value||0;
  hit('/group/set?g='+g+'&name='+encodeURIComponent($('grpName').value||('G'+(g+1)))+'&start='+(+$('grpStart').value||1)+'&end='+(+$('grpEnd').value||1)+'&en=1');
}
function applyGroup(){
  const g=+$('grpId').value||0;
  const v=+$('grpValue').value||0;
  hit('/group/apply?g='+g+'&v='+v);
}
function groupValue(g,v){const el=$('gv'+g);if(el)el.textContent=v;clearTimeout(groupTimers[g]);groupTimers[g]=setTimeout(()=>hit('/group/apply?g='+g+'&v='+v),35);}
function groupBump(g,v){const sl=$('gf'+g);if(sl)sl.value=v;groupValue(g,v);}
function groupEdit(g){const x=vjGroups.find(a=>a.id===g);if(!x)return;$('grpId').value=x.id;$('grpName').value=x.name;$('grpStart').value=x.start;$('grpEnd').value=x.end;}
function groupAllOff(){for(let g=0;g<8;g++)groupBump(g,0);}
function renderGroupDeck(groups){
  vjGroups=groups||[];
  const sig=vjGroups.map(g=>[g.id,g.name,g.start,g.end,g.enabled].join(':')).join('|');
  if(sig===groupSig) return;
  groupSig=sig;
  $('groupDeck').innerHTML=vjGroups.map(g=>`<div class="groupTile"><div class="top"><b>${escHtml(g.name)}</b><span>${g.start}-${g.end}</span></div><input id="gf${g.id}" type="range" min="0" max="255" value="0" oninput="groupValue(${g.id},+this.value)"><div class="top"><span id="gv${g.id}">0</span><button class="alt" onclick="groupEdit(${g.id})">Edit</button></div><div class="miniActions"><button onclick="groupBump(${g.id},255)">Bump</button><button class="alt" onclick="groupBump(${g.id},0)">Out</button></div></div>`).join('');
}
async function refreshVj(){
  const [fx,cue,groups]=await Promise.all([qs('/fx/status'),qs('/cue/status'),qs('/groups')]);
  if(!fx||!cue||!groups) return;
  $('fxModeSel').value=fx.mode_name||'none'; $('fxBpm').value=fx.bpm||120; $('fxDepth').value=fx.depth||255;
  setBpmLabel(fx.bpm||120); setDepthLabel(fx.depth||255); markFx(fx.mode_name||'none');
  $('colorR').value=fx.r??255; $('colorG').value=fx.g??255; $('colorB').value=fx.b??255; $('colorEn').checked=!!fx.color_en;
  colorLabel();
  $('cueCount').value=cue.count||1;
  renderGroupDeck(groups.groups||[]);
  $('vjDump').textContent=JSON.stringify({fx, cue},null,2);
}
function applyMode(){hit('/mode/set?m='+$('modeSel').value);} function applyNetMode(){if(confirm('Switch network profile and reboot?')) hit('/netmode/set?m='+$('netModeSel').value);}
let netMasterTimer=null;
function netBlackout(){hit('/net/blackout');}
function netFull(){hit('/net/full');}
function queueNetMaster(v){$('netMasterPct').textContent=Math.round(v/255*100)+'%';clearTimeout(netMasterTimer);netMasterTimer=setTimeout(()=>hit('/net/master?v='+v),40);}
function netRecall(i){hit('/net/scene/recall?n='+i+'&fade='+(+$('netFade').value||0));}
async function loadFleet(){const r=await qs('/peers');if(!r)return;const c=r.count||0;$('fleetList').innerHTML=c?r.peers.map(p=>`<b>${escHtml(p.name)}</b> · ${escHtml(p.ip)} · ${p.age_s}s ago`).join('<br>'):'This node only — no peers on network';}

function initRouteWork(){
  if(route==='/patch'){
    loadFixtures();
    loadPage();
    fixtureTimer=setInterval(loadFixtures,4000);
    pagePollTimer=setInterval(async()=>{const s=await qs('/page?i='+curPage); if(!s) return; pageWeb=s.web||[]; pageOut=s.out||[]; refreshOutGrid();},1000);
    return;
  }
  if(route==='/system'){
    setTimeout(()=>{refreshMonitor(); loadManifest();},30);
    return;
  }
  if(route==='/network'){
    setTimeout(loadPeers,30);
    return;
  }
  if(route==='/vj'){
    refreshVj();
    vjTimer=setInterval(refreshVj,2000);
    loadFleet();
    fleetTimer=setInterval(loadFleet,5000);
  }
}

$('modeSel').addEventListener('change',applyMode); $('netModeSel').addEventListener('change',applyNetMode); $('master').addEventListener('input',e=>queueMaster(+e.target.value)); $('perfMaster').addEventListener('input',e=>queueMaster(+e.target.value)); $('pageSel').addEventListener('change',e=>{curPage=+e.target.value;loadPage();});

const ws=new WebSocket('ws://'+location.host+'/ws');
ws.onmessage=e=>{let s; try{s=JSON.parse(e.data);}catch{return;} state=s; updatePills(s); syncFields(s);};
ws.onclose=()=>setTimeout(()=>location.reload(),2000);

setActiveRoute(); makeScenes(); makePages();
setUndergroundModeUI();
initRouteWork();
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
  uint8_t apClients = WiFi.softAPgetStationNum();
  int32_t staRssi = staCon ? WiFi.RSSI() : 0;
  uint8_t snapPeers = 0;
  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(2)) == pdTRUE) {
    snapPeers = peerCount;
    xSemaphoreGive(gLock);
  }

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
    "\"ao_target\":\"%s\","
    "\"ap_clients\":%u,"
    "\"sta_rssi\":%d,"
    "\"mac\":\"%s\","
    "\"peer_count\":%u,"
    "\"burn_safe\":%s,"
    "\"artnet_fallback_to_web\":%s,"
    "\"safe_ceiling\":%u,"
    "\"safe_slew_step\":%u"
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
    outTarget,
    apClients,
    staRssi,
    WiFi.macAddress().c_str(),
    snapPeers,
    burnSafeMode ? "true" : "false",
    artnetFallbackToWeb ? "true" : "false",
    effectiveSafeMaxLevel(),
    effectiveSafeSlewStep()
  );
  if (written < 0) {
    buf[0] = '\0';
    return 0;
  }
  if (size_t(written) >= n) return n - 1;
  return size_t(written);
}

static String statusJSON() {
  char buf[800];
  fillStatusJSON(buf, sizeof(buf));
  return buf;
}

static String peersJSON() {
  char buf[1024];
  uint32_t now = millis();
  int pos = 0;
  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) == pdTRUE) {
    jsonAppend(buf, sizeof(buf), pos, "{\"count\":%u,\"peers\":[", peerCount);
    for (uint8_t i = 0; i < peerCount; i++) {
      if (i) jsonAppend(buf, sizeof(buf), pos, ",");
      char eName[44], eMdns[52];
      jsonEsc(eName, sizeof(eName), peers[i].name);
      jsonEsc(eMdns, sizeof(eMdns), peers[i].mdns);
      char eMac[20]; jsonEsc(eMac, sizeof(eMac), peers[i].mac);
      jsonAppend(buf, sizeof(buf), pos,
        "{\"name\":\"%s\",\"ip\":\"%s\",\"mdns\":\"%s\",\"mac\":\"%s\",\"age_s\":%lu}",
        eName, peers[i].ip, eMdns, eMac,
        (unsigned long)((now - peers[i].lastSeenMs) / 1000));
    }
    xSemaphoreGive(gLock);
  } else {
    jsonAppend(buf, sizeof(buf), pos, "{\"count\":0,\"peers\":[");
  }
  jsonAppend(buf, sizeof(buf), pos, "]}");
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
  int pos = 0;
  jsonAppend(buf, sizeof(buf), pos,
    "{\"page\":%u,\"base_ch\":%u,\"web\":[", page, base + 1);
  for (int i = 0; i < PAGE_SIZE; i++)
    jsonAppend(buf, sizeof(buf), pos, i < PAGE_SIZE-1 ? "%u," : "%u", snapWeb[i]);
  jsonAppend(buf, sizeof(buf), pos, "],\"out\":[");
  for (int i = 0; i < PAGE_SIZE; i++)
    jsonAppend(buf, sizeof(buf), pos, i < PAGE_SIZE-1 ? "%u," : "%u", snapOut[i]);
  jsonAppend(buf, sizeof(buf), pos, "]}");
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
  int pos = 0;
  jsonAppend(buf, sizeof(buf), pos, "{\"busy\":%s,\"out\":[", busy ? "true" : "false");
  for (int i = 0; i < 64; i++)
    jsonAppend(buf, sizeof(buf), pos, i < 63 ? "%u," : "%u", snapOut[i]);
  jsonAppend(buf, sizeof(buf), pos, "]}");
  return buf;
}

static String groupsJSON() {
  char buf[900];
  GroupDef snap[MAX_GROUPS];
  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) == pdTRUE) {
    memcpy(snap, groups, sizeof(snap));
    xSemaphoreGive(gLock);
  } else {
    memset(snap, 0, sizeof(snap));
  }
  int pos = 0;
  jsonAppend(buf, sizeof(buf), pos, "{\"count\":%u,\"groups\":[", MAX_GROUPS);
  for (uint8_t i = 0; i < MAX_GROUPS; i++) {
    if (i) jsonAppend(buf, sizeof(buf), pos, ",");
    char eName[24];
    jsonEsc(eName, sizeof(eName), snap[i].name);
    jsonAppend(buf, sizeof(buf), pos,
      "{\"id\":%u,\"name\":\"%s\",\"start\":%u,\"end\":%u,\"enabled\":%s}",
      i, eName, snap[i].start, snap[i].end, snap[i].enabled ? "true" : "false");
  }
  jsonAppend(buf, sizeof(buf), pos, "]}");
  return buf;
}

static String fixturesJSON() {
  char buf[1800];
  FixtureDef snap[MAX_FIXTURES];
  uint8_t snapCount = 0;
  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) == pdTRUE) {
    memcpy(snap, fixtures, sizeof(snap));
    snapCount = fixtureCount;
    xSemaphoreGive(gLock);
  } else {
    memset(snap, 0, sizeof(snap));
  }
  int pos = 0;
  jsonAppend(buf, sizeof(buf), pos, "{\"count\":%u,\"fixtures\":[", snapCount);
  for (uint8_t i = 0; i < snapCount && i < MAX_FIXTURES; i++) {
    if (i) jsonAppend(buf, sizeof(buf), pos, ",");
    char eName[24];
    jsonEsc(eName, sizeof(eName), snap[i].name);
    jsonAppend(buf, sizeof(buf), pos,
      "{\"id\":%u,\"name\":\"%s\",\"start\":%u,\"end\":%u,\"x\":%u,\"y\":%u,\"enabled\":%s}",
      i, eName, snap[i].start, snap[i].end, snap[i].x, snap[i].y, snap[i].enabled ? "true" : "false");
  }
  jsonAppend(buf, sizeof(buf), pos, "]}");
  return buf;
}

static const char* fxModeName(FxMode m) {
  switch (m) {
    case FX_STROBE: return "strobe";
    case FX_CHASE:  return "chase";
    case FX_PULSE:  return "pulse";
    case FX_SINE:   return "sine";
    case FX_SPARKLE:return "sparkle";
    case FX_COMET:  return "comet";
    case FX_BARS:   return "bars";
    case FX_GLITCH: return "glitch";
    case FX_RADAR:  return "radar";
    default:        return "none";
  }
}

static String cueJSON() {
  char buf[1200];
  CueStep snap[MAX_CUES];
  uint8_t snapCount = 0;
  uint8_t snapIndex = 0;
  bool snapRunning = false;
  uint32_t snapNext = 0;
  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) == pdTRUE) {
    memcpy(snap, cueList, sizeof(snap));
    snapCount = cueCount;
    snapIndex = cueIndex;
    snapRunning = cueRunning;
    snapNext = cueNextMs;
    xSemaphoreGive(gLock);
  }
  int pos = 0;
  jsonAppend(buf, sizeof(buf), pos,
    "{\"running\":%s,\"count\":%u,\"index\":%u,\"next_ms\":%lu,\"steps\":[",
    snapRunning ? "true" : "false", snapCount, snapIndex, (unsigned long)snapNext);
  for (uint8_t i = 0; i < snapCount && i < MAX_CUES; i++) {
    if (i) jsonAppend(buf, sizeof(buf), pos, ",");
    jsonAppend(buf, sizeof(buf), pos,
      "{\"idx\":%u,\"scene\":%u,\"dwell\":%lu,\"fade\":%lu}",
      i, snap[i].scene, (unsigned long)snap[i].dwellMs, (unsigned long)snap[i].fadeMs);
  }
  jsonAppend(buf, sizeof(buf), pos, "]}");
  return buf;
}

static String fxJSON() {
  char buf[300];
  FxMode m = FX_NONE;
  bool en = false;
  uint16_t bpm = 120;
  uint8_t depth = 255;
  uint8_t r = 255, g = 255, b = 255;
  bool cEn = false;
  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) == pdTRUE) {
    m = fxMode;
    en = fxEnabled;
    bpm = fxBpm;
    depth = fxDepth;
    r = colorR;
    g = colorG;
    b = colorB;
    cEn = colorEnabled;
    xSemaphoreGive(gLock);
  }
  snprintf(buf, sizeof(buf),
    "{\"enabled\":%s,\"mode\":%u,\"mode_name\":\"%s\",\"bpm\":%u,\"depth\":%u,\"color_en\":%s,\"r\":%u,\"g\":%u,\"b\":%u,\"osc_port\":%u}",
    en ? "true" : "false", uint8_t(m), fxModeName(m), bpm, depth, cEn ? "true" : "false", r, g, b, OSC_PORT);
  return buf;
}

static String safetyJSON() {
  char buf[180];
  snprintf(buf, sizeof(buf),
    "{\"burn_safe\":%s,\"safe_ceiling\":%u,\"safe_slew_step\":%u,\"normal_ceiling\":%u,\"normal_slew_step\":%u,\"artnet_fallback_to_web\":%s}",
    burnSafeMode ? "true" : "false",
    effectiveSafeMaxLevel(),
    effectiveSafeSlewStep(),
    SAFE_MAX_LEVEL,
    SAFE_SLEW_STEP,
    artnetFallbackToWeb ? "true" : "false");
  return buf;
}

static String nodeManifestJSON() {
  bool staCon = (WiFi.status() == WL_CONNECTED);
  FxMode snapFxMode = FX_NONE;
  bool snapFxEnabled = false;
  bool snapColorEnabled = false;
  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) == pdTRUE) {
    snapFxMode = fxMode;
    snapFxEnabled = fxEnabled;
    snapColorEnabled = colorEnabled;
    xSemaphoreGive(gLock);
  }
  char eName[80], eSsid[80], eStaSsid[80], eFw[80];
  jsonEsc(eName,    sizeof(eName),    nodeName);
  jsonEsc(eSsid,    sizeof(eSsid),    apSsid);
  jsonEsc(eStaSsid, sizeof(eStaSsid), staSSID);
  jsonEsc(eFw,      sizeof(eFw),      FW_TAG);

  String apIp = ipStr(WiFi.softAPIP());
  String staIp = staCon ? ipStr(WiFi.localIP()) : "";
  String mac = WiFi.macAddress();
  String outTarget = ipStr(artOutTarget());

  char buf[3400];
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
      "{\"id\":\"http\",\"role\":\"control\",\"routes\":[\"/\",\"/control\",\"/patch\",\"/scenes\",\"/network\",\"/system\",\"/performance\",\"/vj\",\"/status\",\"/page\",\"/monitor\",\"/blackout\",\"/full\",\"/master\",\"/mode/set\",\"/netmode/set\",\"/web/set\",\"/artnet/set\",\"/artout/set\",\"/artout/peer\",\"/scene/save\",\"/scene/recall\",\"/wifi/scan\",\"/wifi/set\",\"/wifi/forget\",\"/node/set\",\"/reboot\",\"/discover\",\"/peers\",\"/peer/cmd\",\"/net/blackout\",\"/net/full\",\"/net/master\",\"/net/scene/recall\",\"/groups\",\"/group/set\",\"/group/apply\",\"/cue/status\",\"/cue/count\",\"/cue/set\",\"/cue/run\",\"/cue/next\",\"/fx/status\",\"/fx/set\",\"/fx/tap\",\"/color/set\",\"/node/manifest\",\"/manifest.json\"]},"
      "{\"id\":\"websocket\",\"role\":\"live-status\",\"endpoint\":\"/ws\",\"push_ms\":400},"
      "{\"id\":\"artnet\",\"role\":\"input-output\",\"port\":%u,\"universe\":%u,\"output_target\":\"%s\",\"output_enabled\":%s},"
      "{\"id\":\"sacn\",\"role\":\"input\",\"port\":%u,\"universe\":%u},"
      "{\"id\":\"osc\",\"role\":\"input\",\"transport\":\"udp\",\"port\":%u},"
      "{\"id\":\"dmx512\",\"role\":\"physical-output\",\"channels\":%d}"
    "],"
    "\"state\":{\"output_mode\":\"%s\",\"master\":%u,\"artnet_active\":%s,\"sacn_active\":%s,\"web_enabled\":%s,\"fx_mode\":\"%s\",\"fx_enabled\":%s,\"color_enabled\":%s,\"burn_safe\":%s,\"safe_ceiling\":%u,\"safe_slew_step\":%u},"
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
    OSC_PORT,
    MAX_CH,
    modeName(mode), masterDimmer, artnetActive() ? "true" : "false", sacnActive() ? "true" : "false", webEnabled ? "true" : "false", fxModeName(snapFxMode), snapFxEnabled ? "true" : "false", snapColorEnabled ? "true" : "false", burnSafeMode ? "true" : "false", effectiveSafeMaxLevel(), effectiveSafeSlewStep()
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
    AsyncWebServerResponse* res = r->beginResponse_P(
      200, "text/html", reinterpret_cast<const uint8_t*>(APP_HTML), sizeof(APP_HTML) - 1);
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
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
  server.on("/safety/status", HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, safetyJSON()); });
  server.on("/node/manifest", HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, nodeManifestJSON()); });
  server.on("/manifest.json", HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, nodeManifestJSON()); });

  server.on("/page", HTTP_GET, [](AsyncWebServerRequest* r){
    uint16_t p = r->hasArg("i")
      ? uint16_t(constrain(r->arg("i").toInt(), 0, PAGE_COUNT - 1)) : 0;
    sendJSON(r, pageJSON(p));
  });

  auto routeSet = [](AsyncWebServerRequest* r){
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
  };
  server.on("/set", HTTP_GET, routeSet);
  server.on("/set", HTTP_POST, routeSet);

  auto routeBlackout = [](AsyncWebServerRequest* r){
    // A blackout must never be dropped. If the mutex is busy, defer to loop()
    // rather than returning 503 and doing nothing — the UI does not retry, so
    // the old behaviour meant a pressed Blackout button could be a no-op.
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) {
      pendingBlackout = true;
      r->send(202, "text/plain", "queued");
      return;
    }
    memset(webVals, 0, MAX_CH);
    fadeActive = false;
    xSemaphoreGive(gLock);
    r->send(204);
  };
  server.on("/blackout", HTTP_GET, routeBlackout);
  server.on("/blackout", HTTP_POST, routeBlackout);

  // One atomic panic. The old UI fired /master?v=255 and /blackout as two
  // unordered fetches, so if the blackout lost the race for the mutex the
  // panic button set the rig to full instead of killing it.
  auto routePanic = [](AsyncWebServerRequest* r){
    pendingPanic = true;
    r->send(204);
  };
  server.on("/panic", HTTP_GET, routePanic);
  server.on("/panic", HTTP_POST, routePanic);

  auto routeFull = [](AsyncWebServerRequest* r){
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    memset(webVals, 255, MAX_CH);
    fadeActive = false;
    xSemaphoreGive(gLock);
    r->send(204);
  };
  server.on("/full", HTTP_GET, routeFull);
  server.on("/full", HTTP_POST, routeFull);

  auto routeMaster = [](AsyncWebServerRequest* r){
    if (r->hasArg("v")) {
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
        r->send(503, "text/plain", "busy");
        return;
      }
      masterDimmer = uint8_t(constrain(r->arg("v").toInt(), 0, 255));
      xSemaphoreGive(gLock);
    }
    r->send(204);
  };
  server.on("/master", HTTP_GET, routeMaster);
  server.on("/master", HTTP_POST, routeMaster);

  auto routeArtNetSet = [](AsyncWebServerRequest* r){
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    if (r->hasArg("net"))    artNet    = uint8_t(constrain(r->arg("net").toInt(),    0, 127));
    if (r->hasArg("subnet")) artSubnet = uint8_t(constrain(r->arg("subnet").toInt(), 0, 15));
    if (r->hasArg("uni"))    artUni    = uint8_t(constrain(r->arg("uni").toInt(),    0, 15));
    xSemaphoreGive(gLock);
    saveConfig();
    refreshSacnSocket();
    r->send(204);
  };
  server.on("/artnet/set", HTTP_GET, routeArtNetSet);
  server.on("/artnet/set", HTTP_POST, routeArtNetSet);

  auto routeArtOutSet = [](AsyncWebServerRequest* r){
    if (r->hasArg("en")) {
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
        r->send(503, "text/plain", "busy");
        return;
      }
      artOutEnabled = r->arg("en").toInt() != 0;
      xSemaphoreGive(gLock);
    }
    saveConfig(); r->send(204);
  };
  server.on("/artout/set", HTTP_GET, routeArtOutSet);
  server.on("/artout/set", HTTP_POST, routeArtOutSet);

  auto routeModeSet = [](AsyncWebServerRequest* r){
    if (!r->hasArg("m")) { r->send(400, "text/plain", "missing arg"); return; }
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    mode = Mode(constrain(r->arg("m").toInt(), 0, int(MODE_HTP)));
    xSemaphoreGive(gLock);
    saveConfig(); r->send(204);
  };
  server.on("/mode/set", HTTP_GET, routeModeSet);
  server.on("/mode/set", HTTP_POST, routeModeSet);

  auto routeModeFallback = [](AsyncWebServerRequest* r){
    if (!r->hasArg("en")) { r->send(400, "text/plain", "missing arg"); return; }
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    artnetFallbackToWeb = r->arg("en").toInt() != 0;
    xSemaphoreGive(gLock);
    saveConfig();
    r->send(204);
  };
  server.on("/mode/fallback", HTTP_GET, routeModeFallback);
  server.on("/mode/fallback", HTTP_POST, routeModeFallback);

  auto routeNetModeSet = [](AsyncWebServerRequest* r){
    if (!r->hasArg("m")) { r->send(400, "text/plain", "missing arg"); return; }
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    netMode = NetMode(constrain(r->arg("m").toInt(), 0, int(NET_AP_ONLY)));
    xSemaphoreGive(gLock);
    saveConfig();
    r->send(200, "text/plain", "ok,rebooting");
    pendingReboot = true;
  };
  server.on("/netmode/set", HTTP_GET, routeNetModeSet);
  server.on("/netmode/set", HTTP_POST, routeNetModeSet);

  auto routeWebSet = [](AsyncWebServerRequest* r){
    if (!r->hasArg("en")) { r->send(400, "text/plain", "missing arg"); return; }
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    webEnabled = r->arg("en").toInt() != 0;
    xSemaphoreGive(gLock);
    saveConfig();
    r->send(200, "text/plain", "ok,rebooting");
    pendingReboot = true;
  };
  server.on("/web/set", HTTP_GET, routeWebSet);
  server.on("/web/set", HTTP_POST, routeWebSet);

  auto routeSafetySet = [](AsyncWebServerRequest* r){
    if (!r->hasArg("burn")) { r->send(400, "text/plain", "missing burn"); return; }
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    burnSafeMode = r->arg("burn").toInt() != 0;
    xSemaphoreGive(gLock);
    saveConfig();
    r->send(204);
  };
  server.on("/safety/set", HTTP_GET, routeSafetySet);
  server.on("/safety/set", HTTP_POST, routeSafetySet);

  auto routeSceneSave = [](AsyncWebServerRequest* r){
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
  };
  server.on("/scene/save", HTTP_GET, routeSceneSave);
  server.on("/scene/save", HTTP_POST, routeSceneSave);

  auto routeSceneRecall = [](AsyncWebServerRequest* r){
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
  };
  server.on("/scene/recall", HTTP_GET, routeSceneRecall);
  server.on("/scene/recall", HTTP_POST, routeSceneRecall);

  server.on("/wifi/scan",   HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, wifiScanJSON()); });

  auto routeWifiSet = [](AsyncWebServerRequest* r){
    if (!r->hasArg("ssid")) { r->send(400, "text/plain", "missing ssid"); return; }
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    staSSID = r->arg("ssid");
    staPass = r->hasArg("pass") ? r->arg("pass") : "";
    if (netMode == NET_AP_ONLY) netMode = NET_AP_STA;
    xSemaphoreGive(gLock);
    saveConfig();
    pendingWifiReconnect = true;
    r->send(204);
  };
  server.on("/wifi/set", HTTP_GET, routeWifiSet);
  server.on("/wifi/set", HTTP_POST, routeWifiSet);

  auto routeWifiForget = [](AsyncWebServerRequest* r){
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    staSSID = ""; staPass = "";
    xSemaphoreGive(gLock);
    saveConfig();
    pendingWifiForget = true;
    r->send(204);
  };
  server.on("/wifi/forget", HTTP_GET, routeWifiForget);
  server.on("/wifi/forget", HTTP_POST, routeWifiForget);

  auto routeNodeSet = [](AsyncWebServerRequest* r){
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    // fillStatusJSON writes into a fixed 800-byte buffer. An unbounded name
    // truncates every WebSocket frame into invalid JSON, which the browser
    // silently swallows — the console then freezes on stale values forever.
    if (r->hasArg("name"))    { nodeName = r->arg("name"); if (nodeName.length() > MAX_NAME_LEN) nodeName.remove(MAX_NAME_LEN); }
    if (r->hasArg("ap_ssid")) { apSsid   = r->arg("ap_ssid"); if (apSsid.length() > MAX_NAME_LEN) apSsid.remove(MAX_NAME_LEN); }
    if (r->hasArg("ap_pass")) { apPass   = r->arg("ap_pass"); if (apPass.length() > MAX_PASS_LEN) apPass.remove(MAX_PASS_LEN); }
    xSemaphoreGive(gLock);
    saveConfig();
    startMdns();
    r->send(204);
  };
  server.on("/node/set", HTTP_GET, routeNodeSet);
  server.on("/node/set", HTTP_POST, routeNodeSet);

  auto routeReboot = [](AsyncWebServerRequest* r){
    r->send(200, "text/plain", "Rebooting…");
    pendingReboot = true;
  };
  server.on("/reboot", HTTP_GET, routeReboot);
  server.on("/reboot", HTTP_POST, routeReboot);

  // Factory reset: erase NVS and reboot. Restores MAC-derived SSID, default password.
  server.on("/factory-reset", HTTP_POST, [](AsyncWebServerRequest* r){
    r->send(200, "text/plain", "Factory reset — rebooting with defaults…");
    pendingFactoryReset = true;
  });

  server.on("/discover", HTTP_GET, [](AsyncWebServerRequest* r){
    char buf[256];
    char eName[64];   jsonEsc(eName,   sizeof(eName),   nodeName);
    char eApSsid[64]; jsonEsc(eApSsid, sizeof(eApSsid), apSsid);
    char apIp[16];    ipToCStr(apIp, sizeof(apIp), WiFi.softAPIP());
    String mac = WiFi.macAddress();
    snprintf(buf, sizeof(buf),
      "{\"name\":\"%s\",\"ip\":\"%s\",\"ap_ip\":\"%s\",\"ap_ssid\":\"%s\",\"mdns\":\"%s.local\",\"mac\":\"%s\",\"product\":\"vizzz.di\"}",
      eName, ipStr(WiFi.localIP()).c_str(), apIp, eApSsid, mdnsName.c_str(), mac.c_str());
    r->send(200, "application/json", buf);
    sendBeacon();
  });

  server.on("/peers", HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, peersJSON()); });

  server.on("/artout/peer", HTTP_GET, [](AsyncWebServerRequest* r){
    // Written on the AsyncTCP task, read by loop() at DMX rate. Without the
    // lock a String realloc frees the buffer out from under artOutTarget().
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    artOutPeerIp = r->hasArg("ip") ? r->arg("ip") : "";
    xSemaphoreGive(gLock);
    r->send(204);
  });

  server.on("/groups", HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, groupsJSON()); });

  server.on("/fixtures", HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, fixturesJSON()); });

  auto routeFixtureSet = [](AsyncWebServerRequest* r){
    if (!r->hasArg("i")) { r->send(400, "text/plain", "missing i"); return; }
    uint8_t i = uint8_t(constrain(r->arg("i").toInt(), 0, MAX_FIXTURES - 1));
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    if (r->hasArg("name")) r->arg("name").toCharArray(fixtures[i].name, sizeof(fixtures[i].name));
    if (r->hasArg("start")) fixtures[i].start = uint16_t(constrain(r->arg("start").toInt(), 1, MAX_CH));
    if (r->hasArg("end")) fixtures[i].end = uint16_t(constrain(r->arg("end").toInt(), fixtures[i].start, MAX_CH));
    if (r->hasArg("x")) fixtures[i].x = uint8_t(constrain(r->arg("x").toInt(), 0, 255));
    if (r->hasArg("y")) fixtures[i].y = uint8_t(constrain(r->arg("y").toInt(), 0, 255));
    if (r->hasArg("en")) fixtures[i].enabled = r->arg("en").toInt() != 0;
    if (r->hasArg("count")) fixtureCount = uint8_t(constrain(r->arg("count").toInt(), 1, MAX_FIXTURES));
    for (uint8_t n = 0; n < MAX_FIXTURES; n++) fixtures[n].enabled = (n < fixtureCount) ? fixtures[n].enabled : false;
    xSemaphoreGive(gLock);
    saveFixtures();
    r->send(204);
  };
  server.on("/fixture/set", HTTP_GET, routeFixtureSet);
  server.on("/fixture/set", HTTP_POST, routeFixtureSet);

  auto routeGroupSet = [](AsyncWebServerRequest* r){
    if (!r->hasArg("g")) { r->send(400, "text/plain", "missing g"); return; }
    uint8_t g = uint8_t(constrain(r->arg("g").toInt(), 0, MAX_GROUPS - 1));
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    if (r->hasArg("name")) {
      String n = r->arg("name");
      n.toCharArray(groups[g].name, sizeof(groups[g].name));
    }
    if (r->hasArg("start")) groups[g].start = uint16_t(constrain(r->arg("start").toInt(), 1, MAX_CH));
    if (r->hasArg("end")) groups[g].end = uint16_t(constrain(r->arg("end").toInt(), groups[g].start, MAX_CH));
    if (r->hasArg("en")) groups[g].enabled = r->arg("en").toInt() != 0;
    xSemaphoreGive(gLock);
    saveVj();
    r->send(204);
  };
  server.on("/group/set", HTTP_GET, routeGroupSet);
  server.on("/group/set", HTTP_POST, routeGroupSet);

  auto routeGroupApply = [](AsyncWebServerRequest* r){
    if (!r->hasArg("g") || !r->hasArg("v")) { r->send(400, "text/plain", "missing arg"); return; }
    uint8_t g = uint8_t(constrain(r->arg("g").toInt(), 0, MAX_GROUPS - 1));
    uint8_t v = uint8_t(constrain(r->arg("v").toInt(), 0, 255));
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    if (!groups[g].enabled) {
      xSemaphoreGive(gLock);
      r->send(409, "text/plain", "group disabled");
      return;
    }
    uint16_t s = constrain(groups[g].start, 1, MAX_CH);
    uint16_t e = constrain(groups[g].end, s, MAX_CH);
    for (uint16_t ch = s; ch <= e; ch++) webVals[ch - 1] = v;
    xSemaphoreGive(gLock);
    r->send(204);
  };
  server.on("/group/apply", HTTP_GET, routeGroupApply);
  server.on("/group/apply", HTTP_POST, routeGroupApply);

  server.on("/cue/status", HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, cueJSON()); });

  auto routeCueCount = [](AsyncWebServerRequest* r){
    if (!r->hasArg("c")) { r->send(400, "text/plain", "missing c"); return; }
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    cueCount = uint8_t(constrain(r->arg("c").toInt(), 1, MAX_CUES));
    if (cueIndex >= cueCount) cueIndex = 0;
    xSemaphoreGive(gLock);
    saveVj();
    r->send(204);
  };
  server.on("/cue/count", HTTP_GET, routeCueCount);
  server.on("/cue/count", HTTP_POST, routeCueCount);

  auto routeCueSet = [](AsyncWebServerRequest* r){
    if (!r->hasArg("i")) { r->send(400, "text/plain", "missing i"); return; }
    uint8_t i = uint8_t(constrain(r->arg("i").toInt(), 0, MAX_CUES - 1));
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    if (r->hasArg("scene")) cueList[i].scene = uint8_t(constrain(r->arg("scene").toInt(), 0, SCENE_COUNT - 1));
    if (r->hasArg("dwell")) cueList[i].dwellMs = uint32_t(max(0L, r->arg("dwell").toInt()));
    if (r->hasArg("fade"))  cueList[i].fadeMs  = uint32_t(max(0L, r->arg("fade").toInt()));
    xSemaphoreGive(gLock);
    saveVj();
    r->send(204);
  };
  server.on("/cue/set", HTTP_GET, routeCueSet);
  server.on("/cue/set", HTTP_POST, routeCueSet);

  auto routeCueRun = [](AsyncWebServerRequest* r){
    if (!r->hasArg("en")) { r->send(400, "text/plain", "missing en"); return; }
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    cueRunning = (r->arg("en").toInt() != 0) && cueCount > 0;
    if (cueRunning) {
      cueIndex = 0;
      xSemaphoreGive(gLock);
      triggerCueStep(0);
      r->send(204);
      return;
    }
    xSemaphoreGive(gLock);
    r->send(204);
  };
  server.on("/cue/run", HTTP_GET, routeCueRun);
  server.on("/cue/run", HTTP_POST, routeCueRun);

  auto routeCueNext = [](AsyncWebServerRequest* r){
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    if (cueCount == 0) { xSemaphoreGive(gLock); r->send(400, "text/plain", "no cues"); return; }
    cueIndex = (cueIndex + 1) % cueCount;
    uint8_t next = cueIndex;
    xSemaphoreGive(gLock);
    triggerCueStep(next);
    r->send(204);
  };
  server.on("/cue/next", HTTP_GET, routeCueNext);
  server.on("/cue/next", HTTP_POST, routeCueNext);

  server.on("/fx/status", HTTP_GET, [](AsyncWebServerRequest* r){ sendJSON(r, fxJSON()); });

  auto routeFxSet = [](AsyncWebServerRequest* r){
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    if (r->hasArg("mode")) {
      String m = r->arg("mode");
      if (m == "strobe" || m == "1") fxMode = FX_STROBE;
      else if (m == "chase"   || m == "2") fxMode = FX_CHASE;
      else if (m == "pulse"   || m == "3") fxMode = FX_PULSE;
      else if (m == "sine"    || m == "4") fxMode = FX_SINE;
      else if (m == "sparkle" || m == "5") fxMode = FX_SPARKLE;
      else if (m == "comet"   || m == "6") fxMode = FX_COMET;
      else if (m == "bars"    || m == "7") fxMode = FX_BARS;
      else if (m == "glitch"  || m == "8") fxMode = FX_GLITCH;
      else if (m == "radar"   || m == "9") fxMode = FX_RADAR;
      else fxMode = FX_NONE;
    }
    if (r->hasArg("en")) fxEnabled = r->arg("en").toInt() != 0;
    if (r->hasArg("bpm")) fxBpm = uint16_t(constrain(r->arg("bpm").toInt(), 20, 240));
    if (r->hasArg("depth")) fxDepth = uint8_t(constrain(r->arg("depth").toInt(), 0, 255));
    if (fxMode == FX_NONE) fxEnabled = false;
    xSemaphoreGive(gLock);
    r->send(204);
  };
  server.on("/fx/set", HTTP_GET, routeFxSet);
  server.on("/fx/set", HTTP_POST, routeFxSet);

  auto routeFxTap = [](AsyncWebServerRequest* r){
    uint32_t now = millis();
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    if (lastTapMs > 0) {
      uint32_t dt = now - lastTapMs;
      if (dt >= 150 && dt <= 2000) {
        tapIntervals[tapPos] = dt;
        tapPos = (tapPos + 1) % 4;
        if (tapCount < 4) tapCount++;
        uint32_t sum = 0;
        for (uint8_t i = 0; i < tapCount; i++) sum += tapIntervals[i];
        if (sum > 0) fxBpm = uint16_t(constrain(60000UL / (sum / tapCount), 20UL, 240UL));
      }
    }
    lastTapMs = now;
    xSemaphoreGive(gLock);
    r->send(204);
  };
  server.on("/fx/tap", HTTP_GET, routeFxTap);
  server.on("/fx/tap", HTTP_POST, routeFxTap);

  auto routeColorSet = [](AsyncWebServerRequest* r){
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    if (r->hasArg("r")) colorR = uint8_t(constrain(r->arg("r").toInt(), 0, 255));
    if (r->hasArg("g")) colorG = uint8_t(constrain(r->arg("g").toInt(), 0, 255));
    if (r->hasArg("b")) colorB = uint8_t(constrain(r->arg("b").toInt(), 0, 255));
    if (r->hasArg("en")) colorEnabled = r->arg("en").toInt() != 0;
    if (colorEnabled) applyColorWash_locked();
    xSemaphoreGive(gLock);
    r->send(204);
  };
  server.on("/color/set", HTTP_GET, routeColorSet);
  server.on("/color/set", HTTP_POST, routeColorSet);

  // ── Peer proxy (forward a command to one specific known peer) ────────────────
  server.on("/peer/cmd", HTTP_GET, [](AsyncWebServerRequest* r){
    if (!r->hasArg("ip") || !r->hasArg("path")) { r->send(400, "text/plain", "missing arg"); return; }
    String ip   = r->arg("ip");
    String path = r->arg("path");
    if (!isAllowedPeerPath(path.c_str())) { r->send(400, "text/plain", "unsupported path"); return; }
    // Only allow forwarding to known peers (prevents open proxy abuse)
    bool known = false;
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) == pdTRUE) {
      for (uint8_t i = 0; i < peerCount; i++) {
        if (ip == peers[i].ip) { known = true; break; }
      }
      xSemaphoreGive(gLock);
    }
    if (!known) { r->send(403, "text/plain", "unknown peer"); return; }
    if (!enqueueBcast(path.c_str(), ip.c_str())) { r->send(503, "text/plain", "queue full"); return; }
    r->send(204);
  });

  // ── Fleet (broadcast to self + all peers) ────────────────────────────────────
  auto routeNetBlackout = [](AsyncWebServerRequest* r){
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    memset(webVals, 0, MAX_CH);
    fadeActive = false;
    xSemaphoreGive(gLock);
    if (!enqueueBcast("/blackout")) { r->send(503, "text/plain", "queue full"); return; }
    r->send(204);
  };
  server.on("/net/blackout", HTTP_GET, routeNetBlackout);
  server.on("/net/blackout", HTTP_POST, routeNetBlackout);

  auto routeNetFull = [](AsyncWebServerRequest* r){
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    memset(webVals, 255, MAX_CH);
    fadeActive = false;
    xSemaphoreGive(gLock);
    if (!enqueueBcast("/full")) { r->send(503, "text/plain", "queue full"); return; }
    r->send(204);
  };
  server.on("/net/full", HTTP_GET, routeNetFull);
  server.on("/net/full", HTTP_POST, routeNetFull);

  auto routeNetMaster = [](AsyncWebServerRequest* r){
    if (r->hasArg("v")) {
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
        r->send(503, "text/plain", "busy");
        return;
      }
      masterDimmer = uint8_t(constrain(r->arg("v").toInt(), 0, 255));
      xSemaphoreGive(gLock);
    }
    char path[24];
    snprintf(path, sizeof(path), "/master?v=%u", masterDimmer);
    if (!enqueueBcast(path)) { r->send(503, "text/plain", "queue full"); return; }
    r->send(204);
  };
  server.on("/net/master", HTTP_GET, routeNetMaster);
  server.on("/net/master", HTTP_POST, routeNetMaster);

  auto routeNetSceneRecall = [](AsyncWebServerRequest* r){
    if (!r->hasArg("n")) { r->send(400, "text/plain", "missing n"); return; }
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
    char path[56];
    snprintf(path, sizeof(path), "/scene/recall?n=%u&fade=%lu", n, (unsigned long)ft);
    if (!enqueueBcast(path)) { r->send(503, "text/plain", "queue full"); return; }
    r->send(204);
  };
  server.on("/net/scene/recall", HTTP_GET, routeNetSceneRecall);
  server.on("/net/scene/recall", HTTP_POST, routeNetSceneRecall);

  server.onNotFound([](AsyncWebServerRequest* r){ r->send(404, "text/plain", "Not found"); });
  server.begin();
}

// ── Setup / Loop ──────────────────────────────────────────────────────────────
void setup() {
  // Park the RS485 driver in receive before anything else. Until dmx_set_pin()
  // runs — after NVS load and several memsets — this pin is otherwise a
  // floating input, and a transceiver whose DE floats high can drive the bus
  // with an undriven TX line.
  pinMode(DMX_DIR, OUTPUT);
  digitalWrite(DMX_DIR, LOW);

  Serial.begin(115200);

  gLock = xSemaphoreCreateMutex();

  loadConfig();
  initVjDefaults();
  // loadFixtures() existed but was never called, so the patch was written to
  // flash on every edit and silently reset to defaults on every boot.
  loadFixtures();
  loadVj();

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
  discoverUdp.begin(DISCOVERY_PORT);
  oscUdp.begin(OSC_PORT);
  // The console always starts, even when it is configured off. Previously
  // /web/set?en=0 persisted to NVS and setup() skipped setupWeb() entirely,
  // leaving no HTTP server at all — so /web/set?en=1, /reboot and
  // /factory-reset were all unreachable and the only way back was a serial
  // reflash. Now every boot gives WEB_GRACE_MS to reach the console and undo
  // it, after which loop() shuts the server down as asked.
  startBcastTask();
  setupWeb();

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
  Serial.printf("OSC IN   : udp/%u\n", OSC_PORT);
  if (apPass == "changeme123") {
    Serial.printf("WARNING  : placeholder AP password in use.\n");
    Serial.printf("           cp src/secrets.example.h src/secrets.h, set your\n");
    Serial.printf("           own AP_PASS_DEFAULT, then reflash.\n");
  }
}

void loop() {
  if (pendingReboot) { delay(150); ESP.restart(); }

  if (pendingFactoryReset) {
    pendingFactoryReset = false;
    Preferences cfg;
    cfg.begin("cfg", false);
    cfg.clear();
    cfg.end();
    Preferences sc;
    sc.begin("scenes", false);
    sc.clear();
    sc.end();
    // 'fixtures' was added after this route was written and was never cleared,
    // so a factory-reset node kept the previous owner's patch.
    Preferences fx;
    fx.begin("fixtures", false);
    fx.clear();
    fx.end();
    Preferences vj;
    vj.begin("vj", false);
    vj.clear();
    vj.end();
    delay(80);
    ESP.restart();
  }

  if (pendingPanic) {
    if (xSemaphoreTake(gLock, portMAX_DELAY) == pdTRUE) {
      pendingPanic = false;
      pendingBlackout = false;
      mode         = MODE_WEB;      // stop network input driving the rig
      cueRunning   = false;
      fxEnabled    = false;
      colorEnabled = false;
      fadeActive   = false;
      masterDimmer = 255;
      memset(webVals, 0, MAX_CH);
      xSemaphoreGive(gLock);
    }
  }

  if (pendingBlackout) {
    if (xSemaphoreTake(gLock, portMAX_DELAY) == pdTRUE) {
      pendingBlackout = false;
      memset(webVals, 0, MAX_CH);
      fadeActive = false;
      xSemaphoreGive(gLock);
    }
  }

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
  pollOsc();

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
  tickCueEngine(now);

  // DMX output cycle
  static uint32_t lastDmx = 0;
  if (now - lastDmx >= DMX_PERIOD_MS) {
    lastDmx = now;

    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) == pdTRUE) {
      updateFade_locked();
      computeOutput_locked(artnetActive(), sacnActive(), now);
      xSemaphoreGive(gLock);
    }

    sendDMX();
    sendArtNetOut();
  }

  // Discovery beacon every 30s; poll for incoming beacons every loop tick
  {
    static uint32_t lastBeacon = 0;
    if (now - lastBeacon >= 30000) { lastBeacon = now; sendBeacon(); }
    pollDiscovery();
  }

  // Expire stale peers every 10s
  {
    static uint32_t lastExpiry = 0;
    if (now - lastExpiry >= 10000) {
      lastExpiry = now;
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) == pdTRUE) {
        uint8_t w = 0;
        for (uint8_t r = 0; r < peerCount; r++) {
          if (now - peers[r].lastSeenMs < PEER_EXPIRE_MS) {
            if (w != r) peers[w] = peers[r];
            w++;
          }
        }
        peerCount = w;
        xSemaphoreGive(gLock);
      }
    }
  }

  // WebSocket status push ~400ms (only when web stack is enabled)
  if (!webEnabled && !webStopped && millis() >= WEB_GRACE_MS) {
    webStopped = true;
    ws.closeAll();
    server.end();
    Serial.printf("Web disabled after %lu ms grace window\n", (unsigned long)WEB_GRACE_MS);
  }

  if (!webStopped) {
    static uint32_t lastWs = 0;
    if (now - lastWs >= 400) {
      lastWs = now;
      if (ws.count() > 0) {
        char statusBuf[800];
        size_t len = fillStatusJSON(statusBuf, sizeof(statusBuf));
        ws.textAll(statusBuf, len);
      }
      ws.cleanupClients();
    }
  }
}
