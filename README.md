# AutoTLM Core

**The library for the [AutoTLM One](https://autotlm.com) — read a car and
push telemetry in a few lines.**

Select the **AutoTLM One** board, install **AutoTLM Core**, write your sketch.
That's the whole platform thesis: car telemetry without the discouraging
parts. OBD-II PIDs, trouble codes and VIN; GNSS; an IMU; browser-based
first-boot setup; and a road-proven cloud push — behind one clean facade.

```cpp
#include <AutoTLM.h>
AutoTLM car;

void setup() {
  car.begin();       // OBD + GNSS + IMU up
  car.provision();   // saved settings? go. fresh unit? phone-based setup portal.
  car.onDTC([](const char* code){ Serial.println(code); });
}

void loop() {
  car.update();                                 // pump sensors + push
  if (car.obd().connected()) Serial.println(car.obd().rpm());
  AutoTLMGPS g = car.gps();
  if (g.fix) Serial.printf("%.5f, %.5f\n", g.lat, g.lng);
}
```

MIT licensed. Works in the **Arduino IDE** and **PlatformIO**.

## Getting started (three steps)

**1. Get the AutoTLM One board into your IDE.**
File → Preferences → *Additional boards manager URLs*, add:

```
https://raw.githubusercontent.com/AcidAlchamy/autotlm-core/master/board-package/package_autotlm_index.json
```

then in **Boards Manager** install **esp32** (by Espressif, 3.x) and
**AutoTLM Boards**, and pick **Tools → Board → AutoTLM One**.
(FQBN for the CLI: `autotlm:esp32:one`. Details: [board-package/](board-package/).)

**2. Install AutoTLM Core** — Library Manager ("AutoTLM"), or clone this repo
into your sketchbook's `libraries/` folder.

**3. Flash an example.** `File → Examples → AutoTLM → 01_HelloCar`, plug the
unit into the OBD-II port, open the Serial Monitor @115200, turn the key.

No pin tables, no board defines, no third-party libraries: select the board
and AutoTLM Core speaks ISO 15765-4 itself, including ISO-TP multi-frame
for VIN and long DTC lists.

## First-boot provisioning (no code edits)

`car.provision()` is the onboarding: on a fresh unit it raises a WiFi access
point (**AutoTLM-XXXX**). Join it from your phone — the setup page opens by
itself:

- pick your **WiFi network** (live scan) and password,
- paste your **cloud ingest URL + token**, choose the push rate,
- set **GPS** on/off and **display units** (metric / imperial).

Save → the unit reboots and streams. Settings live in flash, survive
reflashing, and every later boot `car.provision()` applies them silently.
Re-provision any time with `car.beginPortal()` (wire it to a button hold).
Prefer hardcoding? `car.wifi(ssid, pass)` + `car.cloud(url, token)` still
work exactly as before — see `03_PushToCloud`.

## Examples

| Sketch | Shows |
|---|---|
| `01_HelloCar` | RPM + coolant + speed on the Serial Monitor — the AutoTLM "blink". |
| `02_GpsToSerial` | Streaming a GNSS fix (with optional raw-NMEA echo). |
| `03_PushToCloud` | The full unit with hardcoded credentials: OBD + GNSS + IMU → JSON → your HTTP ingest, 1/s. |
| `04_ReadDTCs` | Decoding the check-engine light, new-code callback, clearing codes. |
| `05_Provisioning` | The zero-code path: captive-portal setup on first boot, saved settings forever after. |
| `06_LiveGauges` | RPM/speed/coolant/throttle as live bar gauges redrawing in a serial terminal. |
| `07_ModuleScan` | Every module (ECU) in the car like a scan tool: per-module stored/pending/permanent codes. |

## Why it's built the way it is

Every "opinion" in this library earned its place on the road, in a real car,
on a real LTE hotspot:

| Design choice | Why |
|---|---|
| **Plain HTTP push** (TLS refused) | The TLS handshake stalls for seconds on weak cellular; plain HTTP completes in <1 s on the same signal. A bearer token still authenticates every POST. |
| **Cached DNS** (5-min TTL, re-resolve on failure) | DNS is the flakiest step of a push over cellular. |
| **Fresh connection per push**, `Connection: close` | Keep-alive through CDN edges caused multi-second stalls. |
| **Networking on its own CPU core** | Blocking OBD reads (core 1) can never starve uploads (core 0), and vice versa. |
| **Lazy OBD init** | Talking to a car bus can stall forever (ignition off, no car). The ECU connection is retried in the background and never blocks WiFi/GNSS/push startup. |
| **Tiered PID polling** | Five headline gauge PIDs every cycle + round-robin through everything the ECU supports; DTCs read rarely (slow). |
| **Persistent diagnostics** (NVS) | Push/WiFi/OBD counters survive the drive, so a failure in the field is readable over USB back at the desk. |
| **Status LED convention** | Fast blink = no WiFi · slow blink = WiFi but not pushing · brief pulse = streaming. Diagnosis from across the garage. |

## Hardware

**The target is the AutoTLM One.** Select it in the IDE and `car.begin()`
brings up the right hardware layer on its own. What the library speaks for
you: OBD-II over CAN (native ISO 15765-4 @ 500 kbps 11-bit, ISO-TP
multi-frame, bus-off recovery), GNSS (checksum-validated NMEA), motion
(3-axis accel + gyro), raw CAN for power users, and the status LED.

Pin assignments are overridable via `BoardAutoTLMOne::Pins` if you need them.
Completely custom hardware: subclass `AutoTLMHAL` (one small interface: OBD
transport, GNSS bytes, IMU, LED) and pass it to `car.begin(yourHal)` —
nothing else in the library knows what board it's on.

> **Deprecated:** the Freematics ONE+ compatibility HAL
> (`AUTOTLM_BOARD_FREEMATICS_ONEPLUS`) still compiles if you install the
> patched FreematicsPlus library, but it was only ever a benchmark and will
> be removed. `BoardGenericEsp32` is now an alias of `BoardAutoTLMOne`.

## arduino-cli

```
arduino-cli compile --fqbn autotlm:esp32:one --library /path/to/autotlm-core examples/01_HelloCar
```

## API overview

**Full reference — every function, with units and defaults: [docs/API.md](docs/API.md).**
The short version:

**Facade (`AutoTLM car`)** — `begin()`, `begin(hal)`, `provision()`,
`beginPortal()`, `wifi(ssid, pass)`, `cloud(url, token, intervalMs)`, `pushNow()`,
`update()`, `gps()`, `motion()`, `onDTC(cb)`, `frame()`, `deviceId(id)`,
`printDiagnostics()`, `statusLed(on)`, `setLogStream(s)`.

**OBD (`car.obd()`)** — `connected()`, `rpm()`, `speedKph()`, `coolantC()`,
`loadPct()`, `throttlePct()`, `volts()`, `vin()`, `hasPid(pid)`,
`pidValue(pid)`, `supportedCount()`, `dtcCount()`, `dtcAt(i)`, `mil()`,
`clearDTCs()`, `setPollInterval(ms)` — plus the multi-module view:
`moduleCount()`, `module(i)`, `moduleDtcAt(i, j)`, `modulePendingAt(i, j)`,
`modulePermanentAt(i, j)`. A real car is several computers (engine,
transmission, ABS, airbag...); AutoTLM enumerates them automatically and
reads each module's stored/pending/permanent codes — `car.modules()` for the
count, and `onDTC` accepts a `(code, moduleId)` callback.

**GNSS (`car.gnss()`)** — `data()` → `AutoTLMGPS {fix, lat, lng, altM,
speedKph, course, hdop, sats, ageMs}`, `alive()`, `echoTo(stream)`,
`onSentence(cb)`.

**IMU (`car.imu()`)** — `data()` → `AutoTLMMotion {ax..az (g), gx..gz
(deg/s)}`, `available()`, `setSampleInterval(ms)`.

**Net (`car.net()`)** — `wifiConnected()`, `rssi()`, `state()`, `pushOk()`,
`pushFail()`, `lastHttp()`, `wifiDrops()`, `setVerbose(on)`.

**Provisioning (`car.provisioning()`)** — `active()`, `saved()`, `apName()`,
`setRestartOnSave(on)`, `stop()`.

**Config (`car.config()`)** — persisted WiFi/cloud settings, `gpsEnabled()`,
`units(buf, cap)`, session diagnostics, and a small key/value store for your
own sketch (`putString`/`getString`, `putInt`/`getInt`).

Values use SI units end to end: km/h, °C, kPa, g/s. Dashboards convert for
display — the portal's units choice is stored for them (and for your sketch
via `car.config().units(...)`); the frame itself stays SI.

