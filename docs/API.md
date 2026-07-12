# AutoTLM Core — API reference

Complete public API of AutoTLM Core v0.3.0: **~130 public functions across
the facade, 7 modules, the HAL interface and 2 helpers.** This file is the
source of truth for the website/wiki — field names, units and defaults here
match the code exactly.

**Units policy:** SI end to end — km/h, °C, kPa, g, deg/s. Dashboards convert
for display; the portal's metric/imperial choice is stored for them
(`car.config().units(...)`) but never changes the data.

---

## `AutoTLM` — the facade (25)

One object that owns the board HAL, all modules, the live telemetry frame and
the status LED. Beginner API lives here; power users reach through
`obd()`/`gnss()`/`imu()`/`net()`/`config()`/`provisioning()`.

| Function | What it does |
|---|---|
| `bool begin()` | Bring the unit up on the auto-selected board (the AutoTLM One). GNSS + IMU start now; the ECU connection is deliberately lazy so a stalled car bus never blocks connectivity. |
| `bool begin(AutoTLMHAL& hal)` | Same, on your own board implementation. |
| `bool provision()` | The zero-code onboarding line. Provisioned unit → applies saved WiFi + cloud and returns `true`. Fresh unit → raises the captive setup portal and returns `false` (keep calling `update()`; the unit reboots itself after the form is saved). |
| `bool beginPortal(apName = nullptr, apPass = nullptr)` | Force the setup portal up now (the "reconfigure me" path — wire to a button hold). Default AP name `AutoTLM-XXXX`, open network. |
| `AutoTLMProvision& provisioning()` | The provisioning module (below). |
| `void wifi(ssid, pass)` | Connect to WiFi (non-blocking; a core-0 task reconnects forever). Credentials persist to flash; pass `nullptr` to reuse saved ones. |
| `void cloud(url, token, intervalMs = 1000)` | Stream frames to an HTTP ingest endpoint. Plain HTTP by design (TLS stalls on weak cellular); `token` goes out as `Authorization: Bearer`. |
| `bool pushNow()` | One out-of-cycle push, now — event uploads (new DTC, script `push`). Thread-safe flag serviced by the network task; fires on reconnect if WiFi is down. |
| `void update()` | The heartbeat — call every `loop()`. Pumps GNSS/IMU/OBD (self-throttling), the portal while active, the frame and the LED. |
| `AutoTLMGPS gps()` | Latest GNSS snapshot (struct below). |
| `AutoTLMMotion motion()` | Latest IMU snapshot (struct below). |
| `void onDTC(cb)` | Callback fired once per newly-appearing trouble code (`"P0171"`) — or pass a `(code, moduleId)` callback to also learn WHICH module stored it. |
| `int modules()` | How many diagnosable modules (ECUs) the car has — engine, transmission, ABS... Details via `car.obd().module(i)`. 0 = single-module view. |
| `AutoTLMOBD& obd()` / `AutoTLMGNSS& gnss()` / `AutoTLMIMU& imu()` / `AutoTLMNet& net()` / `AutoTLMConfig& config()` | Module access (5 functions). |
| `AutoTLMFrame frame()` | Coherent copy of the current telemetry frame (what the cloud receives). |
| `void deviceId(id)` | Override the unit id in telemetry (default: chip id). |
| `void printDiagnostics(out = Serial)` | Previous-session + live health counters. |
| `uint32_t maxLoopUs()` | Worst `update()` duration seen, µs (bus-blocking spikes show here). |
| `void statusLed(enabled)` | Enable/disable the LED convention: fast blink = no WiFi · slow blink = WiFi but not pushing · brief pulse = frame pushed · off = networking unused. |
| `void setLogStream(s)` | Quiet or redirect all library logging (`nullptr` = silent). |

---

## `car.obd()` — OBD-II: PIDs, DTCs, VIN (29)

Lazy init (never blocks startup; retries every 10 s in the background),
tiered polling (5 headline gauge PIDs every cycle + round-robin through
everything the ECU supports), DTCs read every 20 s.

