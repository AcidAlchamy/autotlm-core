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

#include "AutoTLMFrame.h"
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
