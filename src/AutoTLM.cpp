/*
 * AutoTLM.cpp — facade implementation (100% board-agnostic).
 * Part of AutoTLM Core — MIT licensed.
 */
#include "AutoTLM.h"

// Refresh the shared frame at most this often; the copy under the lock is
// cheap but there is no point doing it at loop() speed for a 1 Hz push.
#define COMPOSE_MS 100

static void frameProviderThunk(void* ctx, AutoTLMFrame& out) {
  ((AutoTLM*)ctx)->snapshotFrame(out);
}
static void diagSaverThunk(void* ctx) { ((AutoTLM*)ctx)->saveDiagnostics(); }

bool AutoTLM::begin(AutoTLMHAL& hal) {
  m_hal = &hal;

#if defined(ESP32)
  if (!m_mutex) m_mutex = xSemaphoreCreateMutex();
#endif

  m_config.begin();
  if (m_log) m_config.printPrevSession(*m_log);

  const bool boardOk = m_hal->begin();
  if (m_log) m_log->printf("AUTOTLM:%s board=%s\n", boardOk ? "OK" : "BOARD-FAIL", m_hal->boardId());

  // Device identity: a deviceId() override wins (even one set before
  // begin()); otherwise board type + chip id. Written under the lock — the
  // net task may already be running if wifi() was called before begin().
  lock();
  m_frame.clear();
  strncpy(m_frame.deviceType, m_hal->deviceType(), sizeof(m_frame.deviceType) - 1);
  if (m_idOverride[0]) {
    strncpy(m_frame.deviceId, m_idOverride, sizeof(m_frame.deviceId) - 1);
  } else {
#if defined(ESP32)
    snprintf(m_frame.deviceId, sizeof(m_frame.deviceId), "%08X", (uint32_t)(ESP.getEfuseMac() >> 16));
#else
    strncpy(m_frame.deviceId, m_hal->boardId(), sizeof(m_frame.deviceId) - 1);
#endif
  }
  strncpy(m_frame.mems, m_hal->imuName(), sizeof(m_frame.mems) - 1);
  unlock();

  // GNSS + IMU come up now (fast, local). OBD stays lazy: it talks to the
  // car's bus and can stall, which must never block connectivity.
  m_gnssBegan = m_gnss.begin(hal);
  if (m_log) m_log->printf("GNSS:%s\n", m_gnssBegan ? "OK" : "NO");

  const bool imuOk = m_imu.begin(hal);
  if (m_log) m_log->printf("MEMS:%s\n", imuOk ? m_hal->imuName() : "NO");

  m_obd.begin(hal);
  m_obd.setLogStream(m_log);

  m_net.attach(frameProviderThunk, diagSaverThunk, this);
  m_net.setLogStream(m_log);

  m_prov.attach(&m_config, m_log);

  return boardOk;
}

bool AutoTLM::provision() {
  if (m_config.hasWifi()) {
    // Provisioned: come up on the saved settings. wifi(nullptr) reuses the
    // stored credentials; the cloud endpoint applies only if one was saved.
    wifi(nullptr, nullptr);
    char url[AUTOTLM_NET_PATH_LEN + AUTOTLM_NET_HOST_LEN], token[AUTOTLM_NET_TOKEN_LEN];
    uint32_t intervalMs = 1000;
    if (m_config.loadCloud(url, sizeof(url), token, sizeof(token), &intervalMs))
      cloud(url, token, intervalMs);
    return true;
  }
  beginPortal();
  return false;  // not provisioned yet — the portal (if it came up) takes it from here
}

bool AutoTLM::beginPortal(const char* apName, const char* apPass) {
  return m_prov.start(apName, apPass);
}

void AutoTLM::wifi(const char* ssid, const char* pass) {
  char savedSsid[33], savedPass[65];
  if (ssid && ssid[0]) {
    // Fresh credentials win and are persisted (so the unit reconnects on its
    // own forever after, even in sketches that stop passing them).
    m_config.saveWifi(ssid, pass ? pass : "");
    m_net.wifi(ssid, pass ? pass : "");
  } else if (m_config.loadWifi(savedSsid, sizeof(savedSsid), savedPass, sizeof(savedPass))) {
    if (m_log) m_log->printf("WIFI:using saved credentials \"%s\"\n", savedSsid);
    m_net.wifi(savedSsid, savedPass);
  } else if (m_log) {
    m_log->println("WIFI:no credentials (pass ssid/pass to car.wifi, or save them once)");
  }
}

void AutoTLM::cloud(const char* url, const char* token, uint32_t intervalMs) {
  m_net.cloud(url, token, intervalMs);
}

void AutoTLM::update() {
  if (!m_hal) return;
  const uint32_t t0 = micros();

  if (m_prov.active()) m_prov.tick();

  ledTick();
  m_gnss.tick();
  m_imu.tick();
  m_obd.tick();  // includes the lazy ECU bring-up

  const uint32_t now = millis();
  if (now - m_lastCompose >= COMPOSE_MS) {
    m_lastCompose = now;
    composeFrame();
  }

  const uint32_t dt = micros() - t0;
  if (dt > m_maxLoopUs) m_maxLoopUs = dt;
}

