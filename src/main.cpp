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
#include <atomic>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include "esp_dmx.h"
#include "vizzz_core.h"
#include "desk_ui_gz.h"

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
static constexpr uint32_t   SACN_TIMEOUT_MS   = vizzz::sacn::kLossTimeoutMs;  // E1.31-2018 §6.7.1: 2.5 s
static constexpr uint32_t   ART_MERGE_TIMEOUT_MS = vizzz::artnet::kMergeTimeoutMs;  // Art-Net 4 "Data Merging": 10 s
// USB serial in: the ENTTEC DMX USB PRO widget protocol ("DMX USB PRO API",
// ENTTEC) on UART0 (`Serial`), the same UART as the text log. DMX itself stays
// on DMX_NUM_1/UART2 (AGENTS.md). The baud is di.iiii's: serverXR/src/lighting/
// enttec.js sets 250000 first (115200 only if the port refuses it), and a widget
// host never negotiates, so both ends must simply agree. Host side: stty/mode.
static constexpr uint32_t   USB_WIDGET_BAUD    = 250000;
// Loss timeout for the USB layer. The widget API defines none (a real widget holds
// its last frame forever); di sends every tick (~40 Hz, enttec.js "Transmit
// CONTINUOUSLY"), so silence means the desk is gone. E1.31-2018 §6.7.1's 2.5 s
// network-data-loss rule is used, like sACN, so a closed desk never latches the rig
// on in HTP; ARTNET_ONLY still holds or falls back per artnetFallbackToWeb.
static constexpr uint32_t   USB_TIMEOUT_MS     = vizzz::sacn::kLossTimeoutMs;
static constexpr size_t     USB_RX_BUFFER      = 4096;  // ~160 ms of 250 kbaud; loop() can sit ~25 ms in sendDMX()
static constexpr uint16_t   USB_BYTES_PER_PASS = 2048;  // bounded per loop pass, like the UDP packet budgets
static constexpr uint32_t   USB_FRAME_GAP_MS   = 100;   // a frame stalled this long is dropped and re-scanned
static constexpr uint16_t   USB_WIDGET_FW      = 0x0100; // label 3 "firmware version": vizzz widget layer 1.0, not an ENTTEC number
static constexpr uint32_t   ART_OUT_KEEPALIVE_MS = 1000;  // resend an unchanged frame; inside the 3 s loss timeout
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
static_assert(MODE_WEB == vizzz::merge::kWebOnly && MODE_ARTNET == vizzz::merge::kArtnetOnly &&
              MODE_HTP == vizzz::merge::kHtp, "merge rule (vizzz_core.h) and Mode must agree");
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
// Art OUT unicast target as a raw IPAddress word (0 = none → broadcast). Written
// by the web task, read by loop() at DMX rate: one aligned 32-bit atomic, so no
// String buffer can be freed under the reader (audit 2026-10-15 1-C2).
static std::atomic<uint32_t> artOutPeerAddr{0};
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

// sACN universe = Art-Net Port-Address + 1, never the reserved 0 (E1.31-2018
// §6.2.7). Desk universe 1 is then Art-Net 0 and sACN 1, as di sends them.
static uint16_t sacnUniverse() {
  return vizzz::sacn::universeForPortAddress(universe());
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

// ── Shared identity strings (audit 2026-10-15 1-C2) ───────────────────────────
// nodeName, apSsid, apPass, staSSID and staPass are Arduino Strings written by
// the AsyncTCP task (/node/set, /wifi/set, /wifi/forget) and read by loop().
// String::operator= reallocates and frees the old buffer, so an unlocked
// reader on the other task copies from freed heap. Rule: every write holds
// gLock; every reader outside setup() copies them into this fixed snapshot
// under gLock (FreeRTOS mutex, held only for the strlcpy) and uses the copy.
struct IdentitySnap {
  char name[MAX_NAME_LEN + 1];
  char apSsid[MAX_NAME_LEN + 1];
  char apPass[MAX_PASS_LEN + 1];
  char staSsid[33];             // 802.11 SSID <= 32 octets
  char staPass[MAX_PASS_LEN + 2];
  char mdns[MAX_NAME_LEN + 1];
};
static bool snapIdentity(IdentitySnap& s, TickType_t wait = pdMS_TO_TICKS(20));

// Callers must NOT hold gLock: the strings are copied under it, then written
// to NVS outside it (AGENTS.md: never hold gLock across a blocking call).
static void saveConfig() {
  IdentitySnap id;
  if (!snapIdentity(id, pdMS_TO_TICKS(50))) {
    Serial.println("saveConfig: lock busy, identity not saved");
    return;
  }
  prefs.begin("cfg", false);
  prefs.putString("name",     id.name);
  prefs.putString("ap_ssid",  id.apSsid);
  prefs.putString("ap_pass",  id.apPass);
  prefs.putString("sta_ssid", id.staSsid);
  prefs.putString("sta_pass", id.staPass);
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
  // An AP pass-phrase stored by older firmware may be 1-7 chars (softAP()
  // refuses it: no AP at all) or empty (an open AP that drives the rig).
  // Neither is a valid WPA2 pass-phrase (1-H1); boot with the build default.
  if (!vizzz::isValidWpaPassphrase(apPass.c_str(), apPass.length())) {
    Serial.printf("cfg: stored AP password invalid (%u chars), using build default\n", unsigned(apPass.length()));
    apPass = AP_PASS_DEFAULT;
    needSaveConfig = true;
  }

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
static uint8_t usbVals  [MAX_CH];   // USB widget IN (label 6) — written under gLock
static uint8_t outVals  [MAX_CH];   // final computed output

static volatile uint32_t lastArtnetMs = 0;
static volatile uint32_t lastSacnMs   = 0;
static volatile uint32_t lastUsbMs    = 0;
static volatile uint32_t usbFrames    = 0;   // label-6 frames applied since boot
static vizzz::widget::Parser usbRx;          // loop() only
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
    for (uint8_t i = 0; i < MAX_FIXTURES; i++) {
      fixtures[i].enabled = (i < fixtureCount) ? fixtures[i].enabled : false;
      // Repair what was read, as loadVj() does (1-L8): a corrupted blob of the
      // right size must not make fixturesJSON over-read a name with %s or put
      // an out-of-range channel into the output path.
      fixtures[i].name[sizeof(fixtures[i].name) - 1] = '\0';
      vizzz::clampChannelRange(fixtures[i].start, fixtures[i].end, MAX_CH);
    }
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
static volatile bool     pendingMdnsRestart   = false;  // set by /node/set, run in loop()
static volatile bool     pendingBeacon        = false;  // set by /discover, run in loop()
static volatile bool     pendingSockRefresh   = false;  // set by /artnet/set, run in loop()
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

static void jsonEsc(char* dst, size_t n, const char* src) {
  size_t d = 0;
  for (size_t i = 0; src && src[i] != '\0' && d + 3 < n; i++) {
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
static void jsonEsc(char* dst, size_t n, const String& src) { jsonEsc(dst, n, src.c_str()); }

static char mdnsName[MAX_NAME_LEN + 1] = "vizzz-di";  // written under gLock

static bool snapIdentity(IdentitySnap& s, TickType_t wait) {
  memset(&s, 0, sizeof(s));
  if (xSemaphoreTake(gLock, wait) != pdTRUE) return false;
  strlcpy(s.name,    nodeName.c_str(), sizeof(s.name));
  strlcpy(s.apSsid,  apSsid.c_str(),   sizeof(s.apSsid));
  strlcpy(s.apPass,  apPass.c_str(),   sizeof(s.apPass));
  strlcpy(s.staSsid, staSSID.c_str(),  sizeof(s.staSsid));
  strlcpy(s.staPass, staPass.c_str(),  sizeof(s.staPass));
  strlcpy(s.mdns,    mdnsName,         sizeof(s.mdns));
  xSemaphoreGive(gLock);
  return true;
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
  uint32_t peer = artOutPeerAddr.load(std::memory_order_relaxed);
  if (peer != 0) return IPAddress(peer);
  if (WiFi.status() == WL_CONNECTED) {
    uint32_t ip   = (uint32_t)WiFi.localIP();
    uint32_t mask = (uint32_t)WiFi.subnetMask();
    return IPAddress((ip & mask) | (~mask));
  }
  return IPAddress(192, 168, 4, 255);
}

static_assert(sizeof(AP_PASS_DEFAULT) - 1 >= 8 && sizeof(AP_PASS_DEFAULT) - 1 <= 63,
              "AP_PASS_DEFAULT must be a valid WPA2 pass-phrase (8-63 chars)");
static bool apFallbackActive = false;  // true when the configured AP was refused

// Core 2.0.17 WiFi.softAP() returns false on an empty SSID, a 1-7 char
// pass-phrase (WiFiAP.cpp:145) or a driver error. An AP-only node with no AP
// is unreachable, so on refusal fall back to the MAC SSID + build-time
// pass-phrase and say so on serial and in /status ("ap_fb") (1-H1).
static void startSoftAP() {
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  IdentitySnap id;
  snapIdentity(id, pdMS_TO_TICKS(100));
  if (WiFi.softAP(id.apSsid, id.apPass)) { apFallbackActive = false; return; }
  String fbSsid = String(AP_SSID_PREFIX) + "_" + macSuffix6();
  Serial.printf("AP: softAP(\"%s\", %u-char pass) refused; falling back to \"%s\" + build pass\n",
                id.apSsid, unsigned(strlen(id.apPass)), fbSsid.c_str());
  apFallbackActive = true;
  if (!WiFi.softAP(fbSsid.c_str(), AP_PASS_DEFAULT)) Serial.println("AP: fallback softAP refused too");
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

// STA join state, owned by loop() (serviceStaJoin) and setup().
static constexpr uint32_t STA_JOIN_WINDOW_MS = 10000;  // one join attempt
static uint32_t staAttemptMs   = 0;      // when the current attempt started
static uint32_t staParkedMs    = 0;      // when STA retries were parked
static uint8_t  staFailedJoins = 0;
static bool     staParked      = false;  // auto-reconnect off, waiting for back-off
static bool     staBootResolved = false; // STA_ONLY boot safety net done

static void startWiFi() {
  wifi_mode_t wm = WIFI_AP_STA;
  if (netMode == NET_AP_ONLY) wm = WIFI_AP;
  if (netMode == NET_STA_ONLY) wm = WIFI_STA;
  // DHCP hostname BEFORE WiFi.mode(): core 2.0.17 copies it to the STA netif
  // inside mode() (WiFiGeneric.cpp:1265), so setting it later left the lease
  // as "esp32-XXXXXX" (1-L2).
  buildMdnsName();
  {
    IdentitySnap id;
    snapIdentity(id, pdMS_TO_TICKS(100));
    WiFi.setHostname(id.mdns);
  }
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
        xSemaphoreTake(gLock, portMAX_DELAY);  // setup(): no other holder yet
        apSsid = expected;
        xSemaphoreGive(gLock);
        needSaveConfig = true;
      }
    }
  }

  if (netMode != NET_STA_ONLY) {
    startSoftAP();
  }

  IdentitySnap cred;
  snapIdentity(cred, pdMS_TO_TICKS(100));
  if (netMode != NET_AP_ONLY && cred.staSsid[0]) {
    // Non-blocking (1-M6): the old 10 s wait here ran before loop() sent its
    // first DMX frame. serviceStaJoin() in loop() now owns the join window
    // and the STA_ONLY safety-net AP.
    WiFi.begin(cred.staSsid, cred.staPass);
    staAttemptMs = millis();
  } else if (netMode == NET_STA_ONLY) {
    // No STA credentials in STA_ONLY: expose AP so the node is still recoverable.
    WiFi.mode(WIFI_AP_STA);
    startSoftAP();
  }
}

// ── mDNS ──────────────────────────────────────────────────────────────────────
// Only loop()/setup() call this (the web task sets pendingMdnsRestart).
static void buildMdnsName() {
  IdentitySnap id;
  if (!snapIdentity(id, pdMS_TO_TICKS(100))) return;
  char label[sizeof(mdnsName)];
  vizzz::sanitizeHostLabel(id.name, label, sizeof(label), "vizzz-di");
  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(100)) != pdTRUE) return;
  strlcpy(mdnsName, label, sizeof(mdnsName));
  xSemaphoreGive(gLock);
}

