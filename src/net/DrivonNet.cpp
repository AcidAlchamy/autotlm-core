/*
 * DrivonNet.cpp — WiFi + plain-HTTP cloud push on a dedicated core.
 * Part of Drivon Core — MIT licensed.
 */
#include "DrivonNet.h"

#include <string.h>

#if defined(ESP32)

// Retry WiFi this often while disconnected.
#define WIFI_RETRY_MS 8000
// Re-resolve the ingest host this often even when pushes succeed.
#define DNS_REFRESH_MS 300000
// Persist diagnostics this often.
#define DIAG_SAVE_MS 20000
// Heartbeat log line (verbose mode).
#define DIAG_PRINT_MS 10000

static void netTaskTrampoline(void* arg) { ((DrivonNet*)arg)->taskLoop(); }

void DrivonNet::wifi(const char* ssid, const char* pass) {
  strncpy(m_ssid, ssid ? ssid : "", sizeof(m_ssid) - 1);
  strncpy(m_pass, pass ? pass : "", sizeof(m_pass) - 1);
  m_wifiWanted = m_ssid[0] != 0;
  if (!m_wifiWanted) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(m_ssid, m_pass);
  if (m_log) m_log->printf("WIFI:connecting to \"%s\"\n", m_ssid);
  ensureTask();
}

void DrivonNet::cloud(const char* url, const char* token, uint32_t intervalMs) {
  if (!url) return;

  // Parse http://host[:port]/path. TLS is refused by design: the handshake
  // is what broke pushes over weak cellular. A bearer token still guards the
  // endpoint; use an ingest path that accepts plain HTTP.
  const char* p = url;
  if (strncmp(p, "https://", 8) == 0) {
    p += 8;
    if (m_log)
      m_log->println("DRIVON: https:// requested but TLS is not supported (it stalls on weak "
                     "cellular). Pushing plain HTTP on port 80 — point Drivon at an http:// ingest.");
  } else if (strncmp(p, "http://", 7) == 0) {
    p += 7;
  }

  const char* slash = strchr(p, '/');
  const char* colon = strchr(p, ':');
  if (colon && (!slash || colon < slash)) {
    size_t n = (size_t)(colon - p);
    if (n >= sizeof(m_host)) n = sizeof(m_host) - 1;
    memcpy(m_host, p, n);
    m_host[n] = 0;
    m_port = (uint16_t)atoi(colon + 1);
    if (m_port == 0) m_port = 80;
  } else {
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    if (n >= sizeof(m_host)) n = sizeof(m_host) - 1;
    memcpy(m_host, p, n);
    m_host[n] = 0;
    m_port = 80;
  }
  strncpy(m_path, slash ? slash : "/", sizeof(m_path) - 1);
  strncpy(m_token, token ? token : "", sizeof(m_token) - 1);
  m_intervalMs = intervalMs ? intervalMs : 1000;
  m_cloudWanted = m_host[0] != 0;
  m_haveIp = false;

  if (m_cloudWanted) {
    if (m_log) m_log->printf("CLOUD:%s:%u%s every %lums\n", m_host, m_port, m_path,
                             (unsigned long)m_intervalMs);
    ensureTask();
  }
}

void DrivonNet::attach(DrivonFrameProvider provider, DrivonDiagSaver diagSaver, void* ctx) {
  m_provider = provider;
  m_diagSaver = diagSaver;
  m_ctx = ctx;
}

bool DrivonNet::wifiConnected() const { return WiFi.status() == WL_CONNECTED; }

int DrivonNet::rssi() const { return wifiConnected() ? (int)WiFi.RSSI() : 0; }

DrivonNetState DrivonNet::state() const {
  if (!m_wifiWanted) return DRIVON_NET_DISABLED;
  if (!wifiConnected()) return DRIVON_NET_OFFLINE;
  if (!m_cloudWanted) return DRIVON_NET_NO_PUSH;
  if (m_lastPushMs != 0 && millis() - m_lastPushMs < 4000) return DRIVON_NET_STREAMING;
  return DRIVON_NET_NO_PUSH;
}

// The whole network life runs here, pinned to core 0. Sensor code on core 1
// can block on the car bus all it likes; pushes keep flowing.
void DrivonNet::ensureTask() {
  if (m_task) return;
  xTaskCreatePinnedToCore(netTaskTrampoline, "drivon-net", 12288, this, 1, &m_task, 0);
}

