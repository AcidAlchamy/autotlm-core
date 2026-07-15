/*
 * AutoTLMOBD.h — the OBD-II module: PIDs, DTCs, VIN.
 *
 * Behavior baked in from road testing:
 *  - LAZY INIT. Talking to a car bus can stall for seconds (or forever, off
 *    the car). The ECU connection is therefore never attempted in begin();
 *    tick() brings it up in the background, retrying every 10 s, so OBD can
 *    never block WiFi or the cloud push.
 *  - TIERED POLLING. After connecting, the ECU's supported-PID bitmap is
 *    swept once; then every poll cycle reads the five headline gauge PIDs
 *    (RPM, speed, coolant, load, throttle) plus a rotating handful of the
 *    rest, so gauges stay live while the full sensor grid still fills in.
 *  - DTCs are read rarely (every 20 s) because the read is slow.
 *
 * Part of AutoTLM Core — MIT licensed.
 */
#ifndef AUTOTLM_OBD_H
#define AUTOTLM_OBD_H

#include <Arduino.h>
#include "../hal/AutoTLMHAL.h"

/** Fired when a trouble code appears that wasn't present before. */
typedef void (*AutoTLMDTCCallback)(const char* code);
/** Same, but identifies WHICH module stored it (CAN responder id, e.g. 0x7E8). */
typedef void (*AutoTLMDTCModuleCallback)(const char* code, uint32_t moduleId);

#define AUTOTLM_MAX_DTCS 10
/** ISO 15765-4 gives 11-bit CAN eight diagnosable modules (0x7E8..0x7EF). */
#define AUTOTLM_MAX_MODULES 8
/** Per-module, per-kind code capacity. */
#define AUTOTLM_MODULE_DTCS 8

/** One diagnosable module (ECU) found on the bus. */
struct AutoTLMModuleInfo {
  uint32_t id;        ///< CAN responder id, 0x7E8..0x7EF
  uint8_t stored;     ///< mode-03 (confirmed) code count
  uint8_t pending;    ///< mode-07 code count
  uint8_t permanent;  ///< mode-0A code count (survive a clear)
};

class AutoTLMOBD {
 public:
  /** Wire up the HAL. Does not touch the car. */
  void begin(AutoTLMHAL& hal);

  /**
   * Pump the module: lazy ECU bring-up, PID polling, periodic DTC reads.
   * Called from AutoTLM::update(); may block briefly while talking to the bus.
   */
  void tick();

  // ------------------------------------------------------------- gauges
  /** True once the ECU is answering. */
  bool connected() const { return m_connected; }
  int rpm() const { return m_pidVal[0x0C]; }          ///< engine RPM
  int speedKph() const { return m_pidVal[0x0D]; }     ///< vehicle speed, km/h
  int coolantC() const { return m_pidVal[0x05]; }     ///< coolant temp, °C
  int loadPct() const { return m_pidVal[0x04]; }      ///< engine load, %
  int throttlePct() const { return m_pidVal[0x11]; }  ///< throttle, %
  float volts() const { return m_volts; }             ///< battery voltage
  const char* vin() const { return m_vin; }           ///< VIN ("" until read)

  // ---------------------------------------------------------- full sweep
  /** True if a value for this mode-01 PID has been read. */
  bool hasPid(uint8_t pid) const { return m_pidHave[pid]; }
  /** Latest normalized value for a PID (see AutoTLMPids.h for units + fixed-point decimals). */
  int pidValue(uint8_t pid) const { return m_pidVal[pid]; }
  /** How many PIDs are being polled (advertised ∩ decodable). */
  int supportedCount() const { return m_nSupported; }
  /** How many PIDs the car ADVERTISES via its supported-PID bitmasks. */
  int advertisedCount() const { return m_nAdvertised; }
  /** Advertised PID i (sorted ascending; 0 when out of range). */
  uint8_t advertisedAt(int i) const {
    return (i >= 0 && i < m_nAdvertised) ? m_advertised[i] : 0;
  }

  // ---------------------------------------------------------------- DTCs
  int dtcCount() const { return m_dtcCount; }                ///< stored codes
  const char* dtcAt(int i) const;                            ///< e.g. "P0171"
  bool mil() const { return m_dtcCount > 0; }                ///< check-engine light inferred
  const char* dtcString() const { return m_dtcStr; }         ///< "P0171,P0420"
  /** Clear stored codes / the MIL (mode 04). */
  void clearDTCs();
  /** Register a callback fired once per newly-seen code. */
  void onDTC(AutoTLMDTCCallback cb) { m_dtcCb = cb; }
  /** Same, with the storing module's id (multi-module cars). */
  void onDTC(AutoTLMDTCModuleCallback cb) { m_dtcModCb = cb; }

