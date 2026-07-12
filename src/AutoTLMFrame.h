/*
 * AutoTLMFrame.h — the canonical AutoTLM telemetry frame.
 *
 * One struct holds everything the device knows about the car right now:
 * OBD-II values, GNSS fix, IMU motion, DTCs and device health. Field names in
 * the JSON serialization match the AutoTLM Dash / ingest contract exactly
 * (obd.speed_kph, gps.lat, imu.ax, ...), so a frame pushed by this library
 * renders on the existing dashboards without any translation.
 *
 * Every value is "nullable-ish": `pidHave[]` flags which PIDs are real, and
 * consumers treat zero + not-connected as "unknown" the same way the
 * dashboards render missing values as "—".
 *
 * Part of AutoTLM Core — MIT licensed.
 */
#ifndef AUTOTLM_FRAME_H
#define AUTOTLM_FRAME_H

#include <Arduino.h>

/** Maximum characters of a VIN (17) plus terminator, rounded up. */
#define AUTOTLM_VIN_LEN 24
/** Comma-separated DTC list buffer, e.g. "P0171,P0420". */
#define AUTOTLM_DTC_STR_LEN 64
/** Device id / type string buffers. */
#define AUTOTLM_ID_LEN 20
#define AUTOTLM_TYPE_LEN 12

/**
 * The full telemetry frame. Plain-old-data on purpose: it is copied across
 * FreeRTOS cores with a struct assignment, so it must stay trivially copyable
 * (no String, no pointers to heap).
 */
struct AutoTLMFrame {
  // ---- device ----
  char deviceId[AUTOTLM_ID_LEN];     ///< unit id (defaults to the chip id)
  char deviceType[AUTOTLM_TYPE_LEN]; ///< board/device type string
  char mems[16];                    ///< IMU chip name ("ICM-42627", "MPU-6050", "")
  bool gnssUp;                      ///< GNSS UART alive (a valid sentence was seen)
  int  rssi;                        ///< WiFi RSSI in dBm (0 when offline)
  uint8_t moduleCount;              ///< diagnosable modules (ECUs) found; 0 = single-module view

  // ---- OBD-II ----
  bool  obdConnected;               ///< true once the ECU answers
  int   rpm;                        ///< engine RPM
  int   speedKph;                   ///< vehicle speed, km/h
  int   coolantC;                   ///< coolant temperature, °C
  int   loadPct;                    ///< calculated engine load, %
  int   throttlePct;                ///< throttle position, %
  float volts;                      ///< battery voltage
  char  vin[AUTOTLM_VIN_LEN];        ///< vehicle identification number
  int   pidVal[256];                ///< normalized value for every mode-01 PID
  bool  pidHave[256];               ///< which entries of pidVal are real

  // ---- DTCs (the check-engine light) ----
  bool mil;                         ///< malfunction indicator lamp inferred (any code stored)
  char dtc[AUTOTLM_DTC_STR_LEN];     ///< comma-separated codes, e.g. "P0171,P0420"

  // ---- GNSS ----
  bool   fix;                       ///< true when position is valid
  double lat, lng;                  ///< decimal degrees (WGS84)
  float  altM;                      ///< altitude, meters
  float  gpsSpeedKph;               ///< ground speed, km/h
  float  course;                    ///< course over ground, degrees
  float  hdop;                      ///< horizontal dilution of precision
  int    sats;                      ///< satellites in use

  // ---- IMU ----
  bool  imuHave;                    ///< IMU readings valid
  float ax, ay, az;                 ///< acceleration, g
  float gx, gy, gz;                 ///< rotation rate, deg/s

  /** Reset every field to the "unknown" state. */
  void clear();

  /**
   * Serialize to the AutoTLM ingest JSON (same shape the dashboards already
   * consume). Writes at most `cap-1` chars + NUL into `buf`.
   * @return number of characters written (0 if the buffer is hopeless).
   */
  size_t toJson(char* buf, size_t cap) const;
};

#endif // AUTOTLM_FRAME_H