static void startMdns() {
  buildMdnsName();
  IdentitySnap id;
  snapIdentity(id, pdMS_TO_TICKS(100));
  MDNS.end();
  if (MDNS.begin(id.mdns[0] ? id.mdns : "vizzz-di")) {
    MDNS.addService("http", "tcp", 80);
  }
}

static void sendBeacon() {
  if (WiFi.status() != WL_CONNECTED) return;
  uint32_t ip   = (uint32_t)WiFi.localIP();
  uint32_t mask = (uint32_t)WiFi.subnetMask();
  IPAddress bcast((ip & mask) | ~mask);
  char buf[240];
  IdentitySnap id;
  if (!snapIdentity(id)) return;  // next beacon in 30 s
  char eName[64];   jsonEsc(eName,   sizeof(eName),   id.name);
  char eApSsid[64]; jsonEsc(eApSsid, sizeof(eApSsid), id.apSsid);
  String mac = WiFi.macAddress();
  snprintf(buf, sizeof(buf),
    "{\"name\":\"%s\",\"ip\":\"%s\",\"ap_ssid\":\"%s\",\"mdns\":\"%s.local\",\"mac\":\"%s\",\"product\":\"vizzz.di\"}",
    eName, ipStr(WiFi.localIP()).c_str(), eApSsid, id.mdns, mac.c_str());
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
    bool added = false;
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
      added = true;
    }
    // "New" only when a peer was actually added: with the table full, any
    // unknown MAC used to count as new and got a reply every second (1-L7).
    bool isNewPeer = added;
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

static bool usbActive() {
  return lastUsbMs && (millis() - lastUsbMs < USB_TIMEOUT_MS);
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
    uint8_t g[4];
    vizzz::sacn::multicastGroup(sacnUniverse(), g);
    IPAddress group(g[0], g[1], g[2], g[3]);
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

// Loads cue `idx` and starts its fade. Returns false (state untouched) when
// the lock is busy, so the caller retries the SAME cue instead of skipping
// one (1-L6). The step is never shorter than CUE_MIN_STEP_MS (1-M1).
static bool triggerCueStep(uint8_t idx) {
  uint8_t scene = 0;
  uint32_t fade = 0;
  uint32_t dwell = 0;
  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  if (idx >= cueCount) {
    xSemaphoreGive(gLock);
    return true;  // nothing to run; not a retryable failure
  }
  scene = cueList[idx].scene;
  fade = cueList[idx].fadeMs;
  dwell = cueList[idx].dwellMs;
  xSemaphoreGive(gLock);

  uint8_t target[MAX_CH];
  loadScene(scene, target);

  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  startFade_locked(target, fade);
  cueIndex     = idx;
  cueStartedMs = millis();
  cueStepMs    = vizzz::cueStepMs(fade, dwell);
  cueNextMs    = cueStartedMs + cueStepMs;  // display only; may wrap
  xSemaphoreGive(gLock);
  return true;
}

static void tickCueEngine(uint32_t now) {
  // After a failed step (busy lock) wait CUE_MIN_STEP_MS before retrying, so
  // a contended lock cannot turn into an NVS read per loop() pass.
  static uint32_t failedAtMs = 0;
  static bool retryPending = false;
  if (retryPending && !vizzz::elapsedAtLeast(now, failedAtMs, vizzz::CUE_MIN_STEP_MS)) return;
  uint8_t nextIdx = 0;
  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) != pdTRUE) return;
  // Delta comparison, not `now < cueNextMs`. The absolute form free-runs for
  // ~300 ms every 49.7 days when millis() wraps, firing a scene change per loop.
  if (!cueRunning || cueCount == 0 ||
      !vizzz::cueStepDue(now, cueStartedMs, vizzz::cueStepMs(0, cueStepMs))) {
    xSemaphoreGive(gLock);
    return;
  }
  nextIdx = uint8_t((cueIndex + 1) % cueCount);
  xSemaphoreGive(gLock);
  retryPending = !triggerCueStep(nextIdx);  // cueIndex advances only on success
  failedAtMs = now;
}

// One blackout for every route (/blackout, /net/blackout, the deferred one in
// loop()). It also stops the cue list and FX, as /panic does: otherwise
// tickCueEngine fires the next step within fade+dwell and fades the rig back
// up by itself (audit 2026-10-15 1-H2). Call with gLock held.
static void applyBlackout_locked() {
  memset(webVals, 0, MAX_CH);
  fadeActive = false;
  cueRunning = false;
  fxEnabled  = false;
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

// One OSC message (address, type tags, first argument).
static void handleOscMessage(const uint8_t* pkt, int n) {
  if (n < 8) return;

  int off = 0;
  char addr[48], tags[16];
  if (!oscReadPaddedString(pkt, n, off, addr, sizeof(addr))) return;
  if (!oscReadPaddedString(pkt, n, off, tags, sizeof(tags))) return;
  if (tags[0] != ',') return;
  int32_t v = 0, lv = 0;  // v: index/flag value; lv: level value
  if (tags[1] == 'i') {
    if (!oscReadInt(pkt, n, off, v)) return;
    lv = v;
  } else if (tags[1] == 'f') {
    float fv = 0.0f;
    if (!oscReadFloat(pkt, n, off, fv)) return;
    v = vizzz::oscFloatToInt(fv);     // audit 1-M7: no truncation, no UB on NaN/Inf
    lv = vizzz::oscFloatToLevel(fv);  // 0.0-1.0 is a normalised fader
  } else {
    return;
  }

  if (strncmp(addr, "/ch/", 4) == 0) {
    int ch = atoi(addr + 4);
    if (ch >= 1 && ch <= MAX_CH) {
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) == pdTRUE) {
        webVals[ch - 1] = uint8_t(constrain(lv, 0, 255));
        xSemaphoreGive(gLock);
      }
    }
  } else if (strncmp(addr, "/group/", 7) == 0) {
    int g = atoi(addr + 7);
    if (g >= 0 && g < MAX_GROUPS) {
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (!groups[g].enabled) {
          xSemaphoreGive(gLock);
          return;
        }
        uint16_t s = constrain(groups[g].start, 1, MAX_CH);
        uint16_t e = constrain(groups[g].end, s, MAX_CH);
        for (uint16_t ch = s; ch <= e; ch++) webVals[ch - 1] = uint8_t(constrain(lv, 0, 255));
        xSemaphoreGive(gLock);
      }
    }
  } else if (strcmp(addr, "/master") == 0) {
    masterDimmer = uint8_t(constrain(lv, 0, 255));
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
        return;
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
      colorR = uint8_t(constrain(lv, 0, 255));
      colorEnabled = true;
      applyColorWash_locked();
      xSemaphoreGive(gLock);
    }
  } else if (strcmp(addr, "/color/g") == 0) {
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) == pdTRUE) {
      colorG = uint8_t(constrain(lv, 0, 255));
      colorEnabled = true;
      applyColorWash_locked();
      xSemaphoreGive(gLock);
    }
  } else if (strcmp(addr, "/color/b") == 0) {
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) == pdTRUE) {
      colorB = uint8_t(constrain(lv, 0, 255));
      colorEnabled = true;
      applyColorWash_locked();
      xSemaphoreGive(gLock);
    }
  }
}