## The telemetry frame

`car.frame().toJson(...)` — and the cloud push — produce the AutoTLM frame
format (every field the dashboards understand):

```json
{"source":"device",
 "device":{"id":"A6445000","type":"one","mems":"MPU-6050","fw_gnss":"OK","rssi":-51,"modules":2},
 "obd":{"connected":true,"speed_kph":58,"rpm":1840,"coolant_c":88,"load_pct":23,
        "throttle_pct":14,"volts":14.2,"vin":"YV0EXAMPLE0000000",
        "pids":{"04":23,"05":88,"0C":1840,"0D":58,"11":14}},
 "dtc":{"mil":true,"codes":["P0171"]},
 "gps":{"fix":true,"lat":36.114647,"lng":-115.172813,"alt_m":610.0,
        "speed_kph":57.9,"course":271,"sats":12,"hdop":0.8},
 "imu":{"ax":0.02,"ay":-0.11,"az":1.00,"gx":0.4,"gy":-0.2,"gz":0.1}}
```

Field names are stable — AutoTLM Dash and AutoTLM Cloud consume them as-is.
PID map keys are uppercase mode-01 hex; values are normalized integers
(RPM in rpm, temps in °C, percents 0–100 — see `src/core/AutoTLMPids.h`).

## Roadmap

- AutoTLM One production hardware (same sketches, no changes)
- TLMscript on top of this API
- OTA updates via AutoTLM Cloud

## License

MIT — see [LICENSE](LICENSE).
