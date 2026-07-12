/*
 * AutoTLMProvision.h — browser-based first-boot provisioning.
 *
 * The "no code edits" onboarding path: the unit raises its own WiFi access
 * point (AutoTLM-XXXX) with a captive portal. Join it from a phone or
 * laptop, the setup page opens by itself (or at http://192.168.4.1), and
 * you fill in:
 *
 *   - WiFi network + password (with a live scan of nearby networks)
 *   - cloud ingest URL + bearer token + push interval
 *   - GPS on/off
 *   - display units (metric / imperial — stored for dashboards + sketches)
 *
 * Everything is saved to NVS via AutoTLMConfig and survives reflash. On
 * save the unit reboots into normal operation; from then on
 * car.provision() applies the saved settings at every boot.
 *
 * The portal deliberately runs BEFORE car.wifi()/car.cloud() ever start the
 * network task: the softAP owns the radio during setup, and the reboot hands
 * the driver over cleanly. Typical use is one facade call in setup():
 *
 *   car.begin();
 *   car.provision();   // saved settings? apply them. none? raise the portal.
 *
 * Part of AutoTLM Core — MIT licensed.
 */
#ifndef AUTOTLM_PROVISION_H
#define AUTOTLM_PROVISION_H

#include <Arduino.h>

#include "../core/AutoTLMConfig.h"

#if defined(ESP32)
#include <DNSServer.h>
#include <WebServer.h>
#endif

class AutoTLMProvision {
 public:
  /** Wire the settings store + log sink (done by the AutoTLM facade). */
  void attach(AutoTLMConfig* config, Stream* log) {
    m_config = config;
    m_log = log;
  }

  /**
   * Raise the captive portal: softAP + DNS catch-all + the setup page.
   * @param apName  AP SSID; nullptr = "AutoTLM-XXXX" from the chip id
   * @param apPass  AP password; nullptr/"" = open network (the default —
   *                provisioning APs are short-lived and a printed password
   *                is one more thing to get wrong on first boot)
   * @return true if the AP came up
   */
  bool start(const char* apName = nullptr, const char* apPass = nullptr);

  /** Service DNS + HTTP. The facade calls this from car.update(). */
  void tick();

  /** Tear the portal down (AP off). Settings already saved stay saved. */
  void stop();

  /** True while the portal is up. */
  bool active() const { return m_active; }

  /** True once the user has submitted the form (settings are in NVS). */
  bool saved() const { return m_saved; }

  /**
   * Reboot into the saved settings after a successful save (default true —
   * the cleanest possible radio handover). Disable to handle it yourself
   * via saved().
   */
  void setRestartOnSave(bool on) { m_restartOnSave = on; }

  /** The AP SSID in use (valid after start()). */
  const char* apName() const { return m_apName; }

 private:
#if defined(ESP32)
  void handleRoot();
  void handleScan();
  void handleSave();
  void handleNotFound();

  DNSServer m_dns;
  WebServer m_http{80};
  uint32_t m_restartAt = 0;
#endif

  AutoTLMConfig* m_config = nullptr;
  Stream* m_log = nullptr;
  char m_apName[33] = "";
  bool m_active = false;
  bool m_saved = false;
  bool m_restartOnSave = true;
};

#endif // AUTOTLM_PROVISION_H
