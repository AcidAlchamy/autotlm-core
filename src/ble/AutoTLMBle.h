/*
 * AutoTLMBle.h — the BLE change-WiFi / provisioning service (NimBLE host).
 *
 * The owner's "change WiFi from the phone, on the fly" feature. A phone app
 * discovers the unit over BLE, proves possession with the per-device setup
 * code (the same 8-char label password that secures the WPA2 portal),
 * unlocks the session, picks a network from a device-side scan, writes
 * credentials, and watches an honest testing → connected | reverted(reason)
 * state machine — a projection of Core's validate-and-rollback engine.
 *
 * HOST STACK: NimBLE (h2zero/NimBLE-Arduino — the one owner-sanctioned
 * third-party library). The previous Bluedroid implementation could not
 * coexist with WiFi in the plain ESP32's internal RAM: with Bluedroid
 * resident, the WiFi driver's DMA RX buffers failed to allocate ("Expected
 * to init 4 rx buffer, actual is 0"), starving the very change-WiFi
 * validation this service exists to run. NimBLE's resident footprint is a
 * fraction of Bluedroid's; the owner ruled the migration on 2026-07-17.
 *
 * COMPILED IN only when AUTOTLM_ENABLE_BLE — resolved in AutoTLMBle.cpp as
 * a BUILD-WIDE macro (default ON for the AutoTLM One via ARDUINO_AUTOTLM_ONE,
 * OFF for generic boards; override with a `-DAUTOTLM_ENABLE_BLE=…` build
 * property). Never define it in a sketch: a sketch #define cannot reach the
 * library's translation units, so it would silently split the class layout
 * (the v0.7.1 opt-in worked that way and was a latent ODR bug — this class
 * is now layout-identical in every build, and the stub begin() just returns
 * false when BLE is compiled out).
 *
 * Security model (the locked cross-lane rulings + the 07-16 hardening pass):
 *  - LE Secure Connections, Just Works (no display on the unit), WITH
 *    bonding — the link is encrypted; iOS shows its system pairing prompt
 *    once. Just Works has no MITM protection, so encryption alone is NOT
 *    treated as "the owner"; every privileged operation additionally
 *    requires possession proof.
 *  - POSSESSION PROOF gates BOTH scan and creds. The phone first writes
 *    {"op":"auth","code":"XXXXXXXX"} (the 8-char label code); only then are
 *    scans honored and scan results populated, and only then is a creds
 *    write acted on. The check happens INSIDE this module against
 *    AutoTLMConfig::apPassword() — firmware consumers never handle the
 *    code, and the code is never stored server-side. Auth is
 *    per-connection and cleared on disconnect; an unauthed central is
 *    dropped after a grace window so a stalled connect can't camp the unit.
 *  - Advertising carries the Complete 128-bit Service-UUID list (so iOS
 *    `scanForPeripherals(withServices:)` matches) plus the exact device id
 *    in the scan-response service data (an identifier, not a credential).
 *    WHEN to advertise is the firmware's policy (advertise(bool)); the
 *    library never hard-codes always-on — a driving car must not be a
 *    followable beacon.
 *
 * GATT (service AUTOTLM_BLE_SERVICE_UUID):
 *  - CTRL   (write, encrypted):  {"op":"auth","code":"…"} then {"op":"scan"}.
 *  - SCAN   (read/notify, enc):  {"seq":N,"nets":[{"ssid":"…","rssi":-52,
 *                                "sec":true},…]} top 10 by RSSI, deduped.
 *                                Notify is a FRESHNESS SIGNAL — READ the
 *                                characteristic for the full list; "seq"
 *                                detects a mid-read refresh. "[]" until the
 *                                session is authed.
 *  - CREDS  (write, encrypted):  {"code":"…","ssid":"…","pass":"…"}.
 *  - STATUS (read/notify, enc):  2 bytes [state, detail]. state: 0 idle,
 *                                1 creds_received, 2 testing, 3 connected,
 *                                4 reverted, 5 busy, 6 auth_ok. detail
 *                                (state 4): 1 ssid not found, 2 auth
 *                                failed, 3 timeout/other, 4 rejected (bad
 *                                setup code / not authed), 5 missing ssid.
 *                                Terminal states hold until the next CREDS
 *                                write, any other WiFi change, or an auth
 *                                op — reconnecting apps READ STATUS FIRST
 *                                to collect a held verdict, then auth.
 *
 * BLE callbacks run on the NimBLE host task: they only copy bytes and set
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
// TELEMETRY (read/notify, encrypted): the local live-data stream. Notifies a
// compact AutoTLMFrame (AutoTLMFrame::toJsonLive — same field names as the
// cloud frame, trimmed to one MTU) at ~telemetryHz to an AUTHED, subscribed
// phone, so the app/CarPlay reads live gauges straight from the unit instead
// of round-tripping the cloud. Gated behind the session setup-code auth (the
// frame carries GPS). Streams only while a central is connected + authed.
#define AUTOTLM_BLE_TELEM_UUID   "a0817000-7a1c-4c9a-9d10-000000000006"

/** STATUS characteristic states (byte 0). */
enum AutoTLMBleState {
  AUTOTLM_BLE_IDLE = 0,
  AUTOTLM_BLE_CREDS_RECEIVED = 1,
  AUTOTLM_BLE_TESTING = 2,
  AUTOTLM_BLE_CONNECTED = 3,
  AUTOTLM_BLE_REVERTED = 4,
  AUTOTLM_BLE_BUSY = 5,     ///< a change is already validating; retry after it settles
  AUTOTLM_BLE_AUTH_OK = 6,  ///< setup code accepted — session unlocked (positive ack)
};
/** STATUS detail (byte 1) beyond the net-layer revert reasons 1..3. */
#define AUTOTLM_BLE_DETAIL_REJECTED 4  ///< write refused: bad setup code / not authed
#define AUTOTLM_BLE_DETAIL_NO_SSID  5  ///< authed creds write but ssid was missing/empty

