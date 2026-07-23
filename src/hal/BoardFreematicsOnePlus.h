/*
 * BoardFreematicsOnePlus.h — AutoTLMHAL for the Freematics ONE+ (ESP32).
 *
 * DEPRECATED. The ONE+ was only ever a capability benchmark while AutoTLM's
 * own hardware came together; the AutoTLM One (BoardAutoTLMOne) is the
 * product and needs no third-party library. This HAL is slated for removal
 * in a future release and gets no new features.
 *
 * The hardware recipe:
 *  - OBD-II via the co-processor link (FreematicsPlus COBD) — protocol
 *    negotiation, PID normalization and voltage sense all happen there.
 *  - GNSS (Beitian BE-220) on UART1, RX = GPIO26 @ 38400 baud — the module's
 *    TX/RX are swapped vs the Freematics factory pinout (GPIO34 @ 115200),
 *    which is why stock firmware reports GNSS:NO. Power on GPIO12.
 *  - IMU: ICM-42627 over I2C.
 *  - Status LED on GPIO4.
 *
 * Header-only so the sketch-level AUTOTLM_BOARD_FREEMATICS_ONEPLUS define can
 * select it. Requires the FreematicsPlus library (the arduino-esp32 3.x
 * patched copy lives in the volvo-telematics repo under tools/Freematics).
 *
 * Part of AutoTLM Core — MIT licensed.
 */
#ifndef AUTOTLM_BOARD_FREEMATICS_ONEPLUS_H
#define AUTOTLM_BOARD_FREEMATICS_ONEPLUS_H

#warning "BoardFreematicsOnePlus is deprecated and will be removed — target the AutoTLM One (BoardAutoTLMOne) instead."

#include <FreematicsPlus.h>

#include "AutoTLMHAL.h"

// The BE-220's real wiring (found the hard way; see repo history).
#define AUTOTLM_ONEPLUS_GNSS_RX 26
#define AUTOTLM_ONEPLUS_GNSS_BAUD 38400
#define AUTOTLM_ONEPLUS_GNSS_VCC 12
// PIN_LED (GPIO4) comes from FreematicsPlus.h.

class BoardFreematicsOnePlus : public AutoTLMHAL {
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
    if (!m_linkUp) return -1;                  // no link → no answer, not "no codes"
    return m_obd.readDTC(codes, maxCodes);     // co-processor: code count (0 = none)
  }

  int obdClearDTC(AutoTLMClearResponder* out, int max) override {
    (void)out; (void)max;
    if (!m_linkUp) return -1;
    // The co-processor sends Mode $04 but surfaces no per-ECU verdict, so this
    // board cannot honestly confirm the clear. Report "sent, unconfirmed" (0
    // responders) rather than fake a success — the caller then wipes nothing.
    m_obd.clearDTC();
    return 0;
  }

  bool obdVIN(char* buf, size_t bufsize) override {
    if (!m_linkUp || !m_obd.getVIN(buf, bufsize)) return false;
    // The co-processor hex-decodes whatever the bus said; keep only the
    // alphanumerics a real VIN can contain so garbage bytes can't reach
    // the telemetry frame.
    size_t out = 0;
    for (size_t i = 0; i < bufsize && buf[i]; i++) {
      if (isalnum((unsigned char)buf[i])) buf[out++] = buf[i];
    }
    if (out < bufsize) buf[out] = 0;
    return out > 0;
  }

  float obdBatteryVoltage() override {
    // The co-processor senses battery voltage directly (works without ECU).
    return m_linkUp ? m_obd.getVoltage() : NAN;
  }

  // ----------------------------------------------------------------- GNSS
  bool gnssBegin() override {
    m_gnss.begin(AUTOTLM_ONEPLUS_GNSS_BAUD, SERIAL_8N1, AUTOTLM_ONEPLUS_GNSS_RX, -1);
    return true;
  }

  void gnssPower(bool on) override {
    pinMode(AUTOTLM_ONEPLUS_GNSS_VCC, OUTPUT);
    digitalWrite(AUTOTLM_ONEPLUS_GNSS_VCC, on ? HIGH : LOW);
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

#endif // AUTOTLM_BOARD_FREEMATICS_ONEPLUS_H
