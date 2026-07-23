/*
 * AutoTLMBle.cpp — BLE change-WiFi service implementation (NimBLE host).
 * Part of AutoTLM Core — MIT licensed.
 *
 * THREADING MODEL (learned the hard way — an earlier cut raced on all of
 * this): the NimBLE host task and the sketch core both touch this module.
 * The rule here is strict — **tick() on the sketch core is the ONLY writer
 * of NimBLE state** (advertising start/stop, characteristic setValue/notify,
 * disconnect). The host-task callbacks (onConnect/onDisconnect/onWrite) do
 * nothing but copy bytes and raise flags under the shared portMUX. Single
 * central is enforced structurally: we never advertise while a central is
 * connected, so a second one can't attach and inherit the session.
 */
#include "AutoTLMBle.h"

// BUILD-WIDE gate (never a sketch #define — see the header's ODR note):
// default ON for the AutoTLM One board, OFF elsewhere. Override with a
// `-DAUTOTLM_ENABLE_BLE=…` compiler build property.
#ifndef AUTOTLM_ENABLE_BLE
#  if defined(ARDUINO_AUTOTLM_ONE)
#    define AUTOTLM_ENABLE_BLE 1
#  else
#    define AUTOTLM_ENABLE_BLE 0
#  endif
#endif

#if defined(ESP32) && AUTOTLM_ENABLE_BLE

#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <new>

#include "../AutoTLM.h"

// Minimum gap between honored scans — throttles an authed-but-hostile peer.
#define BLE_SCAN_MIN_INTERVAL_MS 3000
// The device id is truncated to this for the scan-response service data.
#define BLE_ADV_ID_MAX 10
// A central that connects but never presents the setup code is dropped.
#define BLE_UNAUTHED_GRACE_MS 45000
// Bad setup-code attempts on one connection before we drop it (a reconnect
// costs seconds over BLE, so this plus single-central makes brute-forcing the
// code impractical; the deeper fix — a setup code NOT derived from the MAC —
// is flagged to the owner).
#define BLE_MAX_AUTH_FAILS 5
// NimBLE's bring-up is light, but a fragmented heap still gets a polite refusal.
#define BLE_MIN_FREE_HEAP 30720
#define BLE_MIN_BLOCK 12288

// Pull one flat string field ("key":"value") out of a small JSON payload.
static bool jsonField(const char* json, const char* key, char* out, size_t cap) {
  if (!json || !key || !out || !cap) return false;
  out[0] = 0;
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = strstr(json, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (*p != '"') return false;
  p++;
  size_t n = 0;
  while (*p && *p != '"' && n + 1 < cap) {
    if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) p++;
    out[n++] = *p++;
  }
  out[n] = 0;
  return *p == '"';
}

// ---------------------------------------------------------------- the guts
class AutoTLMBleImpl {
 public:
  NimBLEServer* server = nullptr;
  NimBLECharacteristic* ctrl = nullptr;
  NimBLECharacteristic* scan = nullptr;
  NimBLECharacteristic* creds = nullptr;
  NimBLECharacteristic* status = nullptr;
  NimBLECharacteristic* telem = nullptr;

