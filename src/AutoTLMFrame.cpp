/*
 * AutoTLMFrame.cpp — frame reset + JSON serialization.
 * Part of AutoTLM Core — MIT licensed.
 */
#include "AutoTLMFrame.h"

#include <stdarg.h>
#include <string.h>

#include "core/AutoTLMPids.h"

void AutoTLMFrame::clear() {
  memset(this, 0, sizeof(*this));
  rssi = 0;
  volts = 0.0f;
}

// Bounds-checked printf-append. Keeps `len` honest even when snprintf would
// have overflowed, so a too-small buffer degrades to truncated-but-terminated
// JSON instead of memory corruption.
static void jcat(char* buf, size_t cap, size_t& len, const char* fmt, ...) {
  if (len + 1 >= cap) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + len, cap - len, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  len += ((size_t)n < cap - len) ? (size_t)n : (cap - len - 1);
}

// Append a string as JSON string *content*: escape the two characters that
// can break out of a quoted string and drop control bytes. Device ids and
// VINs come from buses and user code — one stray '"' must not invalidate
// every frame of a session.
static void jstr(char* buf, size_t cap, size_t& len, const char* s) {
  for (; s && *s; s++) {
    const unsigned char c = (unsigned char)*s;
    if (c < 0x20) continue;
    if (c == '"' || c == '\\') jcat(buf, cap, len, "\\%c", c);
    else jcat(buf, cap, len, "%c", c);
  }
}

size_t AutoTLMFrame::toJson(char* buf, size_t cap) const {
  if (!buf || cap < 2) return 0;
  size_t len = 0;

  jcat(buf, cap, len, "{\"source\":\"device\",\"device\":{\"id\":\"");
  jstr(buf, cap, len, deviceId);
  jcat(buf, cap, len, "\",\"type\":\"");
  jstr(buf, cap, len, deviceType);
  jcat(buf, cap, len, "\",\"mems\":\"");
  jstr(buf, cap, len, mems);
  jcat(buf, cap, len, "\",\"fw_gnss\":\"%s\",\"rssi\":%d,\"modules\":%d},",
       gnssUp ? "OK" : "NO", rssi, (int)moduleCount);

  // Offline catch-up frames carry their capture-to-send age so ingest can
  // reconstruct capture time (ts = receivedAt - age_ms). Live frames omit it.
  if (ageMs > 0) jcat(buf, cap, len, "\"age_ms\":%lu,", (unsigned long)ageMs);

  // Contract rule (AutoTLM Cloud API.md): sub-objects that have nothing real
  // to say are OMITTED — never emitted zero-filled. No ECU answering = no
  // "obd", no fix = no "gps", no IMU fitted = no "imu", no codes and no MIL
  // = no "dtc". Consumers null-check; a zeroed lat/lng is a lie on a map.

  if (obdConnected) {
    jcat(buf, cap, len,
         "\"obd\":{\"connected\":true,\"speed_kph\":%d,\"rpm\":%d,\"coolant_c\":%d,"
         "\"load_pct\":%d,\"throttle_pct\":%d,\"volts\":%.1f,\"vin\":\"",
         speedKph, rpm, coolantC, loadPct, throttlePct, volts);
    jstr(buf, cap, len, vin);
    jcat(buf, cap, len, "\",\"pids\":{");

    bool first = true;
    for (int p = 0; p < 256; p++) {
      if (!pidHave[p]) continue;
      // Fixed-point PIDs (trims, O2 volts, lambda, timing) emit with their
      // decimals restored; everything else stays a bare integer.
      const uint8_t d = autotlm::pidDecimals((uint8_t)p);
      if (d == 0) {
        jcat(buf, cap, len, "%s\"%02X\":%d", first ? "" : ",", p, pidVal[p]);
      } else {
        int div = 1;
        for (uint8_t k = 0; k < d; k++) div *= 10;
        jcat(buf, cap, len, "%s\"%02X\":%.*f", first ? "" : ",", p, (int)d,
             (double)pidVal[p] / div);
      }
      first = false;
    }
    jcat(buf, cap, len, "}");

    // What the car advertises (supported-PID bitmasks) — the sensor picker's
    // menu; a superset of the keys in pids.
    if (supportedCount > 0) {
      jcat(buf, cap, len, ",\"supported\":[");
      for (int i = 0; i < supportedCount; i++)
        jcat(buf, cap, len, "%s\"%02X\"", i ? "," : "", supported[i]);
      jcat(buf, cap, len, "]");
    }
    jcat(buf, cap, len, "},");
  }

  if (mil || dtc[0]) {
    // dtc: "P0171,P0420" -> ["P0171","P0420"]
    jcat(buf, cap, len, "\"dtc\":{\"mil\":%s,\"codes\":[", mil ? "true" : "false");
    const char* p = dtc;
    bool firstCode = true;
    while (*p) {
      const char* comma = strchr(p, ',');
      size_t n = comma ? (size_t)(comma - p) : strlen(p);
      jcat(buf, cap, len, "%s\"%.*s\"", firstCode ? "" : ",", (int)n, p);
      firstCode = false;
      p += n;
      if (*p == ',') p++;
    }
    jcat(buf, cap, len, "]");

    // Freeze frame: the sensor state from when freezeCode set, keyed by code
    // (a map, so richer multi-code freeze data stays contract-compatible).
    if (freezeCount > 0 && freezeCode[0]) {
      jcat(buf, cap, len, ",\"freeze\":{\"");
      jstr(buf, cap, len, freezeCode);
      jcat(buf, cap, len, "\":{");
      for (int i = 0; i < freezeCount; i++) {
        const uint8_t d = autotlm::pidDecimals(freezePid[i]);
        if (d == 0) {
          jcat(buf, cap, len, "%s\"%02X\":%d", i ? "," : "", freezePid[i], freezeVal[i]);
        } else {
          int div = 1;
          for (uint8_t k = 0; k < d; k++) div *= 10;
          jcat(buf, cap, len, "%s\"%02X\":%.*f", i ? "," : "", freezePid[i],
               (int)d, (double)freezeVal[i] / div);
        }
      }
      jcat(buf, cap, len, "}}");
    }
    jcat(buf, cap, len, "},");
  }

  if (fix) {
    // source:"internal" = the device's own GNSS composed this fix. "phone" is
    // written only by AutoTLM Cloud when it merges phone GPS (its merge rule
    // keys on this field); the device never emits it.
    jcat(buf, cap, len,
         "\"gps\":{\"fix\":true,\"source\":\"internal\",\"lat\":%.6f,\"lng\":%.6f,"
         "\"alt_m\":%.1f,\"speed_kph\":%.1f,\"course\":%.0f,\"sats\":%d,\"hdop\":%.2f},",
         lat, lng, altM, gpsSpeedKph, course, sats, hdop);
  }

  if (imuHave) {
    jcat(buf, cap, len,
         "\"imu\":{\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,\"gx\":%.1f,\"gy\":%.1f,\"gz\":%.1f},",
         ax, ay, az, gx, gy, gz);
  }

  // Every section above ends with ',' so sections can drop out freely; the
  // frame always has at least source+device, so swap the last ',' for '}'.
  if (len && buf[len - 1] == ',') buf[len - 1] = '}';
  else jcat(buf, cap, len, "}");

  return len;
}

