/*
 * AutoTLMBle.h — the BLE change-WiFi / provisioning service.
 *
 * The owner's "change WiFi from the phone, on the fly" feature. A phone app
 * discovers the unit over BLE, proves possession with the per-device setup
 * code (the same 8-char label password that secures the WPA2 portal),
 * unlocks the session, picks a network from a device-side scan, writes
 * credentials, and watches an honest testing → connected | reverted(reason)
 * state machine — a projection of Core's validate-and-rollback engine.
 *
 * Security model (the locked cross-lane rulings + the 07-16 hardening pass):
 *  - LE Secure Connections, Just Works (no display on the unit), WITH
 *    bonding — the link is encrypted; iOS shows its system pairing prompt
 *    once. Just Works has no MITM protection, so encryption alone is NOT
 *    treated as "the owner"; every privileged operation additionally requires
 *    possession proof.
 *  - POSSESSION PROOF gates BOTH scan and creds. The phone first writes
 *    {"op":"auth","code":"XXXXXXXX"} (the 8-char label code); only then are
 *    scans honored and scan results populated, and only then is a creds write
 *    acted on. The check happens INSIDE this module against
 *    AutoTLMConfig::apPassword() — firmware consumers never handle the code,
 *    and the code is never stored server-side. Auth is per-connection and
 *    cleared on disconnect.
 *  - Advertising carries a Complete 128-bit Service-UUID list (so iOS
 *    `scanForPeripherals(withServices:)` matches) plus the exact device id in
 *    the scan-response service data (an identifier, not a credential). WHEN
 *    to advertise is the firmware's policy (advertise(bool)); the library
 *    never hard-codes always-on — a driving car must not be a followable
 *    beacon.
 *
 * GATT (service AUTOTLM_BLE_SERVICE_UUID):
 *  - CTRL   (write, encrypted):  {"op":"auth","code":"…"} then {"op":"scan"}.
 *  - SCAN   (read/notify, enc):  [{"ssid":"…","rssi":-52,"sec":true},…] top
 *                                10 by RSSI, deduped. Notify is a FRESHNESS
 *                                SIGNAL (payload may be MTU-truncated) — the
 *                                app must READ the characteristic (long read)
 *                                to get the full list. A leading "seq" field
 *                                lets the app detect a mid-read refresh.
 *  - CREDS  (write, encrypted):  {"code":"…","ssid":"…","pass":"…"}.
 *  - STATUS (read/notify, enc):  2 bytes [state, detail]. state: 0 idle,
 *                                1 creds_received, 2 testing, 3 connected,
 *                                4 reverted, 5 busy. detail (state 4): 1 ssid
 *                                not found, 2 auth failed, 3 timeout/other,
 *                                4 rejected (bad setup code), 5 missing ssid.
 *                                Terminal states hold until the next CREDS
 *                                write OR any other WiFi change (USB/sketch).
 *
 * BLE callbacks run on the Bluetooth task: they only copy bytes and set
 * flags (under the shared portMUX); everything real (auth, scan, changeWifi)
 * happens in tick() on the sketch core. Part of AutoTLM Core — MIT licensed.
 */
#ifndef AUTOTLM_BLE_H
#define AUTOTLM_BLE_H

#include <Arduino.h>

#define AUTOTLM_BLE_SERVICE_UUID "a0817000-7a1c-4c9a-9d10-000000000001"
#define AUTOTLM_BLE_CTRL_UUID    "a0817000-7a1c-4c9a-9d10-000000000002"
#define AUTOTLM_BLE_SCAN_UUID    "a0817000-7a1c-4c9a-9d10-000000000003"
#define AUTOTLM_BLE_CREDS_UUID   "a0817000-7a1c-4c9a-9d10-000000000004"
#define AUTOTLM_BLE_STATUS_UUID  "a0817000-7a1c-4c9a-9d10-000000000005"

/** STATUS characteristic states (byte 0). */
enum AutoTLMBleState {
  AUTOTLM_BLE_IDLE = 0,
  AUTOTLM_BLE_CREDS_RECEIVED = 1,
  AUTOTLM_BLE_TESTING = 2,
  AUTOTLM_BLE_CONNECTED = 3,
  AUTOTLM_BLE_REVERTED = 4,
  AUTOTLM_BLE_BUSY = 5,  ///< a change is already validating; retry after it settles
};
/** STATUS detail (byte 1) beyond the net-layer revert reasons 1..3. */
#define AUTOTLM_BLE_DETAIL_REJECTED 4  ///< write refused: bad setup code / not authed
#define AUTOTLM_BLE_DETAIL_NO_SSID  5  ///< authed creds write but ssid was missing/empty

class AutoTLM;
class AutoTLMBleImpl;  // Bluedroid-facing guts, hidden from consumers

/**
 * The BLE provisioning service. Owned by the AutoTLM facade — use
 * car.bleBegin() / car.bleAdvertise(on) / car.ble(); don't instantiate.
 */
class AutoTLMBle {
 public:
  bool begin(AutoTLM& car, const char* deviceId);

  /** Start/stop advertising (service data = device id). Policy lives with the caller. */
  void advertise(bool on);

  bool active() const { return m_impl != nullptr; }
  bool advertising() const { return m_advertising; }
  int state() const { return m_state; }

  /**
   * Feed a facade-observed WiFi-change transition (state = AUTOTLM_WIFI_*,
   * reason = AUTOTLM_WIFI_REASON_*). Called by AutoTLM::serviceWifiChange()
   * from a single coherent state snapshot BEFORE the terminal state is
   * consumed — this is what makes the STATUS characteristic race-free.
   */
  void feedWifiChange(int wifiState, int reason);

  /** Pump deferred work (auth, scan, pending creds → changeWifi). Facade calls from update(). */
  void tick();

  void setLogStream(Stream* s) { m_log = s; }

  // Connection lifecycle — called from the BLE server callbacks (BT task).
  void onDisconnect();

 private:
  void setStatus(uint8_t state, uint8_t detail);
  void runScanStep();

  AutoTLM* m_car = nullptr;
  AutoTLMBleImpl* m_impl = nullptr;
  Stream* m_log = &Serial;
  bool m_advertising = false;

  volatile uint8_t m_state = AUTOTLM_BLE_IDLE;
  uint8_t m_detail = 0;

  // Per-connection possession-proof gate (set in tick() after a valid code;
  // cleared on disconnect). Scans and creds are refused until authed.
  volatile bool m_sessionAuthed = false;

  // BT-task → tick() handoff (BT callbacks copy under m_lock, set the flag).
  volatile bool m_authPending = false;
  char m_pendAuthCode[16] = "";
  volatile bool m_credsPending = false;
  char m_pendCode[16] = "";
  char m_pendSsid[33] = "";
  char m_pendPass[65] = "";
  volatile bool m_scanRequested = false;
  bool m_scanRunning = false;
  bool m_scanRetried = false;
  uint32_t m_lastScanMs = 0;
  uint8_t m_scanSeq = 0;
#if defined(ESP32)
  portMUX_TYPE m_lock = portMUX_INITIALIZER_UNLOCKED;
#endif

  friend class AutoTLMBleImpl;
};

#endif  // AUTOTLM_BLE_H
