/*
 * AutoTLMFrame.cpp — frame reset + JSON serialization.
 * Part of AutoTLM Core — MIT licensed.
 */
#include "AutoTLMFrame.h"

#include <stdarg.h>
#include <string.h>

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

  jcat(buf, cap, len,
       "\"obd\":{\"connected\":%s,\"speed_kph\":%d,\"rpm\":%d,\"coolant_c\":%d,"
       "\"load_pct\":%d,\"throttle_pct\":%d,\"volts\":%.1f,\"vin\":\"",
       obdConnected ? "true" : "false", speedKph, rpm, coolantC, loadPct,
       throttlePct, volts);
  jstr(buf, cap, len, vin);
  jcat(buf, cap, len, "\",\"pids\":{");

  bool first = true;
  for (int p = 0; p < 256; p++) {
    if (!pidHave[p]) continue;
    jcat(buf, cap, len, "%s\"%02X\":%d", first ? "" : ",", p, pidVal[p]);
    first = false;
  }
  jcat(buf, cap, len, "}},");

  // dtc: "P0171,P0420" -> ["P0171","P0420"]
  jcat(buf, cap, len, "\"dtc\":{\"mil\":%s,\"codes\":[", mil ? "true" : "false");
  if (dtc[0]) {
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
  }
  jcat(buf, cap, len, "]},");

  jcat(buf, cap, len,
       "\"gps\":{\"fix\":%s,\"lat\":%.6f,\"lng\":%.6f,\"alt_m\":%.1f,"
       "\"speed_kph\":%.1f,\"course\":%.0f,\"sats\":%d,\"hdop\":%.2f},",
       fix ? "true" : "false", lat, lng, altM, gpsSpeedKph, course, sats, hdop);

  jcat(buf, cap, len,
       "\"imu\":{\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,\"gx\":%.1f,\"gy\":%.1f,\"gz\":%.1f}}",
       ax, ay, az, gx, gy, gz);

  return len;
}
