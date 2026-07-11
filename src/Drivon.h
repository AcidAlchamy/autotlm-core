/*
 * Drivon.h — the Drivon Core facade.
 *
 * Read a car and push telemetry in a few lines:
 *
 *   #include <Drivon.h>
 *   Drivon car;
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
 *     DrivonGPS g = car.gps();
 *     if (g.fix) Serial.printf("%.5f, %.5f\n", g.lat, g.lng);
 *   }
 *
 * Board selection is a compile-time define placed BEFORE the include:
 *
 *   #define DRIVON_BOARD_GENERIC_ESP32        // plain ESP32 + CAN transceiver (primary)
 *   #define DRIVON_BOARD_FREEMATICS_ONEPLUS   // Freematics ONE+ (compat / benchmark)
 *   #include <Drivon.h>
 *
 * With no define, an ESP32 target defaults to the generic board. Custom
 * hardware: subclass DrivonHAL and pass it to car.begin(yourHal).
 *
 * Part of Drivon Core — MIT licensed.
 */
#ifndef DRIVON_H
#define DRIVON_H

#include <Arduino.h>

#include "DrivonFrame.h"
#include "core/DrivonConfig.h"
#include "gnss/DrivonGNSS.h"
#include "hal/DrivonHAL.h"
#include "imu/DrivonIMU.h"
#include "net/DrivonNet.h"
#include "obd/DrivonOBD.h"

#if defined(ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

/**
 * The Drivon facade: one object that owns the HAL, all modules, the live
 * telemetry frame and the status LED. Beginner API on the facade itself;
 * power users reach through obd()/gnss()/imu()/net()/config().
 */
class Drivon {
 public:
  /**
   * Bring the unit up on the board selected by the DRIVON_BOARD_* define
   * (defined inline at the bottom of this header). GNSS + IMU start now; the
   * ECU connection is deliberately lazy so a stalled car bus never blocks
   * connectivity.
   */
  bool begin();

  /** Bring the unit up on your own DrivonHAL implementation. */
  bool begin(DrivonHAL& hal);

  /**
   * Connect to a WiFi network (non-blocking; a core-0 task keeps it alive
   * and reconnects forever). Credentials are persisted to flash.
   * Pass nullptr/"" to reuse the last saved credentials.
   */
  void wifi(const char* ssid, const char* pass);

  /**
   * Stream telemetry to an HTTP ingest endpoint (plain HTTP by design — see
   * DrivonNet.h for why TLS is refused). Runs on core 0, independent of
   * sensor reads.
   */
  void cloud(const char* url, const char* token, uint32_t intervalMs = 1000);

  /**
   * The heartbeat: pump GNSS/IMU/OBD, refresh the frame, drive the LED.
   * Call every loop(); individual sensors self-throttle internally.
   */
  void update();

  // ------------------------------------------------------------ shortcuts
  /** Latest GNSS fix snapshot. */
  DrivonGPS gps() { return m_gnss.data(); }
  /** Latest motion snapshot. */
  DrivonMotion motion() { return m_imu.data(); }
  /** Fired once per newly-appearing trouble code ("P0171"). */
  void onDTC(DrivonDTCCallback cb) { m_obd.onDTC(cb); }

  // -------------------------------------------------------------- modules
  DrivonOBD& obd() { return m_obd; }
  DrivonGNSS& gnss() { return m_gnss; }
  DrivonIMU& imu() { return m_imu; }
  DrivonNet& net() { return m_net; }
  DrivonConfig& config() { return m_config; }

  // ---------------------------------------------------------------- frame
  /** Coherent copy of the current telemetry frame (what the cloud receives). */
  DrivonFrame frame();
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
  void snapshotFrame(DrivonFrame& out);
  void saveDiagnostics();

 private:
  void composeFrame();
  void ledTick();
  void lock();
  void unlock();

  DrivonHAL* m_hal = nullptr;
  DrivonOBD m_obd;
  DrivonGNSS m_gnss;
  DrivonIMU m_imu;
  DrivonNet m_net;
  DrivonConfig m_config;

  DrivonFrame m_frame;
  char m_idOverride[DRIVON_ID_LEN] = "";
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
// Board auto-selection. Header-only on purpose: the sketch's DRIVON_BOARD_*
// define is only visible in the sketch's own translation unit, so the chosen
// board implementation must ride along in this header rather than live in a
// library .cpp file.
// ---------------------------------------------------------------------------
#if defined(DRIVON_BOARD_FREEMATICS_ONEPLUS)
#include "hal/BoardFreematicsOnePlus.h"
#define DRIVON_DEFAULT_BOARD BoardFreematicsOnePlus
#elif defined(DRIVON_BOARD_GENERIC_ESP32) || defined(ESP32)
#include "hal/BoardGenericEsp32.h"
#define DRIVON_DEFAULT_BOARD BoardGenericEsp32
#endif

inline bool Drivon::begin() {
#if defined(DRIVON_DEFAULT_BOARD)
  static DRIVON_DEFAULT_BOARD s_board;
  return begin(s_board);
#else
#warning "No Drivon board for this target — call car.begin(yourHal) with a custom DrivonHAL."
  return false;
#endif
}

#endif // DRIVON_H
