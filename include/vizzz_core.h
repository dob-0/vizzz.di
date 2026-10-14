#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vizzz {

constexpr uint16_t packUniverse(uint8_t artNet, uint8_t artSubnet, uint8_t artUni) {
  return (uint16_t(artNet) << 8) | (uint16_t(artSubnet) << 4) | (artUni & 0x0F);
}

constexpr uint8_t applyMaster(uint8_t value, uint8_t master) {
  return master >= 255 ? value : uint8_t((uint16_t(value) * master) >> 8);
}

constexpr uint8_t clampLevel(uint8_t value, uint8_t ceiling) {
  return value > ceiling ? ceiling : value;
}

inline uint8_t slewToward(uint8_t current, uint8_t target, uint8_t step) {
  if (step == 0 || current == target) return target;
  if (target > current) {
    uint16_t next = uint16_t(current) + step;
    return next > target ? target : uint8_t(next);
  }
  uint16_t down = uint16_t(target) + step;
  return down < current ? uint8_t(current - step) : target;
}

// ── Wi-Fi credentials ─────────────────────────────────────────────────────────
// IEEE 802.11i-2004 Annex H.4.1 (carried into IEEE 802.11-2020 Annex J.4.1): a
// WPA/WPA2-Personal pass-phrase is 8..63 ASCII characters, each in 32..126.
// arduino-esp32 2.0.17 WiFi.softAP() returns false for 1..7 chars
// (WiFiAP.cpp:145) and opens the AP for an empty one; neither may be stored.
inline bool isValidWpaPassphrase(const char* s, size_t len) {
  if (!s || len < 8 || len > 63) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 32 || c > 126) return false;
  }
  return true;
}

// ── Host name ─────────────────────────────────────────────────────────────────
// RFC 1123 §2.1 / RFC 952 host label: letters, digits and '-', no leading or
// trailing '-'. Everything else becomes '-'. Output is NUL-terminated and at
// most outSize-1 chars; an empty result becomes `fallback`. Returns the length.
inline size_t sanitizeHostLabel(const char* in, char* out, size_t outSize, const char* fallback) {
  if (!out || outSize == 0) return 0;
  size_t w = 0;
  for (size_t i = 0; in && in[i] != '\0' && w + 1 < outSize; i++) {
    char c = in[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    char o = ok ? c : '-';
    if (w == 0 && o == '-') continue;  // no leading hyphen
    out[w++] = o;
  }
  while (w > 0 && out[w - 1] == '-') w--;  // no trailing hyphen
  out[w] = '\0';
  if (w == 0 && fallback) {
    for (; fallback[w] != '\0' && w + 1 < outSize; w++) out[w] = fallback[w];
    out[w] = '\0';
  }
  return w;
}

// ── IPv4 text ─────────────────────────────────────────────────────────────────
// Strict dotted quad "a.b.c.d", each 0..255 in decimal, nothing else.
inline bool parseIpv4(const char* s, uint8_t out[4]) {
  if (!s) return false;
  for (int part = 0; part < 4; part++) {
    if (*s < '0' || *s > '9') return false;
    uint16_t v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
      v = uint16_t(v * 10 + (*s - '0'));
      if (++digits > 3 || v > 255) return false;
      s++;
    }
    out[part] = uint8_t(v);
    if (part < 3) {
      if (*s != '.') return false;
      s++;
    }
  }
  return *s == '\0';
}

// ── Cue timing ────────────────────────────────────────────────────────────────
// A cue step shorter than this would re-trigger a scene (an NVS read and a new
// fade) on every loop() pass — scenes alternating at frame rate (audit 1-M1).
constexpr uint32_t CUE_MIN_STEP_MS = 50;

// Clamp a requested dwell or fade (ms, possibly negative from toInt()) to 0..maxMs.
constexpr uint32_t clampCueTimeMs(long requested, uint32_t maxMs) {
  return requested <= 0 ? 0u : (uint32_t(requested) > maxMs ? maxMs : uint32_t(requested));
}

// Length of one cue step: fade + dwell, never below CUE_MIN_STEP_MS, never wraps.
constexpr uint32_t cueStepMs(uint32_t fadeMs, uint32_t dwellMs) {
  return (uint64_t(fadeMs) + dwellMs) < CUE_MIN_STEP_MS
             ? CUE_MIN_STEP_MS
             : ((uint64_t(fadeMs) + dwellMs) > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(fadeMs + dwellMs));
}

// True when at least `ms` have passed since `since`. Delta form, so a
// millis() wrap (every 49.7 days) is harmless.
constexpr bool elapsedAtLeast(uint32_t now, uint32_t since, uint32_t ms) {
  return uint32_t(now - since) >= ms;
}

// True when the running cue step is over.
constexpr bool cueStepDue(uint32_t now, uint32_t startedMs, uint32_t stepMs) {
  return elapsedAtLeast(now, startedMs, stepMs);
}

// ── STA re-join back-off ──────────────────────────────────────────────────────
// In AP+STA the soft-AP follows the STA's channel (ESP-IDF Wi-Fi guide,
// "Station/AP coexistence"), so every STA scan for an absent router moves the
// AP and drops the operator's phone (audit 1-M9). After a failed join, wait
// 30 s, then 60, 120, capped at 300 s, before the next attempt.
constexpr uint32_t STA_RETRY_BASE_MS = 30000;
constexpr uint32_t STA_RETRY_MAX_MS  = 300000;
inline uint32_t staRetryDelayMs(uint8_t failedJoins) {
  if (failedJoins == 0) return 0;
  uint32_t d = STA_RETRY_BASE_MS;
  for (uint8_t i = 1; i < failedJoins && d < STA_RETRY_MAX_MS; i++) d <<= 1;
  return d > STA_RETRY_MAX_MS ? STA_RETRY_MAX_MS : d;
}

}  // namespace vizzz
