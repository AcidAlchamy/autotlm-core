/*
 * DrivonNet.h — WiFi + cloud telemetry push.
 *
 * This module encodes the connectivity lessons that made the platform work
 * in a moving car; they are deliberate design, not shortcuts:
 *
 *  - PLAIN HTTP, NOT HTTPS. The TLS handshake chokes on weak cellular
 *    backhaul (an LTE hotspot at highway speed); plain HTTP pushes complete
 *    in well under a second on the same signal. A bearer token still
 *    authenticates every POST.
 *  - CACHED DNS. The resolved server IP is kept for 5 minutes and only
 *    re-resolved on failure — DNS is the flakiest step over cellular.
 *  - FRESH CONNECTION PER PUSH ("Connection: close"). Keep-alive through CDN
 *    edges caused multi-second stalls; a clean connect each time gives
 *    consistent latency.
 *  - DEDICATED CORE. The whole reconnect + push loop runs on a FreeRTOS task
 *    pinned to core 0, so blocking OBD reads on core 1 can never starve
 *    uploads (and vice versa).
 *
 * Part of Drivon Core — MIT licensed.
 */
#ifndef DRIVON_NET_H
#define DRIVON_NET_H

#include <Arduino.h>
#include "../DrivonFrame.h"

#if defined(ESP32)
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

/** Copies a coherent snapshot of the live frame into `out`. */
typedef void (*DrivonFrameProvider)(void* ctx, DrivonFrame& out);
/** Called every ~20 s from the network task so the owner can persist diagnostics. */
typedef void (*DrivonDiagSaver)(void* ctx);

/** Connectivity state, used by the status-LED convention. */
enum DrivonNetState {
  DRIVON_NET_DISABLED,   ///< wifi() never called
  DRIVON_NET_OFFLINE,    ///< WiFi not associated
  DRIVON_NET_NO_PUSH,    ///< WiFi up but pushes failing / none recent
  DRIVON_NET_STREAMING,  ///< pushes landing
};

class DrivonNet {
 public:
  /** Store WiFi credentials and start associating (non-blocking). */
  void wifi(const char* ssid, const char* pass);

  /**
   * Configure the telemetry destination and start the core-0 push task.
   * @param url        e.g. "http://yourserver.com/api/ingest" — plain HTTP
   *                   (an https:// URL is accepted with a loud warning and
   *                   pushed to port 80: TLS is unusable on weak cellular)
   * @param token      sent as "Authorization: Bearer <token>"
   * @param intervalMs how often to POST a frame (default 1 s)
   */
  void cloud(const char* url, const char* token, uint32_t intervalMs = 1000);

  /** Wire the frame source + diag persistence (done by the Drivon facade). */
  void attach(DrivonFrameProvider provider, DrivonDiagSaver diagSaver, void* ctx);

  // ------------------------------------------------------------- status
  bool wifiConnected() const;
  int rssi() const;                       ///< dBm, 0 when offline
  DrivonNetState state() const;
  uint32_t pushOk() const { return m_pushOk; }
  uint32_t pushFail() const { return m_pushFail; }
  int lastHttp() const { return m_lastHttp; }          ///< -1 connect fail, -2 no response
  uint32_t wifiDrops() const { return m_wifiDrops; }
  uint32_t lastPushMs() const { return m_lastPushMs; } ///< millis() of last 200 OK

  /** Print PUSH/DIAG lines as it works (default on — field debugging gold). */
  void setVerbose(bool v) { m_verbose = v; }
  /** Route log lines somewhere else (nullptr = silent). */
  void setLogStream(Stream* s) { m_log = s; }

  /** The network task body — public only for the FreeRTOS trampoline. */
  void taskLoop();

 private:
  void ensureTask();
  void pushFrame();
  void resolveHost();

  char m_ssid[33] = "";
  char m_pass[65] = "";
  bool m_wifiWanted = false;

  char m_host[96] = "";
  char m_path[128] = "/";
  char m_token[80] = "";
  uint16_t m_port = 80;
  uint32_t m_intervalMs = 1000;
  bool m_cloudWanted = false;

  DrivonFrameProvider m_provider = nullptr;
  DrivonDiagSaver m_diagSaver = nullptr;
  void* m_ctx = nullptr;

  volatile uint32_t m_pushOk = 0, m_pushFail = 0, m_wifiDrops = 0;
  volatile int m_lastHttp = 0;
  volatile uint32_t m_lastPushMs = 0;

  bool m_verbose = true;
  Stream* m_log = &Serial;

#if defined(ESP32)
  TaskHandle_t m_task = nullptr;
  IPAddress m_ip;
  bool m_haveIp = false;
  uint32_t m_ipAt = 0;
  DrivonFrame m_snapshot;   // task-owned copy (never touched by core 1)
  char m_json[2048];
#endif
};

#endif // DRIVON_NET_H