static void pollOsc() {
  uint8_t pkt[256];
  for (uint8_t processed = 0; processed < OSC_PKT_BUDGET; processed++) {
    int sz = oscUdp.parsePacket();
    if (sz <= 0) break;
    int n = oscUdp.read(pkt, min(int(sizeof(pkt)), sz));
    oscUdp.flush();  // OSC bundles routinely exceed sizeof(pkt); drain the rest
    if (n < 8) continue;
    if (vizzz::oscIsBundle(pkt, size_t(n))) {
      // OSC 1.0 bundle: "#bundle", time tag, then (int32 size, element)*.
      // Elements run at once (time tags are not scheduled); one level only.
      int off = 16;
      while (off + 4 <= n) {
        int32_t esz = (int32_t(pkt[off]) << 24) | (int32_t(pkt[off + 1]) << 16) |
                      (int32_t(pkt[off + 2]) << 8) | int32_t(pkt[off + 3]);
        off += 4;
        if (esz <= 0 || (esz & 3) || off + esz > n) break;
        if (!vizzz::oscIsBundle(pkt + off, size_t(esz))) handleOscMessage(pkt + off, esz);
        off += esz;
      }
      continue;
    }
    handleOscMessage(pkt, n);
  }
}

// ── Output ────────────────────────────────────────────────────────────────────
// Network-class sources: Art-Net, sACN and the USB widget (label 6). Among
// themselves they merge HTP; the result obeys the mode exactly as Art-Net always
// did: WEB_ONLY ignores all three, ARTNET_ONLY outputs them alone (and holds or
// falls back to web when all are idle), MERGE_HTP takes max(web, network).
static void computeOutput_locked(bool aActive, bool sActive, bool uActive, uint32_t nowMs) {
  bool netActive = aActive || sActive || uActive;
  uint8_t ceiling = effectiveSafeMaxLevel();
  uint8_t slewStep = effectiveSafeSlewStep();

  for (int i = 0; i < MAX_CH; i++) {
    uint8_t netIn = vizzz::merge::networkIn(aActive, artVals[i], sActive, sacnVals[i], uActive, usbVals[i]);
    uint8_t v = vizzz::merge::forMode(uint8_t(mode), netActive, netIn, webVals[i], outVals[i],
                                      artnetFallbackToWeb);

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
  static uint8_t  seq = 0;
  static uint8_t  lastSent[MAX_CH];
  static uint32_t lastSentMs = 0;
  uint32_t now = millis();
  // Send on change, and resend an unchanged frame once a second so receivers
  // keep it inside their 3 s loss timeout (audit 1-L11: no 43 Hz of repeats).
  if (lastSentMs && memcmp(lastSent, dmxFrame + 1, MAX_CH) == 0 &&
      (uint32_t)(now - lastSentMs) < ART_OUT_KEEPALIVE_MS) return;
  seq = vizzz::artnet::nextSequence(seq);   // 1..255, never 0 ("sequencing off")
  uint8_t pkt[vizzz::artnet::kDmxHeaderLen + MAX_CH];
  // Physical carries the vizzz tag so another node with Art OUT on drops it (1-C1).
  vizzz::artnet::buildDmxHeader(pkt, universe(), seq, vizzz::artnet::kVizzzPhysicalTag, MAX_CH);
  memcpy(pkt + vizzz::artnet::kDmxHeaderLen, dmxFrame + 1, MAX_CH);
  memcpy(lastSent, dmxFrame + 1, MAX_CH);
  lastSentMs = now;
  artOutUdp.beginPacket(artOutTarget(), ARTNET_PORT);
  artOutUdp.write(pkt, sizeof(pkt));
  artOutUdp.endPacket();
}

// ── Art-Net IN ────────────────────────────────────────────────────────────────
// Loop-task state for the two network inputs (Art-Net 4 merge, E1.31 sources).
static vizzz::artnet::Merger artMerge;
static vizzz::sacn::Receiver sacnRx;
static volatile uint8_t artSourceCount = 0;
static uint16_t artPollReplyCount = 0;

// Art-Net 4, ArtPollReply: answer every ArtPoll, unicast to the poller on 6454,
// from the Art-Net socket so the source port is 6454 too. The reply names the
// interface the poll arrived on (AP or STA subnet).
static void sendArtPollReply(const IPAddress& to) {
  vizzz::artnet::PollReplyInfo in{};
  bool staUp = WiFi.status() == WL_CONNECTED;
  IPAddress ap = WiFi.softAPIP();
  bool viaAp = !staUp || (to[0] == ap[0] && to[1] == ap[1] && to[2] == ap[2]);
  IPAddress me = viaAp ? ap : WiFi.localIP();
  for (int i = 0; i < 4; i++) in.ip[i] = me[i];
  if (viaAp) WiFi.softAPmacAddress(in.mac); else WiFi.macAddress(in.mac);

  char shortName[18], longName[64], report[48];
  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) != pdTRUE) return;  // nodeName is shared with the web task
  strlcpy(shortName, nodeName.c_str(), sizeof(shortName));
  snprintf(longName, sizeof(longName), "%s %s", PRODUCT_NAME, nodeName.c_str());
  xSemaphoreGive(gLock);

  bool aOn = artnetActive(), sOn = sacnActive();
  snprintf(report, sizeof(report), "art %s/%u sacn %s u%u", aOn ? "on" : "idle",
           unsigned(artSourceCount), sOn ? "on" : "idle", unsigned(sacnUniverse()));
  in.net = artNet; in.subnet = artSubnet; in.uni = artUni;
  in.shortName = shortName; in.longName = longName; in.report = report;
  in.replyCount = artPollReplyCount = uint16_t((artPollReplyCount + 1) % 10000);
  in.outputActive = true;                  // the DMX task transmits every frame
  in.merging = artSourceCount > 1;
  in.sacnSource = sOn && !aOn;
  in.dhcp = !viaAp;
  in.refreshHz = uint8_t(1000 / DMX_PERIOD_MS);

  uint8_t out[vizzz::artnet::kPollReplyLen];
  size_t len = vizzz::artnet::buildPollReply(out, in);
  artInUdp.beginPacket(to, ARTNET_PORT);
  artInUdp.write(out, len);
  artInUdp.endPacket();
}

static void pollArtNet() {
  uint8_t pkt[vizzz::artnet::kDmxHeaderLen + MAX_CH];
  uint32_t now = millis();
  bool changed = false;

  for (uint8_t processed = 0; processed < ARTNET_PKT_BUDGET; processed++) {
    int size = artInUdp.parsePacket();
    if (size <= 0) break;
    int n = artInUdp.read(pkt, min(int(sizeof(pkt)), size));
    artInUdp.flush();
    if (n < int(vizzz::artnet::kPollMinLen)) continue;  // ArtPoll is 14 bytes; check opcode before ArtDmx's 18
    IPAddress from = artInUdp.remoteIP();
    if (isOwnIp(from)) continue;  // never merge our own Art-Net OUT back in

    uint16_t op = vizzz::artnet::opcode(pkt, size_t(n));
    if (op == vizzz::artnet::kOpPoll) {
      sendArtPollReply(from);
      continue;
    }
    vizzz::artnet::DmxView v;
    if (!vizzz::artnet::parseDmx(pkt, size_t(n), v)) continue;  // ID, OpDmx, ProtVer >= 14, length
    if (v.portAddress != universe()) continue;
    // Two-node latch (audit 1-C1): a node that re-emits on Art OUT never takes
    // another node's re-emitted frame back in. Master -> slave still works,
    // because a slave has Art OUT off.
    if (artOutEnabled && v.physical == vizzz::artnet::kVizzzPhysicalTag) continue;
    if (vizzz::artnet::mergerAccept(artMerge, (uint32_t)from, v.sequence, v.data, v.length, now) ==
        vizzz::artnet::MergeResult::Accepted) changed = true;
  }

  uint8_t before = vizzz::artnet::mergerSources(artMerge);
  vizzz::artnet::mergerExpire(artMerge, now, ART_MERGE_TIMEOUT_MS, ARTNET_TIMEOUT_MS);
  if (changed || vizzz::artnet::mergerSources(artMerge) != before) {
    artSourceCount = vizzz::artnet::mergerOutput(artMerge, artVals);  // HTP of up to two sources
    if (changed) lastArtnetMs = now;
  }
}

// ── sACN (E1.31) IN ─────────────────────────────────────────────────────────
static void pollSacn() {
  uint8_t pkt[638]; // enough for E1.31 packet with full universe
  uint32_t now = millis();
  bool changed = false;

  for (uint8_t processed = 0; processed < SACN_PKT_BUDGET; processed++) {
    int size = sacnUdp.parsePacket();
    if (size <= 0) break;
    int n = sacnUdp.read(pkt, min(int(sizeof(pkt)), size));
    sacnUdp.flush();
    if (n <= 0) continue;

    vizzz::sacn::DataView v;
    if (!vizzz::sacn::parseData(pkt, size_t(n), v)) continue;  // root, framing, DMP layers (§5-§7)
    if (v.universe != sacnUniverse()) continue;
    vizzz::sacn::Result r = vizzz::sacn::receiverAccept(sacnRx, v, now);
    if (r == vizzz::sacn::Result::Accepted || r == vizzz::sacn::Result::Terminated) changed = true;
  }

  uint8_t before = vizzz::sacn::receiverSources(sacnRx);
  vizzz::sacn::receiverExpire(sacnRx, now);  // §6.7.1: 2.5 s per source
  if (changed || vizzz::sacn::receiverSources(sacnRx) != before) {
    uint8_t live = vizzz::sacn::receiverOutput(sacnRx, sacnVals);
    lastSacnMs = live ? now : 0;  // terminated or lost: no sACN layer at once
  }
}