  class ServerCb : public NimBLEServerCallbacks {
   public:
    explicit ServerCb(AutoTLMBle* o) : owner(o) {}
    void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
      owner->onConnect(info.getConnHandle());
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int) override {
      owner->onDisconnect(info.getConnHandle());
    }
    AutoTLMBle* owner;
  };

  class CtrlCb : public NimBLECharacteristicCallbacks {
   public:
    explicit CtrlCb(AutoTLMBle* o) : owner(o) {}
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
      NimBLEAttValue v = c->getValue();
      if (strstr(v.c_str(), "\"auth\"")) {
        char code[16];
        jsonField(v.c_str(), "code", code, sizeof(code));
        taskENTER_CRITICAL(&owner->m_lock);
        memcpy(owner->m_pendAuthCode, code, sizeof(code));
        owner->m_authPending = true;
        taskEXIT_CRITICAL(&owner->m_lock);
      } else if (strstr(v.c_str(), "\"scan\"")) {
        owner->m_scanRequested = true;  // honored in tick() only if authed
      }
    }
    AutoTLMBle* owner;
  };

  class CredsCb : public NimBLECharacteristicCallbacks {
   public:
    explicit CredsCb(AutoTLMBle* o) : owner(o) {}
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
      // Last-writer-wins: overwrite the pend buffers under the lock even if a
      // previous write is still pending (tick() consumes atomically).
      NimBLEAttValue v = c->getValue();
      char code[16], ssid[33], pass[65];
      jsonField(v.c_str(), "code", code, sizeof(code));
      jsonField(v.c_str(), "ssid", ssid, sizeof(ssid));
      jsonField(v.c_str(), "pass", pass, sizeof(pass));
      taskENTER_CRITICAL(&owner->m_lock);
      memcpy(owner->m_pendCode, code, sizeof(code));
      memcpy(owner->m_pendSsid, ssid, sizeof(ssid));
      memcpy(owner->m_pendPass, pass, sizeof(pass));
      owner->m_credsPending = true;
      taskEXIT_CRITICAL(&owner->m_lock);
      memset(pass, 0, sizeof(pass));
    }
    AutoTLMBle* owner;
  };

  ServerCb* serverCb = nullptr;
  CtrlCb* ctrlCb = nullptr;
  CredsCb* credsCb = nullptr;
};

