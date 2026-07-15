/*
 * AutoTLMConfig.cpp — NVS-backed settings + persistent diagnostics.
 * Part of AutoTLM Core — MIT licensed.
 */
#include "AutoTLMConfig.h"

#if defined(ESP32)

bool AutoTLMConfig::begin() {
  if (m_up) return true;
  if (!m_diag.begin("autotlm-diag", false)) return false;

  m_prev.pushOk    = m_diag.getULong("pushOk", 0);
  m_prev.pushFail  = m_diag.getULong("pushFail", 0);
  m_prev.lastHttp  = m_diag.getInt("lastHttp", 0);
  m_prev.wifiDrops = m_diag.getULong("wifiDrops", 0);
  m_prev.obdEver   = m_diag.getBool("obdEver", false);
  m_prev.maxLoopUs = m_diag.getULong("maxLoopUs", 0);
  m_prev.boots     = m_diag.getULong("boots", 0) + 1;
  m_diag.putULong("boots", m_prev.boots);

  // Zero the live counters so this session starts clean.
  AutoTLMDiag fresh = {};
  fresh.boots = m_prev.boots;
  saveDiag(fresh);

  m_up = true;
  return true;
}

void AutoTLMConfig::saveWifi(const char* ssid, const char* pass) {
  m_prefs.begin("autotlm-wifi", false);
  m_prefs.putString("ssid", ssid ? ssid : "");
  m_prefs.putString("pass", pass ? pass : "");
  m_prefs.end();
}

bool AutoTLMConfig::loadWifi(char* ssid, size_t ssidCap, char* pass, size_t passCap) {
  m_prefs.begin("autotlm-wifi", true);
  String s = m_prefs.getString("ssid", "");
  String p = m_prefs.getString("pass", "");
  m_prefs.end();
  if (ssid && ssidCap) { strncpy(ssid, s.c_str(), ssidCap - 1); ssid[ssidCap - 1] = 0; }
  if (pass && passCap) { strncpy(pass, p.c_str(), passCap - 1); pass[passCap - 1] = 0; }
  return s.length() > 0;
}

bool AutoTLMConfig::hasWifi() {
  m_prefs.begin("autotlm-wifi", true);
  const bool has = m_prefs.getString("ssid", "").length() > 0;
  m_prefs.end();
  return has;
}

