/*
 * AutoTLMNet.h — WiFi + cloud telemetry push.
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
 *    uploads (and vice versa). The task is the ONLY code that touches the
 *    WiFi driver; wifi()/cloud() just publish configuration under a mutex,
 *    so they are safe to call at any time, from setup() or years into
 *    uptime.
 *
 * Part of AutoTLM Core — MIT licensed.
 */
#ifndef AUTOTLM_NET_H
#define AUTOTLM_NET_H

#include <Arduino.h>
#include "../AutoTLMFrame.h"

#if defined(ESP32)
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

/** Copies a coherent snapshot of the live frame into `out`. */
typedef void (*AutoTLMFrameProvider)(void* ctx, AutoTLMFrame& out);
/** Called every ~20 s from the network task so the owner can persist diagnostics. */
typedef void (*AutoTLMDiagSaver)(void* ctx);

/** Connectivity state, used by the status-LED convention. */
enum AutoTLMNetState {
  AUTOTLM_NET_DISABLED,   ///< neither wifi() nor cloud() ever called
  AUTOTLM_NET_OFFLINE,    ///< WiFi not associated
  AUTOTLM_NET_NO_PUSH,    ///< WiFi up but pushes failing / none recent
  AUTOTLM_NET_STREAMING,  ///< pushes landing
};

/** Result of a tryWifi() validation attempt (poll via wifiChangeState()). */
enum AutoTLMWifiChange {
  AUTOTLM_WIFI_IDLE,        ///< no change in progress
  AUTOTLM_WIFI_VALIDATING,  ///< trying the new credentials
  AUTOTLM_WIFI_OK,          ///< new credentials associated — now the working set
  AUTOTLM_WIFI_REVERTED,    ///< new credentials failed; reverted to the old ones
};

/** Why a tryWifi() attempt REVERTED (wifiChangeReason(); 0 unless reverted). */
enum AutoTLMWifiRevertReason {
  AUTOTLM_WIFI_REASON_NONE = 0,
  AUTOTLM_WIFI_REASON_NOT_FOUND = 1,  ///< SSID not seen (AP reported no-AP-found)
  AUTOTLM_WIFI_REASON_AUTH = 2,       ///< association rejected (wrong password)
  AUTOTLM_WIFI_REASON_TIMEOUT = 3,    ///< never associated / AP silent — render as "couldn't connect", not "wrong password"
};

// Config buffer sizes. The token is sized for real-world bearer tokens
// (JWTs routinely exceed 200 chars).
#define AUTOTLM_NET_SSID_LEN 33
#define AUTOTLM_NET_PASS_LEN 65
#define AUTOTLM_NET_HOST_LEN 96
#define AUTOTLM_NET_PATH_LEN 160
#define AUTOTLM_NET_TOKEN_LEN 256

// Offline buffer: frames captured while WiFi is down (tunnel, parking garage)
// and replayed as one batched POST on reconnect (ingest accepts arrays ≤ 50).
// 24 frames ≈ 35 KB heap, allocated lazily on first use; oldest drop first.
#define AUTOTLM_NET_BUFFER_FRAMES 24
// Batch serialization buffer (heap, lazy): bounds frames-per-POST in practice.
#define AUTOTLM_NET_BATCH_BYTES 16384

class AutoTLMNet {
 public:
  /**
   * Publish WiFi credentials; the core-0 task associates and reconnects
   * forever. Safe to call again at runtime to switch networks.
   */
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

  /** Wire the frame source + diag persistence (done by the AutoTLM facade). */
  void attach(AutoTLMFrameProvider provider, AutoTLMDiagSaver diagSaver, void* ctx);

  /**
   * Request one out-of-cycle push NOW (event upload: a DTC just appeared, a
   * script said "push"). Thread-safe from any core: this only raises a flag;
   * the core-0 network task performs the push on its next pass — immediately
   * when WiFi is up, or the moment it reconnects. The interval cadence is
   * unaffected.
   * @return true if a cloud endpoint is configured (the request is queued)
   */
  bool pushNow();

  /**
   * Resize the offline frame buffer (default AUTOTLM_NET_BUFFER_FRAMES;
   * 0 disables buffering). Takes effect on the next capture; the buffer is
   * heap-allocated on first use, so unused = zero cost.
   */
  void setBufferFrames(uint8_t n) { m_bufWantCap = n; }
  /** Frames currently waiting in the offline buffer. */
  uint16_t buffered() const { return m_bufCount; }

  // ------------------------------------------------ validate-and-rollback
  /**
   * Try NEW WiFi credentials without losing the working ones. The current
   * credentials are kept staged; the network task attempts the new ones for
   * ~30 s and, if they don't associate, reverts to the old working network.
   * Non-blocking — poll wifiChangeState(). This is the safe primitive behind
   * every "change my WiFi" path (USB, BLE, app): a wrong password can never
   * strand the device off-network.
   */
  void tryWifi(const char* ssid, const char* pass);
  /** Current validate-and-rollback state (AUTOTLM_WIFI_*). */
  int wifiChangeState() const { return m_wifiChange; }
  /** Why the last attempt REVERTED (AUTOTLM_WIFI_REASON_*; 0 otherwise). */
  int wifiChangeReason() const { return m_changeReason; }
  /** Monotonic id of the tryWifi() attempt a terminal state belongs to. */
  uint32_t wifiChangeGen() const { return m_changeGen; }
  /** True while a tryWifi() attempt is validating (creds can't be re-staged safely). */
  bool wifiChangeBusy() const { return m_wifiChange == AUTOTLM_WIFI_VALIDATING; }
  /** Acknowledge an OK/REVERTED result, returning to IDLE. */
  void clearWifiChange() { m_wifiChange = AUTOTLM_WIFI_IDLE; }
  /**
   * Milliseconds since the station was last associated (0 while connected).
   * The offline-reprovision policy uses this to tell "briefly offline
   * mid-drive" from "this network is really gone".
   */
  uint32_t sinceConnectedMs() const;