| Function | What it does |
|---|---|
| `bool connected()` | True once the ECU is answering. |
| `int rpm()` | Engine RPM. |
| `int speedKph()` | Vehicle speed, km/h. |
| `int coolantC()` | Coolant temperature, °C. |
| `int loadPct()` | Calculated engine load, %. |
| `int throttlePct()` | Throttle position, %. |
| `float volts()` | Battery voltage (board sense, or PID 0x42 fallback). |
| `const char* vin()` | Vehicle identification number (`""` until read). |
| `bool hasPid(pid)` | Has a value for this mode-01 PID been read? |
| `int pidValue(pid)` | Latest normalized value for any PID (units per `AutoTLMPids.h`). |
| `int supportedCount()` | How many PIDs the car declared support for. |
| `int dtcCount()` / `const char* dtcAt(i)` | Stored trouble codes, e.g. `"P0171"` — on multi-module cars, the UNION across all modules. |
| `bool mil()` | Check-engine light (inferred: any code stored). |
| `const char* dtcString()` | All codes comma-joined: `"P0171,P0420"`. |
| `void clearDTCs()` | Clear stored codes / the MIL (mode 04). |
| `void onDTC(cb)` | New-code callback (same as the facade shortcut); module-aware `(code, moduleId)` overload available. |
| `int moduleCount()` | Diagnosable modules that answered enumeration (up to 8 per ISO 15765-4). |
| `AutoTLMModuleInfo module(i)` | Module i: `{id, stored, pending, permanent}` — CAN responder id + per-kind DTC counts. |
| `const char* moduleDtcAt(i, j)` | Module i's stored code j (`"C0035"`, `"U0101"`...). |
| `const char* modulePendingAt(i, j)` / `modulePermanentAt(i, j)` | Same for mode-07 pending and mode-0A permanent codes (2 functions). |
| `void setPollInterval(ms)` | PID polling cadence (default 300 ms). |
| `void setDtcInterval(ms)` | DTC read cadence (default 20 s). |
| `bool everConnected()` | Did the ECU answer at any point this session? |
| `void setLogStream(s)` | Module-local log routing. |
| `void begin(hal)` / `void tick()` / `void fillFrame(...)` | Wired by the facade — you don't call these. |

---

## `car.gnss()` — GNSS (6)

NMEA (GGA + RMC) → position/speed/course; every sentence checksum-validated
so serial garbage can't corrupt a fix.

| Function | What it does |
|---|---|
| `AutoTLMGPS data()` | Latest fix snapshot. |
| `bool alive()` | True once one checksum-valid sentence has arrived. |
| `void echoTo(stream)` | Echo raw NMEA bytes to a stream. |
| `void onSentence(cb)` | Get each complete, valid sentence (no CR/LF). |
| `bool begin(hal)` / `void tick()` | Wired by the facade. |

**`AutoTLMGPS`**: `fix` (bool), `lat`/`lng` (double, decimal degrees WGS84),
`altM` (m), `speedKph`, `course` (deg true), `hdop`, `sats`, `ageMs`.

---

## `car.imu()` — motion (5)

| Function | What it does |
|---|---|
| `AutoTLMMotion data()` | Latest sample. |
| `bool available()` | Was a sensor found at begin()? |
| `void setSampleInterval(ms)` | Sampling cadence (default 200 ms / 5 Hz). |
| `bool begin(hal)` / `void tick()` | Wired by the facade. |

**`AutoTLMMotion`**: `valid` (bool), `ax ay az` (g), `gx gy gz` (deg/s).

---

## `car.net()` — WiFi + cloud push (13)

Runs on its own CPU core (core 0) so blocking OBD reads can never starve
uploads. Plain HTTP + cached DNS + fresh connection per push — the recipe
that survives weak cellular.

| Function | What it does |
|---|---|
| `void wifi(ssid, pass)` / `void cloud(url, token, intervalMs)` | Same as the facade calls (which persist too — prefer those). |
| `bool pushNow()` | The out-of-cycle push request behind `car.pushNow()`. |
| `bool wifiConnected()` | WiFi associated? |
| `int rssi()` | Signal, dBm (0 when offline). |
| `AutoTLMNetState state()` | `AUTOTLM_NET_DISABLED / OFFLINE / NO_PUSH / STREAMING` — what the LED shows. |
| `uint32_t pushOk()` / `uint32_t pushFail()` | Push counters. |
| `int lastHttp()` | Last HTTP status (−1 connect fail, −2 no response, −3 oversized header). |
| `uint32_t wifiDrops()` | Reconnect attempts. |
| `uint32_t lastPushMs()` | `millis()` of the last 200 OK. |
| `void setVerbose(on)` | PUSH/DIAG log lines (default on — field-debugging gold). |
| `void setLogStream(s)` | Module-local log routing. |

---

## `car.provisioning()` — the setup portal (7)

Captive portal on softAP `AutoTLM-XXXX`: WiFi scan + credentials, cloud
URL/token/rate, GPS on/off, metric/imperial. Saves to NVS, reboots into
normal operation. Normally driven entirely by `car.provision()`.

| Function | What it does |
|---|---|
| `bool start(apName = nullptr, apPass = nullptr)` | Raise the portal (what `beginPortal()` calls). |
| `bool active()` | Portal currently up? |
| `bool saved()` | Has the user submitted the form (settings in NVS)? |
| `void setRestartOnSave(on)` | Reboot after save (default true — cleanest radio handover). |
| `const char* apName()` | The AP SSID in use. |
| `void stop()` | Tear the portal down. |
| `void tick()` | Pumped by `car.update()`. |