size_t AutoTLMConfig::apPassword(char* out, size_t cap) {
  if (!out || cap < 9) { if (out && cap) out[0] = 0; return 0; }
  // 8 chars from the 48-bit MAC, mapped to an unambiguous alphabet (no
  // 0/O/1/I/l) so it reads cleanly off a label. Deterministic per unit.
  static const char abc[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";  // 32 symbols
  uint64_t mac = ESP.getEfuseMac();
  for (int i = 0; i < 8; i++) {
    out[i] = abc[mac & 0x1F];
    mac >>= 5;
  }
  out[8] = 0;
  return 8;
}

void AutoTLMConfig::saveCloud(const char* url, const char* token, uint32_t intervalMs) {
  m_prefs.begin("autotlm-cloud", false);
  m_prefs.putString("url", url ? url : "");
  m_prefs.putString("token", token ? token : "");
  m_prefs.putULong("interval", intervalMs ? intervalMs : 1000);
  m_prefs.end();
}

bool AutoTLMConfig::loadCloud(char* url, size_t urlCap, char* token, size_t tokenCap,
                              uint32_t* intervalMs) {
  m_prefs.begin("autotlm-cloud", true);
  String u = m_prefs.getString("url", "");
  String t = m_prefs.getString("token", "");
  uint32_t iv = m_prefs.getULong("interval", 1000);
  m_prefs.end();
  if (url && urlCap) { strncpy(url, u.c_str(), urlCap - 1); url[urlCap - 1] = 0; }
  if (token && tokenCap) { strncpy(token, t.c_str(), tokenCap - 1); token[tokenCap - 1] = 0; }
  if (intervalMs) *intervalMs = iv;
  return u.length() > 0;
}

void AutoTLMConfig::saveGpsEnabled(bool on) {
  m_prefs.begin("autotlm-dev", false);
  m_prefs.putBool("gps", on);
  m_prefs.end();
}

bool AutoTLMConfig::gpsEnabled() {
  m_prefs.begin("autotlm-dev", true);
  const bool on = m_prefs.getBool("gps", true);
  m_prefs.end();
  return on;
}

void AutoTLMConfig::saveUnits(const char* units) {
  m_prefs.begin("autotlm-dev", false);
  m_prefs.putString("units", units ? units : "metric");
  m_prefs.end();
}

size_t AutoTLMConfig::units(char* out, size_t cap) {
  m_prefs.begin("autotlm-dev", true);
  String v = m_prefs.getString("units", "metric");
  m_prefs.end();
  if (!out || !cap) return 0;
  strncpy(out, v.c_str(), cap - 1);
  out[cap - 1] = 0;
  return strlen(out);
}

void AutoTLMConfig::saveDiag(const AutoTLMDiag& d) {
  m_diag.putULong("pushOk", d.pushOk);
  m_diag.putULong("pushFail", d.pushFail);
  m_diag.putInt("lastHttp", d.lastHttp);
  m_diag.putULong("wifiDrops", d.wifiDrops);
  m_diag.putBool("obdEver", d.obdEver);
  m_diag.putULong("maxLoopUs", d.maxLoopUs);
}

void AutoTLMConfig::printPrevSession(Stream& out) const {
  out.printf(
      "DIAG PREV-SESSION: pushOk=%lu pushFail=%lu lastHttp=%ld wifiDrops=%lu "
      "obdEver=%d maxLoopMs=%lu (boot #%lu)\n",
      (unsigned long)m_prev.pushOk, (unsigned long)m_prev.pushFail,
      (long)m_prev.lastHttp, (unsigned long)m_prev.wifiDrops,
      (int)m_prev.obdEver, (unsigned long)(m_prev.maxLoopUs / 1000),
      (unsigned long)m_prev.boots);
}

void AutoTLMConfig::putString(const char* key, const char* value) {
  m_prefs.begin("autotlm-user", false);
  m_prefs.putString(key, value ? value : "");
  m_prefs.end();
}

size_t AutoTLMConfig::getString(const char* key, char* out, size_t cap, const char* fallback) {
  m_prefs.begin("autotlm-user", true);
  String v = m_prefs.getString(key, fallback ? fallback : "");
  m_prefs.end();
  if (!out || !cap) return 0;
  strncpy(out, v.c_str(), cap - 1);
  out[cap - 1] = 0;
  return strlen(out);
}

void AutoTLMConfig::putInt(const char* key, int32_t value) {
  m_prefs.begin("autotlm-user", false);
  m_prefs.putInt(key, value);
  m_prefs.end();
}

int32_t AutoTLMConfig::getInt(const char* key, int32_t fallback) {
  m_prefs.begin("autotlm-user", true);
  int32_t v = m_prefs.getInt(key, fallback);
  m_prefs.end();
  return v;
}

#else  // !ESP32 — no NVS: everything is a polite no-op.

bool AutoTLMConfig::begin() { m_up = true; return true; }
void AutoTLMConfig::saveWifi(const char*, const char*) {}
bool AutoTLMConfig::loadWifi(char* ssid, size_t ssidCap, char* pass, size_t passCap) {
  if (ssid && ssidCap) ssid[0] = 0;
  if (pass && passCap) pass[0] = 0;
  return false;
}
bool AutoTLMConfig::hasWifi() { return false; }
size_t AutoTLMConfig::apPassword(char* out, size_t cap) {
  if (!out || cap < 9) { if (out && cap) out[0] = 0; return 0; }
  strncpy(out, "autotlm0", cap - 1);
  out[cap - 1] = 0;
  return strlen(out);
}
void AutoTLMConfig::saveCloud(const char*, const char*, uint32_t) {}
bool AutoTLMConfig::loadCloud(char* url, size_t urlCap, char* token, size_t tokenCap, uint32_t* intervalMs) {
  if (url && urlCap) url[0] = 0;
  if (token && tokenCap) token[0] = 0;
  if (intervalMs) *intervalMs = 1000;
  return false;
}
void AutoTLMConfig::saveGpsEnabled(bool) {}
bool AutoTLMConfig::gpsEnabled() { return true; }
void AutoTLMConfig::saveUnits(const char*) {}
size_t AutoTLMConfig::units(char* out, size_t cap) {
  if (!out || !cap) return 0;
  strncpy(out, "metric", cap - 1);
  out[cap - 1] = 0;
  return strlen(out);
}
void AutoTLMConfig::saveDiag(const AutoTLMDiag&) {}
void AutoTLMConfig::printPrevSession(Stream& out) const { out.println("DIAG: not available on this platform"); }
void AutoTLMConfig::putString(const char*, const char*) {}
size_t AutoTLMConfig::getString(const char*, char* out, size_t cap, const char* fallback) {
  if (!out || !cap) return 0;
  strncpy(out, fallback ? fallback : "", cap - 1);
  out[cap - 1] = 0;
  return strlen(out);
}
void AutoTLMConfig::putInt(const char*, int32_t) {}
int32_t AutoTLMConfig::getInt(const char*, int32_t fallback) { return fallback; }

#endif
