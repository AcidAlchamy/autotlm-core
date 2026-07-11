/*
 * BoardFreematicsOnePlus.h — DrivonHAL for the Freematics ONE+ (ESP32).
 *
 * This is the exact hardware recipe proven in the car:
 *  - OBD-II via the co-processor link (FreematicsPlus COBD) — protocol
 *    negotiation, PID normalization and voltage sense all happen there.
 *  - GNSS (Beitian BE-220) on UART1, RX = GPIO26 @ 38400 baud — the module's
 *    TX/RX are swapped vs the Freematics factory pinout (GPIO34 @ 115200),
 *    which is why stock firmware reports GNSS:NO. Power on GPIO12.
 *  - IMU: ICM-42627 over I2C.
 *  - Status LED on GPIO4.
 *
 * Header-only so the sketch-level DRIVON_BOARD_FREEMATICS_ONEPLUS define can
 * select it. Requires the FreematicsPlus library (the arduino-esp32 3.x
 * patched copy lives in the volvo-telematics repo under tools/Freematics).
 *
 * Part of Drivon Core — MIT licensed.
 */
#ifndef DRIVON_BOARD_FREEMATICS_ONEPLUS_H
#define DRIVON_BOARD_FREEMATICS_ONEPLUS_H

#include <FreematicsPlus.h>

#include "DrivonHAL.h"

// The BE-220's real wiring (found the hard way; see repo history).
#define DRIVON_ONEPLUS_GNSS_RX 26
#define DRIVON_ONEPLUS_GNSS_BAUD 38400
#define DRIVON_ONEPLUS_GNSS_VCC 12
// PIN_LED (GPIO4) comes from FreematicsPlus.h.

class BoardFreematicsOnePlus : public DrivonHAL {
 public:
  BoardFreematicsOnePlus() : m_gnss(1) {}

  bool begin() override {
    pinMode(PIN_LED, OUTPUT);
    // Co-processor link up (no cellular). This does NOT talk to the car —
    // obd.begin() only stores the link pointer.
    m_linkUp = m_sys.begin(true, false);
    if (m_linkUp) m_obd.begin(m_sys.link);
    snprintf(m_devType, sizeof(m_devType), "%u", (unsigned)m_sys.devType);
    return true;  // GNSS + IMU are independent of the link; board is usable
  }

  const char* boardId() const override { return "freematics-oneplus"; }
  const char* deviceType() const override { return m_devType; }

  // ------------------------------------------------------------------ OBD
  bool obdInit() override {
    if (!m_linkUp || !m_sys.link) return false;
    return m_obd.init(PROTO_AUTO);
  }

  void obdEnd() override {
    if (m_linkUp) m_obd.reset();
  }

  bool obdReadPID(uint8_t pid, int& value) override {
    return m_linkUp && m_obd.readPID(pid, value);
  }

  bool obdIsPIDSupported(uint8_t pid) override {
    return m_linkUp && m_obd.isValidPID(pid);
  }

  int obdReadDTC(uint16_t* codes, int maxCodes) override {
    if (!m_linkUp) return 0;
    return m_obd.readDTC(codes, maxCodes);
  }

  void obdClearDTC() override {
    if (m_linkUp) m_obd.clearDTC();
  }

  bool obdVIN(char* buf, size_t bufsize) override {
    return m_linkUp && m_obd.getVIN(buf, bufsize);
  }

  float obdBatteryVoltage() override {
    // The co-processor senses battery voltage directly (works without ECU).
    return m_linkUp ? m_obd.getVoltage() : NAN;
  }

  // ----------------------------------------------------------------- GNSS
  bool gnssBegin() override {
    m_gnss.begin(DRIVON_ONEPLUS_GNSS_BAUD, SERIAL_8N1, DRIVON_ONEPLUS_GNSS_RX, -1);
    return true;
  }

  void gnssPower(bool on) override {
    pinMode(DRIVON_ONEPLUS_GNSS_VCC, OUTPUT);
    digitalWrite(DRIVON_ONEPLUS_GNSS_VCC, on ? HIGH : LOW);
  }

  int gnssAvailable() override { return m_gnss.available(); }
  int gnssRead() override { return m_gnss.read(); }

  // ------------------------------------------------------------------ IMU
  bool imuBegin() override {
    m_imuUp = m_mems.begin() != 0;
    return m_imuUp;
  }

  bool imuRead(float acc[3], float gyr[3]) override {
    return m_imuUp && m_mems.read(acc, gyr);
  }

  const char* imuName() const override { return "ICM-42627"; }

  // ----------------------------------------------------------------- misc
  void led(bool on) override { digitalWrite(PIN_LED, on ? HIGH : LOW); }

  /** Direct access for power users (buzzer, xBee, co-processor commands). */
  FreematicsESP32& sys() { return m_sys; }
  COBD& rawObd() { return m_obd; }

 private:
  FreematicsESP32 m_sys;
  COBD m_obd;
  ICM_42627 m_mems;
  HardwareSerial m_gnss;
  bool m_linkUp = false;
  bool m_imuUp = false;
  char m_devType[12] = "16";
};

#endif // DRIVON_BOARD_FREEMATICS_ONEPLUS_H
