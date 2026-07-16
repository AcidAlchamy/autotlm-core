# AutoTLM Core — API reference

Complete public API of AutoTLM Core v0.7.0: **~160 public functions across
the facade, 8 modules, the HAL interface and 3 helpers.** This file is the
source of truth for the website/wiki — field names, units and defaults here
match the code exactly.

**Units policy:** SI end to end — km/h, °C, kPa, g, deg/s. Dashboards convert
for display; the portal's metric/imperial choice is stored for them
(`car.config().units(...)`) but never changes the data.

---

## `AutoTLM` — the facade (28)

One object that owns the board HAL, all modules, the live telemetry frame and
the status LED. Beginner API lives here; power users reach through
`obd()`/`gnss()`/`imu()`/`net()`/`config()`/`provisioning()`.

| Function | What it does |
|---|---|
| `bool begin()` | Bring the unit up on the auto-selected board (the AutoTLM One). GNSS + IMU start now; the ECU connection is deliberately lazy so a stalled car bus never blocks connectivity. |
| `bool begin(AutoTLMHAL& hal)` | Same, on your own board implementation. |
| `bool provision()` | The zero-code onboarding line. Provisioned unit → applies saved WiFi + cloud and returns `true`. Fresh unit → raises the captive setup portal and returns `false` (keep calling `update()`; the unit reboots itself after the form is saved). |
| `bool beginPortal(apName = nullptr, apPass = nullptr)` | Force the setup portal up now (the "reconfigure me" path — wire to a button hold). Default AP name `AutoTLM-XXXX`, secured WPA2 with the per-device password. |
| `bool changeWifi(ssid, pass)` | Change WiFi SAFELY — validate-and-rollback. Tries the new network while keeping the current one; reverts if it can't associate in ~30 s (a wrong password never strands the unit). Non-blocking; persists the new creds on success. Poll `wifiChangeState()`. |
| `int wifiChangeState()` | Live WiFi-change state: `AUTOTLM_WIFI_IDLE`/`VALIDATING`/`OK`/`REVERTED`. |
| `int wifiChangeReason()` | Why the last change REVERTED: 1 = SSID not found, 2 = auth failed, 3 = timeout/other (render 3 as "couldn't connect", never "wrong password"). 0 otherwise. |
| `void onWifiChange(cb, ctx)` | RACE-FREE change observation: `cb(state, reason, ctx)` fires from inside `update()` on every transition, including `OK`/`REVERTED` *before* the facade consumes them — a poll of `wifiChangeState()` can miss a terminal state; this can't. |
| `bool bleBegin()` | Bring up the BLE change-WiFi service (contract below). Does NOT advertise. |
| `void bleAdvertise(on)` | Start/stop BLE advertising — WHEN to be discoverable is YOUR policy (recommended: unprovisioned / post-power-on window / SETUP press / WiFi lost; dark while streaming, so a driving car is never a followable beacon). Implies `bleBegin()`. |
| `AutoTLMBle& ble()` | The BLE module (state, advertising flag). |
| `void setReprovisionOnLostWifi(on, afterMs = 120000)` | Provisioned unit offline for `afterMs` → re-raise the (WPA2) setup AP so a phone can re-pair it, no button/cable. Opt-in; conservative (a brief dead zone won't trigger it). |
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

## `car.obd()` — OBD-II: PIDs, DTCs, VIN, freeze frames (35)

Lazy init (never blocks startup; retries every 10 s in the background),
tiered polling (5 headline gauge PIDs every cycle + round-robin through
everything the ECU supports), DTCs + the freeze frame read every 20 s.
Hardened against bus corruption: an implausible VIN (wrong length or
charset) is rejected in favor of the last good one, and a mid-session
re-discovery can only ADD to the supported-PID sets, never shrink them
(a fresh boot — or a different car's VIN — still starts clean).

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
| `int pidValue(pid)` | Latest normalized value for any PID (units per `AutoTLMPids.h`; fixed-point ×10^`pidDecimals(pid)` for trims/O2/λ/timing). |
| `int supportedCount()` | How many PIDs are being polled (advertised ∩ decodable). |
| `int advertisedCount()` | How many PIDs the car ADVERTISES via its bitmasks (the frame's `obd.supported`). |
| `uint8_t advertisedAt(i)` | Advertised PID i, sorted ascending (0 out of range). |
| `const char* freezeCode()` | The DTC the stored freeze frame belongs to (`""` = none). |
| `int freezeCount()` / `uint8_t freezePidAt(i)` / `int freezeValAt(i)` | The freeze-frame PID snapshot (3 functions; same normalization as live PIDs). |
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

## `car.net()` — WiFi + cloud push (19)

Runs on its own CPU core (core 0) so blocking OBD reads can never starve
uploads. Plain HTTP + cached DNS + fresh connection per push — the recipe
that survives weak cellular.

| Function | What it does |
|---|---|
| `void wifi(ssid, pass)` / `void cloud(url, token, intervalMs)` | Same as the facade calls (which persist too — prefer those). |
| `bool pushNow()` | The out-of-cycle push request behind `car.pushNow()`. |
| `void setBufferFrames(n)` | Offline buffer size (default 24 frames, 0 = off; heap-allocated on first use). While WiFi is down (or the server rate-limits), one frame per push interval is captured; on reconnect they go up as ONE batched POST (array, ≤50 per the ingest contract), oldest first. Overflow drops oldest. |
| `uint16_t buffered()` | Frames currently waiting in the offline buffer. |
| `void tryWifi(ssid, pass)` | Validate-and-rollback primitive behind `car.changeWifi()` — stage new creds, keep the old, revert on failure. |
| `int wifiChangeState()` / `void clearWifiChange()` | Read / acknowledge the validate-and-rollback result (2 functions). |
| `uint32_t sinceConnectedMs()` | ms since the station was last associated (0 while connected) — the offline-reprovision policy's "is this network really gone?" signal. |
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

## `car.provisioning()` — the setup portal (9)

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
| `const char* apPassword()` | The AP's WPA2 password ("" if open). |
| `bool startAlongsideStation(...)` | Raise the portal in AP+STA mode (keeps the station link up) — for a live re-pair AP without dropping the current connection. |
| `void stop()` | Tear the portal down. |
| `void tick()` | Pumped by `car.update()`. |

---

## `car.ble()` — the BLE change-WiFi service (6)

The owner-priority "change WiFi from the phone" feature. GATT service
`a0817000-7a1c-4c9a-9d10-000000000001`; the unit advertises the service UUID
with **service data = the exact 8-char `device.id`** (match it against the
owned ids from Cloud's `/api/auth/me`; the id is an identifier, not a
credential) and local name `AutoTLM-XXXX` (humans only — don't match on it).

Security: **LE Secure Connections, Just Works, bonded** (the unit has no
display; iOS shows its system pairing prompt once). Just Works has no MITM
protection, so encryption alone is **not** treated as "the owner" —
**possession proof gates every privileged operation.** The phone first
unlocks the session with the unit's 8-char setup code (the label /
WPA2-portal password): `{"op":"auth","code":"XXXXXXXX"}`. Only then are scans
honored and results populated, and only then is a creds write acted on. The
check happens inside the library (`config().apPassword()`); firmware never
handles the code, the code is never stored server-side (Cloud's ruling — a
phone may cache it locally), and auth is **per-connection** (cleared on
disconnect).

| Characteristic (`…0002`–`…0005`) | Access | Payload |
|---|---|---|
| CTRL `…0002` | write (encrypted) | `{"op":"auth","code":"XXXXXXXX"}` unlocks the session; `{"op":"scan"}` then starts an async SSID scan (refused until authed) |
| SCAN `…0003` | read/notify (enc) | `{"seq":N,"nets":[{"ssid":"…","rssi":-52,"sec":true},…]}` — top 10 by RSSI, deduped, ≤512 B. **Notify is a freshness signal only** (Bluedroid truncates notifications to MTU−3); the app must **read** the characteristic (long read) for the full list, and use `seq` to detect a refresh that landed mid-read. Empty/`"[]"` until the session is authed. |
| CREDS `…0004` | write (encrypted) | `{"code":"XXXXXXXX","ssid":"…","pass":"…"}` |
| STATUS `…0005` | read/notify (enc) | 2 bytes `[state, detail]` — see below |

STATUS states: `0` idle · `1` creds_received · `2` testing · `3` connected
(new network persisted — done) · `4` reverted · `5` busy (a change was
already validating — retry after it settles). `detail` is nonzero only for
state 4: `1` SSID not found · `2` auth failed (wrong password) · `3`
timeout/other (render as "couldn't connect", never "wrong password") · `4`
rejected (bad/absent setup code) · `5` no SSID in the creds write. A terminal
state **holds until the next CREDS write _or_ any other WiFi change** (a
USB/sketch `changeWifi()` also drives STATUS, so the app always reflects
reality). The characteristic feeds off `onWifiChange()` from a single
coherent state snapshot, so it is race-free by construction.

| Function | What it does |
|---|---|
| `bool begin(car, deviceId)` | Facade-wired via `car.bleBegin()` — service up, advertising off. |
| `void advertise(on)` / `bool advertising()` | Discoverability switch + its state (policy is the firmware's). |
| `bool active()` | Service initialized? |
| `int state()` | Current STATUS state byte. |
| `void tick()` / `feedWifiChange(...)` | Pumped/fed by the facade — you don't call these. |

Flash note: BLE + WiFi need more than the classic 1.3 MB app partition — the
AutoTLM One's standard table (1.9 MB OTA slots, boards ≥ 0.3.0) fits it;
generic ESP32 boards need a big-app scheme (example 08's header says so).

**Compiled in only when `AUTOTLM_ENABLE_BLE`.** To keep lean telemetry
sketches from linking the ~0.5 MB Bluedroid stack, BLE defaults **on for the
AutoTLM One** (its OTA partition has room) and **off for a generic ESP32**.
Force it either way by defining `AUTOTLM_ENABLE_BLE 1` (or `0`) before
`#include <AutoTLM.h>`. When it's off, `car.bleBegin()` is a no-op returning
`false` and `car.ble()` isn't declared.

---

## `car.config()` — persisted settings + diagnostics (18)

NVS-backed; every method is a safe no-op on platforms without NVS.

| Function | What it does |
|---|---|
| `void saveWifi(ssid, pass)` / `bool loadWifi(...)` / `bool hasWifi()` | WiFi credentials (survive reflash). `hasWifi()` is the "is this unit provisioned?" test. |
| `void saveCloud(url, token, intervalMs)` / `bool loadCloud(...)` | Cloud endpoint. |
| `void saveGpsEnabled(on)` / `bool gpsEnabled()` | Portal GPS switch (default on). |
| `void saveUnits(units)` / `size_t units(out, cap)` | `"metric"` / `"imperial"` display preference (default metric). |
| `size_t apPassword(out, cap)` | The device's per-unit provisioning-AP WPA2 password (8 chars, derived from the chip id — stable, unique, label-printable). |
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

`device.{id,type,mems,fw_gnss,rssi,modules}` · `age_ms` ·
`obd.{connected,speed_kph,rpm,coolant_c,load_pct,throttle_pct,volts,vin,
pids{...},supported[]}` · `dtc.{mil,codes[],freeze{...}}` ·
`gps.{fix,source,lat,lng,alt_m,speed_kph,course,sats,hdop}` ·
`imu.{ax,ay,az,gx,gy,gz}`

**Absent sub-objects are OMITTED, never zero-filled** (the AutoTLM Cloud
ingest contract): no ECU answering → no `obd`, no fix → no `gps`, no IMU →
no `imu`, no codes and no MIL → no `dtc`, no freeze frame → no `dtc.freeze`,
live (non-buffered) frame → no `age_ms`. Consumers null-check.

### The v0.6.0 contract additions

- **`obd.supported`** — what THIS car advertises via the supported-PID
  bitmasks (`00/20/40/60/80/A0/C0`): uppercase hex, sorted, deduped, present
  in every frame where `obd` is present. A superset of the keys in `pids`
  (advertised ≠ polled) — sensor pickers grey out the rest.

- **`dtc.freeze`** — mode-02 freeze frame, a map of **code → PID snapshot**.
  Snapshot shape mirrors `obd.pids` (uppercase hex keys, same normalization)
  so decode logic is reused verbatim. Omitted when no freeze data; refreshes
  on the DTC cadence (~20 s). v1 carries the ECU's stored freeze frame (frame
  0) keyed by the code the ECU attributes it to; the map shape means richer
  per-module freeze data can land later without a contract change:

  ```json
  "dtc": { "mil": true, "codes": ["P0171"],
           "freeze": { "P0171": { "04": 27, "05": 89, "0C": 1320, "0D": 20, "11": 14 } } }
  ```

- **`gps.source`** — location provenance, inside `gps`: `"internal"` whenever
  the device composed a real GNSS fix. `"phone"` is written by AutoTLM Cloud
  when it merges phone GPS (never by the device); no fix keeps its existing
  representation (no `gps` object). Cloud's merge rule keys on it: inject
  phone GPS only when `gps` is absent or `source != "internal"`.

- **`age_ms`** — top-level, on batched offline catch-up frames only:
  milliseconds between frame capture and the POST that carried it (monotonic;
  the device has no RTC). Omitted on live frames (= 0). Ingest reconstructs
  capture time as `receivedAt − age_ms`.

### `pids` values: units and precision

Keys are uppercase mode-01 hex. Values are JSON **numbers**: integers for
most PIDs, fixed-decimal (and signed where the PID is signed) for the rest —
parse as numbers, not ints. The polled set is the car's advertised PIDs ∩
the ~58-PID decode table (trims, narrowband O2, timing, EGR/evap, cat temps,
pedals, torque, rates, ...). Non-integer/signed PIDs:

| PID(s) | Channel | Emitted as |
|---|---|---|
| `06`–`09` | short/long-term fuel trim B1/B2 | %, signed, 1 decimal (−100.0…+99.2) |
| `2D` | EGR error | %, signed, 1 decimal |
| `0E` | timing advance | °, signed, 1 decimal (−64.0…+63.5) |
| `14`–`1B` | narrowband O2 voltage B1S1…B2S4 | V, 3 decimals (0…1.275) |
| `44` | commanded air–fuel equivalence ratio λ | ratio, 2 decimals |
| `32` | evap system vapor pressure | Pa, signed integer |
| `61` / `62` | demanded / actual engine torque | %, signed integer (−125…130) |

Everything else keeps the established integer conventions (RPM in rpm, temps
in °C, percents 0–100, pressures in kPa, voltage `42` in V).

---

## `AutoTLMHAL` — porting to new hardware (26 virtuals)

Subclass this + pass to `car.begin(yourHal)`; nothing else in the library
knows what board it's on. Anything the board lacks can honestly return
false — modules treat that as "not fitted" and carry on.

Required: `begin, boardId, obdInit, obdReadPID, obdIsPIDSupported,
obdReadDTC, gnssBegin, gnssAvailable, gnssRead, imuBegin, imuRead`.
Optional: `deviceType, obdEnd, obdClearDTC, obdVIN, obdFreezeDTC,
obdReadFreezePID, obdBatteryVoltage, obdEnumerate, obdReadDTCFrom,
canAvailable, canRead, canWrite, gnssPower, imuName, led` (+ virtual dtor).
`obdFreezeDTC`/`obdReadFreezePID` power `dtc.freeze` (mode 02, frame 0);
boards that can't read mode 02 simply never emit freeze data. `obdEnumerate`/`obdReadDTCFrom` power the
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

## Helpers — `namespace autotlm` (3)

| Function | What it does |
|---|---|
| `int normalizePid(pid, A, B)` | Raw OBD bytes → the shared conventions (RPM = raw/4, temps = A−40 °C, percents = A·100/255, voltage = raw/1000...). PIDs with `pidDecimals(pid) > 0` store fixed-point ×10^decimals. Same math on every board, so a frame value means the same thing everywhere. |
| `uint8_t pidDecimals(pid)` | Decimal places a PID's stored value carries (trims/EGR err/timing = 1, λ = 2, O2 volts = 3, else 0) — the frame serializer divides them back out. |
| `void formatDTC(code, buf)` | 16-bit DTC → `"P0171"` string form. |

Plus the full set of `PID_*` constants (mode-01 names → hex) in
`AutoTLMPids.h`.

---

## Function count by area

| Area | Public functions |
|---|---|
| `AutoTLM` facade | 35 |
| OBD | 35 |
| BLE | 6 |
| GNSS | 6 |
| IMU | 5 |
| Net | 19 |
| Provisioning | 9 |
| Config | 18 |
| Frame | 2 (+40 documented fields) |
| HAL interface | 26 virtuals |
| Helpers | 3 |
| **Total** | **≈160** |
