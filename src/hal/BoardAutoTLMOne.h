/*
 * BoardAutoTLMOne.h — AutoTLMHAL for the AutoTLM One dev dongle.
 *
 * The AutoTLM One HAL. Speaks OBD-II itself: ISO 15765-4 over the built-in
 * TWAI controller at 500 kbps / 11-bit — including ISO-TP multi-frame
 * reassembly for VIN and long DTC lists. Values are normalized with the
 * shared rules in AutoTLMPids.h.
 *
 * Pin assignments come from the AutoTLM One board package's variant
 * (AUTOTLM_PIN_* in pins_arduino.h) and can be overridden via the Pins
 * struct.
 *
 * Header-only so the sketch-level board define can select it.
 *
 * Part of AutoTLM Core — MIT licensed.
 */
#ifndef AUTOTLM_BOARD_AUTOTLM_ONE_H
#define AUTOTLM_BOARD_AUTOTLM_ONE_H

#if defined(ESP32)

#include <Wire.h>

#include "driver/twai.h"

#include "../core/AutoTLMPids.h"
#include "AutoTLMHAL.h"

// Pin defaults — the AutoTLM One board package's variant (pins_arduino.h) is
// the source of truth; these fallbacks match it.
#ifndef AUTOTLM_PIN_CAN_TX
#define AUTOTLM_PIN_CAN_TX 5
#endif
#ifndef AUTOTLM_PIN_CAN_RX
#define AUTOTLM_PIN_CAN_RX 4
#endif
#ifndef AUTOTLM_PIN_GNSS_RX
#define AUTOTLM_PIN_GNSS_RX 16
#endif
#ifndef AUTOTLM_PIN_GNSS_TX
#define AUTOTLM_PIN_GNSS_TX 17
#endif
#ifndef AUTOTLM_GNSS_BAUD
#define AUTOTLM_GNSS_BAUD 9600
#endif
#ifndef AUTOTLM_PIN_IMU_SDA
#define AUTOTLM_PIN_IMU_SDA 21
#endif
#ifndef AUTOTLM_PIN_IMU_SCL
#define AUTOTLM_PIN_IMU_SCL 22
#endif
#ifndef AUTOTLM_PIN_LED
#define AUTOTLM_PIN_LED 2
#endif

// OBD-II over CAN: functional request id + the ECU response id window.
#define AUTOTLM_OBD_REQ_ID 0x7DF
#define AUTOTLM_OBD_RESP_MIN 0x7E8
#define AUTOTLM_OBD_RESP_MAX 0x7EF
// A real ECU answers in tens of ms; give slow ones some slack.
#define AUTOTLM_OBD_TIMEOUT_MS 200

#define AUTOTLM_MPU6050_ADDR 0x68

class BoardAutoTLMOne : public AutoTLMHAL {
 public:
  /** Override any pin assignment; defaults suit the AutoTLM One. */
  struct Pins {
    int canTx = AUTOTLM_PIN_CAN_TX;
    int canRx = AUTOTLM_PIN_CAN_RX;
    int gnssRx = AUTOTLM_PIN_GNSS_RX;
    int gnssTx = AUTOTLM_PIN_GNSS_TX;
    long gnssBaud = AUTOTLM_GNSS_BAUD;
    int imuSda = AUTOTLM_PIN_IMU_SDA;
    int imuScl = AUTOTLM_PIN_IMU_SCL;
    int led = AUTOTLM_PIN_LED;  ///< -1 = no LED
  };

  BoardAutoTLMOne() : m_gnss(2) {}
  explicit BoardAutoTLMOne(const Pins& pins) : m_pins(pins), m_gnss(2) {}

  bool begin() override {
    if (m_pins.led >= 0) pinMode(m_pins.led, OUTPUT);
    return true;  // CAN comes up lazily with obdInit(); GNSS/IMU on request
  }

