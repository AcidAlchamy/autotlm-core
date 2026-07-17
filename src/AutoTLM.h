/*
 * AutoTLM.h — the AutoTLM Core facade.
 *
 * Read a car and push telemetry in a few lines:
 *
 *   #include <AutoTLM.h>
 *   AutoTLM car;
 *
 *   void setup() {
 *     car.begin();                                  // OBD + GNSS + IMU up
 *     car.wifi("MyHotspot", "password");
 *     car.cloud("http://yourserver.com/api/ingest", "TOKEN");
 *     car.onDTC([](const char* code) { Serial.println(code); });
 *   }
 *
 *   void loop() {
 *     car.update();                                 // pump sensors + push
 *     if (car.obd().connected()) Serial.println(car.obd().rpm());
 *     AutoTLMGPS g = car.gps();
 *     if (g.fix) Serial.printf("%.5f, %.5f\n", g.lat, g.lng);
 *   }
 *
 * Board selection: none needed. Pick "AutoTLM One" in the IDE and begin()
 * brings up the AutoTLM One HAL automatically. Custom hardware: subclass AutoTLMHAL and pass it to
 * car.begin(yourHal). The deprecated Freematics ONE+ benchmark HAL is still
 * selectable with #define AUTOTLM_BOARD_FREEMATICS_ONEPLUS before the
 * include, until it is removed.
 *
 * Part of AutoTLM Core — MIT licensed.
 */
#ifndef AUTOTLM_H
#define AUTOTLM_H

#include <Arduino.h>

// BLE (the change-WiFi service) links the whole Bluedroid stack — ~0.5 MB of
// flash. It's ON by default only on the AutoTLM One (whose OTA partition has
// room for it); on a generic ESP32 it's OFF so lean telemetry sketches keep
// fitting the stock 1.3 MB app partition. Force it either way by defining
// AUTOTLM_ENABLE_BLE before including this header (example 08 does).
#ifndef AUTOTLM_ENABLE_BLE
#  if defined(ARDUINO_AUTOTLM_ONE)
#    define AUTOTLM_ENABLE_BLE 1
#  else
#    define AUTOTLM_ENABLE_BLE 0
#  endif
#endif

#include "AutoTLMFrame.h"
#if AUTOTLM_ENABLE_BLE
#include "ble/AutoTLMBle.h"
#endif
#include "core/AutoTLMConfig.h"
#include "gnss/AutoTLMGNSS.h"
#include "hal/AutoTLMHAL.h"
#include "imu/AutoTLMIMU.h"
#include "net/AutoTLMNet.h"
#include "obd/AutoTLMOBD.h"
#include "provision/AutoTLMProvision.h"

#if defined(ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

/**
 * The AutoTLM facade: one object that owns the HAL, all modules, the live
 * telemetry frame and the status LED. Beginner API on the facade itself;
 * power users reach through obd()/gnss()/imu()/net()/config().
 */
class AutoTLM {
 public:
  /**
   * Bring the unit up on the board selected by the AUTOTLM_BOARD_* define
   * (defined inline at the bottom of this header). GNSS + IMU start now; the
   * ECU connection is deliberately lazy so a stalled car bus never blocks
   * connectivity.
   */
  bool begin();

  /** Bring the unit up on your own AutoTLMHAL implementation. */
  bool begin(AutoTLMHAL& hal);

  /**
   * Connect to a WiFi network (non-blocking; a core-0 task keeps it alive
   * and reconnects forever). Credentials are persisted to flash.
   * Pass nullptr/"" to reuse the last saved credentials.
   */
  void wifi(const char* ssid, const char* pass);

  /**
   * Stream telemetry to an HTTP ingest endpoint (plain HTTP by design — see
   * AutoTLMNet.h for why TLS is refused). Runs on core 0, independent of
   * sensor reads.
   */
  void cloud(const char* url, const char* token, uint32_t intervalMs = 1000);

  /**
   * Push one frame out of cycle, NOW — for event uploads (a trouble code
   * just appeared, a script's push statement). Thread-safe: raises a flag
   * the network task services on its next pass (immediately when WiFi is
   * up; on reconnect otherwise). The regular interval keeps its cadence.
   * @return true if a cloud endpoint is configured
   */
  bool pushNow() { return m_net.pushNow(); }

  // --------------------------------------------------------- provisioning
  /**
   * The zero-code onboarding line. If the unit has been provisioned (WiFi
   * saved — via the portal or an earlier car.wifi()), applies the saved
   * WiFi + cloud settings and returns true. Otherwise raises the captive
   * setup portal (join "AutoTLM-XXXX" from a phone, the page pops up) and
   * returns false — keep calling update(); the unit reboots itself into the
   * saved settings once the form is submitted.
   */
  bool provision();

  /**
   * Force the setup portal up right now, even if already provisioned (the
   * "reconfigure me" path — wire it to a button hold). Portal + normal
   * operation don't mix: call it instead of wifi()/cloud(), not after.
   */
  bool beginPortal(const char* apName = nullptr, const char* apPass = nullptr);

