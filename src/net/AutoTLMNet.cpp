/*
 * AutoTLMNet.cpp — WiFi + plain-HTTP cloud push on a dedicated core.
 * Part of AutoTLM Core — MIT licensed.
 */
#include "AutoTLMNet.h"

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

static void netTaskTrampoline(void* arg) { ((AutoTLMNet*)arg)->taskLoop(); }

void AutoTLMNet::ensureMutex() {
  // Only ever called on the sketch core before the task exists (every task
  // launch goes through wifi()/cloud(), which call this first) — so the
  // lazy creation itself cannot race.
  if (!m_cfgMutex) m_cfgMutex = xSemaphoreCreateMutex();
}

void AutoTLMNet::lockCfg() const {
  if (m_cfgMutex) xSemaphoreTake(m_cfgMutex, portMAX_DELAY);
}

void AutoTLMNet::unlockCfg() const {
  if (m_cfgMutex) xSemaphoreGive(m_cfgMutex);
}

void AutoTLMNet::wifi(const char* ssid, const char* pass) {
  if (!ssid || !ssid[0]) return;
  ensureMutex();

  lockCfg();
  strncpy(m_ssid, ssid, sizeof(m_ssid) - 1);
  m_ssid[sizeof(m_ssid) - 1] = 0;
  strncpy(m_pass, pass ? pass : "", sizeof(m_pass) - 1);
  m_pass[sizeof(m_pass) - 1] = 0;
  m_wifiWanted = true;
  m_reassoc = true;  // the task owns the WiFi driver; ask it to (re)associate
  unlockCfg();

  if (m_log) m_log->printf("WIFI:connecting to \"%s\"\n", ssid);
  ensureTask();
}