bool AutoTLMBle::begin(AutoTLM& car, const char* deviceId) {
  if (m_impl) return true;
  if (m_beginFailed) return false;  // latch — don't retry a failed bring-up every loop()
  m_car = &car;

  // Refuse rather than abort on a starved heap. Canonical order: call
  // car.bleBegin() right after car.begin(), BEFORE provision().
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (freeHeap < BLE_MIN_FREE_HEAP || largest < BLE_MIN_BLOCK) {
    m_beginFailed = true;
    if (m_log)
      m_log->printf(
          "BLE:not enough heap to start (%u free, %u largest block; need %u/%u). "
          "Call car.bleBegin() right after car.begin(), before provision().\n",
          (unsigned)freeHeap, (unsigned)largest, (unsigned)BLE_MIN_FREE_HEAP,
          (unsigned)BLE_MIN_BLOCK);
    return false;
  }

  const char* id = deviceId ? deviceId : "";
  char name[24];
  snprintf(name, sizeof(name), "AutoTLM-%s", (strlen(id) >= 4) ? id + strlen(id) - 4 : "0000");

  if (!NimBLEDevice::init(name)) {
    m_beginFailed = true;
    if (m_log) m_log->println("BLE:NimBLE init failed — service not started");
    return false;
  }
  // LE Secure Connections + bonding, Just Works. Pairs on demand at first
  // encrypted access (the standard iOS flow) — no force-security-at-connect.
  NimBLEDevice::setSecurityAuth(true /*bonding*/, false /*mitm*/, true /*sc*/);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  // Distribute encryption + IDENTITY (IRK) keys both ways: the IRK lets a
  // bonded phone resolve our rotating private address (below). ENC|ID = 0x03.
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  // RESOLVABLE PRIVATE ADDRESS: advertise with a rotating address only a
  // bonded phone (holding our IRK) can resolve — the unit stays discoverable
  // to its OWNER continuously (no more going dark / reboot-to-re-pair) without
  // being a followable beacon to strangers. IRK lives in NimBLE's bond store
  // (NVS) and is stable across reboots; only a factory wipe / clearBonds()
  // regenerates it (which re-pairs anyway).
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RPA_RANDOM_DEFAULT);
  // A larger ATT MTU lets the compact telemetry frame ride in ONE notify and
  // the auth/creds JSON in single writes (iOS negotiates down as needed).
  NimBLEDevice::setMTU(512);

  m_impl = new (std::nothrow) AutoTLMBleImpl();
  m_impl && (m_impl->serverCb = new (std::nothrow) AutoTLMBleImpl::ServerCb(this));
  m_impl && (m_impl->ctrlCb = new (std::nothrow) AutoTLMBleImpl::CtrlCb(this));
  m_impl && (m_impl->credsCb = new (std::nothrow) AutoTLMBleImpl::CredsCb(this));
  if (!m_impl || !m_impl->serverCb || !m_impl->ctrlCb || !m_impl->credsCb) {
    if (m_log) m_log->println("BLE:alloc failed — service not started");
    if (m_impl) {
      delete m_impl->serverCb; delete m_impl->ctrlCb; delete m_impl->credsCb;
      delete m_impl; m_impl = nullptr;
    }
    NimBLEDevice::deinit(true);
    m_beginFailed = true;
    return false;
  }

  m_impl->server = NimBLEDevice::createServer();
  NimBLEService* svc = m_impl->server ? m_impl->server->createService(AUTOTLM_BLE_SERVICE_UUID) : nullptr;
  if (!svc) {
    if (m_log) m_log->println("BLE:GATT service alloc failed — service not started");
    delete m_impl->serverCb; delete m_impl->ctrlCb; delete m_impl->credsCb;
    delete m_impl; m_impl = nullptr;
    NimBLEDevice::deinit(true);
    m_beginFailed = true;
    return false;
  }
  m_impl->server->setCallbacks(m_impl->serverCb);
  m_impl->server->advertiseOnDisconnect(false);  // OUR policy (reconcileAdvertising), not the stack's

  // Value reads/writes are _ENC-gated. The subscribe/notify path is guarded
  // separately: tick() only notify()s when the tracked central's link is
  // actually encrypted (peerEncrypted()) — NimBLE's auto-CCCD is NOT
  // encryption-gated, so gating the notify is what keeps STATUS/SCAN
  // transitions off an unencrypted link (a defense the Bluedroid version had).
  m_impl->ctrl = svc->createCharacteristic(
      AUTOTLM_BLE_CTRL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  m_impl->ctrl->setCallbacks(m_impl->ctrlCb);

  m_impl->scan = svc->createCharacteristic(
      AUTOTLM_BLE_SCAN_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY);
  m_impl->scan->setValue("[]");

  m_impl->creds = svc->createCharacteristic(
      AUTOTLM_BLE_CREDS_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  m_impl->creds->setCallbacks(m_impl->credsCb);

  m_impl->status = svc->createCharacteristic(
      AUTOTLM_BLE_STATUS_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY);
  uint8_t init[2] = {AUTOTLM_BLE_IDLE, 0};
  m_impl->status->setValue(init, 2);

  // Local live-telemetry stream. NOTIFY-ONLY (no READ): the frame carries GPS,
  // and READ_ENC would only require an encrypted link — which a Just-Works-
  // bonded but NOT setup-code-authed phone has — letting it READ the last
  // stored fix. Push-only closes that: NimBLE delivers notifies solely to
  // subscribers, and tick() only notifies when the session is authed. It also
  // removes the concurrent-read-vs-setValue path entirely.
  m_impl->telem = svc->createCharacteristic(
      AUTOTLM_BLE_TELEM_UUID, NIMBLE_PROPERTY::NOTIFY);

  svc->start();

  // Advertisement: the Complete 128-bit Service-UUID list in the primary AD
  // (iOS `scanForPeripherals(withServices:)` matches only UUID-list ADs), and
  // the exact device id in the scan-response service data (the app's
  // exact-match key). No name in the scan response — flags(3)+128-bit UUID or
  // service-data(2+16+id) already crowds the 31-byte budget, and setName would
  // fail silently; the readable GAP name (NimBLEDevice::init) covers humans.
  char advId[BLE_ADV_ID_MAX + 1];
  strncpy(advId, id, BLE_ADV_ID_MAX);
  advId[BLE_ADV_ID_MAX] = 0;
  if (strlen(id) > BLE_ADV_ID_MAX && m_log)
    m_log->printf("BLE:device id > %d chars — advertising a truncated id\n", BLE_ADV_ID_MAX);

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setCompleteServices(NimBLEUUID(AUTOTLM_BLE_SERVICE_UUID));
  NimBLEAdvertisementData resp;
  resp.setServiceData(NimBLEUUID(AUTOTLM_BLE_SERVICE_UUID), std::string(advId));
  bool advOk = adv->setAdvertisementData(advData);
  advOk = adv->setScanResponseData(resp) && advOk;
  adv->enableScanResponse(true);  // 2.5.0: without this the scan-response (the id) is never sent
  if (!advOk && m_log) m_log->println("BLE:advertisement data rejected (payload too large?)");

  if (m_log)
    m_log->printf("BLE:service up (NimBLE) as \"%s\" (id %s) — advertising OFF until policy says so\n",
                  name, id);
  return true;
}

// advertise() is a pure setter of the DESIRED state. tick() drives the radio
// to match — so the radio has exactly one writer (the sketch core) and
// advertise(false) can never race a host-task restart into an ON state.
void AutoTLMBle::advertise(bool on) {
  m_advertising = on;
}

void AutoTLMBle::telemetry(bool on, uint8_t hz) {
  m_telemetryOn = on;
  if (hz < 1) hz = 1;
  if (hz > 10) hz = 10;
  m_telemetryIntervalMs = 1000u / hz;
}

void AutoTLMBle::reconcileAdvertising() {
  if (!m_impl) return;
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  // Never advertise while a central is connected: that both honors the
  // dark-while-connected policy AND structurally enforces single-central (no
  // second peer can attach to inherit the session).
  const bool want = m_advertising && m_connHandle == 0xFFFF;
  const bool is = adv->isAdvertising();
  if (want && !is) {
    if (!adv->start() && m_log) m_log->println("BLE:advertising start FAILED");
  } else if (!want && is) {
    adv->stop();
  }
}

bool AutoTLMBle::peerEncrypted() const {
  if (!m_impl || m_connHandle == 0xFFFF) return false;
  return m_impl->server->getPeerInfoByHandle(m_connHandle).isEncrypted();
}

bool AutoTLMBle::clearBonds() {
  if (!m_impl) return false;
  const bool ok = NimBLEDevice::deleteAllBonds();
  if (m_log) m_log->printf("BLE:bonds %s\n", ok ? "cleared" : "clear FAILED");
  return ok;
}

// Host task: FLAGS ONLY (honors the single-writer rule at the top of this file
// — the actual NimBLE work happens in tick() on the sketch core).
void AutoTLMBle::onConnect(uint16_t connHandle) {
  // ADOPT the connecting central. In the normal case m_connHandle is already
  // 0xFFFF here: we never advertise while connected, so the prior link's
  // onDisconnect must have run (clearing the handle and re-enabling advertising)
  // before this central could attach. The adopt-over-stale branch only bites in
  // the rare window where a second central attaches before we saw the first
  // drop; there we take the new handle, start a fresh (unauthed) session, and
  // hand the old handle to tick() for teardown. NOTE: a genuinely *missed*
  // disconnect can't be fixed here (the device is dark, so no onConnect fires) —
  // tick()'s liveness watchdog recovers that case.
  taskENTER_CRITICAL(&m_lock);
  if (m_connHandle != 0xFFFF && m_connHandle != connHandle) m_staleToDrop = m_connHandle;
  m_connHandle = connHandle;
  m_connectMs = millis();
  m_sessionAuthed = false;
  m_authedHandle = 0xFFFF;
  m_authFails = 0;
  // Defense in depth: never let a superseded session's queued auth/creds be
  // consumed for the freshly-adopted central. A write on the new link can only
  // arrive AFTER this callback, so nothing legitimate is discarded.
  m_authPending = false;
  m_credsPending = false;
  memset(m_pendAuthCode, 0, sizeof(m_pendAuthCode));
  memset(m_pendCode, 0, sizeof(m_pendCode));
  memset(m_pendSsid, 0, sizeof(m_pendSsid));
  memset(m_pendPass, 0, sizeof(m_pendPass));
  taskEXIT_CRITICAL(&m_lock);
}

void AutoTLMBle::onDisconnect(uint16_t connHandle) {
  // Only clear our tracking if it's the CURRENT central dropping. A superseded/
  // stale handle disconnecting late (see onConnect) must NOT clobber the fresh
  // session we just adopted for the reconnected phone.
  taskENTER_CRITICAL(&m_lock);
  if (connHandle != m_connHandle && m_connHandle != 0xFFFF) {
    taskEXIT_CRITICAL(&m_lock);
    return;
  }
  m_connHandle = 0xFFFF;
  m_sessionAuthed = false;
  m_authedHandle = 0xFFFF;
  m_authPending = false;
  m_credsPending = false;
  m_needScanReset = true;  // tick() blanks SCAN so the next peer can't read a stale list
  memset(m_pendCode, 0, sizeof(m_pendCode));
  memset(m_pendSsid, 0, sizeof(m_pendSsid));
  memset(m_pendPass, 0, sizeof(m_pendPass));
  memset(m_pendAuthCode, 0, sizeof(m_pendAuthCode));
  taskEXIT_CRITICAL(&m_lock);
}

// Sketch core only. setValue always (for a later encrypted read); notify only
// on an encrypted link, so STATUS transitions never leak in cleartext.
void AutoTLMBle::setStatus(uint8_t state, uint8_t detail) {
  m_state = state;
  m_detail = detail;
  if (!m_impl) return;
  uint8_t v[2] = {state, detail};
  m_impl->status->setValue(v, 2);
  if (peerEncrypted()) m_impl->status->notify();
}

void AutoTLMBle::feedWifiChange(int wifiState, int reason) {
  if (!m_impl) return;
  switch (wifiState) {
    case AUTOTLM_WIFI_VALIDATING:
      setStatus(AUTOTLM_BLE_TESTING, 0);
      break;
    case AUTOTLM_WIFI_OK:
      setStatus(AUTOTLM_BLE_CONNECTED, 0);
      break;
    case AUTOTLM_WIFI_REVERTED:
      setStatus(AUTOTLM_BLE_REVERTED, (uint8_t)reason);
      break;
    default:
      break;  // IDLE holds the terminal state for a (re)connecting reader
  }
}

void AutoTLMBle::runScanStep() {
  if (m_scanRequested && !m_scanRunning) {
    m_scanRequested = false;
    // Possession proof, bound to THIS connection.
    if (!m_sessionAuthed || m_authedHandle != m_connHandle) return;
    if (m_car && m_car->wifiChangeState() == AUTOTLM_WIFI_VALIDATING) {
      m_scanRequested = true;  // defer, don't drop
      return;
    }
    const uint32_t now = millis();
    if (m_lastScanMs && now - m_lastScanMs < BLE_SCAN_MIN_INTERVAL_MS) {
      m_scanRequested = true;  // defer, don't drop — runs when the throttle clears
      return;
    }
    m_lastScanMs = now;
    if (m_car) m_car->net().setRadioBusy(true);
    WiFi.scanNetworks(true);
    m_scanRunning = true;
    m_scanRetried = false;
    if (m_log) m_log->println("BLE:scan started");
    return;
  }
  if (!m_scanRunning) return;
  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  if (n < 0) {
    if (!m_scanRetried) {
      m_scanRetried = true;
      WiFi.scanDelete();
      WiFi.scanNetworks(true);
      return;
    }
    m_scanRunning = false;
    if (m_car) m_car->net().setRadioBusy(false);
    m_impl->scan->setValue("[]");
    if (peerEncrypted()) m_impl->scan->notify();
    return;
  }
  m_scanRunning = false;
  if (m_car) m_car->net().setRadioBusy(false);
  // The requesting session may have ended while the scan ran — don't publish
  // a network list a now-unauthed (or replaced) peer could read.
  if (!m_sessionAuthed || m_authedHandle != m_connHandle) {
    WiFi.scanDelete();
    m_impl->scan->setValue("[]");
    return;
  }

  int idx[10];
  int count = 0;
  for (int i = 0; i < n; i++) {
    if (!WiFi.SSID(i).length()) continue;
    bool dup = false;
    for (int k = 0; k < count; k++) {
      if (WiFi.SSID(idx[k]) == WiFi.SSID(i)) {
        if (WiFi.RSSI(i) > WiFi.RSSI(idx[k])) idx[k] = i;
        dup = true;
        break;
      }
    }
    if (dup) continue;
    if (count < 10) {
      idx[count++] = i;
    } else {
      int weakest = 0;
      for (int k = 1; k < count; k++)
        if (WiFi.RSSI(idx[k]) < WiFi.RSSI(idx[weakest])) weakest = k;
      if (WiFi.RSSI(i) > WiFi.RSSI(idx[weakest])) idx[weakest] = i;
    }
  }
  for (int a = 1; a < count; a++) {
    const int v = idx[a];
    int b = a - 1;
    while (b >= 0 && WiFi.RSSI(idx[b]) < WiFi.RSSI(v)) { idx[b + 1] = idx[b]; b--; }
    idx[b + 1] = v;
  }

  char buf[512];
  size_t len = 0;
  len += snprintf(buf, sizeof(buf), "{\"seq\":%u,\"nets\":[", (unsigned)(++m_scanSeq));
  for (int k = 0; k < count; k++) {
    char item[128];
    char ssid[70];
    size_t sn = 0;  // escape " and \ in SSIDs
    const char* s = WiFi.SSID(idx[k]).c_str();
    for (; *s && sn + 2 < sizeof(ssid); s++) {
      if (*s == '"' || *s == '\\') ssid[sn++] = '\\';
      ssid[sn++] = *s;
    }
    ssid[sn] = 0;
    const int m =
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"sec\":%s}",
                 k ? "," : "", ssid, (int)WiFi.RSSI(idx[k]),
                 WiFi.encryptionType(idx[k]) == WIFI_AUTH_OPEN ? "false" : "true");
    // snprintf returns the WOULD-BE length: skip any item that truncated
    // (a hostile 32-char all-escape SSID), never memcpy past `item`.
    if (m < 0 || m >= (int)sizeof(item)) continue;
    if (len + (size_t)m + 3 >= sizeof(buf)) break;
    memcpy(buf + len, item, m);
    len += m;
  }
  if (len + 3 < sizeof(buf)) len += snprintf(buf + len, sizeof(buf) - len, "]}");
  WiFi.scanDelete();

  m_impl->scan->setValue((uint8_t*)buf, len);
  if (peerEncrypted()) m_impl->scan->notify();  // freshness signal; the app reads for the full list
  if (m_log) m_log->printf("BLE:scan done (%d networks, seq %u)\n", count, (unsigned)m_scanSeq);
}