  /**
   * Tell the net task another owner (the BLE service) is using the WiFi radio
   * for a scan, so its offline-reconnect loop pauses instead of aborting the
   * scan. Raised around WiFi.scanNetworks(); cleared when the scan completes.
   */
  void setRadioBusy(bool busy) { m_radioBusy = busy; }

  // ------------------------------------------------------------- status
  bool wifiConnected() const;
  int rssi() const;                       ///< dBm, 0 when offline
  /**
   * Live connectivity state, derived from what is actually happening (a
   * sketch that manages WiFi itself but uses cloud() still reads STREAMING
   * while pushes land).
   */
  AutoTLMNetState state() const;
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
  void ensureMutex();
  void ensureTask();
  void lockCfg() const;
  void unlockCfg() const;
  void pushFrame();
  void resolveHost(const char* host);
  void bufferLiveFrame();
  void noteFailure(int code);   ///< buffer-worthy failure bookkeeping (+ 429 backoff)

  // ---- configuration (written by sketch core, read by the task; every
  // ---- access goes through the config mutex) ----
  char m_ssid[AUTOTLM_NET_SSID_LEN] = "";
  char m_pass[AUTOTLM_NET_PASS_LEN] = "";
  bool m_wifiWanted = false;
  bool m_reassoc = false;   ///< task should (re)run WiFi.begin with fresh creds

  char m_host[AUTOTLM_NET_HOST_LEN] = "";
  char m_path[AUTOTLM_NET_PATH_LEN] = "/";
  char m_token[AUTOTLM_NET_TOKEN_LEN] = "";
  uint16_t m_port = 80;
  uint32_t m_intervalMs = 1000;
  bool m_cloudWanted = false;
  bool m_pushNow = false;  ///< one-shot out-of-cycle push request (mutex-guarded)

  // validate-and-rollback: staged creds the task tries while keeping m_ssid/
  // m_pass as the fallback (all mutex-guarded on write; see tryWifi).
  char m_stagedSsid[AUTOTLM_NET_SSID_LEN] = "";
  char m_stagedPass[AUTOTLM_NET_PASS_LEN] = "";
  bool m_validateReq = false;
  volatile int m_wifiChange = AUTOTLM_WIFI_IDLE;   ///< AUTOTLM_WIFI_*
  volatile int m_changeReason = 0;                 ///< AUTOTLM_WIFI_REASON_* (set with REVERTED)
  volatile uint32_t m_changeGen = 0;               ///< bumped per tryWifi(); tags the terminal state
  volatile bool m_radioBusy = false;               ///< a BLE scan owns the radio; pause reconnect
  volatile uint8_t m_discReason = 0;               ///< last WiFi disconnect reason during validation
  bool m_discEvtRegistered = false;
  volatile uint32_t m_lastAssocMs = 0;             ///< millis() of last association

  AutoTLMFrameProvider m_provider = nullptr;
  AutoTLMDiagSaver m_diagSaver = nullptr;
  void* m_ctx = nullptr;

  // ---- live stats (written only by the task; plain reads elsewhere) ----
  volatile uint32_t m_pushOk = 0, m_pushFail = 0, m_wifiDrops = 0;
  volatile int m_lastHttp = 0;
  volatile uint32_t m_lastPushMs = 0;

  bool m_verbose = true;
  Stream* m_log = &Serial;

  // Offline-buffer bookkeeping (readable on every platform; the buffer
  // itself only exists on ESP32).
  volatile uint16_t m_bufCount = 0;
  uint8_t m_bufWantCap = AUTOTLM_NET_BUFFER_FRAMES;

#if defined(ESP32)
  SemaphoreHandle_t m_cfgMutex = nullptr;
  TaskHandle_t m_task = nullptr;
  IPAddress m_ip;             // task-owned DNS cache
  bool m_haveIp = false;      // (cloud() invalidates it under the mutex)
  uint32_t m_ipAt = 0;
  AutoTLMFrame m_snapshot;     // task-owned copy (never touched by core 1)
  char m_json[3072];           // sized for wide-PID frames (+supported/freeze)
  char m_one[3072];            // per-ring-frame serialize buffer (off the task stack)

  // ---- offline buffer + batching (all task-owned) ----
  AutoTLMFrame* m_buf = nullptr;  ///< lazy ring buffer
  uint8_t m_bufCap = 0;
  uint8_t m_bufHead = 0;          ///< next write slot
  char* m_batch = nullptr;        ///< lazy batch TX buffer
  uint32_t m_backoffUntil = 0;    ///< 429 rate-limit backoff deadline
  uint8_t m_backoffN = 0;

  bool m_validating = false;      ///< task-owned: a tryWifi attempt is live
  uint32_t m_validateStart = 0;
#endif
};

#endif // AUTOTLM_NET_H