  /** The provisioning module (portal state, saved(), setRestartOnSave...). */
  AutoTLMProvision& provisioning() { return m_prov; }

  /**
   * Change the WiFi network SAFELY — validate-and-rollback. Tries the new
   * credentials while keeping the current ones; if they don't associate
   * within ~30 s the unit stays on its old network (a wrong password can
   * never strand it off-network). Non-blocking: poll wifiChangeState(); on
   * success the new credentials are persisted for you. This is the primitive
   * behind every live "change my WiFi" path (USB, BLE, app).
   */
  bool changeWifi(const char* ssid, const char* pass);
  /** Live WiFi-change state: AUTOTLM_WIFI_IDLE/VALIDATING/OK/REVERTED. */
  int wifiChangeState() { return m_net.wifiChangeState(); }
  /** Why the last change REVERTED: AUTOTLM_WIFI_REASON_NOT_FOUND/AUTH/TIMEOUT (0 otherwise). */
  int wifiChangeReason() { return m_net.wifiChangeReason(); }

  /**
   * Observe WiFi-change transitions RACE-FREE. Fired from inside update()
   * on every state transition — including OK/REVERTED *before* the facade
   * consumes them — so a consumer can never miss the terminal outcome the
   * way polling wifiChangeState() can. This is the hook the BLE status
   * characteristic (and USB Live's wifi_change event) feed from.
   * @param cb (state = AUTOTLM_WIFI_*, reason = AUTOTLM_WIFI_REASON_*, ctx)
   */
  void onWifiChange(void (*cb)(int state, int reason, void* ctx), void* ctx = nullptr) {
    m_wifiCb = cb;
    m_wifiCbCtx = ctx;
  }

  // -------------------------------------------------------------- BLE
  // Compiled in only when AUTOTLM_ENABLE_BLE (default: on for the AutoTLM One,
  // off for a generic ESP32 so lean sketches don't link Bluedroid). Define
  // AUTOTLM_ENABLE_BLE=1 before including AutoTLM.h to force it on elsewhere.
#if AUTOTLM_ENABLE_BLE
  /**
   * Bring up the BLE change-WiFi service (GATT service + characteristics;
   * see ble/AutoTLMBle.h for the contract). Does NOT advertise — call
   * bleAdvertise(true) per your own policy (recommended: advertise while
   * unprovisioned, briefly after power-on, on a SETUP press, and whenever
   * station WiFi is lost; go dark while associated and pushing, so a driving
   * car is never a followable beacon).
   *
   * CALL IT EARLY: right after begin(), BEFORE provision(). The BT stack
   * needs a large contiguous allocation, and the captive portal (SoftAP +
   * DNS + web server) fragments the heap enough to starve it on a real
   * firmware. Called too late it returns false (with a log) rather than
   * bringing the unit down.
   * @return true if the service is up
   */
  bool bleBegin() { return m_ble.begin(*this, m_frame.deviceId); }
  /** Start/stop BLE advertising (implies bleBegin() on first use). */
  void bleAdvertise(bool on) {
    if (on && !m_ble.active()) bleBegin();
    m_ble.advertise(on);
  }
  /** The BLE module (status state, advertising flag). */
  AutoTLMBle& ble() { return m_ble; }
#else
  /** BLE compiled out (define AUTOTLM_ENABLE_BLE=1 to enable). No-op. */
  bool bleBegin() { return false; }
  void bleAdvertise(bool) {}
#endif

  /**
   * Offer a re-pair setup AP when a PROVISIONED unit stays offline too long
   * (network changed / hotspot gone), so a customer with only a phone can
   * re-pair — no button, no USB. Conservative by design: the AP comes up only
   * after `afterMs` of sustained no-association (default 2 min), it's WPA2
   * (not an open network a passer-by can hijack), and a brief mid-drive dead
   * zone that recovers before the window never triggers it. Off by default.
   * @param afterMs sustained-offline window before offering the AP
   */
  void setReprovisionOnLostWifi(bool on, uint32_t afterMs = 120000) {
    m_reproEnabled = on;
    m_reproAfterMs = afterMs;
  }

  /**
   * The heartbeat: pump GNSS/IMU/OBD, refresh the frame, drive the LED.
   * Call every loop(); individual sensors self-throttle internally.
   */
  void update();

  // ------------------------------------------------------------ shortcuts
  /** Latest GNSS fix snapshot. */
  AutoTLMGPS gps() { return m_gnss.data(); }
  /** Latest motion snapshot. */
  AutoTLMMotion motion() { return m_imu.data(); }
  /** Fired once per newly-appearing trouble code ("P0171"). */
  void onDTC(AutoTLMDTCCallback cb) { m_obd.onDTC(cb); }
  /** Same, with the storing module's id — multi-module cars (see modules()). */
  void onDTC(AutoTLMDTCModuleCallback cb) { m_obd.onDTC(cb); }
  /**
   * How many diagnosable modules (ECUs) the car has — engine, transmission,
   * ABS, airbag... Details via car.obd().module(i) and the per-module DTC
   * accessors. 0 = single-module view (board can't enumerate).
   */
  int modules() { return m_obd.moduleCount(); }