// ── USB widget IN (ENTTEC DMX USB PRO API on UART0) ─────────────────────────
// Replies (labels 3, 10) go out as binary on the same UART as the text log, so a
// host reading the log sees binary only after it asked for it.
static void handleUsbFrame(uint8_t label, const uint8_t* d, uint16_t len) {
  static uint8_t reply[5 + 5 + vizzz::widget::kMaxUserConfig];
  switch (label) {
    case vizzz::widget::kLabelSendDmx: {
      if (len < 2 || d[0] != 0x00) return;  // only start code 0 is dimmer data
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(5)) != pdTRUE) return;  // next frame is ~25 ms away
      vizzz::widget::applyDmx(d, len, usbVals);
      xSemaphoreGive(gLock);
      lastUsbMs = millis();
      usbFrames = usbFrames + 1;
      return;
    }
    case vizzz::widget::kLabelGetParams: {
      vizzz::widget::Params prm{USB_WIDGET_FW, uint16_t(dmx_get_break_len(dmxPort)),
                                uint16_t(dmx_get_mab_len(dmxPort)), uint16_t(1000 / DMX_PERIOD_MS)};
      size_t n = vizzz::widget::buildParamsReply(reply, sizeof(reply), prm,
                                                 vizzz::widget::paramsRequestSize(d, len));
      if (n) Serial.write(reply, n);
      return;
    }
    case vizzz::widget::kLabelGetSerial: {
      uint8_t mac[6];
      WiFi.macAddress(mac);  // 24 low MAC bits as 8 decimal digits: stable per board
      size_t n = vizzz::widget::buildSerialReply(reply, sizeof(reply),
                                                 (uint32_t(mac[3]) << 16) | (uint32_t(mac[4]) << 8) | mac[5]);
      if (n) Serial.write(reply, n);
      return;
    }
    case vizzz::widget::kLabelSetParams:  // accepted; the node's DMX timing is its own driver's
    default:
      return;
  }
}

static void pollUsb() {
  static uint32_t lastByteMs = 0;
  const uint32_t now = millis();
  uint8_t buf[256];
  uint16_t budget = USB_BYTES_PER_PASS;
  while (budget) {
    int avail = Serial.available();
    if (avail <= 0) break;
    size_t want = min(size_t(avail), min(sizeof(buf), size_t(budget)));
    size_t n = Serial.read(buf, want);
    if (!n) break;
    vizzz::widget::feed(usbRx, buf, n, handleUsbFrame);
    budget = uint16_t(budget - n);
    lastByteMs = now;
  }
  if (usbRx.rawLen && now - lastByteMs >= USB_FRAME_GAP_MS)
    vizzz::widget::resync(usbRx, handleUsbFrame);  // a cut frame never blocks the next one
}

// ── Desk UI (di.iiii's light desk, served from flash) ────────────────────────
// include/desk_ui_gz.h is generated from desk_ui.lock (tools/fetch_desk_ui.py).
// Each file goes out straight from flash (beginResponse with a uint8_t* is an
// AsyncProgmemResponse in ESPAsyncWebServer 3.6; beginResponse_P is deprecated there),
// already gzipped: no heap copy.
// ETag = the first 16 hex of the raw file's sha256; Cache-Control: no-cache
// makes the browser revalidate, and a matching If-None-Match gets a bodiless 304.
static const DeskUiFile* findDeskFile(const char* path) {
  for (size_t i = 0; i < kDeskUiFileCount; i++)
    if (strcmp(kDeskUiFiles[i].path, path) == 0) return &kDeskUiFiles[i];
  return nullptr;
}

static void sendDeskFile(AsyncWebServerRequest* r, const char* routePath) {
  const DeskUiFile* f = findDeskFile(vizzz::deskFilePath(routePath));
  if (!f) { r->send(404, "text/plain", "Not found"); return; }
  char etag[40];
  snprintf(etag, sizeof(etag), "\"%s\"", f->etag);
  const bool notModified = r->hasHeader("If-None-Match") &&
                           vizzz::etagMatches(r->header("If-None-Match").c_str(), f->etag);
  AsyncWebServerResponse* res = notModified
      ? r->beginResponse(304)
      : r->beginResponse(200, f->mime, f->data, f->len);  // AsyncProgmemResponse: points at flash, no copy
  if (!notModified) res->addHeader("Content-Encoding", "gzip");
  res->addHeader("ETag", etag);
  res->addHeader("Cache-Control", "no-cache");
  r->send(res);
}

static void sendDeskRedirect(AsyncWebServerRequest* r, const char* routePath) {
  const char* to = vizzz::redirectTarget(routePath);
  AsyncWebServerResponse* res = r->beginResponse(301);
  res->addHeader("Location", to ? to : "/");
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

// ── JSON helpers ──────────────────────────────────────────────────────────────
// One CORS rule (audit 2026-10-15 2-M7): Access-Control-Allow-Origin: * goes
// on the JSON reads the route table marks GetShared (/status, /page, /discover,
// /peers, the manifests, /safety/status, /cue/status, /fx/status, /groups,
// /fixtures, /monitor) and nowhere else: not on /wifi/scan, not on any command
// route, not on 4xx answers. di reads /status cross-origin through it.
static void sendJSON(AsyncWebServerRequest* req, const String& body) {
  AsyncWebServerResponse* res = req->beginResponse(200, "application/json", body);
  int ri = vizzz::findRoute(req->url().c_str());
  if (ri >= 0 && vizzz::routeSendsAcao(vizzz::kRoutes[ri].kind))
    res->addHeader("Access-Control-Allow-Origin", "*");
  res->addHeader("Cache-Control", "no-store");
  req->send(res);
}

// Firmware build for /status (1-L4): git short SHA (+dirty) from the
// PlatformIO pre-script tools/pio_fw_version.py, plus the build time.
#ifndef VIZZZ_GIT_SHA
#define VIZZZ_GIT_SHA "nogit"
#endif
static constexpr const char* FW_BUILD = VIZZZ_GIT_SHA " " __DATE__ " " __TIME__;

static const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
  }
}
// One snapshot of the health facts; /status, the /ws push and the manifest
// all print it through vizzz::healthJsonFields.
static vizzz::HealthFacts healthFacts() {
  return vizzz::HealthFacts{ESP.getFreeHeap(), ESP.getMaxAllocHeap(), uint32_t(ws.count())};
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
  IdentitySnap id;
  snapIdentity(id);  // on a busy lock the strings read empty for one push
  jsonEsc(eName,    sizeof(eName),    id.name);
  jsonEsc(eSsid,    sizeof(eSsid),    id.apSsid);
  jsonEsc(eStaSsid, sizeof(eStaSsid), id.staSsid);
  jsonEsc(eMdns,    sizeof(eMdns),    id.mdns);
  uint8_t apClients = WiFi.softAPgetStationNum();
  int32_t staRssi = staCon ? WiFi.RSSI() : 0;
  uint8_t snapPeers = 0;
  if (xSemaphoreTake(gLock, pdMS_TO_TICKS(2)) == pdTRUE) {
    snapPeers = peerCount;
    xSemaphoreGive(gLock);
  }

  char healthFields[96];
  vizzz::healthJsonFields(healthFields, sizeof(healthFields), healthFacts());

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
    "\"safe_slew_step\":%u,"
    "\"ap_fb\":%s,"
    "\"usb_active\":%s,"
    "\"usb_frames\":%lu,"
    "\"usb_baud\":%lu,"
    "\"rst\":\"%s\","
    "%s,"
    "\"fw\":\"%s\""
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
    effectiveSafeSlewStep(),
    apFallbackActive ? "true" : "false",
    usbActive() ? "true" : "false",
    (unsigned long)usbFrames,
    (unsigned long)USB_WIDGET_BAUD,
    resetReasonName(esp_reset_reason()),
    healthFields,
    FW_BUILD
  );
  if (written < 0) {
    buf[0] = '\0';
    return 0;
  }
  if (size_t(written) >= n) return n - 1;
  return size_t(written);
}

// STATUS_JSON_MAX: worst case measured from the format at ~920 bytes (~420
// literal + three 79-byte escaped names + IPs, MAC, numbers); 800 could truncate.
// The USB fields add at most 62 (~982), so 1152 keeps the margin.
static constexpr size_t STATUS_JSON_MAX = 1280;



