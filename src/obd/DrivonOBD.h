/*
 * DrivonOBD.h — the OBD-II module: PIDs, DTCs, VIN.
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
 * Part of Drivon Core — MIT licensed.
 */
#ifndef DRIVON_OBD_H
#define DRIVON_OBD_H

#include <Arduino.h>
#include "../hal/DrivonHAL.h"

/** Fired when a trouble code appears that wasn't present before. */
typedef void (*DrivonDTCCallback)(const char* code);

#define DRIVON_MAX_DTCS 10

class DrivonOBD {
 public:
  /** Wire up the HAL. Does not touch the car. */
  void begin(DrivonHAL& hal);

  /**
   * Pump the module: lazy ECU bring-up, PID polling, periodic DTC reads.
   * Called from Drivon::update(); may block briefly while talking to the bus.
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
  /** Latest normalized value for a PID (see DrivonPids.h for units). */
  int pidValue(uint8_t pid) const { return m_pidVal[pid]; }
  /** How many PIDs the car declared support for. */
  int supportedCount() const { return m_nSupported; }

  // ---------------------------------------------------------------- DTCs
  int dtcCount() const { return m_dtcCount; }                ///< stored codes
  const char* dtcAt(int i) const;                            ///< e.g. "P0171"
  bool mil() const { return m_dtcCount > 0; }                ///< check-engine light inferred
  const char* dtcString() const { return m_dtcStr; }         ///< "P0171,P0420"
  /** Clear stored codes / the MIL (mode 04). */
  void clearDTCs();
  /** Register a callback fired once per newly-seen code. */
  void onDTC(DrivonDTCCallback cb) { m_dtcCb = cb; }

  // -------------------------------------------------------------- tuning
  void setPollInterval(uint32_t ms) { m_pollMs = ms; }   ///< default 300 ms
  void setDtcInterval(uint32_t ms) { m_dtcMs = ms; }     ///< default 20 s
  /** Did the ECU answer at any point this session (for diagnostics)? */
  bool everConnected() const { return m_everConnected; }
  /** Route library log lines somewhere else (nullptr = quiet). */
  void setLogStream(Stream* s) { m_log = s; }

  // Snapshot support for the facade (copies state into the frame).
  void fillFrame(bool* pidHave, int* pidVal) const;

 private:
  void tryInit();
  void discover();
  void pollPids();
  void readDTCs();

  DrivonHAL* m_hal = nullptr;
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
  uint8_t m_supported[64];
  int m_nSupported = 0;
  int m_sweepIdx = 0;

  float m_volts = 0;
  char m_vin[24] = "";

  char m_dtcCodes[DRIVON_MAX_DTCS][8];
  int m_dtcCount = 0;
  char m_dtcStr[64] = "";
  uint16_t m_seenCodes[DRIVON_MAX_DTCS];
  int m_seenCount = 0;
  DrivonDTCCallback m_dtcCb = nullptr;
};

#endif // DRIVON_OBD_H