size_t AutoTLMFrame::toJsonLive(char* buf, size_t cap) const {
  // 96 is the floor for a complete `{device}` object even with the longest
  // (19-char) id; below it we can't guarantee valid JSON, so return nothing.
  if (!buf || cap < 96) { if (buf && cap) buf[0] = 0; return 0; }
  size_t len = 0;

  // A compact frame for the LOCAL BLE telemetry stream: the same field names
  // as toJson (the app reuses its parser) but trimmed to fit ONE BLE notify at
  // the NEGOTIATED MTU (the caller passes MTU-3 as `cap`). Dropped vs the full
  // frame: the bulky `obd.supported` and `dtc.freeze`. Priority under a tight
  // budget: gauges > dtc + gps > extra PIDs — so gps (map data, and the reason
  // this stream is auth-gated) is NEVER the silent sacrifice; PIDs yield first.
  // Every section is room-guarded, so the result is ALWAYS valid JSON <= cap.

  jcat(buf, cap, len, "{\"source\":\"device\",\"live\":true,\"device\":{\"id\":\"");
  jstr(buf, cap, len, deviceId);
  jcat(buf, cap, len, "\",\"modules\":%d},", (int)moduleCount);

  // Reserve the ACTUAL tail (dtc + gps + closer) so the PID fill can't crowd
  // them out. dtc: "dtc":{"mil":false,"codes":[<comma-list>]}, ~ strlen+30.
  // gps: the fixed block ~ 110. closer + slack: 8.
  size_t reserve = 8;
  if (mil || dtc[0]) reserve += strlen(dtc) + 30;
  if (fix) reserve += 110;

  if (obdConnected && len + 130 < cap) {
    jcat(buf, cap, len,
         "\"obd\":{\"connected\":true,\"speed_kph\":%d,\"rpm\":%d,\"coolant_c\":%d,"
         "\"load_pct\":%d,\"throttle_pct\":%d,\"volts\":%.1f,\"pids\":{",
         speedKph, rpm, coolantC, loadPct, throttlePct, volts);
    bool first = true;
    for (int p = 0; p < 256; p++) {
      if (!pidHave[p]) continue;
      if (len + reserve + 16 >= cap) break;  // never eat the dtc/gps tail room
      const uint8_t d = autotlm::pidDecimals((uint8_t)p);
      if (d == 0) {
        jcat(buf, cap, len, "%s\"%02X\":%d", first ? "" : ",", p, pidVal[p]);
      } else {
        int div = 1;
        for (uint8_t k = 0; k < d; k++) div *= 10;
        jcat(buf, cap, len, "%s\"%02X\":%.*f", first ? "" : ",", p, (int)d,
             (double)pidVal[p] / div);
      }
      first = false;
    }
    jcat(buf, cap, len, "}},");
  }

  if ((mil || dtc[0]) && len + (size_t)strlen(dtc) + 30 < cap) {
    jcat(buf, cap, len, "\"dtc\":{\"mil\":%s,\"codes\":[", mil ? "true" : "false");
    const char* p = dtc;
    bool firstCode = true;
    while (*p && len + 24 < cap) {
      const char* comma = strchr(p, ',');
      size_t n = comma ? (size_t)(comma - p) : strlen(p);
      jcat(buf, cap, len, "%s\"%.*s\"", firstCode ? "" : ",", (int)n, p);
      firstCode = false;
      p += n;
      if (*p == ',') p++;
    }
    jcat(buf, cap, len, "]},");
  }

  if (fix && len + 110 < cap) {
    jcat(buf, cap, len,
         "\"gps\":{\"fix\":true,\"source\":\"internal\",\"lat\":%.6f,\"lng\":%.6f,"
         "\"speed_kph\":%.1f,\"course\":%.0f},",
         lat, lng, gpsSpeedKph, course);
  }

  if (len && buf[len - 1] == ',') buf[len - 1] = '}';
  else jcat(buf, cap, len, "}");

  return len;
}