  const char* boardId() const override { return "autotlm-one"; }
  const char* deviceType() const override { return "one"; }

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
    value = autotlm::normalizePid(pid, A, B);
    return true;
  }

  bool obdIsPIDSupported(uint8_t pid) override { return pidBit(pid); }

  int obdFreezeDTC() override {
    // Mode 02 PID 02, frame 0 → "42 02 00 <code hi> <code lo>".
    const uint8_t req[] = {0x02, 0x02, 0x00};
    uint8_t resp[8];
    const int n = obdRequest(req, sizeof(req), resp, sizeof(resp));
    if (n < 5 || resp[0] != 0x42 || resp[1] != 0x02) return -1;
    return ((int)resp[3] << 8) | resp[4];  // 0 = no freeze frame stored
  }

  bool obdReadFreezePID(uint8_t pid, int& value) override {
    // Mode 02 payload mirrors mode 01 with a frame byte: "42 <pid> <frame> <data...>".
    const uint8_t req[] = {0x02, pid, 0x00};
    uint8_t resp[8];
    const int n = obdRequest(req, sizeof(req), resp, sizeof(resp));
    if (n < 4 || resp[0] != 0x42 || resp[1] != pid) return false;
    const uint8_t A = resp[3];
    const uint8_t B = (n > 4) ? resp[4] : 0;
    value = autotlm::normalizePid(pid, A, B);
    return true;
  }

  int obdReadDTC(uint16_t* codes, int maxCodes) override {
    const uint8_t req[] = {0x03};
    uint8_t resp[64];
    const int n = obdRequest(req, sizeof(req), resp, sizeof(resp));
    if (n < 1 || resp[0] != 0x43) return 0;
    return parseDtcPayload(resp, n, codes, maxCodes);
  }

  int obdEnumerate(uint32_t* respIds, int max) override {
    if (max <= 0 || (!m_twaiUp && !twaiUp())) return 0;

    // Drain stale frames, then ask a question EVERY emissions module must
    // answer (mode 01 PID 00) and collect the distinct responders for the
    // full timeout window — there is no way to know how many will speak.
    twai_message_t rx;
    while (twai_receive(&rx, 0) == ESP_OK) {}

    twai_message_t tx = {};
    tx.identifier = AUTOTLM_OBD_REQ_ID;
    tx.data_length_code = 8;
    tx.data[0] = 2;
    tx.data[1] = 0x01;
    tx.data[2] = 0x00;
    if (twai_transmit(&tx, pdMS_TO_TICKS(20)) != ESP_OK) {
      twaiUp();
      return 0;
    }

    int n = 0;
    const uint32_t deadline = millis() + AUTOTLM_OBD_TIMEOUT_MS;
    while ((int32_t)(deadline - millis()) > 0 && n < max) {
      if (twai_receive(&rx, pdMS_TO_TICKS(20)) != ESP_OK) continue;
      if (rx.extd || rx.identifier < AUTOTLM_OBD_RESP_MIN || rx.identifier > AUTOTLM_OBD_RESP_MAX)
        continue;
      bool dup = false;
      for (int i = 0; i < n; i++) {
        if (respIds[i] == rx.identifier) { dup = true; break; }
      }
      if (!dup) respIds[n++] = rx.identifier;
    }
    return n;
  }

  int obdReadDTCFrom(uint32_t respId, uint8_t mode, uint16_t* codes, int maxCodes) override {
    if (respId < AUTOTLM_OBD_RESP_MIN || respId > AUTOTLM_OBD_RESP_MAX) return -1;
    if (mode != 0x03 && mode != 0x07 && mode != 0x0A) return -1;
    const uint8_t req[] = {mode};
    uint8_t resp[64];
    // Physical addressing: 0x7E8 listens on 0x7E0, etc.; accept only this
    // module's answer so a chatty neighbor can't be misattributed.
    const int n = obdRequestTo(respId - 8, respId, req, sizeof(req), resp, sizeof(resp));
    if (n < 1 || resp[0] != (uint8_t)(mode + 0x40)) return -1;
    return parseDtcPayload(resp, n, codes, maxCodes);
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
    // Payload: 49 02 <count> <17 VIN chars>. Require the full 17 bytes — a
    // short answer is a broken transfer, not a VIN.
    if (n < 20 || resp[0] != 0x49 || resp[1] != 0x02) return false;
    size_t out = 0;
    for (int i = 3; i < n && out + 1 < bufsize; i++) {
      // Real VINs are strictly alphanumeric; anything else is line noise
      // (and would need escaping downstream in the JSON frame).
      if (isalnum(resp[i])) buf[out++] = (char)resp[i];
    }
    buf[out] = 0;
    return out > 0;
  }

  // No analog battery sense on this revision: AutoTLMOBD falls back to PID 0x42.
  float obdBatteryVoltage() override { return NAN; }

  // -------------------------------------------------------------- raw CAN
  bool canAvailable() const override { return m_twaiUp; }

  bool canRead(AutoTLMCanMsg& msg, uint32_t timeoutMs) override {
    if (!m_twaiUp) return false;
    twai_message_t rx;
    if (twai_receive(&rx, pdMS_TO_TICKS(timeoutMs)) != ESP_OK) return false;
    msg.id = rx.identifier;
    msg.extended = rx.extd;
    msg.len = rx.data_length_code;
    memcpy(msg.data, rx.data, 8);
    return true;
  }

  bool canWrite(const AutoTLMCanMsg& msg) override {
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
    Wire.beginTransmission(AUTOTLM_MPU6050_ADDR);
    Wire.write(0x6B);  // PWR_MGMT_1
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) return false;
    m_imuUp = true;
    return true;
  }

  bool imuRead(float acc[3], float gyr[3]) override {
    if (!m_imuUp) return false;
    Wire.beginTransmission(AUTOTLM_MPU6050_ADDR);
    Wire.write(0x3B);  // ACCEL_XOUT_H
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(AUTOTLM_MPU6050_ADDR, 14) != 14) return false;
    int16_t raw[7];
    for (int i = 0; i < 7; i++) {
      // Two named reads: C++ leaves the operand order of | unspecified, and
      // a byte-swapped IMU sample would be silent garbage.
      const uint8_t hi = (uint8_t)Wire.read();
      const uint8_t lo = (uint8_t)Wire.read();
      raw[i] = (int16_t)(((uint16_t)hi << 8) | lo);
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
  // knocks it over (unplugged from the car, bus-off after error storms).
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

  /** Payload after the 0x43/0x47/0x4A service byte: either DTC byte-pairs
      directly, or (newer ECUs) a count byte first — a leading count makes
      the remainder length odd. @return codes extracted */
  static int parseDtcPayload(const uint8_t* resp, int n, uint16_t* codes, int maxCodes) {
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

  /** Functional request: broadcast id, first answering module wins. */
  int obdRequest(const uint8_t* req, uint8_t reqLen, uint8_t* out, size_t outCap) {
    return obdRequestTo(AUTOTLM_OBD_REQ_ID, 0, req, reqLen, out, outCap);
  }

  /**
   * One OBD request/response over ISO-TP. `req` is the bare service bytes
   * (e.g. {0x01, 0x0C}); the reassembled response payload (starting at the
   * 0x4x service byte) lands in `out`.
   * @param reqId    CAN id to send on (0x7DF functional, 0x7E0.. physical)
   * @param onlyResp accept answers only from this responder (0 = whole
   *                 0x7E8..0x7EF window, first answer wins)
   * @return payload length, or -1 on timeout/error
   */
  int obdRequestTo(uint32_t reqId, uint32_t onlyResp, const uint8_t* req, uint8_t reqLen,
                   uint8_t* out, size_t outCap) {
    if (!m_twaiUp && !twaiUp()) return -1;

    // Drop stale frames so we can't match a previous request's answer.
    twai_message_t rx;
    while (twai_receive(&rx, 0) == ESP_OK) {}

    twai_message_t tx = {};
    tx.identifier = reqId;
    tx.data_length_code = 8;
    tx.data[0] = reqLen;  // single-frame PCI: length in the low nibble
    memcpy(&tx.data[1], req, reqLen);
    if (twai_transmit(&tx, pdMS_TO_TICKS(20)) != ESP_OK) {
      twaiUp();  // nudge recovery for next time
      return -1;
    }

    uint32_t deadline = millis() + AUTOTLM_OBD_TIMEOUT_MS;
    int total = -1;    // expected payload length (multi-frame)
    int have = 0;      // bytes collected so far
    uint8_t nextSeq = 1;

    // What a positive answer to THIS request must start with. On a
    // multi-module car several ECUs answer every functional request; a
    // straggler answering the PREVIOUS request can land in this window, and
    // accepting it would fail the read (and, repeated, drop the connection).
    const uint8_t wantSvc = req[0] + 0x40;
    const bool wantPid = reqLen >= 2;

    while ((int32_t)(deadline - millis()) > 0) {
      if (twai_receive(&rx, pdMS_TO_TICKS(20)) != ESP_OK) continue;
      if (rx.extd || rx.identifier < AUTOTLM_OBD_RESP_MIN || rx.identifier > AUTOTLM_OBD_RESP_MAX)
        continue;
      if (onlyResp && rx.identifier != onlyResp) continue;

      const uint8_t pci = rx.data[0] >> 4;

      if (pci == 0x0 && total < 0) {
        // Single frame: payload length in the low nibble.
        const int len = rx.data[0] & 0x0F;
        if (len < 1 || len > 7) continue;
        if (rx.data[1] == 0x7F) {
          // Negative response. NRC 0x78 = "response pending": the ECU asks
          // for more time (common for VIN/DTC requests) — grant it.
          if (len >= 3 && rx.data[3] == 0x78) deadline = millis() + 2000;
          continue;
        }
        // Not an answer to this request? Keep waiting for the one that is.
        if (rx.data[1] != wantSvc) continue;
        if (wantPid && (len < 2 || rx.data[2] != req[1])) continue;
        const int n = (len <= (int)outCap) ? len : (int)outCap;
        memcpy(out, &rx.data[1], n);
        return n;
      }

      if (pci == 0x1 && total < 0) {
        // First frame of a multi-frame response — same match rule as above
        // (payload starts at data[2] in a first frame).
        if (rx.data[2] != wantSvc) continue;
        if (wantPid && rx.data[3] != req[1]) continue;
        total = (((int)rx.data[0] & 0x0F) << 8) | rx.data[1];
        if (total <= 0 || total > (int)outCap) return -1;  // can't hold it -> honest failure
        have = 0;
        for (int i = 2; i < 8 && have < total; i++) out[have++] = rx.data[i];

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
        for (int i = 1; i < 8 && have < total; i++) out[have++] = rx.data[i];
        if (have >= total) return have;
      }
    }
    // Timed out. A half-collected multi-frame response is NOT a response —
    // returning it would hand callers truncated VINs and phantom DTC pairs.
    return -1;
  }

  Pins m_pins;
  HardwareSerial m_gnss;
  bool m_twaiUp = false;
  bool m_imuUp = false;
  uint8_t m_pidmap[32] = {0};
};

#endif // ESP32
#endif // AUTOTLM_BOARD_AUTOTLM_ONE_H
