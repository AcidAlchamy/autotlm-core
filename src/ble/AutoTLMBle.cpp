/*
 * AutoTLMBle.cpp — BLE change-WiFi service implementation (Bluedroid).
 * Part of AutoTLM Core — MIT licensed.
 */
#include "AutoTLMBle.h"

#if defined(ESP32) && defined(CONFIG_BT_ENABLED)

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <WiFi.h>

#include "../AutoTLM.h"

// Minimum gap between honored scans — throttles an authed-but-hostile peer
// from spamming all-channel scans (each disrupts the associated link).
#define BLE_SCAN_MIN_INTERVAL_MS 3000
// The device id is truncated to this for the advertisement's service data so
// the whole AD element can never overflow the 31-byte budget and vanish.
#define BLE_ADV_ID_MAX 10

// Pull one flat string field ("key":"value") out of a small JSON payload.
// Handles \" and \\ escapes; enough for the fixed payload shapes this service
// accepts — deliberately not a general JSON parser (no third-party libs).
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
  BLEServer* server = nullptr;
  BLECharacteristic* ctrl = nullptr;
  BLECharacteristic* scan = nullptr;
  BLECharacteristic* creds = nullptr;
  BLECharacteristic* status = nullptr;

  // Restart advertising when a central drops (esp32 3.3.8 does NOT auto-resume
  // legacy advertising on disconnect) + clear the per-connection auth gate.
  class ServerCb : public BLEServerCallbacks {
   public:
    explicit ServerCb(AutoTLMBle* o) : owner(o) {}
    void onConnect(BLEServer*) override {}
    void onDisconnect(BLEServer*) override { owner->onDisconnect(); }
    AutoTLMBle* owner;
  };

  class CtrlCb : public BLECharacteristicCallbacks {
   public:
    explicit CtrlCb(AutoTLMBle* o) : owner(o) {}
    void onWrite(BLECharacteristic* c) override {
      // BT-task context: copy only (under the lock); tick() authenticates/acts.
      String v = c->getValue();
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

  class CredsCb : public BLECharacteristicCallbacks {
   public:
    explicit CredsCb(AutoTLMBle* o) : owner(o) {}
    void onWrite(BLECharacteristic* c) override {
      // Last-writer-wins: overwrite the pend buffers under the lock even if a
      // previous write is still pending, so a re-submit is never silently lost
      // (tick() consumes atomically under the same lock).
      String v = c->getValue();
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
    }
    AutoTLMBle* owner;
  };

  ServerCb* serverCb = nullptr;
  CtrlCb* ctrlCb = nullptr;
  CredsCb* credsCb = nullptr;
};

bool AutoTLMBle::begin(AutoTLM& car, const char* deviceId) {
  if (m_impl) return true;
  m_car = &car;

  const char* id = deviceId ? deviceId : "";
  char name[24];
  snprintf(name, sizeof(name), "AutoTLM-%s", (strlen(id) >= 4) ? id + strlen(id) - 4 : "0000");

  BLEDevice::init(name);  // also the GAP device name (readable after connect)
  // LE Secure Connections + bonding, Just Works (no display/keypad). The
  // 3-arg overload is the one that actually arms library security and sets
  // the correct encryption level; the uint8_t overload does NOT.
  BLESecurity::setAuthenticationMode(true /*bonding*/, false /*mitm*/, true /*sc*/);
  BLESecurity::setCapability(ESP_IO_CAP_NONE);
  BLESecurity::setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  m_impl = new AutoTLMBleImpl();
  m_impl->server = BLEDevice::createServer();
  m_impl->serverCb = new AutoTLMBleImpl::ServerCb(this);
  m_impl->server->setCallbacks(m_impl->serverCb);
  BLEService* svc = m_impl->server->createService(AUTOTLM_BLE_SERVICE_UUID);

  const uint32_t encRW = ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED;

  m_impl->ctrl = svc->createCharacteristic(AUTOTLM_BLE_CTRL_UUID,
                                           BLECharacteristic::PROPERTY_WRITE);
  m_impl->ctrl->setAccessPermissions(encRW);
  m_impl->ctrlCb = new AutoTLMBleImpl::CtrlCb(this);
  m_impl->ctrl->setCallbacks(m_impl->ctrlCb);

  m_impl->scan = svc->createCharacteristic(
      AUTOTLM_BLE_SCAN_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  m_impl->scan->setAccessPermissions(encRW);
  BLE2902* scanCccd = new BLE2902();  // CCCD needs its own encrypted perms, else
  scanCccd->setAccessPermissions(encRW);  // an unbonded central could subscribe
  m_impl->scan->addDescriptor(scanCccd);
  m_impl->scan->setValue("[]");

  m_impl->creds = svc->createCharacteristic(AUTOTLM_BLE_CREDS_UUID,
                                            BLECharacteristic::PROPERTY_WRITE);
  m_impl->creds->setAccessPermissions(encRW);
  m_impl->credsCb = new AutoTLMBleImpl::CredsCb(this);
  m_impl->creds->setCallbacks(m_impl->credsCb);

  m_impl->status = svc->createCharacteristic(
      AUTOTLM_BLE_STATUS_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  m_impl->status->setAccessPermissions(encRW);
  BLE2902* statusCccd = new BLE2902();
  statusCccd->setAccessPermissions(encRW);
  m_impl->status->addDescriptor(statusCccd);
  uint8_t init[2] = {AUTOTLM_BLE_IDLE, 0};
  m_impl->status->setValue(init, 2);

  svc->start();

  // Advertisement: the Complete 128-bit Service-UUID list in the primary AD
  // (so iOS `scanForPeripherals(withServices:)` matches — service DATA alone
  // does not satisfy that filter), and the exact device id in the scan
  // response service data (the app's exact-match key). Splitting them keeps
  // each packet within the 31-byte budget. The id is capped so the element
  // can never overflow and be silently dropped.
  char advId[BLE_ADV_ID_MAX + 1];
  strncpy(advId, id, BLE_ADV_ID_MAX);
  advId[BLE_ADV_ID_MAX] = 0;
  if (strlen(id) > BLE_ADV_ID_MAX && m_log)
    m_log->printf("BLE:device id > %d chars — advertising a truncated id\n", BLE_ADV_ID_MAX);

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  BLEAdvertisementData advData;
  advData.setFlags(0x06);  // general discoverable, no BR/EDR
  advData.setCompleteServices(BLEUUID(AUTOTLM_BLE_SERVICE_UUID));
  adv->setAdvertisementData(advData);
  BLEAdvertisementData resp;
  resp.setServiceData(BLEUUID(AUTOTLM_BLE_SERVICE_UUID), String(advId));
  adv->setScanResponseData(resp);

  if (m_log)
    m_log->printf("BLE:service up as \"%s\" (id %s) — advertising OFF until policy says so\n",
                  name, id);
  return true;
}

void AutoTLMBle::advertise(bool on) {
  if (!m_impl) return;
  if (on == m_advertising) return;
  if (on) BLEDevice::getAdvertising()->start();
  else BLEDevice::getAdvertising()->stop();
  m_advertising = on;
  if (m_log) m_log->printf("BLE:advertising %s\n", on ? "ON" : "OFF");
}

void AutoTLMBle::onDisconnect() {
  // Auth is per-connection; a new central must re-prove possession.
  m_sessionAuthed = false;
  taskENTER_CRITICAL(&m_lock);
  m_authPending = false;
  m_credsPending = false;
  memset(m_pendCode, 0, sizeof(m_pendCode));
  memset(m_pendSsid, 0, sizeof(m_pendSsid));
  memset(m_pendPass, 0, sizeof(m_pendPass));
  memset(m_pendAuthCode, 0, sizeof(m_pendAuthCode));
  taskEXIT_CRITICAL(&m_lock);
  if (m_impl) m_impl->scan->setValue("[]");  // don't leave a scan list readable to the next peer
  // esp32 3.3.8 does not auto-resume legacy advertising after a disconnect;
  // re-arm it if policy still wants us discoverable.
  if (m_advertising) BLEDevice::getAdvertising()->start();
}

void AutoTLMBle::setStatus(uint8_t state, uint8_t detail) {
  m_state = state;
  m_detail = detail;
  if (!m_impl) return;
  uint8_t v[2] = {state, detail};
  m_impl->status->setValue(v, 2);
  m_impl->status->notify();
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
      // IDLE transitions don't clear a terminal state — it holds until the
      // next creds write, so the app can never miss the outcome.
      break;
  }
}

void AutoTLMBle::runScanStep() {
  if (m_scanRequested && !m_scanRunning) {
    m_scanRequested = false;
    if (!m_sessionAuthed) return;  // possession proof required before any scan
    // Don't churn the radio while a validation attempt owns it, and throttle
    // back-to-back scans (an authed-but-hostile peer must not free-run them).
    if (m_car && m_car->wifiChangeState() == AUTOTLM_WIFI_VALIDATING) {
      m_scanRequested = true;  // defer, don't drop — auto-runs after validation
      return;
    }
    const uint32_t now = millis();
    if (m_lastScanMs && now - m_lastScanMs < BLE_SCAN_MIN_INTERVAL_MS) return;  // rate-limited
    m_lastScanMs = now;
    if (m_car) m_car->net().setRadioBusy(true);  // pause the net task's reconnect loop
    WiFi.scanNetworks(true /* async */);
    m_scanRunning = true;
    m_scanRetried = false;
    if (m_log) m_log->println("BLE:scan started");
    return;
  }
  if (!m_scanRunning) return;
  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  if (n < 0) {
    // Failed (radio was busy). Retry once before giving up, so a single
    // collision doesn't hand the app an empty list.
    if (!m_scanRetried) {
      m_scanRetried = true;
      WiFi.scanDelete();
      WiFi.scanNetworks(true);
      return;
    }
    m_scanRunning = false;
    if (m_car) m_car->net().setRadioBusy(false);
    m_impl->scan->setValue("[]");
    m_impl->scan->notify();
    return;
  }
  m_scanRunning = false;
  if (m_car) m_car->net().setRadioBusy(false);

  // Strongest-first, deduped, top 10, into one ≤512-byte attribute. A leading
  // "seq" lets the app tell a fresh list from a stale one if a refresh lands
  // mid-read.
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
  for (int a = 1; a < count; a++) {  // insertion sort by RSSI desc
    const int v = idx[a];
    int b = a - 1;
    while (b >= 0 && WiFi.RSSI(idx[b]) < WiFi.RSSI(v)) { idx[b + 1] = idx[b]; b--; }
    idx[b + 1] = v;
  }

  char buf[512];
  size_t len = 0;
  len += snprintf(buf, sizeof(buf), "{\"seq\":%u,\"nets\":[", (unsigned)(++m_scanSeq));
  for (int k = 0; k < count; k++) {
    char item[96];
    char ssid[65];
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
    if (m < 0 || len + m + 3 >= sizeof(buf)) break;
    memcpy(buf + len, item, m);
    len += m;
  }
  len += snprintf(buf + len, sizeof(buf) - len, "]}");
  WiFi.scanDelete();

  m_impl->scan->setValue((uint8_t*)buf, len);
  m_impl->scan->notify();  // freshness signal only — the app reads for the full list
  if (m_log) m_log->printf("BLE:scan done (%d networks, seq %u)\n", count, (unsigned)m_scanSeq);
}

void AutoTLMBle::tick() {
  if (!m_impl || !m_car) return;

  // Possession proof: the setup code must match the per-device WPA2 label
  // password. Verified HERE (sketch core), never on the BT task, never by
  // the consumer. apPassword() is chip-id math — stable per unit.
  if (m_authPending) {
    char code[16];
    taskENTER_CRITICAL(&m_lock);
    memcpy(code, m_pendAuthCode, sizeof(code));
    m_authPending = false;
    taskEXIT_CRITICAL(&m_lock);
    char want[16];
    m_car->config().apPassword(want, sizeof(want));
    if (want[0] && strcasecmp(code, want) == 0) {
      m_sessionAuthed = true;
      if (m_log) m_log->println("BLE:session unlocked (setup code accepted)");
    } else {
      m_sessionAuthed = false;
      setStatus(AUTOTLM_BLE_REVERTED, AUTOTLM_BLE_DETAIL_REJECTED);
      if (m_log) m_log->println("BLE:auth rejected (bad setup code)");
    }
  }

  if (m_credsPending) {
    char code[16], ssid[33], pass[65];
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
    const bool codeOk = m_sessionAuthed && want[0] && strcasecmp(code, want) == 0;
    if (!codeOk) {
      setStatus(AUTOTLM_BLE_REVERTED, AUTOTLM_BLE_DETAIL_REJECTED);
      if (m_log) m_log->println("BLE:creds rejected (not authed / bad setup code)");
    } else if (!ssid[0]) {
      // Authed but no network named — every creds write must yield a visible
      // outcome, so say so rather than silently swallowing it.
      setStatus(AUTOTLM_BLE_REVERTED, AUTOTLM_BLE_DETAIL_NO_SSID);
      if (m_log) m_log->println("BLE:creds rejected (no ssid)");
    } else if (m_car->wifiChangeState() == AUTOTLM_WIFI_VALIDATING) {
      // A change is already in flight — starting another would clobber the
      // validate-and-rollback handshake. Tell the app to retry after it settles.
      setStatus(AUTOTLM_BLE_BUSY, 0);
      if (m_log) m_log->println("BLE:creds deferred (a change is already validating)");
    } else {
      setStatus(AUTOTLM_BLE_CREDS_RECEIVED, 0);
      m_car->changeWifi(ssid, pass);
      if (m_log) m_log->printf("BLE:creds accepted — validating \"%s\"\n", ssid);
    }
    memset(pass, 0, sizeof(pass));  // don't leave creds on the stack longer than needed
  }

  runScanStep();
}

#else  // !ESP32 or BT disabled — the service politely reports "not available".

bool AutoTLMBle::begin(AutoTLM&, const char*) {
  if (m_log) m_log->println("BLE:not available on this platform");
  return false;
}
void AutoTLMBle::advertise(bool) {}
void AutoTLMBle::onDisconnect() {}
void AutoTLMBle::feedWifiChange(int, int) {}
void AutoTLMBle::setStatus(uint8_t, uint8_t) {}
void AutoTLMBle::runScanStep() {}
void AutoTLMBle::tick() {}

#endif  // ESP32 && CONFIG_BT_ENABLED