void AutoTLMBle::tick() {
  if (!m_impl || !m_car) return;

  // Tear down a superseded central that onConnect adopted over. Done here on the
  // sketch core — the single writer of NimBLE state (file header). Rare: needs
  // two links to have briefly coexisted.
  uint16_t toDrop;
  taskENTER_CRITICAL(&m_lock);
  toDrop = m_staleToDrop;
  m_staleToDrop = 0xFFFF;
  taskEXIT_CRITICAL(&m_lock);
  if (toDrop != 0xFFFF) {
    m_impl->server->disconnect(toDrop);
    if (m_log) m_log->printf("BLE:dropped superseded handle %u\n", (unsigned)toDrop);
  }

  // LIVENESS WATCHDOG — the auth-deaf reconnect-wedge fix. If we still track a
  // central but the controller has no such live connection, that link's
  // disconnect event was lost. Nothing else releases an AUTHED-but-dead handle
  // (the unauthed-grace drop below is !m_sessionAuthed-gated), so the unit would
  // stay dark — not advertising, not re-linkable — until a reboot. Release it
  // here so reconcileAdvertising() resumes advertising and the phone can
  // re-link with no reboot. ble_gap_conn_find() is authoritative (0 == the
  // handle is a live connection); the re-check under the lock makes this safe
  // against a concurrent adopt on the host task.
  if (m_connHandle != 0xFFFF) {
    const uint16_t h = m_connHandle;
    ble_gap_conn_desc d;
    if (ble_gap_conn_find(h, &d) != 0) {
      bool released = false;
      taskENTER_CRITICAL(&m_lock);
      if (m_connHandle == h) {
        m_connHandle = 0xFFFF;
        m_sessionAuthed = false;
        m_authedHandle = 0xFFFF;
        m_authPending = false;
        m_credsPending = false;
        m_needScanReset = true;
        memset(m_pendCode, 0, sizeof(m_pendCode));
        memset(m_pendSsid, 0, sizeof(m_pendSsid));
        memset(m_pendPass, 0, sizeof(m_pendPass));
        memset(m_pendAuthCode, 0, sizeof(m_pendAuthCode));
        released = true;
      }
      taskEXIT_CRITICAL(&m_lock);
      if (released && m_log)
        m_log->printf("BLE:released dead handle %u (missed disconnect) — re-advertising\n", (unsigned)h);
    }
  }

  // Diagnostics: log connection-handle transitions (connect / disconnect /
  // adopt) so a field reconnect issue leaves a serial trace to read back.
  // Snapshot the volatile once so the logged source/target stay self-consistent
  // if the host task mutates m_connHandle mid-block.
  const uint16_t curHandle = m_connHandle;
  if (curHandle != m_lastLoggedHandle) {
    if (m_log) m_log->printf("BLE:conn handle %u -> %u\n",
                             (unsigned)m_lastLoggedHandle, (unsigned)curHandle);
    m_lastLoggedHandle = curHandle;
  }

  reconcileAdvertising();

  if (m_needScanReset) {
    m_needScanReset = false;
    m_impl->scan->setValue("[]");
  }

  // Drop a central that connected but never presented the setup code.
  if (m_connHandle != 0xFFFF && !m_sessionAuthed &&
      millis() - m_connectMs > BLE_UNAUTHED_GRACE_MS) {
    if (m_log) m_log->println("BLE:dropping unauthed central (grace window expired)");
    m_impl->server->disconnect(m_connHandle);  // onDisconnect() clears the tracking
  }

  // Possession proof — verified HERE (sketch core), bound to the connection.
  if (m_authPending) {
    char code[16];
    const uint16_t forHandle = m_connHandle;
    taskENTER_CRITICAL(&m_lock);
    memcpy(code, m_pendAuthCode, sizeof(code));
    m_authPending = false;
    taskEXIT_CRITICAL(&m_lock);
    char want[16];
    m_car->config().apPassword(want, sizeof(want));
    if (forHandle == 0xFFFF) {
      // central vanished mid-auth — nothing to do
    } else if (m_authFails >= BLE_MAX_AUTH_FAILS) {
      if (m_log) m_log->println("BLE:auth attempts exhausted — dropping central");
      m_impl->server->disconnect(forHandle);
    } else if (want[0] && strcasecmp(code, want) == 0) {
      m_sessionAuthed = true;
      m_authedHandle = forHandle;
      // Positive ack — but NEVER clobber a held terminal verdict a
      // reconnecting app may not have read yet (it reads STATUS, then auths).
      if (m_state != AUTOTLM_BLE_CONNECTED && m_state != AUTOTLM_BLE_REVERTED)
        setStatus(AUTOTLM_BLE_AUTH_OK, 0);
      if (m_log) m_log->println("BLE:session unlocked (setup code accepted)");
    } else {
      m_authFails++;
      m_sessionAuthed = false;
      setStatus(AUTOTLM_BLE_REVERTED, AUTOTLM_BLE_DETAIL_REJECTED);
      if (m_log) m_log->printf("BLE:auth rejected (bad setup code, %u/%u)\n",
                               m_authFails, BLE_MAX_AUTH_FAILS);
    }
  }

  // Hold creds while a scan owns the radio — starting validation now would
  // fight the scan for the WiFi driver.
  if (m_credsPending && !m_scanRunning) {
    char code[16], ssid[33], pass[65];
    const uint16_t forHandle = m_connHandle;
    taskENTER_CRITICAL(&m_lock);
    memcpy(code, m_pendCode, sizeof(code));
    memcpy(ssid, m_pendSsid, sizeof(ssid));
    memcpy(pass, m_pendPass, sizeof(pass));
    memset(m_pendCode, 0, sizeof(m_pendCode));
    memset(m_pendSsid, 0, sizeof(m_pendSsid));
    memset(m_pendPass, 0, sizeof(m_pendPass));
    m_credsPending = false;
    taskEXIT_CRITICAL(&m_lock);

    char want[16];
    m_car->config().apPassword(want, sizeof(want));
    const bool codeOk = m_sessionAuthed && m_authedHandle == forHandle && forHandle != 0xFFFF &&
                        want[0] && strcasecmp(code, want) == 0;
    if (!codeOk) {
      setStatus(AUTOTLM_BLE_REVERTED, AUTOTLM_BLE_DETAIL_REJECTED);
      if (m_log) m_log->println("BLE:creds rejected (not authed / bad setup code)");
    } else if (!ssid[0]) {
      setStatus(AUTOTLM_BLE_REVERTED, AUTOTLM_BLE_DETAIL_NO_SSID);
      if (m_log) m_log->println("BLE:creds rejected (no ssid)");
    } else if (m_car->wifiChangeState() == AUTOTLM_WIFI_VALIDATING) {
      setStatus(AUTOTLM_BLE_BUSY, 0);
      if (m_log) m_log->println("BLE:creds deferred (a change is already validating)");
    } else {
      setStatus(AUTOTLM_BLE_CREDS_RECEIVED, 0);
      m_car->changeWifi(ssid, pass);
      if (m_log) m_log->printf("BLE:creds accepted — validating \"%s\"\n", ssid);
    }
    memset(pass, 0, sizeof(pass));
  }

  runScanStep();

  // Local live-telemetry: push a compact frame to an AUTHED, connected phone
  // at the configured rate. Gated on the session auth because the frame
  // carries GPS; skipped entirely when nobody's connected/authed (no wasted
  // radio). NimBLE only delivers the notify to a subscribed central.
  if (m_telemetryOn && m_sessionAuthed && m_authedHandle == m_connHandle &&
      m_connHandle != 0xFFFF && peerEncrypted()) {
    const uint32_t now = millis();
    if (now - m_lastTelemetryMs >= m_telemetryIntervalMs) {
      m_lastTelemetryMs = now;
      // Size the frame to the NEGOTIATED ATT MTU, not our 512 preference: iOS
      // commonly settles at 185, and a notify is capped at MTU-3 — building
      // larger would silently truncate to invalid JSON on the wire. toJsonLive
      // trims to whatever `cap` we pass and is always valid within it.
      const uint16_t mtu = m_impl->server->getPeerInfoByHandle(m_connHandle).getMTU();
      char buf[512];
      size_t cap = (mtu > 23) ? (size_t)(mtu - 3) : 20;
      if (cap > sizeof(buf)) cap = sizeof(buf);
      AutoTLMFrame f = m_car->frame();
      const size_t n = f.toJsonLive(buf, cap);
      if (n) {
        m_impl->telem->setValue((uint8_t*)buf, n);
        m_impl->telem->notify();
      }
    }
  }
}

#else  // BLE compiled out (generic board without -DAUTOTLM_ENABLE_BLE=1) or no ESP32.

bool AutoTLMBle::begin(AutoTLM&, const char*) {
  if (m_log)
    m_log->println(
        "BLE:not compiled in (build with -DAUTOTLM_ENABLE_BLE=1, or select the AutoTLM One board)");
  return false;
}
void AutoTLMBle::advertise(bool) {}
void AutoTLMBle::telemetry(bool, uint8_t) {}
void AutoTLMBle::reconcileAdvertising() {}
bool AutoTLMBle::peerEncrypted() const { return false; }
bool AutoTLMBle::clearBonds() { return false; }
void AutoTLMBle::onConnect(uint16_t) {}
void AutoTLMBle::onDisconnect(uint16_t) {}
void AutoTLMBle::feedWifiChange(int, int) {}
void AutoTLMBle::setStatus(uint8_t, uint8_t) {}
void AutoTLMBle::runScanStep() {}
void AutoTLMBle::tick() {}

#endif  // ESP32 && AUTOTLM_ENABLE_BLE
