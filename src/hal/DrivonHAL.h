/*
 * DrivonHAL.h — the hardware abstraction layer at the heart of Drivon Core.
 *
 * Every Drivon module (OBD, GNSS, IMU, networking) talks to the car and the
 * board through this interface only — never to pins, UARTs or CAN controllers
 * directly. That is what lets the exact same sketch run on a Freematics ONE+
 * today and on a Drivon Link board (or your own hardware) tomorrow: you swap
 * the board class, nothing else.
 *
 * Shipping implementations:
 *   - BoardGenericEsp32       (hal/BoardGenericEsp32.h) — the primary target:
 *     our own ESP32 hardware wired to a 3.3 V CAN transceiver (SN65HVD230).
 *     Speaks ISO 15765-4 (ISO-TP over TWAI) itself, GNSS on a configurable
 *     UART, optional MPU-6050 IMU. This is what Drivon Link is built on.
 *   - BoardFreematicsOnePlus  (hal/BoardFreematicsOnePlus.h) — a compatibility
 *     board and capability benchmark (a commercial ESP32 OBD dongle). OBD via
 *     its co-processor, GNSS on RX=GPIO26 @ 38400, ICM-42627 IMU.
 *
 * Porting to new hardware = subclassing this and implementing what the board
 * has. Anything the board lacks can honestly return false; the modules treat
 * that as "not fitted" and carry on.
 *
 * Part of Drivon Core — MIT licensed.
 */
#ifndef DRIVON_HAL_H
#define DRIVON_HAL_H

#include <Arduino.h>

/** A raw CAN frame, for power users doing bus work beside OBD polling. */
struct DrivonCanMsg {
  uint32_t id;        ///< 11- or 29-bit identifier
  bool     extended;  ///< true = 29-bit id
  uint8_t  len;       ///< 0..8
  uint8_t  data[8];
};

/**
 * Abstract board interface. All methods that talk to the vehicle bus are
 * allowed to block briefly (they run on the sensor core); anything called
 * from the network task must not go through the HAL.
 */
class DrivonHAL {
 public:
  virtual ~DrivonHAL() {}

  /**
   * Bring the board itself up (pins, co-processor link, I2C...). Called once
   * from Drivon::begin(). Must NOT touch the vehicle bus — OBD is initialized
   * lazily so a stalled car bus can never block connectivity.
   * @return true if the board is usable (individual subsystems may still be absent)
   */
  virtual bool begin() = 0;

  /** Short identifier for this board, e.g. "freematics-oneplus". */
  virtual const char* boardId() const = 0;

  /** Device-type string reported in telemetry (e.g. the ONE+ reports "16"). */
  virtual const char* deviceType() const { return boardId(); }

  // ------------------------------------------------------------------ OBD-II
  /**
   * Connect to the ECU (protocol negotiation / supported-PID discovery as the
   * transport needs). Blocking; called lazily and re-tried by DrivonOBD.
   * @return true once the car answers
   */
  virtual bool obdInit() = 0;

  /** Tear the OBD connection down so obdInit() can be retried cleanly. */
  virtual void obdEnd() {}

  /**
   * Read one mode-01 PID, normalized to the Drivon value convention
   * (RPM in rpm, temperatures in °C, percents 0-100, speed in km/h — the
   * same integer normalization the Freematics co-processor applies, so all
   * boards report identical numbers).
   */
  virtual bool obdReadPID(uint8_t pid, int& value) = 0;

  /** True if the ECU advertised this PID in its supported-PID bitmap. */
  virtual bool obdIsPIDSupported(uint8_t pid) = 0;

  /**
   * Read stored diagnostic trouble codes.
   * @return number of codes written into `codes` (0 = none / not readable)
   */
  virtual int obdReadDTC(uint16_t* codes, int maxCodes) = 0;

  /** Clear stored DTCs and turn the MIL off (mode 04). */
  virtual void obdClearDTC() {}

  /** Read the VIN (mode 09 PID 02). @return true and a NUL-terminated string on success */
  virtual bool obdVIN(char* buf, size_t bufsize) { (void)buf; (void)bufsize; return false; }

  /**
   * Battery voltage. Boards with a voltage sense (the ONE+ co-processor)
   * measure it directly; boards without return NAN and DrivonOBD falls back
   * to PID 0x42 (control module voltage).
   */
  virtual float obdBatteryVoltage() { return NAN; }

  // ---------------------------------------------------------------- raw CAN
  /** Optional raw CAN access. Boards route this through the same controller as OBD. */
  virtual bool canAvailable() const { return false; }
  virtual bool canRead(DrivonCanMsg& msg, uint32_t timeoutMs) { (void)msg; (void)timeoutMs; return false; }
  virtual bool canWrite(const DrivonCanMsg& msg) { (void)msg; return false; }

  // ------------------------------------------------------------------- GNSS
  /** Power + open the GNSS UART. @return true if the port opened */
  virtual bool gnssBegin() = 0;
  /** Toggle GNSS module power, where the board controls it. */
  virtual void gnssPower(bool on) { (void)on; }
  /** Bytes waiting on the GNSS UART. */
  virtual int gnssAvailable() = 0;
  /** Read one byte from the GNSS UART (-1 if none). */
  virtual int gnssRead() = 0;

  // -------------------------------------------------------------------- IMU
  /** Initialize the motion sensor. @return true if one is fitted and alive */
  virtual bool imuBegin() = 0;
  /** Latest acceleration (g) and rotation rate (deg/s). */
  virtual bool imuRead(float acc[3], float gyr[3]) = 0;
  /** IMU chip name for telemetry ("" when none). */
  virtual const char* imuName() const { return ""; }

  // ------------------------------------------------------------------- misc
  /** Drive the status LED (no-op on boards without one). */
  virtual void led(bool on) { (void)on; }
};

#endif // DRIVON_HAL_H
