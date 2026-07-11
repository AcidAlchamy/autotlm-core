/*
 * BoardGenericEsp32.h — DrivonHAL for a plain ESP32 + CAN transceiver.
 *
 * The "roll your own Drivon Link" board: any ESP32 dev module wired to a
 * 3.3 V CAN transceiver (SN65HVD230 or similar) on the OBD-II port. Unlike
 * the ONE+ (which delegates OBD to a co-processor), this board speaks the
 * protocol itself: ISO 15765-4 over the ESP32's built-in TWAI controller at
 * 500 kbps / 11-bit — including ISO-TP multi-frame reassembly for VIN and
 * long DTC lists. Values are normalized with the shared rules in
 * DrivonPids.h, so telemetry is identical across boards.
 *
 * Default wiring (all overridable via the Pins struct):
 *   CAN  TX GPIO5 -> transceiver D, RX GPIO4 -> transceiver R
 *        CANH -> OBD pin 6, CANL -> OBD pin 14, GND -> OBD pin 5
 *   GNSS UART2: RX GPIO16, TX GPIO17 @ 9600 (typical NEO-6M/BE-220 module)
 *   IMU  MPU-6050 on I2C: SDA GPIO21, SCL GPIO22
 *   LED  GPIO2 (most devkits' onboard LED)
 *
 * Header-only so the sketch-level DRIVON_BOARD_GENERIC_ESP32 define can
 * select it.
 *
 * Part of Drivon Core — MIT licensed.
 */
#ifndef DRIVON_BOARD_GENERIC_ESP32_H
#define DRIVON_BOARD_GENERIC_ESP32_H

#if defined(ESP32)

#include <Wire.h>

#include "driver/twai.h"

#include "../core/DrivonPids.h"
#include "DrivonHAL.h"

// OBD-II over CAN: functional request id + the ECU response id window.
#define DRIVON_OBD_REQ_ID 0x7DF
#define DRIVON_OBD_RESP_MIN 0x7E8
#define DRIVON_OBD_RESP_MAX 0x7EF
// A real ECU answers in tens of ms; give slow ones some slack.
#define DRIVON_OBD_TIMEOUT_MS 200

#define DRIVON_MPU6050_ADDR 0x68

class BoardGenericEsp32 : public DrivonHAL {
 public:
  /** Override any pin to match your wiring; defaults suit a devkit jig. */
  struct Pins {
    int canTx = 5;
    int canRx = 4;
    int gnssRx = 16;
    int gnssTx = 17;
    long gnssBaud = 9600;
    int imuSda = 21;
    int imuScl = 22;
    int led = 2;  ///< -1 = no LED
  };

  BoardGenericEsp32() : m_gnss(2) {}
  explicit BoardGenericEsp32(const Pins& pins) : m_pins(pins), m_gnss(2) {}

  bool begin() override {
    if (m_pins.led >= 0) pinMode(m_pins.led, OUTPUT);
    return true;  // CAN comes up lazily with obdInit(); GNSS/IMU on request
  }

  const char* boardId() const override { return "generic-esp32"; }
  const char* deviceType() const override { return "esp32"; }

  // ------------------------------------------------------------------ OBD
  bool obdInit() override {
    if (!twaiUp()) return false;

    // Ask for the supported-PID bitmaps (chained: each range advertises the
    // next). No answer at all = no car on the bus.
    memset(m_pidmap, 0, sizeof(m_pidmap));
    bool any = false;
    for (uint8_t base = 0x00; base <= 0x60; base += 0x20) {
      if (base != 0 && !pidBit(base)) break;
      const uint8_t req[] = {0x01, base};
      uint8_t resp[8];
      const int n = obdRequest(req, sizeof(req), resp, sizeof(resp));
      if (n >= 6 && resp[0] == 0x41 && resp[1] == base) {
        memcpy(&m_pidmap[base / 8], &resp[2], 4);
        any = true;
      } else if (base == 0) {
        // One retry on the very first probe — the bus may still be waking.
        delay(100);
        const int n2 = obdRequest(req, sizeof(req), resp, sizeof(resp));
        if (n2 >= 6 && resp[0] == 0x41 && resp[1] == base) {
          memcpy(&m_pidmap[0], &resp[2], 4);
          any = true;
        } else {
          break;
        }
      }
    }
    return any;
  }

  void obdEnd() override {
    // Keep the driver installed; just forget the discovery so init re-probes.
    memset(m_pidmap, 0, sizeof(m_pidmap));
  }

  bool obdReadPID(uint8_t pid, int& value) override {
    const uint8_t req[] = {0x01, pid};
    uint8_t resp[8];
    const int n = obdRequest(req, sizeof(req), resp, sizeof(resp));
    if (n < 3 || resp[0] != 0x41 || resp[1] != pid) return false;
    const uint8_t A = resp[2];
    const uint8_t B = (n > 3) ? resp[3] : 0;
    value = drivon::normalizePid(pid, A, B);
    return true;
  }