// Copy the modules' latest values into the shared frame. Held briefly under
// the lock so the core-0 network task always snapshots a coherent frame.
void AutoTLM::composeFrame() {
  const AutoTLMGPS g = m_gnss.data();
  const AutoTLMMotion m = m_imu.data();

  lock();
  m_frame.gnssUp = m_gnss.alive();
  m_frame.rssi = m_net.rssi();

  m_frame.moduleCount = (uint8_t)m_obd.moduleCount();
  m_frame.obdConnected = m_obd.connected();
  m_frame.rpm = m_obd.rpm();
  m_frame.speedKph = m_obd.speedKph();
  m_frame.coolantC = m_obd.coolantC();
  m_frame.loadPct = m_obd.loadPct();
  m_frame.throttlePct = m_obd.throttlePct();
  m_frame.volts = m_obd.volts();
  strncpy(m_frame.vin, m_obd.vin(), sizeof(m_frame.vin) - 1);
  m_obd.fillFrame(m_frame.pidHave, m_frame.pidVal);

  m_frame.mil = m_obd.mil();
  strncpy(m_frame.dtc, m_obd.dtcString(), sizeof(m_frame.dtc) - 1);

  m_frame.fix = g.fix;
  m_frame.lat = g.lat;
  m_frame.lng = g.lng;
  m_frame.altM = g.altM;
  m_frame.gpsSpeedKph = g.speedKph;
  m_frame.course = g.course;
  m_frame.hdop = g.hdop;
  m_frame.sats = g.sats;

  m_frame.imuHave = m.valid;
  m_frame.ax = m.ax; m_frame.ay = m.ay; m_frame.az = m.az;
  m_frame.gx = m.gx; m_frame.gy = m.gy; m_frame.gz = m.gz;
  unlock();
}

void AutoTLM::snapshotFrame(AutoTLMFrame& out) {
  lock();
  out = m_frame;
  unlock();
}

AutoTLMFrame AutoTLM::frame() {
  AutoTLMFrame f;
  snapshotFrame(f);
  return f;
}

void AutoTLM::deviceId(const char* id) {
  if (!id) return;
  // Remember the override separately so it survives begin() (which resets
  // the frame) no matter which order the sketch calls things in.
  strncpy(m_idOverride, id, sizeof(m_idOverride) - 1);
  m_idOverride[sizeof(m_idOverride) - 1] = 0;
  lock();
  strncpy(m_frame.deviceId, m_idOverride, sizeof(m_frame.deviceId) - 1);
  m_frame.deviceId[sizeof(m_frame.deviceId) - 1] = 0;
  unlock();
}

void AutoTLM::saveDiagnostics() {
  AutoTLMDiag d;
  d.pushOk = m_net.pushOk();
  d.pushFail = m_net.pushFail();
  d.lastHttp = m_net.lastHttp();
  d.wifiDrops = m_net.wifiDrops();
  d.obdEver = m_obd.everConnected();
  d.maxLoopUs = m_maxLoopUs;
  d.boots = m_config.prevSession().boots;
  m_config.saveDiag(d);
}

void AutoTLM::printDiagnostics(Stream& out) {
  m_config.printPrevSession(out);
  out.printf("DIAG NOW: wifi=%d rssi=%d pushOk=%lu pushFail=%lu lastHttp=%d wifiDrops=%lu "
             "obd=%d gnss=%d maxLoopMs=%lu\n",
             m_net.wifiConnected(), m_net.rssi(), (unsigned long)m_net.pushOk(),
             (unsigned long)m_net.pushFail(), m_net.lastHttp(),
             (unsigned long)m_net.wifiDrops(), m_obd.connected(), m_gnss.alive(),
             (unsigned long)(m_maxLoopUs / 1000));
}

void AutoTLM::setLogStream(Stream* s) {
  m_log = s;
  m_obd.setLogStream(s);
  m_net.setLogStream(s);
}

// LED legend (same convention the road firmware proved out):
//   fast 350 ms blink = WiFi down · slow 1 s blink = WiFi up, pushes not
//   landing · brief 90 ms pulse per push = streaming · off = net unused.
void AutoTLM::ledTick() {
  if (!m_ledEnabled || !m_hal) return;
  const uint32_t t = millis();
  bool on = false;
  switch (m_net.state()) {
    case AUTOTLM_NET_DISABLED:  on = false; break;
    case AUTOTLM_NET_OFFLINE:   on = (t / 350) % 2; break;
    case AUTOTLM_NET_NO_PUSH:   on = (t / 1000) % 2; break;
    case AUTOTLM_NET_STREAMING: on = (t - m_net.lastPushMs()) < 90; break;
  }
  m_hal->led(on);
}

void AutoTLM::lock() {
#if defined(ESP32)
  if (m_mutex) xSemaphoreTake(m_mutex, portMAX_DELAY);
#endif
}

void AutoTLM::unlock() {
#if defined(ESP32)
  if (m_mutex) xSemaphoreGive(m_mutex);
#endif
}