static String statusJSON() {
  char buf[STATUS_JSON_MAX];
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
  IdentitySnap id;
  snapIdentity(id);
  jsonEsc(eName,    sizeof(eName),    id.name);
  jsonEsc(eSsid,    sizeof(eSsid),    id.apSsid);
  jsonEsc(eStaSsid, sizeof(eStaSsid), id.staSsid);
  jsonEsc(eFw,      sizeof(eFw),      FW_BUILD);

  String apIp = ipStr(WiFi.softAPIP());
  String staIp = staCon ? ipStr(WiFi.localIP()) : "";
  String mac = WiFi.macAddress();
  String outTarget = ipStr(artOutTarget());

  // The http route list comes from vizzz::kRoutes, the table setupWeb()
  // registers from (audit 2026-10-15 2-M4), so it cannot drift from the
  // server. "routes" keeps the v1 shape (paths); "route_table" adds methods,
  // the console header and CORS per route.
  String out;
  out.reserve(7600);
  char buf[1600];
  snprintf(buf, sizeof(buf),
    "{"
    "\"schema\":\"vizzz.di.node.manifest.v1\","
    "\"kind\":\"firmware-node\","
    "\"product\":\"%s\","
    "\"name\":\"%s\","
    "\"firmware\":{\"tag\":\"%s\",\"git\":\"%s\",\"framework\":\"Arduino/PlatformIO\"},"
    "\"identity\":{\"mac\":\"%s\",\"ap_ssid\":\"%s\",\"sta_ssid\":\"%s\"},"
    "\"network\":{\"mode\":\"%s\",\"ap_ip\":\"%s\",\"sta_connected\":%s,\"sta_ip\":\"%s\"},"
    "\"hardware\":{\"board\":\"ESP32 DevKit\",\"dmx_uart\":\"DMX_NUM_1\",\"dmx_tx_gpio\":%d,\"dmx_dir_gpio\":%d,\"max_channels\":%d,\"dmx_period_ms\":%lu},"
    "\"protocols\":["
      "{\"id\":\"http\",\"role\":\"control\",\"console_header\":\"%s: 1\",\"routes\":[",
    PRODUCT_NAME,
    eName,
    eFw, VIZZZ_GIT_SHA,
    mac.c_str(), eSsid, eStaSsid,
    netModeName(netMode), apIp.c_str(), staCon ? "true" : "false", staIp.c_str(),
    DMX_TX, DMX_DIR, MAX_CH, (unsigned long)DMX_PERIOD_MS,
    vizzz::CONSOLE_HEADER);
  out += buf;
  for (size_t i = 0; i < vizzz::kRouteCount; i++) {
    if (i) out += ',';
    out += '"'; out += vizzz::kRoutes[i].path; out += '"';
  }
  out += "],\"route_table\":[";
  for (size_t i = 0; i < vizzz::kRouteCount; i++) {
    const vizzz::RouteKind k = vizzz::kRoutes[i].kind;
    snprintf(buf, sizeof(buf), "%s{\"path\":\"%s\",\"methods\":\"%s\",\"console_header\":%s,\"cors\":%s}",
             i ? "," : "", vizzz::kRoutes[i].path, vizzz::routeMethods(k),
             k == vizzz::RouteKind::Console ? "true" : "false",
             vizzz::routeSendsAcao(k) ? "true" : "false");
    out += buf;
  }
  const uint16_t pa = universe();
  char healthFields[96];
  vizzz::healthJsonFields(healthFields, sizeof(healthFields), healthFacts());
  snprintf(buf, sizeof(buf),
      "]},"
      "{\"id\":\"websocket\",\"role\":\"live-status\",\"endpoint\":\"/ws\",\"push_ms\":400},"
      "{\"id\":\"artnet\",\"role\":\"input-output\",\"port\":%u,\"universe\":%u,\"net\":%u,\"subnet\":%u,\"uni\":%u,\"output_target\":\"%s\",\"output_enabled\":%s},"
      "{\"id\":\"sacn\",\"role\":\"input\",\"port\":%u,\"universe\":%u,\"universe_rule\":\"artnet port-address + 1\"},"
      "{\"id\":\"osc\",\"role\":\"input\",\"transport\":\"udp\",\"port\":%u},"
      "{\"id\":\"usb\",\"role\":\"input\",\"transport\":\"serial\",\"uart\":\"UART0\",\"protocol\":\"ENTTEC DMX USB PRO widget API\",\"baud\":%lu,\"labels\":[3,4,6,10],\"merge\":\"htp-with-artnet-sacn\",\"timeout_ms\":%lu,\"active\":%s,\"frames\":%lu},"
      "{\"id\":\"dmx512\",\"role\":\"physical-output\",\"channels\":%d}"
    "],"
    "\"state\":{\"output_mode\":\"%s\",\"master\":%u,\"artnet_active\":%s,\"sacn_active\":%s,\"web_enabled\":%s,\"fx_mode\":\"%s\",\"fx_enabled\":%s,\"color_enabled\":%s,\"burn_safe\":%s,\"safe_ceiling\":%u,\"safe_slew_step\":%u,%s},"
    "\"sync\":{\"source\":\"firmware\",\"live_status\":\"/ws\",\"durable_config\":\"ESP32 Preferences NVS\",\"git_policy\":\"verify, commit, push origin/main\"}"
    "}",
    ARTNET_PORT, pa, unsigned(pa >> 8) & 0x7f, unsigned(pa >> 4) & 0x0f, unsigned(pa) & 0x0f,
    outTarget.c_str(), artOutEnabled ? "true" : "false",
    SACN_PORT, sacnUniverse(),
    OSC_PORT,
    (unsigned long)USB_WIDGET_BAUD, (unsigned long)USB_TIMEOUT_MS, usbActive() ? "true" : "false", (unsigned long)usbFrames,
    MAX_CH,
    modeName(mode), masterDimmer, artnetActive() ? "true" : "false", sacnActive() ? "true" : "false", webEnabled ? "true" : "false", fxModeName(snapFxMode), snapFxEnabled ? "true" : "false", snapColorEnabled ? "true" : "false", burnSafeMode ? "true" : "false", effectiveSafeMaxLevel(), effectiveSafeSlewStep(), healthFields
  );
  out += buf;
  return out;
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

// ── Route gate (audit 2026-10-15 2-H2) ───────────────────────────────────────
// Every HTTP route is registered through route(), which looks the path up in
// vizzz::kRoutes (include/vizzz_core.h) and applies its kind's method and
// header rule before the handler runs. A path missing from the table is not
// served (and logged), so the table, the manifest and the server cannot drift.
static bool routeRegistered[vizzz::kRouteCount] = {};

static vizzz::HttpMethod httpMethodOf(AsyncWebServerRequest* r) {
  switch (r->method()) {
    case HTTP_GET:     return vizzz::HttpMethod::Get;
    case HTTP_POST:    return vizzz::HttpMethod::Post;
    case HTTP_OPTIONS: return vizzz::HttpMethod::Options;
    default:           return vizzz::HttpMethod::Other;
  }
}

static void sendGateRefusal(AsyncWebServerRequest* r, vizzz::RouteKind kind, vizzz::Gate g) {
  AsyncWebServerResponse* res = nullptr;
  char body[96];
  switch (g) {
    case vizzz::Gate::Preflight:
      res = r->beginResponse(204);
      res->addHeader("Access-Control-Allow-Origin", "*");
      res->addHeader("Access-Control-Allow-Methods", "GET");
      res->addHeader("Access-Control-Max-Age", "600");
      break;
    case vizzz::Gate::PreflightDenied:
      res = r->beginResponse(403, "application/json", "{\"error\":\"no cross-origin access\"}");
      break;
    case vizzz::Gate::NotAllowed:
      snprintf(body, sizeof(body), "{\"error\":\"method not allowed\",\"allow\":\"%s\"}",
               vizzz::routeMethods(kind));
      res = r->beginResponse(405, "application/json", body);
      res->addHeader("Allow", vizzz::routeMethods(kind));
      break;
    case vizzz::Gate::NeedHeader:
      res = r->beginResponse(403, "application/json",
                             "{\"error\":\"console route: send POST with X-Vizzz-Console: 1\"}");
      break;
    default:
      return;
  }
  r->send(res);
}

// 2-M5: strict numeric arguments. need()/opt() parse with vizzz::parseUintArg
// (digits only, in range) and answer 400 with a short JSON error on the first
// missing or bad value; a handler checks ok before it touches any state.
struct ArgReader {
  AsyncWebServerRequest* r;
  bool ok = true;
  explicit ArgReader(AsyncWebServerRequest* req) : r(req) {}
  void fail(const char* name, const char* why, uint32_t lo, uint32_t hi) {
    if (!ok) return;
    ok = false;
    char b[128];
    snprintf(b, sizeof(b), "{\"error\":\"%s\",\"arg\":\"%s\",\"min\":%lu,\"max\":%lu}",
             why, name, (unsigned long)lo, (unsigned long)hi);
    r->send(400, "application/json", b);
  }
  bool opt(const char* name, uint32_t lo, uint32_t hi, uint32_t& out) {
    if (!ok || !r->hasArg(name)) return false;
    if (!vizzz::parseUintArg(r->arg(name).c_str(), lo, hi, &out)) { fail(name, "bad number", lo, hi); return false; }
    return true;
  }
  bool need(const char* name, uint32_t lo, uint32_t hi, uint32_t& out) {
    if (!ok) return false;
    if (!r->hasArg(name)) { fail(name, "missing", lo, hi); return false; }
    return opt(name, lo, hi, out);
  }
};

static void route(const char* path, ArRequestHandlerFunction fn) {
  int i = vizzz::findRoute(path);
  if (i < 0) {
    Serial.printf("[web] route %s is not in vizzz::kRoutes, not served\n", path);
    return;
  }
  routeRegistered[i] = true;
  const vizzz::RouteKind kind = vizzz::kRoutes[i].kind;
  server.on(path, HTTP_ANY, [kind, fn](AsyncWebServerRequest* r){
    bool hdr = r->hasHeader(vizzz::CONSOLE_HEADER) &&
               vizzz::isConsoleHeaderValue(r->header(vizzz::CONSOLE_HEADER).c_str());
    vizzz::Gate g = vizzz::gateRequest(kind, httpMethodOf(r), hdr);
    if (g != vizzz::Gate::Allow) { sendGateRefusal(r, kind, g); return; }
    fn(r);
  });
}

// ── Web routes ────────────────────────────────────────────────────────────────
static void setupWeb() {
  // WebSocket
  ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType, void*, uint8_t*, size_t){});
  // 2-L7: the status stream goes to the node's own console (or a tool that
  // sends no Origin), not to any page open in the operator's browser.
  ws.handleHandshake([](AsyncWebServerRequest* r){
    String origin = r->hasHeader("Origin") ? r->header("Origin") : String();
    String host = r->host();
    return vizzz::wsOriginAllowed(origin.c_str(), host.c_str());
  });
  server.addHandler(&ws);

  route("/", [](AsyncWebServerRequest* r){ sendDeskFile(r, "/"); });
  route("/app.js", [](AsyncWebServerRequest* r){ sendDeskFile(r, "/app.js"); });
  route("/style.css", [](AsyncWebServerRequest* r){ sendDeskFile(r, "/style.css"); });
  route("/qr.js", [](AsyncWebServerRequest* r){ sendDeskFile(r, "/qr.js"); });
  route("/from.js", [](AsyncWebServerRequest* r){ sendDeskFile(r, "/from.js"); });
  route("/show.js", [](AsyncWebServerRequest* r){ sendDeskFile(r, "/show.js"); });
  route("/caps.js", [](AsyncWebServerRequest* r){ sendDeskFile(r, "/caps.js"); });
  route("/control", [](AsyncWebServerRequest* r){ sendDeskRedirect(r, "/control"); });
  route("/patch", [](AsyncWebServerRequest* r){ sendDeskRedirect(r, "/patch"); });
  route("/scenes", [](AsyncWebServerRequest* r){ sendDeskRedirect(r, "/scenes"); });
  route("/vj", [](AsyncWebServerRequest* r){ sendDeskRedirect(r, "/vj"); });
  route("/network", [](AsyncWebServerRequest* r){ sendDeskRedirect(r, "/network"); });
  route("/system", [](AsyncWebServerRequest* r){ sendDeskRedirect(r, "/system"); });
  route("/performance", [](AsyncWebServerRequest* r){ sendDeskRedirect(r, "/performance"); });

  route("/status", [](AsyncWebServerRequest* r){ sendJSON(r, statusJSON()); });
  route("/monitor", [](AsyncWebServerRequest* r){ sendJSON(r, monitorJSON()); });
  route("/safety/status", [](AsyncWebServerRequest* r){ sendJSON(r, safetyJSON()); });
  route("/node/manifest", [](AsyncWebServerRequest* r){ sendJSON(r, nodeManifestJSON()); });
  route("/manifest.json", [](AsyncWebServerRequest* r){ sendJSON(r, nodeManifestJSON()); });

  route("/page", [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t p = 0;
    a.opt("i", 0, PAGE_COUNT - 1, p);
    if (!a.ok) return;
    sendJSON(r, pageJSON(uint16_t(p)));
  });

  auto routeSet = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t chArg = 0, v = 0;
    a.need("ch", 1, MAX_CH, chArg);
    a.need("v", 0, 255, v);
    if (!a.ok) return;
    uint16_t ch = uint16_t(chArg - 1);
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    webVals[ch] = uint8_t(v);
    xSemaphoreGive(gLock);
    r->send(204);
  };
  route("/set", routeSet);

  auto routeBlackout = [](AsyncWebServerRequest* r){
    // A blackout must never be dropped. If the mutex is busy, defer to loop()
    // rather than returning 503 and doing nothing — the UI does not retry, so
    // the old behaviour meant a pressed Blackout button could be a no-op.
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) {
      pendingBlackout = true;
      r->send(202, "text/plain", "queued");
      return;
    }
    applyBlackout_locked();
    xSemaphoreGive(gLock);
    r->send(204);
  };
  route("/blackout", routeBlackout);

  // One atomic panic. The old UI fired /master?v=255 and /blackout as two
  // unordered fetches, so if the blackout lost the race for the mutex the
  // panic button set the rig to full instead of killing it.
  auto routePanic = [](AsyncWebServerRequest* r){
    pendingPanic = true;
    r->send(204);
  };
  route("/panic", routePanic);

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
  route("/full", routeFull);

  auto routeMaster = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t v = 0;
    bool hasV = a.opt("v", 0, 255, v);
    if (!a.ok) return;
    if (hasV) {
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
        r->send(503, "text/plain", "busy");
        return;
      }
      masterDimmer = uint8_t(v);
      xSemaphoreGive(gLock);
    }
    r->send(204);
  };
  route("/master", routeMaster);

  auto routeArtNetSet = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t net = 0, sub = 0, uni = 0;
    bool hasNet = a.opt("net", 0, 127, net);
    bool hasSub = a.opt("subnet", 0, 15, sub);
    bool hasUni = a.opt("uni", 0, 15, uni);
    if (!a.ok) return;
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    if (hasNet) artNet    = uint8_t(net);
    if (hasSub) artSubnet = uint8_t(sub);
    if (hasUni) artUni    = uint8_t(uni);
    xSemaphoreGive(gLock);
    saveConfig();
    // WiFiUDP::stop() frees the RX buffer loop() may be reading in pollSacn():
    // the rebind runs in loop(), never on this task (audit 2026-10-15 2-H4).
    pendingSockRefresh = true;
    r->send(204);
  };
  route("/artnet/set", routeArtNetSet);

  auto routeArtOutSet = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t en = 0;
    bool hasEn = a.opt("en", 0, 1, en);
    if (!a.ok) return;
    if (hasEn) {
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
        r->send(503, "text/plain", "busy");
        return;
      }
      artOutEnabled = en != 0;
      xSemaphoreGive(gLock);
    }
    saveConfig(); r->send(204);
  };
  route("/artout/set", routeArtOutSet);

  auto routeModeSet = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t mV = 0;
    a.need("m", 0, int(MODE_HTP), mV);
    if (!a.ok) return;
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    mode = Mode(mV);
    xSemaphoreGive(gLock);
    saveConfig(); r->send(204);
  };
  route("/mode/set", routeModeSet);

  auto routeModeFallback = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t enV = 0;
    a.need("en", 0, 1, enV);
    if (!a.ok) return;
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    artnetFallbackToWeb = enV != 0;
    xSemaphoreGive(gLock);
    saveConfig();
    r->send(204);
  };
  route("/mode/fallback", routeModeFallback);

  auto routeNetModeSet = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t mV = 0;
    a.need("m", 0, int(NET_AP_ONLY), mV);
    if (!a.ok) return;
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    netMode = NetMode(mV);
    xSemaphoreGive(gLock);
    saveConfig();
    r->send(200, "text/plain", "ok,rebooting");
    pendingReboot = true;
  };
  route("/netmode/set", routeNetModeSet);

  auto routeWebSet = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t enV = 0;
    a.need("en", 0, 1, enV);
    if (!a.ok) return;
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    webEnabled = enV != 0;
    xSemaphoreGive(gLock);
    saveConfig();
    r->send(200, "text/plain", "ok,rebooting");
    pendingReboot = true;
  };
  route("/web/set", routeWebSet);

  auto routeSafetySet = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t burnV = 0;
    a.need("burn", 0, 1, burnV);
    if (!a.ok) return;
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
      r->send(503, "text/plain", "busy");
      return;
    }
    burnSafeMode = burnV != 0;
    xSemaphoreGive(gLock);
    saveConfig();
    r->send(204);
  };
  route("/safety/set", routeSafetySet);

  auto routeSceneSave = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t nV = 0;
    a.need("n", 0, SCENE_COUNT - 1, nV);
    if (!a.ok) return;
    uint8_t n = uint8_t(nV);
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
  route("/scene/save", routeSceneSave);

  auto routeSceneRecall = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t nV = 0;
    uint32_t fadeV = 0;
    a.need("n", 0, SCENE_COUNT - 1, nV);
    bool has_fade = a.opt("fade", 0, FADE_MAX_MS, fadeV);
    if (!a.ok) return;
    uint8_t  n  = uint8_t(nV);
    uint32_t ft = has_fade ? fadeV : 0;
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
  route("/scene/recall", routeSceneRecall);

  route("/wifi/scan", [](AsyncWebServerRequest* r){ sendJSON(r, wifiScanJSON()); });

  auto routeWifiSet = [](AsyncWebServerRequest* r){
    if (!r->hasArg("ssid")) { r->send(400, "application/json", "{\"error\":\"missing\",\"arg\":\"ssid\"}"); return; }
    // 2-L3: an IEEE 802.11 SSID is 1-32 octets; the pass is empty (open
    // network) or a WPA2 pass-phrase, 8-63 printable ASCII (802.11i H.4.1).
    const String& ssidArg = r->arg("ssid");
    if (ssidArg.isEmpty() || ssidArg.length() > 32) {
      r->send(400, "application/json", "{\"error\":\"ssid must be 1-32 bytes\",\"arg\":\"ssid\"}");
      return;
    }
    if (r->hasArg("pass") && !r->arg("pass").isEmpty() &&
        !vizzz::isValidWpaPassphrase(r->arg("pass").c_str(), r->arg("pass").length())) {
      r->send(400, "application/json", "{\"error\":\"pass must be empty or 8-63 printable ASCII\",\"arg\":\"pass\"}");
      return;
    }
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
  route("/wifi/set", routeWifiSet);

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
  route("/wifi/forget", routeWifiForget);

  auto routeNodeSet = [](AsyncWebServerRequest* r){
    // Validate before changing anything (1-H1): a WPA2 pass-phrase is 8-63
    // printable ASCII chars (IEEE 802.11i Annex H.4.1); empty would open the
    // AP, 1-7 would make softAP() refuse and leave an AP-only node unreachable.
    if (r->hasArg("ap_pass")) {
      const String& pw = r->arg("ap_pass");
      if (!vizzz::isValidWpaPassphrase(pw.c_str(), pw.length())) {
        r->send(400, "text/plain", "ap_pass must be 8-63 printable ASCII characters");
        return;
      }
    }
    if (r->hasArg("ap_ssid") && r->arg("ap_ssid").isEmpty()) {
      r->send(400, "text/plain", "ap_ssid must not be empty");
      return;
    }
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
    pendingMdnsRestart = true;  // MDNS.end()/begin() and mdnsName belong to loop()
    r->send(204);
  };
  route("/node/set", routeNodeSet);

  auto routeReboot = [](AsyncWebServerRequest* r){
    r->send(200, "text/plain", "Rebooting…");
    pendingReboot = true;
  };
  route("/reboot", routeReboot);

  // Factory reset: erase NVS and reboot. Restores MAC-derived SSID, default password.
  route("/factory-reset", [](AsyncWebServerRequest* r){
    r->send(200, "text/plain", "Factory reset — rebooting with defaults…");
    pendingFactoryReset = true;
  });

  route("/discover", [](AsyncWebServerRequest* r){
    char buf[256];
    IdentitySnap id;
    if (!snapIdentity(id)) { r->send(503, "text/plain", "busy"); return; }
    char eName[64];   jsonEsc(eName,   sizeof(eName),   id.name);
    char eApSsid[64]; jsonEsc(eApSsid, sizeof(eApSsid), id.apSsid);
    char apIp[16];    ipToCStr(apIp, sizeof(apIp), WiFi.softAPIP());
    String mac = WiFi.macAddress();
    snprintf(buf, sizeof(buf),
      "{\"name\":\"%s\",\"ip\":\"%s\",\"ap_ip\":\"%s\",\"ap_ssid\":\"%s\",\"mdns\":\"%s.local\",\"mac\":\"%s\",\"product\":\"vizzz.di\"}",
      eName, ipStr(WiFi.localIP()).c_str(), apIp, eApSsid, id.mdns, mac.c_str());
    sendJSON(r, buf);  // a read: no side effect (2-L5); the beacon is POST /announce
  });

  // Ask every node on the LAN to answer now instead of within 30 s. Sends one
  // UDP beacon from loop() (discoverUdp belongs to loop(); never this task).
  route("/announce", [](AsyncWebServerRequest* r){
    pendingBeacon = true;
    r->send(204);
  });

  route("/peers", [](AsyncWebServerRequest* r){ sendJSON(r, peersJSON()); });

  route("/artout/peer", [](AsyncWebServerRequest* r){
    // Written here, read by loop() at DMX rate: stored as one atomic word, so
    // there is no String buffer to free under the reader (1-C2). Empty or
    // missing ip clears the peer (broadcast); anything but a dotted quad is 400.
    uint8_t b[4] = {0, 0, 0, 0};
    String ip = r->hasArg("ip") ? r->arg("ip") : String();
    ip.trim();
    if (ip.length() && !vizzz::parseIpv4(ip.c_str(), b)) {
      r->send(400, "text/plain", "ip must be a dotted quad");
      return;
    }
    artOutPeerAddr.store(uint32_t(IPAddress(b[0], b[1], b[2], b[3])), std::memory_order_relaxed);
    r->send(204);
  });

  route("/groups", [](AsyncWebServerRequest* r){ sendJSON(r, groupsJSON()); });

  route("/fixtures", [](AsyncWebServerRequest* r){ sendJSON(r, fixturesJSON()); });

  auto routeFixtureSet = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t iV = 0;
    uint32_t startV = 0;
    uint32_t endV = 0;
    uint32_t xV = 0;
    uint32_t yV = 0;
    uint32_t enV = 0;
    uint32_t countV = 0;
    a.need("i", 0, MAX_FIXTURES - 1, iV);
    bool has_start = a.opt("start", 1, MAX_CH, startV);
    bool has_end = a.opt("end", 1, MAX_CH, endV);
    bool has_x = a.opt("x", 0, 255, xV);
    bool has_y = a.opt("y", 0, 255, yV);
    bool has_en = a.opt("en", 0, 1, enV);
    bool has_count = a.opt("count", 1, MAX_FIXTURES, countV);
    if (!a.ok) return;
    if (has_start && has_end && endV < startV) { a.fail("end", "end before start", startV, MAX_CH); return; }
    uint8_t i = uint8_t(iV);
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    if (r->hasArg("name")) r->arg("name").toCharArray(fixtures[i].name, sizeof(fixtures[i].name));
    if (has_start) fixtures[i].start = uint16_t(startV);
    if (has_end)   fixtures[i].end   = uint16_t(endV);
    // 2-L6: a start past the old end moves the end with it; start <= end always.
    if (fixtures[i].end < fixtures[i].start) fixtures[i].end = fixtures[i].start;
    if (has_x) fixtures[i].x = uint8_t(xV);
    if (has_y) fixtures[i].y = uint8_t(yV);
    if (has_en) fixtures[i].enabled = enV != 0;
    if (has_count) fixtureCount = uint8_t(countV);
    for (uint8_t n = 0; n < MAX_FIXTURES; n++) fixtures[n].enabled = (n < fixtureCount) ? fixtures[n].enabled : false;
    xSemaphoreGive(gLock);
    saveFixtures();
    r->send(204);
  };
  route("/fixture/set", routeFixtureSet);

  auto routeGroupSet = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t gV = 0;
    uint32_t startV = 0;
    uint32_t endV = 0;
    uint32_t enV = 0;
    a.need("g", 0, MAX_GROUPS - 1, gV);
    bool has_start = a.opt("start", 1, MAX_CH, startV);
    bool has_end = a.opt("end", 1, MAX_CH, endV);
    bool has_en = a.opt("en", 0, 1, enV);
    if (!a.ok) return;
    if (has_start && has_end && endV < startV) { a.fail("end", "end before start", startV, MAX_CH); return; }
    uint8_t g = uint8_t(gV);
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    if (r->hasArg("name")) {
      String n = r->arg("name");
      n.toCharArray(groups[g].name, sizeof(groups[g].name));
    }
    if (has_start) groups[g].start = uint16_t(startV);
    if (has_end)   groups[g].end   = uint16_t(endV);
    if (groups[g].end < groups[g].start) groups[g].end = groups[g].start;  // 2-L6
    if (has_en) groups[g].enabled = enV != 0;
    xSemaphoreGive(gLock);
    saveVj();
    r->send(204);
  };
  route("/group/set", routeGroupSet);

  auto routeGroupApply = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t gV = 0;
    uint32_t vV = 0;
    a.need("g", 0, MAX_GROUPS - 1, gV);
    a.need("v", 0, 255, vV);
    if (!a.ok) return;
    uint8_t g = uint8_t(gV);
    uint8_t v = uint8_t(vV);
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
  route("/group/apply", routeGroupApply);

  route("/cue/status", [](AsyncWebServerRequest* r){ sendJSON(r, cueJSON()); });

  auto routeCueCount = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t cV = 0;
    a.need("c", 1, MAX_CUES, cV);
    if (!a.ok) return;
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    cueCount = uint8_t(cV);
    if (cueIndex >= cueCount) cueIndex = 0;
    xSemaphoreGive(gLock);
    saveVj();
    r->send(204);
  };
  route("/cue/count", routeCueCount);

  auto routeCueSet = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t iV = 0;
    uint32_t sceneV = 0;
    uint32_t dwellV = 0;
    uint32_t fadeV = 0;
    a.need("i", 0, MAX_CUES - 1, iV);
    bool has_scene = a.opt("scene", 0, SCENE_COUNT - 1, sceneV);
    bool has_dwell = a.opt("dwell", 0, FADE_MAX_MS, dwellV);
    bool has_fade = a.opt("fade", 0, FADE_MAX_MS, fadeV);
    if (!a.ok) return;
    uint8_t i = uint8_t(iV);
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    if (has_scene) cueList[i].scene = uint8_t(sceneV);
    // Clamped at set time to what loadVj() re-clamps on boot (1-L5), and the
    // step to at least CUE_MIN_STEP_MS (1-M1): 0/0 re-fired a scene per loop().
    if (has_dwell) cueList[i].dwellMs = vizzz::clampCueTimeMs(long(dwellV), FADE_MAX_MS);
    if (has_fade)  cueList[i].fadeMs  = vizzz::clampCueTimeMs(long(fadeV),  FADE_MAX_MS);
    if (cueList[i].fadeMs + cueList[i].dwellMs < vizzz::CUE_MIN_STEP_MS)
      cueList[i].dwellMs = vizzz::CUE_MIN_STEP_MS - cueList[i].fadeMs;
    xSemaphoreGive(gLock);
    saveVj();
    r->send(204);
  };
  route("/cue/set", routeCueSet);

  auto routeCueRun = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t enV = 0;
    a.need("en", 0, 1, enV);
    if (!a.ok) return;
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    cueRunning = (enV != 0) && cueCount > 0;
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
  route("/cue/run", routeCueRun);

  auto routeCueNext = [](AsyncWebServerRequest* r){
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    if (cueCount == 0) { xSemaphoreGive(gLock); r->send(400, "text/plain", "no cues"); return; }
    cueIndex = (cueIndex + 1) % cueCount;
    uint8_t next = cueIndex;
    xSemaphoreGive(gLock);
    triggerCueStep(next);
    r->send(204);
  };
  route("/cue/next", routeCueNext);

  route("/fx/status", [](AsyncWebServerRequest* r){ sendJSON(r, fxJSON()); });

  auto routeFxSet = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t enV = 0;
    uint32_t bpmV = 0;
    uint32_t depthV = 0;
    bool has_en = a.opt("en", 0, 1, enV);
    bool has_bpm = a.opt("bpm", 20, 240, bpmV);
    bool has_depth = a.opt("depth", 0, 255, depthV);
    if (!a.ok) return;
    static const char* const kFxNames[] = {"none","strobe","chase","pulse","sine","sparkle","comet","bars","glitch","radar"};
    if (r->hasArg("mode")) {
      const String& m = r->arg("mode");
      bool known = false;
      for (uint8_t k = 0; k < 10; k++)
        if (m == kFxNames[k] || (m.length() == 1 && m[0] == char('0' + k))) known = true;
      if (!known) { a.fail("mode", "unknown fx mode", 0, 9); return; }
    }
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
    if (has_en) fxEnabled = enV != 0;
    if (has_bpm) fxBpm = uint16_t(bpmV);
    if (has_depth) fxDepth = uint8_t(depthV);
    if (fxMode == FX_NONE) fxEnabled = false;
    xSemaphoreGive(gLock);
    r->send(204);
  };
  route("/fx/set", routeFxSet);

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
  route("/fx/tap", routeFxTap);

  auto routeColorSet = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t rV = 0;
    uint32_t gV = 0;
    uint32_t bV = 0;
    uint32_t enV = 0;
    bool has_r = a.opt("r", 0, 255, rV);
    bool has_g = a.opt("g", 0, 255, gV);
    bool has_b = a.opt("b", 0, 255, bV);
    bool has_en = a.opt("en", 0, 1, enV);
    if (!a.ok) return;
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) != pdTRUE) { r->send(503, "text/plain", "busy"); return; }
    if (has_r) colorR = uint8_t(rV);
    if (has_g) colorG = uint8_t(gV);
    if (has_b) colorB = uint8_t(bV);
    if (has_en) colorEnabled = enV != 0;
    if (colorEnabled) applyColorWash_locked();
    xSemaphoreGive(gLock);
    r->send(204);
  };
  route("/color/set", routeColorSet);

  // ── Peer proxy (forward a command to one specific known peer) ────────────────
  route("/peer/cmd", [](AsyncWebServerRequest* r){
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
    // Never dropped on a busy lock (1-M5): the local blackout is deferred to
    // loop() exactly as /blackout does, and the fleet broadcast is queued
    // either way.
    bool local = xSemaphoreTake(gLock, pdMS_TO_TICKS(20)) == pdTRUE;
    if (local) {
      applyBlackout_locked();
      xSemaphoreGive(gLock);
    } else {
      pendingBlackout = true;
    }
    if (!enqueueBcast("/blackout")) { r->send(503, "text/plain", "local blackout done; fleet queue full"); return; }
    r->send(local ? 204 : 202);
  };
  route("/net/blackout", routeNetBlackout);

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
  route("/net/full", routeNetFull);

  auto routeNetMaster = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t v = 0;
    bool hasV = a.opt("v", 0, 255, v);
    if (!a.ok) return;
    if (hasV) {
      if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) != pdTRUE) {
        r->send(503, "text/plain", "busy");
        return;
      }
      masterDimmer = uint8_t(v);
      xSemaphoreGive(gLock);
    }
    char path[24];
    snprintf(path, sizeof(path), "/master?v=%u", masterDimmer);
    if (!enqueueBcast(path)) { r->send(503, "text/plain", "queue full"); return; }
    r->send(204);
  };
  route("/net/master", routeNetMaster);

  auto routeNetSceneRecall = [](AsyncWebServerRequest* r){
    ArgReader a(r);
    uint32_t nV = 0;
    uint32_t fadeV = 0;
    a.need("n", 0, SCENE_COUNT - 1, nV);
    bool has_fade = a.opt("fade", 0, FADE_MAX_MS, fadeV);
    if (!a.ok) return;
    uint8_t  n  = uint8_t(nV);
    uint32_t ft = has_fade ? fadeV : 0;
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
  route("/net/scene/recall", routeNetSceneRecall);

  for (size_t i = 0; i < vizzz::kRouteCount; i++)
    if (!routeRegistered[i])
      Serial.printf("[web] route table entry %s has no handler\n", vizzz::kRoutes[i].path);

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

  // UART0 = USB serial: the text log AND the ENTTEC widget protocol (README "USB").
  Serial.setRxBufferSize(USB_RX_BUFFER);  // before begin(), or the 256-byte default stays
  Serial.begin(USB_WIDGET_BAUD);

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
  memset(usbVals,  0, MAX_CH);
  vizzz::widget::parserReset(usbRx);
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
  lastUsbMs = 0;

  // Task watchdog on the DMX path (1-M8). The Arduino loop task is not
  // subscribed by default (core 2.0.17 esp32-hal-misc.c enableLoopWDT());
  // once it is, the core feeds it after every loop() return, and a loop()
  // stuck for CONFIG_ESP_TASK_WDT_TIMEOUT_S (5 s, panic on) reboots the node
  // instead of freezing DMX on its last frame. /status "rst" shows why the
  // last boot happened (esp_reset_reason(), ESP-IDF "Watchdogs" reference).
  enableLoopWDT();

  // The web task is already running: read the shared strings via a snapshot.
  IdentitySnap id;
  snapIdentity(id, pdMS_TO_TICKS(100));
  Serial.printf("\n=== vizzz.di ===\n");
  Serial.printf("AP SSID  : %s\n", id.apSsid);
  Serial.printf("AP IP    : %s\n", ipStr(WiFi.softAPIP()).c_str());
  Serial.printf("mDNS     : http://%s.local\n", id.mdns);
  if (id.staSsid[0]) {
    Serial.printf("STA SSID : %s\n", id.staSsid);
    if (WiFi.status() == WL_CONNECTED)
      Serial.printf("STA IP   : %s\n", ipStr(WiFi.localIP()).c_str());
  }
  Serial.printf("Mode     : %s\n", modeName(mode));
  Serial.printf("Net Mode : %s\n", netModeName(netMode));
  Serial.printf("Art-Net  : net=%u sub=%u uni=%u (15-bit=%u)\n",
    artNet, artSubnet, artUni, universe());
  Serial.printf("sACN     : universe=%u port=%u\n", sacnUniverse(), SACN_PORT);
  Serial.printf("Art-Out  : %s\n", artOutEnabled ? "enabled" : "disabled");
  Serial.printf("Web      : %s\n", webEnabled ? "enabled" : "disabled");
  Serial.printf("OSC IN   : udp/%u\n", OSC_PORT);
  if (strcmp(id.apPass, "changeme123") == 0) {
    Serial.printf("WARNING  : placeholder AP password in use.\n");
    Serial.printf("           cp src/secrets.example.h src/secrets.h, set your\n");
    Serial.printf("           own AP_PASS_DEFAULT, then reflash.\n");
  }
}