  bool obdIsPIDSupported(uint8_t pid) override { return pidBit(pid); }

  int obdReadDTC(uint16_t* codes, int maxCodes) override {
    const uint8_t req[] = {0x03};
    uint8_t resp[64];
    const int n = obdRequest(req, sizeof(req), resp, sizeof(resp));
    if (n < 1 || resp[0] != 0x43) return 0;

    // Payload after 0x43: either DTC byte-pairs directly, or (newer ECUs) a
    // count byte first. A leading count makes the remainder length odd.
    int off = 1;
    int nBytes = n - 1;
    if (nBytes % 2 == 1) { off = 2; nBytes--; }

    int count = 0;
    for (int i = 0; i + 1 < nBytes && count < maxCodes; i += 2) {
      const uint16_t code = ((uint16_t)resp[off + i] << 8) | resp[off + i + 1];
      if (code != 0) codes[count++] = code;
    }
    return count;
  }

  void obdClearDTC() override {
    const uint8_t req[] = {0x04};
    uint8_t resp[8];
    obdRequest(req, sizeof(req), resp, sizeof(resp));  // expect 0x44 (or silence)
  }

  bool obdVIN(char* buf, size_t bufsize) override {
    const uint8_t req[] = {0x09, 0x02};
    uint8_t resp[40];
    const int n = obdRequest(req, sizeof(req), resp, sizeof(resp));
    // Payload: 49 02 <count> <17 VIN chars>
    if (n < 4 || resp[0] != 0x49 || resp[1] != 0x02) return false;
    size_t out = 0;
    for (int i = 3; i < n && out + 1 < bufsize; i++) {
      if (resp[i] >= 0x20 && resp[i] < 0x7F) buf[out++] = (char)resp[i];
    }
    buf[out] = 0;
    return out > 0;
  }

  // No analog battery sense on a bare devkit: DrivonOBD falls back to PID 0x42.
  float obdBatteryVoltage() override { return NAN; }

  // -------------------------------------------------------------- raw CAN
  bool canAvailable() const override { return m_twaiUp; }

  bool canRead(DrivonCanMsg& msg, uint32_t timeoutMs) override {
    if (!m_twaiUp) return false;
    twai_message_t rx;
    if (twai_receive(&rx, pdMS_TO_TICKS(timeoutMs)) != ESP_OK) return false;
    msg.id = rx.identifier;
    msg.extended = rx.extd;
    msg.len = rx.data_length_code;
    memcpy(msg.data, rx.data, 8);
    return true;
  }

  bool canWrite(const DrivonCanMsg& msg) override {
    if (!m_twaiUp && !twaiUp()) return false;
    twai_message_t tx = {};
    tx.identifier = msg.id;
    tx.extd = msg.extended;
    tx.data_length_code = msg.len;
    memcpy(tx.data, msg.data, 8);
    return twai_transmit(&tx, pdMS_TO_TICKS(20)) == ESP_OK;
  }

  // ----------------------------------------------------------------- GNSS
  bool gnssBegin() override {
    m_gnss.begin(m_pins.gnssBaud, SERIAL_8N1, m_pins.gnssRx, m_pins.gnssTx);
    return true;
  }

  int gnssAvailable() override { return m_gnss.available(); }
  int gnssRead() override { return m_gnss.read(); }