  // ------------------------------------------------------------- modules
  /**
   * How many diagnosable modules (ECUs) answered enumeration — a real car
   * is several: engine, transmission, ABS, airbag... 0 means the board
   * can't enumerate (single-module view; everything above still works).
   */
  int moduleCount() const { return m_moduleCount; }
  /** Info for module i (id + per-kind DTC counts). Zeroed when out of range. */
  AutoTLMModuleInfo module(int i) const;
  /** Module i's stored / pending / permanent code j as text ("P0171"; "" OOR). */
  const char* moduleDtcAt(int i, int j) const;
  const char* modulePendingAt(int i, int j) const;
  const char* modulePermanentAt(int i, int j) const;

  // -------------------------------------------------------------- tuning
  void setPollInterval(uint32_t ms) { m_pollMs = ms; }   ///< default 300 ms
  void setDtcInterval(uint32_t ms) { m_dtcMs = ms; }     ///< default 20 s
  /** Did the ECU answer at any point this session (for diagnostics)? */
  bool everConnected() const { return m_everConnected; }
  /** Route library log lines somewhere else (nullptr = quiet). */
  void setLogStream(Stream* s) { m_log = s; }

  // ---------------------------------------------------------- freeze frame
  /** The DTC the stored freeze frame belongs to ("" when none). */
  const char* freezeCode() const { return m_freezeCode; }
  /** PID/value pairs snapshotted when that code set (count; see freezePidAt/freezeValAt). */
  int freezeCount() const { return m_freezeCount; }
  uint8_t freezePidAt(int i) const { return (i >= 0 && i < m_freezeCount) ? m_freezePid[i] : 0; }
  int freezeValAt(int i) const { return (i >= 0 && i < m_freezeCount) ? m_freezeVal[i] : 0; }

  // Snapshot support for the facade (copies state into the frame).
  void fillFrame(bool* pidHave, int* pidVal) const;
  void fillFrameLists(uint8_t* supported, uint8_t* supportedCount, size_t supportedCap,
                      char* freezeCode, size_t freezeCodeCap, uint8_t* freezePid,
                      int* freezeVal, uint8_t* freezeCount, size_t freezeCap) const;

 private:
  struct ModuleState {
    uint32_t id;
    uint16_t stored[AUTOTLM_MODULE_DTCS];
    uint16_t pending[AUTOTLM_MODULE_DTCS];
    uint16_t permanent[AUTOTLM_MODULE_DTCS];
    uint8_t nStored, nPending, nPermanent;
    uint16_t seen[AUTOTLM_MODULE_DTCS];  ///< codes already reported via callback
    uint8_t nSeen;
  };

  void tryInit();
  void discover();
  void pollPids();
  void readDTCs();
  void readModuleDTCs();
  void readFreeze();
  void rebuildAggregate();

  AutoTLMHAL* m_hal = nullptr;
  Stream* m_log = &Serial;

  bool m_connected = false;
  bool m_everConnected = false;
  uint32_t m_lastInitTry = 0;
  uint32_t m_lastPoll = 0;
  uint32_t m_lastDtc = 0;
  uint32_t m_pollMs = 300;
  uint32_t m_dtcMs = 20000;
  int m_failStreak = 0;

  int  m_pidVal[256] = {0};
  bool m_pidHave[256] = {false};
  uint8_t m_supported[96];       ///< polled set: advertised ∩ decodable
  int m_nSupported = 0;
  uint8_t m_advertised[96];      ///< everything the bitmasks advertise (sorted)
  int m_nAdvertised = 0;
  uint32_t m_sweepIdx = 0;  // unsigned: wraparound keeps the modulo in range

  // Freeze frame (mode 02, frame 0): the code that set it + a PID snapshot.
  char m_freezeCode[8] = "";
  uint8_t m_freezePid[12];
  int m_freezeVal[12];
  int m_freezeCount = 0;

  float m_volts = 0;
  char m_vin[24] = "";

  char m_dtcCodes[AUTOTLM_MAX_DTCS][8];
  int m_dtcCount = 0;
  char m_dtcStr[64] = "";
  uint16_t m_seenCodes[AUTOTLM_MAX_DTCS];
  int m_seenCount = 0;
  AutoTLMDTCCallback m_dtcCb = nullptr;
  AutoTLMDTCModuleCallback m_dtcModCb = nullptr;

  ModuleState m_modules[AUTOTLM_MAX_MODULES] = {};
  int m_moduleCount = 0;
  int m_moduleCursor = 0;
  mutable char m_fmtBuf[8];  ///< scratch for moduleDtcAt-family formatting
};

#endif // AUTOTLM_OBD_H
