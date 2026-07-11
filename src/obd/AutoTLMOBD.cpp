/*
 * AutoTLMOBD.cpp — OBD-II module implementation.
 * Part of AutoTLM Core — MIT licensed.
 */
#include "AutoTLMOBD.h"

#include <string.h>
#include "../core/AutoTLMPids.h"

// Every mode-01 PID AutoTLM knows how to normalize; the discovery sweep keeps
// only the ones this car's ECU actually advertises. 0x42 (module voltage) is
// included so boards without their own voltage sense can fall back to it.
static const uint8_t CANDIDATE_PIDS[] = {
    0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x1F, 0x21, 0x2C, 0x2D, 0x2F, 0x30, 0x31, 0x33, 0x3C, 0x3D,
    0x42, 0x43, 0x45, 0x46, 0x49, 0x4C, 0x5C, 0x5E};

// The gauges: read every cycle no matter what.
static const uint8_t FAST_PIDS[] = {PID_RPM, PID_SPEED, PID_COOLANT_TEMP,
                                    PID_ENGINE_LOAD, PID_THROTTLE};

// Round-robin: how many extended PIDs to read per cycle.
#define SWEEP_PER_CYCLE 4
// Consecutive failed READS (not cycles) before declaring the ECU gone — the
// same sensitivity as the road firmware's COBD error counter: one fully-dead
// poll cycle (9 reads) trips it, so a dead ECU can't freeze gauges for long
// or let the slow DTC read fire against a silent bus.
#define MAX_FAIL_STREAK 8
// How often to retry the ECU connection while it's down.
#define INIT_RETRY_MS 10000

void AutoTLMOBD::begin(AutoTLMHAL& hal) { m_hal = &hal; }

void AutoTLMOBD::tick() {
  if (!m_hal) return;

  if (!m_connected) {
    tryInit();
    return;
  }

  const uint32_t now = millis();
  if (now - m_lastPoll >= m_pollMs) {
    m_lastPoll = now;
    pollPids();
  }
  if (m_connected && now - m_lastDtc >= m_dtcMs) {
    m_lastDtc = now;
    readDTCs();
  }
}

// Lazy background bring-up: self-throttled so a car-less bench (or a stalled
// bus) costs one blocked attempt every 10 s and nothing else.
void AutoTLMOBD::tryInit() {
  const uint32_t now = millis();
  if (m_lastInitTry != 0 && now - m_lastInitTry < INIT_RETRY_MS) return;
  m_lastInitTry = now;

  const bool ok = m_hal->obdInit();
  if (m_log) m_log->printf("OBD:%s\n", ok ? "OK" : "NO");
  if (!ok) return;

  m_connected = true;
  m_everConnected = true;
  m_failStreak = 0;
  discover();
}

void AutoTLMOBD::discover() {
  if (m_hal->obdVIN(m_vin, sizeof(m_vin))) {
    if (m_log) m_log->printf("VIN:%s\n", m_vin);
  }
  m_nSupported = 0;
  m_sweepIdx = 0;
  for (size_t i = 0; i < sizeof(CANDIDATE_PIDS) && m_nSupported < (int)sizeof(m_supported); i++) {
    if (m_hal->obdIsPIDSupported(CANDIDATE_PIDS[i])) m_supported[m_nSupported++] = CANDIDATE_PIDS[i];
  }
  if (m_log) m_log->printf("OBD_PIDS:%d\n", m_nSupported);
}

void AutoTLMOBD::pollPids() {
  for (uint8_t pid : FAST_PIDS) {
    int v;
    if (m_hal->obdReadPID(pid, v)) {
      m_pidVal[pid] = v;
      m_pidHave[pid] = true;
      m_failStreak = 0;
    } else if (++m_failStreak >= MAX_FAIL_STREAK) {
      break;
    }
  }

  for (int k = 0; k < SWEEP_PER_CYCLE && m_nSupported > 0 && m_failStreak < MAX_FAIL_STREAK; k++) {
    const uint8_t pid = m_supported[m_sweepIdx++ % m_nSupported];
    int v;
    if (m_hal->obdReadPID(pid, v)) {
      m_pidVal[pid] = v;
      m_pidHave[pid] = true;
      m_failStreak = 0;
    } else {
      ++m_failStreak;
    }
  }

  if (m_failStreak >= MAX_FAIL_STREAK) {
    // The ECU stopped answering (ignition off, unplugged). Drop back to the
    // lazy-init loop — with the retry timer cleared so the first reconnect
    // attempt happens on the very next tick (quick restarts at a light
    // shouldn't leave the gauges dead for 10 s).
    if (m_log) m_log->println("OBD:LOST");
    m_connected = false;
    m_failStreak = 0;
    m_hal->obdEnd();
    m_lastInitTry = 0;
    return;
  }

  float volts = m_hal->obdBatteryVoltage();
  if (isnan(volts) && m_pidHave[PID_CONTROL_MODULE_VOLTAGE]) {
    // Boards without a voltage sense fall back to the ECU's own reading
    // (PID 0x42 is in the discovery sweep; whole-volt granularity).
    volts = (float)m_pidVal[PID_CONTROL_MODULE_VOLTAGE];
  }
  if (!isnan(volts)) m_volts = volts;
}

void AutoTLMOBD::readDTCs() {
  uint16_t codes[AUTOTLM_MAX_DTCS];
  const int n = m_hal->obdReadDTC(codes, AUTOTLM_MAX_DTCS);
  if (n < 0) return;

  m_dtcCount = 0;
  m_dtcStr[0] = 0;
  for (int i = 0; i < n && i < AUTOTLM_MAX_DTCS; i++) {
    autotlm::formatDTC(codes[i], m_dtcCodes[m_dtcCount]);

    if (strlen(m_dtcStr) + strlen(m_dtcCodes[m_dtcCount]) + 2 <= sizeof(m_dtcStr)) {
      if (m_dtcStr[0]) strcat(m_dtcStr, ",");
      strcat(m_dtcStr, m_dtcCodes[m_dtcCount]);
    }

    // Fire the callback only for codes not seen this session.
    bool seen = false;
    for (int s = 0; s < m_seenCount; s++) {
      if (m_seenCodes[s] == codes[i]) { seen = true; break; }
    }
    if (!seen && m_seenCount < AUTOTLM_MAX_DTCS) {
      m_seenCodes[m_seenCount++] = codes[i];
      if (m_log) m_log->printf("DTC:%s\n", m_dtcCodes[m_dtcCount]);
      if (m_dtcCb) m_dtcCb(m_dtcCodes[m_dtcCount]);
    }
    m_dtcCount++;
  }
}

const char* AutoTLMOBD::dtcAt(int i) const {
  if (i < 0 || i >= m_dtcCount) return "";
  return m_dtcCodes[i];
}

void AutoTLMOBD::clearDTCs() {
  if (!m_hal || !m_connected) return;
  m_hal->obdClearDTC();
  m_dtcCount = 0;
  m_dtcStr[0] = 0;
  m_seenCount = 0;
}

void AutoTLMOBD::fillFrame(bool* pidHave, int* pidVal) const {
  memcpy(pidHave, m_pidHave, sizeof(m_pidHave));
  memcpy(pidVal, m_pidVal, sizeof(m_pidVal));
}