class AutoTLM;
class AutoTLMBleImpl;  // NimBLE-facing guts, hidden from consumers

/**
 * The BLE provisioning service. Owned by the AutoTLM facade — use
 * car.bleBegin() / car.bleAdvertise(on) / car.ble(); don't instantiate.
 */
class AutoTLMBle {
 public:
  /**
   * Bring the NimBLE stack + GATT service up (idempotent). Does NOT
   * advertise — discoverability is the caller's policy via advertise().
   * Returns false — never aborts — when BLE is compiled out, or when there
   * isn't enough heap for a healthy bring-up (call it early: right after
   * car.begin(), before provision()).
   */
  bool begin(AutoTLM& car, const char* deviceId);

  /** Start/stop advertising (service data = device id). Policy lives with the caller. */
  void advertise(bool on);

  /**
   * Enable/disable the local BLE telemetry stream (default ON). When on, an
   * authed + subscribed phone receives a compact live frame ~`hz` times a
   * second over the bonded encrypted link — the CarPlay/app "live gauges"
   * feed, no cloud round-trip. @param hz 1..10 (clamped)
   */
  void telemetry(bool on, uint8_t hz = 4);

  bool active() const { return m_impl != nullptr; }
  bool advertising() const { return m_advertising; }
  /** Current STATUS characteristic state byte (AUTOTLM_BLE_*). */
  int state() const { return m_state; }

  /**
   * Forget every bonded phone (NimBLE bond store). Call when the unit is
   * factory-reset / NVS-wiped / re-provisioned for a new owner — a stale
   * bond orphans the phone's keys and every reconnect fails until the user
   * manually "forgets" the device. @return true if bonds were cleared
   */
  bool clearBonds();

  /**
   * Feed a facade-observed WiFi-change transition (state = AUTOTLM_WIFI_*,
   * reason = AUTOTLM_WIFI_REASON_*). Called by AutoTLM::serviceWifiChange()
   * from a single coherent state snapshot BEFORE the terminal state is
   * consumed — this is what makes the STATUS characteristic race-free.
   */
  void feedWifiChange(int wifiState, int reason);

  /** Pump deferred work (auth, scan, pending creds → changeWifi, unauthed-central timeout). */
  void tick();

  void setLogStream(Stream* s) { m_log = s; }

  // Connection lifecycle — called from the NimBLE server callbacks (host task).
  void onConnect(uint16_t connHandle);
  void onDisconnect(uint16_t connHandle);

 private:
  void setStatus(uint8_t state, uint8_t detail);
  void runScanStep();
  void reconcileAdvertising();  // sketch-core: drive the radio to match m_advertising
  bool peerEncrypted() const;   // is the tracked central's link encrypted?

  AutoTLM* m_car = nullptr;
  AutoTLMBleImpl* m_impl = nullptr;
  Stream* m_log = &Serial;
  bool m_advertising = false;   // DESIRED state; tick() reconciles the radio to it
  bool m_beginFailed = false;   // latch: a failed bring-up must not retry every loop()

  volatile uint8_t m_state = AUTOTLM_BLE_IDLE;
  uint8_t m_detail = 0;

  // Possession-proof gate, BOUND TO A CONNECTION. m_sessionAuthed alone is not
  // enough — NimBLE hands out connection handles that get reused, so an auth
  // must only count for the exact connection that presented the code
  // (m_authedHandle == m_connHandle). Cleared on disconnect. A single central
  // at a time is enforced structurally: we don't advertise while connected.
  volatile bool m_sessionAuthed = false;
  volatile uint16_t m_authedHandle = 0xFFFF;  ///< connection the auth belongs to
  uint8_t m_authFails = 0;                     ///< bad-code attempts this connection
  // Unauthed-central grace window: a central that connects and never auths is
  // dropped so a stalled/hostile connect can't camp the unit.
  volatile uint16_t m_connHandle = 0xFFFF;  ///< 0xFFFF = none
  volatile uint32_t m_connectMs = 0;
  volatile bool m_needScanReset = false;    ///< onDisconnect (host task) → tick() blanks SCAN

  // Host-task → tick() handoff (BLE callbacks copy under m_lock, set the flag).
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

  // Local telemetry stream (sketch-core only).
  bool m_telemetryOn = true;
  uint32_t m_telemetryIntervalMs = 250;  // 4 Hz default
  uint32_t m_lastTelemetryMs = 0;
#if defined(ESP32)
  portMUX_TYPE m_lock = portMUX_INITIALIZER_UNLOCKED;
#endif

  friend class AutoTLMBleImpl;
};

#endif  // AUTOTLM_BLE_H