  // ------------------------------------------------------------------ IMU
  bool imuBegin() override {
    Wire.begin(m_pins.imuSda, m_pins.imuScl);
    // Wake the MPU-6050 (it boots asleep) and check it acknowledges.
    Wire.beginTransmission(DRIVON_MPU6050_ADDR);
    Wire.write(0x6B);  // PWR_MGMT_1
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) return false;
    m_imuUp = true;
    return true;
  }

  bool imuRead(float acc[3], float gyr[3]) override {
    if (!m_imuUp) return false;
    Wire.beginTransmission(DRIVON_MPU6050_ADDR);
    Wire.write(0x3B);  // ACCEL_XOUT_H
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(DRIVON_MPU6050_ADDR, 14) != 14) return false;
    int16_t raw[7];
    for (int i = 0; i < 7; i++) {
      raw[i] = ((int16_t)Wire.read() << 8) | Wire.read();
    }
    // raw[3] is the die temperature — skipped.
    acc[0] = raw[0] / 16384.0f;  // ±2 g full scale
    acc[1] = raw[1] / 16384.0f;
    acc[2] = raw[2] / 16384.0f;
    gyr[0] = raw[4] / 131.0f;    // ±250 °/s full scale
    gyr[1] = raw[5] / 131.0f;
    gyr[2] = raw[6] / 131.0f;
    return true;
  }

  const char* imuName() const override { return m_imuUp ? "MPU-6050" : ""; }

  // ----------------------------------------------------------------- misc
  void led(bool on) override {
    if (m_pins.led >= 0) digitalWrite(m_pins.led, on ? HIGH : LOW);
  }

 private:
  // Bring the TWAI controller up at 500 kbps and recover it when the bus
  // knocks it over (no transceiver attached, bus-off after error storms).
  bool twaiUp() {
    if (m_twaiUp) {
      twai_status_info_t st;
      if (twai_get_status_info(&st) == ESP_OK) {
        if (st.state == TWAI_STATE_BUS_OFF) twai_initiate_recovery();
        else if (st.state == TWAI_STATE_STOPPED) twai_start();
      }
      return true;
    }
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)m_pins.canTx, (gpio_num_t)m_pins.canRx, TWAI_MODE_NORMAL);
    g.rx_queue_len = 16;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
    if (twai_start() != ESP_OK) {
      twai_driver_uninstall();
      return false;
    }
    m_twaiUp = true;
    return true;
  }

  /** Supported-PID bitmap lookup (PID 0x00 is always askable). */
  bool pidBit(uint8_t pid) const {
    if (pid == 0) return true;
    const uint8_t i = (uint8_t)(pid - 1);
    return (m_pidmap[i >> 3] & (0x80 >> (i & 7))) != 0;
  }

  /**
   * One OBD request/response over ISO-TP. `req` is the bare service bytes
   * (e.g. {0x01, 0x0C}); the reassembled response payload (starting at the
   * 0x4x service byte) lands in `out`.
   * @return payload length, or -1 on timeout/error
   */
  int obdRequest(const uint8_t* req, uint8_t reqLen, uint8_t* out, size_t outCap) {
    if (!m_twaiUp && !twaiUp()) return -1;

    // Drop stale frames so we can't match a previous request's answer.
    twai_message_t rx;
    while (twai_receive(&rx, 0) == ESP_OK) {}

    twai_message_t tx = {};
    tx.identifier = DRIVON_OBD_REQ_ID;
    tx.data_length_code = 8;
    tx.data[0] = reqLen;  // single-frame PCI: length in the low nibble
    memcpy(&tx.data[1], req, reqLen);
    if (twai_transmit(&tx, pdMS_TO_TICKS(20)) != ESP_OK) {
      twaiUp();  // nudge recovery for next time
      return -1;
    }

    const uint32_t deadline = millis() + DRIVON_OBD_TIMEOUT_MS;
    int total = -1;    // expected payload length (multi-frame)
    int have = 0;      // bytes collected so far
    uint8_t nextSeq = 1;

    while ((int32_t)(deadline - millis()) > 0) {
      if (twai_receive(&rx, pdMS_TO_TICKS(20)) != ESP_OK) continue;
      if (rx.extd || rx.identifier < DRIVON_OBD_RESP_MIN || rx.identifier > DRIVON_OBD_RESP_MAX)
        continue;

      const uint8_t pci = rx.data[0] >> 4;

      if (pci == 0x0 && total < 0) {
        // Single frame: payload length in the low nibble.
        const int len = rx.data[0] & 0x0F;
        if (len < 1 || len > 7) continue;
        if (rx.data[1] == 0x7F) continue;  // negative response — keep waiting
        const int n = (len <= (int)outCap) ? len : (int)outCap;
        memcpy(out, &rx.data[1], n);
        return n;
      }

      if (pci == 0x1 && total < 0) {
        // First frame of a multi-frame response: 12-bit total length.
        total = (((int)rx.data[0] & 0x0F) << 8) | rx.data[1];
        if (total <= 0) return -1;
        have = 0;
        for (int i = 2; i < 8 && have < total && have < (int)outCap; i++) out[have++] = rx.data[i];

        // Flow control back to the ECU that answered: continue, no delay.
        twai_message_t fc = {};
        fc.identifier = rx.identifier - 8;  // 0x7E8 answers to 0x7E0, etc.
        fc.data_length_code = 8;
        fc.data[0] = 0x30;  // FC: ClearToSend, block size 0, no separation time
        twai_transmit(&fc, pdMS_TO_TICKS(20));
        continue;
      }

      if (pci == 0x2 && total > 0) {
        // Consecutive frame: check the 4-bit rolling sequence number.
        if ((rx.data[0] & 0x0F) != (nextSeq & 0x0F)) return -1;
        nextSeq++;
        for (int i = 1; i < 8 && have < total && have < (int)outCap; i++) out[have++] = rx.data[i];
        if (have >= total || have >= (int)outCap) return have;
      }
    }
    return (total > 0 && have > 0) ? have : -1;
  }

  Pins m_pins;
  HardwareSerial m_gnss;
  bool m_twaiUp = false;
  bool m_imuUp = false;
  uint8_t m_pidmap[32] = {0};
};

#endif // ESP32
#endif // DRIVON_BOARD_GENERIC_ESP32_H