// STA join supervision, called from loop() (1-M6, 1-M9).
// - STA_ONLY: if the boot join has not succeeded within STA_JOIN_WINDOW_MS,
//   bring up the temporary AP so the node stays reachable (was a blocking wait).
// - While the soft-AP is up, a STA that cannot join keeps scanning, and the AP
//   follows the STA's channel (ESP-IDF Wi-Fi guide, station/AP coexistence):
//   the operator's phone drops. After a failed window, park the STA
//   (auto-reconnect off, disconnect) and retry after vizzz::staRetryDelayMs()
//   (30 s doubling to 300 s), and only when no client is on the AP.
static void serviceStaJoin(uint32_t now) {
  static uint32_t lastRunMs = 0;
  if ((uint32_t)(now - lastRunMs) < 250) return;
  lastRunMs = now;
  if (netMode == NET_AP_ONLY) return;

  if (WiFi.status() == WL_CONNECTED) {
    if (staFailedJoins || staParked) Serial.println("STA: joined");
    staFailedJoins = 0;
    staParked = false;
    staAttemptMs = now;
    staBootResolved = true;
    return;
  }

  IdentitySnap cred;
  if (!snapIdentity(cred, pdMS_TO_TICKS(5))) return;
  if (!cred.staSsid[0]) { staParked = false; return; }

  if (!staParked) {
    if ((uint32_t)(now - staAttemptMs) < STA_JOIN_WINDOW_MS) return;
    if (netMode == NET_STA_ONLY && !staBootResolved) {
      Serial.println("STA_ONLY: no join in 10 s, temporary AP up");
      WiFi.mode(WIFI_AP_STA);
      startSoftAP();
    }
    staBootResolved = true;
    if (staFailedJoins < 255) staFailedJoins++;
    if (WiFi.getMode() & WIFI_MODE_AP) {
      WiFi.setAutoReconnect(false);
      WiFi.disconnect(false);
      staParked = true;
      staParkedMs = now;
      Serial.printf("STA: join failed (%u), next try in %lu s\n", staFailedJoins,
                    (unsigned long)(vizzz::staRetryDelayMs(staFailedJoins) / 1000));
    } else {
      staAttemptMs = now;  // STA only, no AP to protect: keep core auto-reconnect
    }
    return;
  }

  if (!vizzz::elapsedAtLeast(now, staParkedMs, vizzz::staRetryDelayMs(staFailedJoins))) return;
  if (WiFi.softAPgetStationNum() > 0) return;  // never pull the AP from under a client
  WiFi.setAutoReconnect(true);
  WiFi.begin(cred.staSsid, cred.staPass);
  staParked = false;
  staAttemptMs = now;
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
    esp_task_wdt_reset();  // four NVS namespace erases in one pass
    Preferences vj;
    vj.begin("vj", false);
    vj.clear();
    vj.end();
    delay(80);
    ESP.restart();
  }

  // Bounded waits (1-M8): a stuck lock holder must not hang loop() forever;
  // the flag stays set and the next pass retries, and the task watchdog
  // reboots a loop() that stops returning.
  if (pendingPanic) {
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(500)) == pdTRUE) {
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
    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(500)) == pdTRUE) {
      pendingBlackout = false;
      applyBlackout_locked();
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
    IdentitySnap cred;
    if (snapIdentity(cred)) {
      WiFi.setAutoReconnect(true);
      WiFi.begin(cred.staSsid, cred.staPass);
      staParked = false;       // new credentials: fresh join window, no back-off
      staFailedJoins = 0;
      staAttemptMs = millis();
    } else {
      pendingWifiReconnect = true;  // lock busy: retry next pass
    }
  }

  if (pendingMdnsRestart) {
    pendingMdnsRestart = false;
    startMdns();
  }

  if (pendingBeacon) {
    pendingBeacon = false;
    sendBeacon();
  }

  pollArtNet();
  pollSacn();
  pollUsb();
  pollOsc();

  // Rebind Art-Net/sACN only when the STA link comes up or goes down (the
  // multicast join needs it), not on DISCONNECTED <-> NO_SSID_AVAIL flaps
  // while retrying: each rebind drops Art-Net arriving via the AP (1-L9).
  // A universe change from /artnet/set is serviced here too (2-H4).
  static bool prevStaUp = false;
  bool staUp = (WiFi.status() == WL_CONNECTED);
  bool linkEdge = (staUp != prevStaUp);
  prevStaUp = staUp;
  if (linkEdge && staUp) { startMdns(); sendBeacon(); }
  if (linkEdge) refreshArtNetSocket();
  if (linkEdge || pendingSockRefresh) {
    pendingSockRefresh = false;
    refreshSacnSocket();  // re-joins the multicast group for the new universe
  }

  const uint32_t now = millis();
  serviceStaJoin(now);
  tickCueEngine(now);

  // DMX output cycle
  static uint32_t lastDmx = 0;
  if (now - lastDmx >= DMX_PERIOD_MS) {
    lastDmx = now;

    if (xSemaphoreTake(gLock, pdMS_TO_TICKS(10)) == pdTRUE) {
      updateFade_locked();
      computeOutput_locked(artnetActive(), sacnActive(), usbActive(), now);
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
        char statusBuf[STATUS_JSON_MAX];
        size_t len = fillStatusJSON(statusBuf, sizeof(statusBuf));
        ws.textAll(statusBuf, len);
      }
      ws.cleanupClients();
    }
  }
}
