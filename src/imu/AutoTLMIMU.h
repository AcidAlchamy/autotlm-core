/*
 * AutoTLMIMU.h — motion module: acceleration (g) + rotation rate (deg/s).
 *
 * Thin by design: the HAL owns the chip driver (ICM-42627 on the ONE+,
 * MPU-6050 on generic boards); this module owns the sampling cadence and the
 * latest values.
 *
 * Part of AutoTLM Core — MIT licensed.
 */
#ifndef AUTOTLM_IMU_H
#define AUTOTLM_IMU_H

#include <Arduino.h>
#include "../hal/AutoTLMHAL.h"

/** A motion snapshot, returned by car.motion(). */
struct AutoTLMMotion {
  bool  valid;      ///< false until the first successful read
  float ax, ay, az; ///< acceleration, g
  float gx, gy, gz; ///< rotation rate, deg/s
};

class AutoTLMIMU {
 public:
  /** Probe the motion sensor. @return true if one is fitted */
  bool begin(AutoTLMHAL& hal);

  /** Sample on cadence (default 5 Hz). Called from AutoTLM::update(). */
  void tick();

  /** Latest values. */
  AutoTLMMotion data() const { return m_data; }

  /** True if a sensor was found at begin(). */
  bool available() const { return m_available; }

  /** Change the sampling interval (ms). */
  void setSampleInterval(uint32_t ms) { m_intervalMs = ms; }

 private:
  AutoTLMHAL* m_hal = nullptr;
  bool m_available = false;
  uint32_t m_lastSample = 0;
  uint32_t m_intervalMs = 200;
  AutoTLMMotion m_data = {};
};

#endif // AUTOTLM_IMU_H
