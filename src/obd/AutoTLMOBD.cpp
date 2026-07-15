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
// Widened to the full common J1979 decode set (MOBILE's PID-breadth ask):
// trims, narrowband O2, timing, EGR/evap, cat temps, pedals, torque, rates.
static const uint8_t CANDIDATE_PIDS[] = {
    0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1F, 0x21,
    0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x3C, 0x3D, 0x3E, 0x3F,
    0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D,
    0x4E, 0x52, 0x59, 0x5B, 0x5C, 0x5D, 0x5E, 0x61, 0x62, 0x63};

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
    // Multi-module cars get the per-module sweep (one module per tick, so a
    // slow bus can never stall update() for long); single-module (or boards
    // that can't enumerate) keep the classic functional read.
    if (m_moduleCount > 0) readModuleDTCs();
    else readDTCs();
    // Freeze frames move on the same slow cadence — they're snapshots of a
    // past moment, staleness is correct.
    readFreeze();
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

// A real VIN is exactly 17 chars, uppercase A–Z (never I/O/Q) and 0–9.
// Anything else is bus corruption dressed as a VIN — reject it and keep the
// last good one (EMULATOR-LOOP-TEST-FINDINGS finding 2: "DRV0ErULAT0R00001").
static bool plausibleVin(const char* v) {
  int n = 0;
  for (; v[n]; n++) {
    const char c = v[n];
    const bool okAlpha = (c >= 'A' && c <= 'Z') && c != 'I' && c != 'O' && c != 'Q';
    const bool okDigit = (c >= '0' && c <= '9');
    if (!okAlpha && !okDigit) return false;
  }
  return n == 17;
}