void AutoTLMNet::cloud(const char* url, const char* token, uint32_t intervalMs) {
  if (!url) return;
  ensureMutex();

  // Parse http://host[:port]/path into locals first, then publish atomically.
  // TLS is refused by design: the handshake is what broke pushes over weak
  // cellular. A bearer token still guards the endpoint.
  char host[AUTOTLM_NET_HOST_LEN] = "";
  char path[AUTOTLM_NET_PATH_LEN] = "/";
  uint16_t port = 80;
  bool wasHttps = false;

  const char* p = url;
  if (strncmp(p, "https://", 8) == 0) {
    p += 8;
    wasHttps = true;
  } else if (strncmp(p, "http://", 7) == 0) {
    p += 7;
  }

  const char* slash = strchr(p, '/');
  const char* colon = strchr(p, ':');
  if (colon && (!slash || colon < slash)) {
    size_t n = (size_t)(colon - p);
    if (n >= sizeof(host)) n = sizeof(host) - 1;
    memcpy(host, p, n);
    host[n] = 0;
    port = (uint16_t)atoi(colon + 1);
    if (port == 0) port = 80;
  } else {
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    if (n >= sizeof(host)) n = sizeof(host) - 1;
    memcpy(host, p, n);
    host[n] = 0;
  }
  if (slash) {
    strncpy(path, slash, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
  }
  if (wasHttps) {
    port = 80;  // match the warning below — never plaintext at a TLS port
    if (m_log)
      m_log->println("AUTOTLM: https:// requested but TLS is not supported (it stalls on weak "
                     "cellular). Pushing plain HTTP on port 80 — point AutoTLM at an http:// ingest.");
  }
  if (!host[0]) return;

  lockCfg();
  memcpy(m_host, host, sizeof(m_host));
  memcpy(m_path, path, sizeof(m_path));
  strncpy(m_token, token ? token : "", sizeof(m_token) - 1);
  m_token[sizeof(m_token) - 1] = 0;
  m_port = port;
  m_intervalMs = intervalMs ? intervalMs : 1000;
  m_cloudWanted = true;
  m_haveIp = false;  // endpoint changed — the DNS cache is stale
  unlockCfg();

  if (m_log) m_log->printf("CLOUD:%s:%u%s every %lums\n", host, port, path,
                           (unsigned long)(intervalMs ? intervalMs : 1000));
  ensureTask();
}

void AutoTLMNet::attach(AutoTLMFrameProvider provider, AutoTLMDiagSaver diagSaver, void* ctx) {
  ensureMutex();
  lockCfg();
  // ctx first, provider last: the task treats a non-null provider as
  // "everything is wired".
  m_ctx = ctx;
  m_diagSaver = diagSaver;
  m_provider = provider;
  unlockCfg();
}

bool AutoTLMNet::wifiConnected() const { return WiFi.status() == WL_CONNECTED; }

int AutoTLMNet::rssi() const { return wifiConnected() ? (int)WiFi.RSSI() : 0; }

AutoTLMNetState AutoTLMNet::state() const {
  if (!m_wifiWanted && !m_cloudWanted) return AUTOTLM_NET_DISABLED;
  if (!wifiConnected()) return AUTOTLM_NET_OFFLINE;
  if (m_lastPushMs != 0 && millis() - m_lastPushMs < 4000) return AUTOTLM_NET_STREAMING;
  return AUTOTLM_NET_NO_PUSH;
}

// The whole network life runs here, pinned to core 0. Sensor code on core 1
// can block on the car bus all it likes; pushes keep flowing.
void AutoTLMNet::ensureTask() {
  if (m_task) return;
  xTaskCreatePinnedToCore(netTaskTrampoline, "autotlm-net", 12288, this, 1, &m_task, 0);
}

void AutoTLMNet::taskLoop() {
  uint32_t lastPush = 0, lastWifiTry = 0, lastDiagSave = 0, lastDiagPrint = 0;
  for (;;) {
    const uint32_t now = millis();

    // Snapshot the flags (and any fresh credentials) under the lock.
    lockCfg();
    const bool wifiWanted = m_wifiWanted;
    const bool cloudWanted = m_cloudWanted;
    const uint32_t intervalMs = m_intervalMs;
    const bool reassoc = m_reassoc;
    m_reassoc = false;
    char ssid[AUTOTLM_NET_SSID_LEN], pass[AUTOTLM_NET_PASS_LEN];
    if (reassoc || (wifiWanted && WiFi.status() != WL_CONNECTED)) {
      memcpy(ssid, m_ssid, sizeof(ssid));
      memcpy(pass, m_pass, sizeof(pass));
    }
    AutoTLMDiagSaver diagSaver = m_diagSaver;
    void* ctx = m_ctx;
    unlockCfg();

    if (reassoc) {
      // Fresh credentials (first call or a network switch): associate now.
      WiFi.mode(WIFI_STA);
      WiFi.disconnect();
      WiFi.begin(ssid, pass);
      lastWifiTry = now;
    } else if (wifiWanted && WiFi.status() != WL_CONNECTED) {
      if (now - lastWifiTry > WIFI_RETRY_MS) {
        lastWifiTry = now;
        m_wifiDrops = m_wifiDrops + 1;
        WiFi.disconnect();
        WiFi.begin(ssid, pass);
      }
    } else if (cloudWanted && WiFi.status() == WL_CONNECTED &&
               now - lastPush >= intervalMs) {
      lastPush = now;
      pushFrame();
    }

    if (diagSaver && now - lastDiagSave > DIAG_SAVE_MS) {
      lastDiagSave = now;
      diagSaver(ctx);
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
void AutoTLMNet::resolveHost(const char* host) {
  IPAddress ip;
  if (WiFi.hostByName(host, ip)) {
    lockCfg();
    m_ip = ip;
    m_haveIp = true;
    m_ipAt = millis();
    unlockCfg();
    if (m_log) m_log->printf("DNS:%s -> %s\n", host, ip.toString().c_str());
  } else {
    lockCfg();
    m_haveIp = false;
    unlockCfg();
    if (m_log) m_log->println("DNS:fail");
  }
}

void AutoTLMNet::pushFrame() {
  // Work on a coherent copy of the endpoint config for this whole push.
  lockCfg();
  char host[AUTOTLM_NET_HOST_LEN], path[AUTOTLM_NET_PATH_LEN], token[AUTOTLM_NET_TOKEN_LEN];
  memcpy(host, m_host, sizeof(host));
  memcpy(path, m_path, sizeof(path));
  memcpy(token, m_token, sizeof(token));
  const uint16_t port = m_port;
  AutoTLMFrameProvider provider = m_provider;
  void* ctx = m_ctx;
  unlockCfg();
  if (!provider || !ctx || !host[0]) return;

  const uint32_t t0 = millis();

  lockCfg();
  bool haveIp = m_haveIp;
  IPAddress ip = m_ip;
  const uint32_t ipAt = m_ipAt;
  unlockCfg();
  if (!haveIp || millis() - ipAt > DNS_REFRESH_MS) {
    resolveHost(host);
    lockCfg();
    haveIp = m_haveIp;
    ip = m_ip;
    unlockCfg();
  }

  WiFiClient client;
  client.setTimeout(8000);
  const bool ok = haveIp ? client.connect(ip, port) : client.connect(host, port);
  if (!ok) {
    m_pushFail = m_pushFail + 1;
    m_lastHttp = -1;
    lockCfg();
    m_haveIp = false;  // the cached IP may be stale — re-resolve next time
    unlockCfg();
    if (m_verbose && m_log) m_log->printf("PUSH:connect-fail (%lums)\n", (unsigned long)(millis() - t0));
    return;
  }

  provider(ctx, m_snapshot);
  const size_t bodyLen = m_snapshot.toJson(m_json, sizeof(m_json));

  // Sized for the worst case the config buffers allow (host+path+token+~130).
  char head[672];
  const int headLen = snprintf(head, sizeof(head),
                               "POST %s HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "Authorization: Bearer %s\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: %u\r\n"
                               "Connection: close\r\n\r\n",
                               path, host, token, (unsigned)bodyLen);
  if (headLen < 0 || headLen >= (int)sizeof(head)) {
    // A malformed request would fail confusingly server-side — fail loudly here.
    client.stop();
    m_pushFail = m_pushFail + 1;
    m_lastHttp = -3;
    if (m_log) m_log->println("PUSH:header-overflow (host/path/token too long)");
    return;
  }
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

void AutoTLMNet::wifi(const char*, const char*) {}
void AutoTLMNet::cloud(const char*, const char*, uint32_t) {}
void AutoTLMNet::attach(AutoTLMFrameProvider provider, AutoTLMDiagSaver diagSaver, void* ctx) {
  m_ctx = ctx;
  m_diagSaver = diagSaver;
  m_provider = provider;
}
bool AutoTLMNet::wifiConnected() const { return false; }
int AutoTLMNet::rssi() const { return 0; }
AutoTLMNetState AutoTLMNet::state() const { return AUTOTLM_NET_DISABLED; }
void AutoTLMNet::taskLoop() {}
void AutoTLMNet::ensureMutex() {}
void AutoTLMNet::ensureTask() {}
void AutoTLMNet::lockCfg() const {}
void AutoTLMNet::unlockCfg() const {}
void AutoTLMNet::pushFrame() {}
void AutoTLMNet::resolveHost(const char*) {}

#endif