  // -------------------------------------------------------------- modules
  AutoTLMOBD& obd() { return m_obd; }
  AutoTLMGNSS& gnss() { return m_gnss; }
  AutoTLMIMU& imu() { return m_imu; }
  AutoTLMNet& net() { return m_net; }
  AutoTLMConfig& config() { return m_config; }

  // ---------------------------------------------------------------- frame
  /** Coherent copy of the current telemetry frame (what the cloud receives). */
  AutoTLMFrame frame();
  /** Override the unit id reported in telemetry (default: the chip id). */
  void deviceId(const char* id);

  // ----------------------------------------------------------- diagnostics
  /** Print previous-session + live health counters (pushes, WiFi, OBD). */
  void printDiagnostics(Stream& out = Serial);
  /** Worst update() duration seen, µs (bus-blocking spikes show up here). */
  uint32_t maxLoopUs() const { return m_maxLoopUs; }

  /**
   * The status LED convention (can be disabled — disabling also turns the
   * LED off immediately):
   *   fast blink  = WiFi down · slow 1 s blink = WiFi up but pushes not
   *   landing · brief pulse = frame just pushed OK · off = networking unused
   */
  void statusLed(bool enabled) {
    m_ledEnabled = enabled;
    if (!enabled && m_hal) m_hal->led(false);
  }

  /** Quiet or redirect all library logging (nullptr = silent). */
  void setLogStream(Stream* s);

  // Internal: snapshot + diag-save hooks handed to the net task.
  void snapshotFrame(AutoTLMFrame& out);
  void saveDiagnostics();

 private:
  void composeFrame();
  void serviceWifiChange();
  void ledTick();
  void lock();
  void unlock();

  AutoTLMHAL* m_hal = nullptr;
  AutoTLMOBD m_obd;
  AutoTLMGNSS m_gnss;
  AutoTLMIMU m_imu;
  AutoTLMNet m_net;
  AutoTLMConfig m_config;
  AutoTLMProvision m_prov;

  AutoTLMFrame m_frame;
  char m_idOverride[AUTOTLM_ID_LEN] = "";
  uint32_t m_lastCompose = 0;
  uint32_t m_maxLoopUs = 0;
  bool m_ledEnabled = true;
  bool m_gnssBegan = false;
  Stream* m_log = &Serial;

  // Live WiFi-change (changeWifi): creds staged for NVS persist once the net
  // task reports the new network validated.
  char m_pendingSsid[33] = "";
  char m_pendingPass[65] = "";
  uint32_t m_pendingGen = 0;  // net attempt id these creds belong to
  // Race-free change observation (onWifiChange) + the BLE service it feeds.
  void (*m_wifiCb)(int, int, void*) = nullptr;
  void* m_wifiCbCtx = nullptr;
  int m_lastWifiSt = 0;  // AUTOTLM_WIFI_IDLE
#if AUTOTLM_ENABLE_BLE
  AutoTLMBle m_ble;
#endif
  // Offline-reprovision policy (opt-in; see setReprovisionOnLostWifi).
  bool m_reproEnabled = false;
  uint32_t m_reproAfterMs = 120000;

#if defined(ESP32)
  SemaphoreHandle_t m_mutex = nullptr;
#endif
};

// ---------------------------------------------------------------------------
// Board auto-selection. Header-only on purpose: the sketch's AUTOTLM_BOARD_*
// define is only visible in the sketch's own translation unit, so the chosen
// board implementation must ride along in this header rather than live in a
// library .cpp file.
//
// Any supported target (the AutoTLM One board package defines
// ARDUINO_AUTOTLM_ONE; the toolchain defines ESP32) gets the AutoTLM
// One HAL. AUTOTLM_BOARD_GENERIC_ESP32 is the deprecated old name for the
// same thing; the Freematics ONE+ benchmark HAL is deprecated and must be
// asked for explicitly.
// ---------------------------------------------------------------------------
#if defined(AUTOTLM_BOARD_FREEMATICS_ONEPLUS)
#include "hal/BoardFreematicsOnePlus.h"
#define AUTOTLM_DEFAULT_BOARD BoardFreematicsOnePlus
#elif defined(ARDUINO_AUTOTLM_ONE) || defined(AUTOTLM_BOARD_ONE) || \
    defined(AUTOTLM_BOARD_GENERIC_ESP32) || defined(ESP32)
#include "hal/BoardAutoTLMOne.h"
#define AUTOTLM_DEFAULT_BOARD BoardAutoTLMOne
#endif

inline bool AutoTLM::begin() {
#if defined(AUTOTLM_DEFAULT_BOARD)
  static AUTOTLM_DEFAULT_BOARD s_board;
  return begin(s_board);
#else
#warning "No AutoTLM board for this target — call car.begin(yourHal) with a custom AutoTLMHAL."
  return false;
#endif
}

#endif // AUTOTLM_H
