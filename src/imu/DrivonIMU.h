/*
 * DrivonIMU.h — motion module: acceleration (g) + rotation rate (deg/s).
 *
 * Thin by design: the HAL owns the chip driver (ICM-42627 on the ONE+,
 * MPU-6050 on generic boards); this module owns the sampling cadence and the
 * latest values.
 *
 * Part of Drivon Core — MIT licensed.
 */
#ifndef DRIVON_IMU_H
#define DRIVON_IMU_H

#include <Arduino.h>
#include "../hal/DrivonHAL.h"

/** A motion snapshot, returned by car.motion(). */
struct DrivonMotion {
  bool  valid;      ///< false until the first successful read
  float ax, ay, az; ///< acceleration, g
  float gx, gy, gz; ///< rotation rate, deg/s
};

class DrivonIMU {
 public:
  /** Probe the motion sensor. @return true if one is fitted */
  bool begin(DrivonHAL& hal);

  /** Sample on cadence (default 5 Hz). Called from Drivon::update(). */
  void tick();

  /** Latest values. */
  DrivonMotion data() const { return m_data; }

  /** True if a sensor was found at begin(). */
  bool available() const { return m_available; }

  /** Change the sampling interval (ms). */
  void setSampleInterval(uint32_t ms) { m_intervalMs = ms; }

 private:
  DrivonHAL* m_hal = nullptr;
  bool m_available = false;
  uint32_t m_lastSample = 0;
  uint32_t m_intervalMs = 200;
  DrivonMotion m_data = {};
};

#endif // DRIVON_IMU_H