void AutoTLMOBD::discover() {
  char vin[sizeof(m_vin)] = "";
  if (m_hal->obdVIN(vin, sizeof(vin))) {
    if (!plausibleVin(vin)) {
      if (m_log) m_log->printf("VIN:rejected implausible \"%s\"%s\n", vin,
                               m_vin[0] ? " (keeping previous)" : "");
    } else if (strcmp(vin, m_vin) != 0) {
      // A DIFFERENT (plausible) car: forget the old car's discovered PIDs.
      // Same car reconnecting keeps its session sets (see below).
      strncpy(m_vin, vin, sizeof(m_vin) - 1);
      m_nSupported = 0;
      m_nAdvertised = 0;
      if (m_log) m_log->printf("VIN:%s\n", m_vin);
    }
  }

  // Session sets only GROW (audit 2g: a corrupted supported-PID bitmap on a
  // mid-session reconnect must never shrink an already-confirmed map). A new
  // boot — or a new VIN above — still starts clean.
  m_sweepIdx = 0;
  for (size_t i = 0; i < sizeof(CANDIDATE_PIDS) && m_nSupported < (int)sizeof(m_supported); i++) {
    if (!m_hal->obdIsPIDSupported(CANDIDATE_PIDS[i])) continue;
    bool have = false;
    for (int k = 0; k < m_nSupported; k++) {
      if (m_supported[k] == CANDIDATE_PIDS[i]) { have = true; break; }
    }
    if (!have) m_supported[m_nSupported++] = CANDIDATE_PIDS[i];
  }

  // Everything the car ADVERTISES via the bitmasks (frame `obd.supported`) —
  // a superset of what we poll; the bitmask PIDs themselves are skipped.
  for (int p = 1; p < 256 && m_nAdvertised < (int)sizeof(m_advertised); p++) {
    if ((p % 0x20) == 0) continue;
    if (!m_hal->obdIsPIDSupported((uint8_t)p)) continue;
    bool have = false;
    for (int k = 0; k < m_nAdvertised; k++) {
      if (m_advertised[k] == p) { have = true; break; }
    }
    if (!have) m_advertised[m_nAdvertised++] = (uint8_t)p;
  }
  // Keep the advertised list sorted (the frame contract promises ascending
  // hex) — a grow-only merge can append out of order.
  for (int i = 1; i < m_nAdvertised; i++) {
    const uint8_t v = m_advertised[i];
    int j = i - 1;
    while (j >= 0 && m_advertised[j] > v) { m_advertised[j + 1] = m_advertised[j]; j--; }
    m_advertised[j + 1] = v;
  }
  if (m_log) m_log->printf("OBD_PIDS:%d polled / %d advertised\n", m_nSupported, m_nAdvertised);

  // Which modules live on this bus? (One functional probe, every distinct
  // responder counted — boards that can't do it return 0.)
  uint32_t ids[AUTOTLM_MAX_MODULES];
  const int found = m_hal->obdEnumerate(ids, AUTOTLM_MAX_MODULES);
  memset(m_modules, 0, sizeof(m_modules));
  m_moduleCount = 0;
  m_moduleCursor = 0;
  for (int i = 0; i < found; i++) {
    // Insert sorted by id so module order is stable across boots.
    int at = m_moduleCount;
    while (at > 0 && m_modules[at - 1].id > ids[i]) {
      m_modules[at] = m_modules[at - 1];
      at--;
    }
    m_modules[at] = ModuleState{};
    m_modules[at].id = ids[i];
    m_moduleCount++;
  }
  if (m_log && m_moduleCount)
    m_log->printf("OBD_MODULES:%d (first 0x%lX)\n", m_moduleCount, (unsigned long)m_modules[0].id);
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

// One module per DTC tick: 3 physically-addressed reads (stored / pending /
// permanent), bounded time on the bus. With 8 modules and the default 20 s
// cadence a full sweep completes in ~2.7 min — DTCs move slowly.
void AutoTLMOBD::readModuleDTCs() {
  ModuleState& m = m_modules[m_moduleCursor++ % m_moduleCount];

  int n = m_hal->obdReadDTCFrom(m.id, 0x03, m.stored, AUTOTLM_MODULE_DTCS);
  if (n >= 0) {
    m.nStored = (uint8_t)n;
    for (int i = 0; i < n; i++) {
      bool seen = false;
      for (int s = 0; s < m.nSeen; s++) {
        if (m.seen[s] == m.stored[i]) { seen = true; break; }
      }
      if (!seen && m.nSeen < AUTOTLM_MODULE_DTCS) {
        m.seen[m.nSeen++] = m.stored[i];
        char code[8];
        autotlm::formatDTC(m.stored[i], code);
        if (m_log) m_log->printf("DTC:%s @0x%lX\n", code, (unsigned long)m.id);
        if (m_dtcModCb) m_dtcModCb(code, m.id);
        if (m_dtcCb) m_dtcCb(code);
      }
    }
  }
  n = m_hal->obdReadDTCFrom(m.id, 0x07, m.pending, AUTOTLM_MODULE_DTCS);
  if (n >= 0) m.nPending = (uint8_t)n;
  n = m_hal->obdReadDTCFrom(m.id, 0x0A, m.permanent, AUTOTLM_MODULE_DTCS);
  if (n >= 0) m.nPermanent = (uint8_t)n;

  rebuildAggregate();
}

// Mode-02 freeze frame, v1: ONE stored snapshot (frame 0, functional — the
// primary emissions ECU answers), attributed to the code the ECU says caused
// it. Multi-code cars keep one entry; deeper per-module freeze data can ride
// physical addressing later without a contract change (freeze is a map).
void AutoTLMOBD::readFreeze() {
  if (m_dtcCount == 0) {
    m_freezeCount = 0;
    m_freezeCode[0] = 0;
    return;
  }
  const int raw = m_hal->obdFreezeDTC();
  if (raw <= 0) {  // 0 = none stored, -1 = board can't read mode 02
    m_freezeCount = 0;
    m_freezeCode[0] = 0;
    return;
  }
  autotlm::formatDTC((uint16_t)raw, m_freezeCode);

  // A compact, high-signal snapshot: load, coolant, trims, MAP, RPM, speed,
  // intake temp, MAF, throttle, fuel level, module volts — capped at 12 so
  // the read stays bounded on the bus.
  static const uint8_t SNAP[] = {0x04, 0x05, 0x06, 0x07, 0x0B, 0x0C,
                                 0x0D, 0x0F, 0x10, 0x11, 0x2F, 0x42};
  m_freezeCount = 0;
  for (uint8_t pid : SNAP) {
    if (!m_hal->obdIsPIDSupported(pid)) continue;
    int v;
    if (m_hal->obdReadFreezePID(pid, v) &&
        m_freezeCount < (int)sizeof(m_freezePid)) {
      m_freezePid[m_freezeCount] = pid;
      m_freezeVal[m_freezeCount] = v;
      m_freezeCount++;
    }
  }
  if (m_log && m_freezeCount)
    m_log->printf("FREEZE:%s (%d pids)\n", m_freezeCode, m_freezeCount);
}

// The classic dtcCount()/dtcString()/mil() view (and the frame) become the
// UNION of every module's stored codes — a C-code in the ABS module lights
// mil() just like the check-engine lamp it represents.
void AutoTLMOBD::rebuildAggregate() {
  m_dtcCount = 0;
  m_dtcStr[0] = 0;
  for (int mi = 0; mi < m_moduleCount && m_dtcCount < AUTOTLM_MAX_DTCS; mi++) {
    const ModuleState& m = m_modules[mi];
    for (int i = 0; i < m.nStored && m_dtcCount < AUTOTLM_MAX_DTCS; i++) {
      bool dup = false;
      for (int d = 0; d < m_dtcCount; d++) {
        char existing[8];
        autotlm::formatDTC(m.stored[i], existing);
        if (strcmp(existing, m_dtcCodes[d]) == 0) { dup = true; break; }
      }
      if (dup) continue;
      autotlm::formatDTC(m.stored[i], m_dtcCodes[m_dtcCount]);
      if (strlen(m_dtcStr) + strlen(m_dtcCodes[m_dtcCount]) + 2 <= sizeof(m_dtcStr)) {
        if (m_dtcStr[0]) strcat(m_dtcStr, ",");
        strcat(m_dtcStr, m_dtcCodes[m_dtcCount]);
      }
      m_dtcCount++;
    }
  }
}

AutoTLMModuleInfo AutoTLMOBD::module(int i) const {
  AutoTLMModuleInfo info = {};
  if (i < 0 || i >= m_moduleCount) return info;
  info.id = m_modules[i].id;
  info.stored = m_modules[i].nStored;
  info.pending = m_modules[i].nPending;
  info.permanent = m_modules[i].nPermanent;
  return info;
}

const char* AutoTLMOBD::moduleDtcAt(int i, int j) const {
  if (i < 0 || i >= m_moduleCount || j < 0 || j >= m_modules[i].nStored) return "";
  autotlm::formatDTC(m_modules[i].stored[j], m_fmtBuf);
  return m_fmtBuf;
}

const char* AutoTLMOBD::modulePendingAt(int i, int j) const {
  if (i < 0 || i >= m_moduleCount || j < 0 || j >= m_modules[i].nPending) return "";
  autotlm::formatDTC(m_modules[i].pending[j], m_fmtBuf);
  return m_fmtBuf;
}

const char* AutoTLMOBD::modulePermanentAt(int i, int j) const {
  if (i < 0 || i >= m_moduleCount || j < 0 || j >= m_modules[i].nPermanent) return "";
  autotlm::formatDTC(m_modules[i].permanent[j], m_fmtBuf);
  return m_fmtBuf;
}

const char* AutoTLMOBD::dtcAt(int i) const {
  if (i < 0 || i >= m_dtcCount) return "";
  return m_dtcCodes[i];
}

void AutoTLMOBD::clearDTCs() {
  if (!m_hal || !m_connected) return;
  m_hal->obdClearDTC();  // functional mode 04: every module clears
  m_dtcCount = 0;
  m_dtcStr[0] = 0;
  m_seenCount = 0;
  m_freezeCount = 0;  // mode 04 erases freeze frames too
  m_freezeCode[0] = 0;
  for (int i = 0; i < m_moduleCount; i++) {
    // Keep the ids; wipe the lists. Permanent codes survive a clear by
    // design — the next per-module sweep repopulates them from mode 0A.
    const uint32_t id = m_modules[i].id;
    m_modules[i] = ModuleState{};
    m_modules[i].id = id;
  }
}

void AutoTLMOBD::fillFrame(bool* pidHave, int* pidVal) const {
  memcpy(pidHave, m_pidHave, sizeof(m_pidHave));
  memcpy(pidVal, m_pidVal, sizeof(m_pidVal));
}

void AutoTLMOBD::fillFrameLists(uint8_t* supported, uint8_t* supportedCount,
                                size_t supportedCap, char* freezeCode,
                                size_t freezeCodeCap, uint8_t* freezePid,
                                int* freezeVal, uint8_t* freezeCount,
                                size_t freezeCap) const {
  int n = m_nAdvertised < (int)supportedCap ? m_nAdvertised : (int)supportedCap;
  memcpy(supported, m_advertised, n);
  *supportedCount = (uint8_t)n;

  strncpy(freezeCode, m_freezeCode, freezeCodeCap - 1);
  freezeCode[freezeCodeCap - 1] = 0;
  n = m_freezeCount < (int)freezeCap ? m_freezeCount : (int)freezeCap;
  memcpy(freezePid, m_freezePid, n);
  memcpy(freezeVal, m_freezeVal, n * sizeof(int));
  *freezeCount = (uint8_t)n;
}
