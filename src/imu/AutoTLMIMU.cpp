/*
 * AutoTLMIMU.cpp — motion module implementation.
 * Part of AutoTLM Core — MIT licensed.
 */
#include "AutoTLMIMU.h"

bool AutoTLMIMU::begin(AutoTLMHAL& hal) {
  m_hal = &hal;
  m_available = m_hal->imuBegin();
  return m_available;
}

void AutoTLMIMU::tick() {
  if (!m_available || !m_hal) return;
  const uint32_t now = millis();
  if (now - m_lastSample < m_intervalMs) return;
  m_lastSample = now;

  float acc[3] = {0}, gyr[3] = {0};
  if (m_hal->imuRead(acc, gyr)) {
    m_data.ax = acc[0]; m_data.ay = acc[1]; m_data.az = acc[2];
    m_data.gx = gyr[0]; m_data.gy = gyr[1]; m_data.gz = gyr[2];
    m_data.valid = true;
  }
}