---

## `car.config()` — persisted settings + diagnostics (17)

NVS-backed; every method is a safe no-op on platforms without NVS.

| Function | What it does |
|---|---|
| `void saveWifi(ssid, pass)` / `bool loadWifi(...)` / `bool hasWifi()` | WiFi credentials (survive reflash). `hasWifi()` is the "is this unit provisioned?" test. |
| `void saveCloud(url, token, intervalMs)` / `bool loadCloud(...)` | Cloud endpoint. |
| `void saveGpsEnabled(on)` / `bool gpsEnabled()` | Portal GPS switch (default on). |
| `void saveUnits(units)` / `size_t units(out, cap)` | `"metric"` / `"imperial"` display preference (default metric). |
| `const AutoTLMDiag& prevSession()` | Counters from the previous drive (`pushOk, pushFail, lastHttp, wifiDrops, obdEver, maxLoopUs, boots`). |
| `void printPrevSession(out)` | Print them (done at boot). |
| `void saveDiag(d)` | Persist current counters (the net task does this every 20 s). |
| `putString / getString / putInt / getInt` | Small key/value store for your own sketch (keys ≤ 15 chars). |
| `bool begin()` | Wired by the facade. |

---

## `AutoTLMFrame` — the telemetry frame (2 + 35 fields)

Plain-old-data snapshot of everything the device knows; trivially copyable
across cores. `clear()` resets to "unknown"; `toJson(buf, cap)` serializes to
the ingest contract — **field names are stable**, dashboards consume them
as-is:

`device.{id,type,mems,fw_gnss,rssi,modules}` · `obd.{connected,speed_kph,rpm,
coolant_c,load_pct,throttle_pct,volts,vin,pids{...}}` · `dtc.{mil,codes[]}` ·
`gps.{fix,lat,lng,alt_m,speed_kph,course,sats,hdop}` · `imu.{ax,ay,az,gx,gy,gz}`

PID map keys are uppercase mode-01 hex; values are normalized integers.

---

## `AutoTLMHAL` — porting to new hardware (24 virtuals)

Subclass this + pass to `car.begin(yourHal)`; nothing else in the library
knows what board it's on. Anything the board lacks can honestly return
false — modules treat that as "not fitted" and carry on.

Required: `begin, boardId, obdInit, obdReadPID, obdIsPIDSupported,
obdReadDTC, gnssBegin, gnssAvailable, gnssRead, imuBegin, imuRead`.
Optional: `deviceType, obdEnd, obdClearDTC, obdVIN, obdBatteryVoltage,
obdEnumerate, obdReadDTCFrom, canAvailable, canRead, canWrite, gnssPower,
imuName, led` (+ virtual dtor). `obdEnumerate`/`obdReadDTCFrom` power the
multi-module view: one functional probe collects every responder
(0x7E8..0x7EF), then stored/pending/permanent DTCs are read per module by
physical addressing.

Shipping boards: **`BoardAutoTLMOne`** (the product: from-scratch ISO 15765-4
/ ISO-TP over TWAI @ 500 kbps 11-bit, multi-frame VIN/DTC reassembly, NRC-0x78
patience, bus-off recovery; pins from the board package variant or the `Pins`
struct) and `BoardFreematicsOnePlus` (deprecated benchmark, removal planned).
`BoardGenericEsp32` is a deprecated alias of `BoardAutoTLMOne`.

**Raw CAN** for power users, through the same controller as OBD:
`canAvailable() / canRead(msg, timeoutMs) / canWrite(msg)` with
`AutoTLMCanMsg {id, extended, len, data[8]}`.

---

## Helpers — `namespace autotlm` (2)

| Function | What it does |
|---|---|
| `int normalizePid(pid, A, B)` | Raw OBD bytes → the shared integer conventions (RPM = raw/4, temps = A−40 °C, percents = A·100/255, voltage = raw/1000...). Same math on every board, so a frame value means the same thing everywhere. |
| `void formatDTC(code, buf)` | 16-bit DTC → `"P0171"` string form. |

Plus the full set of `PID_*` constants (mode-01 names → hex) in
`AutoTLMPids.h`.

---

## Function count by area

| Area | Public functions |
|---|---|
| `AutoTLM` facade | 25 |
| OBD | 29 |
| GNSS | 6 |
| IMU | 5 |
| Net | 13 |
| Provisioning | 7 |
| Config | 17 |
| Frame | 2 (+35 documented fields) |
| HAL interface | 24 virtuals |
| Helpers | 2 |
| **Total** | **≈130** |
