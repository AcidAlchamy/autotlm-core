/*
 * AutoTLMConfig.h — NVS-backed settings + persistent field diagnostics.
 *
 * Two jobs:
 *  1. Remember settings (WiFi credentials, user keys) across power cycles.
 *  2. Persist push/WiFi/OBD health counters every few seconds while driving,
 *     so a failure out on the road is readable over USB back at the desk
 *     ("DIAG PREV-SESSION: pushOk=812 pushFail=3 lastHttp=200 ...").
 *
 * Part of AutoTLM Core — MIT licensed.
 */
#ifndef AUTOTLM_CONFIG_H
#define AUTOTLM_CONFIG_H

#include <Arduino.h>

#if defined(ESP32)
#include <Preferences.h>
#endif

/** Health counters persisted to NVS (one snapshot per session). */
struct AutoTLMDiag {
  uint32_t pushOk;     ///< successful cloud pushes
  uint32_t pushFail;   ///< failed cloud pushes
  int32_t  lastHttp;   ///< last HTTP status (-1 connect fail, -2 no response)
  uint32_t wifiDrops;  ///< WiFi reconnect attempts
  bool     obdEver;    ///< did the ECU ever answer this session
  uint32_t maxLoopUs;  ///< worst update() time seen (blocking spikes show here)
  uint32_t boots;      ///< lifetime boot counter
};

/**
 * Settings + diagnostics store. All methods are safe no-ops on platforms
 * without NVS so board-agnostic code can call them unconditionally.
 */
class AutoTLMConfig {
 public:
  /**
   * Open the store, bump the boot counter and capture the previous session's
   * diagnostics (see prevSession()).
   */
  bool begin();

  // ------------------------------------------------------------ WiFi creds
  /** Persist WiFi credentials (survives reflash). */
  void saveWifi(const char* ssid, const char* pass);
  /** Load saved WiFi credentials. @return true if an SSID was stored */
  bool loadWifi(char* ssid, size_t ssidCap, char* pass, size_t passCap);

  // ---------------------------------------------------------- diagnostics
  /** Counters recorded by the previous session (what happened on the drive). */
  const AutoTLMDiag& prevSession() const { return m_prev; }
  /** Persist the current session's counters. Called ~every 20 s by the net task. */
  void saveDiag(const AutoTLMDiag& d);
  /** Print the previous session's counters, e.g. at boot. */
  void printPrevSession(Stream& out) const;

  // ------------------------------------------------- user key/value store
  /** Store a string under `key` (max 15 chars) for your own sketch. */
  void putString(const char* key, const char* value);
  /** Read a string stored with putString. @return chars copied */
  size_t getString(const char* key, char* out, size_t cap, const char* fallback = "");
  void putInt(const char* key, int32_t value);
  int32_t getInt(const char* key, int32_t fallback = 0);

 private:
  AutoTLMDiag m_prev = {};
  bool m_up = false;
#if defined(ESP32)
  Preferences m_prefs;  ///< user + wifi namespace
  Preferences m_diag;   ///< diagnostics namespace (kept open for fast saves)
#endif
};

#endif // AUTOTLM_CONFIG_H