void DrivonNet::taskLoop() {
  uint32_t lastPush = 0, lastWifiTry = 0, lastDiagSave = 0, lastDiagPrint = 0;
  for (;;) {
    const uint32_t now = millis();

    if (m_wifiWanted && WiFi.status() != WL_CONNECTED) {
      if (now - lastWifiTry > WIFI_RETRY_MS) {
        lastWifiTry = now;
        m_wifiDrops = m_wifiDrops + 1;
        WiFi.disconnect();
        WiFi.begin(m_ssid, m_pass);
      }
    } else if (m_cloudWanted && WiFi.status() == WL_CONNECTED &&
               now - lastPush >= m_intervalMs) {
      lastPush = now;
      pushFrame();
    }

    if (m_diagSaver && now - lastDiagSave > DIAG_SAVE_MS) {
      lastDiagSave = now;
      m_diagSaver(m_ctx);
    }
    if (m_verbose && m_log && now - lastDiagPrint > DIAG_PRINT_MS) {
      lastDiagPrint = now;
      m_log->printf("DIAG NOW: wifi=%d rssi=%d pushOk=%lu pushFail=%lu lastHttp=%d wifiDrops=%lu\n",
                    WiFi.status() == WL_CONNECTED, rssi(), (unsigned long)m_pushOk,
                    (unsigned long)m_pushFail, m_lastHttp, (unsigned long)m_wifiDrops);
    }

    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

// Resolve once, remember, refresh every 5 min. DNS over weak cellular is the
// single flakiest step of a push — never pay for it per-frame.
void DrivonNet::resolveHost() {
  IPAddress ip;
  if (WiFi.hostByName(m_host, ip)) {
    m_ip = ip;
    m_haveIp = true;
    m_ipAt = millis();
    if (m_log) m_log->printf("DNS:%s -> %s\n", m_host, ip.toString().c_str());
  } else {
    m_haveIp = false;
    if (m_log) m_log->println("DNS:fail");
  }
}

void DrivonNet::pushFrame() {
  if (!m_provider) return;
  const uint32_t t0 = millis();

  if (!m_haveIp || millis() - m_ipAt > DNS_REFRESH_MS) resolveHost();

  WiFiClient client;
  client.setTimeout(8000);
  const bool ok = m_haveIp ? client.connect(m_ip, m_port) : client.connect(m_host, m_port);
  if (!ok) {
    m_pushFail = m_pushFail + 1;
    m_lastHttp = -1;
    m_haveIp = false;  // the cached IP may be stale — re-resolve next time
    if (m_verbose && m_log) m_log->printf("PUSH:connect-fail (%lums)\n", (unsigned long)(millis() - t0));
    return;
  }

  m_provider(m_ctx, m_snapshot);
  const size_t bodyLen = m_snapshot.toJson(m_json, sizeof(m_json));

  char head[320];
  snprintf(head, sizeof(head),
           "POST %s HTTP/1.1\r\n"
           "Host: %s\r\n"
           "Authorization: Bearer %s\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: %u\r\n"
           "Connection: close\r\n\r\n",
           m_path, m_host, m_token, (unsigned)bodyLen);
  client.print(head);
  client.print(m_json);

  // Only the status line matters; Connection: close discards the rest.
  String status = client.readStringUntil('\n');
  client.stop();

  if (status.length() == 0) {
    m_pushFail = m_pushFail + 1;
    m_lastHttp = -2;
    if (m_verbose && m_log) m_log->printf("PUSH:no-resp (%lums)\n", (unsigned long)(millis() - t0));
    return;
  }

  int code = 0;
  const int sp = status.indexOf(' ');
  if (sp > 0) code = status.substring(sp + 1, sp + 4).toInt();
  m_lastHttp = code;
  if (code == 200) {
    m_pushOk = m_pushOk + 1;
    m_lastPushMs = millis();
  } else {
    m_pushFail = m_pushFail + 1;
  }
  if (m_verbose && m_log) m_log->printf("PUSH:%d (%lums)\n", code, (unsigned long)(millis() - t0));
}

#else  // !ESP32 — no radio on this platform; everything reports "disabled".

void DrivonNet::wifi(const char*, const char*) {}
void DrivonNet::cloud(const char*, const char*, uint32_t) {}
void DrivonNet::attach(DrivonFrameProvider provider, DrivonDiagSaver diagSaver, void* ctx) {
  m_provider = provider;
  m_diagSaver = diagSaver;
  m_ctx = ctx;
}
bool DrivonNet::wifiConnected() const { return false; }
int DrivonNet::rssi() const { return 0; }
DrivonNetState DrivonNet::state() const { return DRIVON_NET_DISABLED; }
void DrivonNet::taskLoop() {}
void DrivonNet::ensureTask() {}
void DrivonNet::pushFrame() {}
void DrivonNet::resolveHost() {}

#endif
