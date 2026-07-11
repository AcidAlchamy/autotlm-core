# AutoTLM Core

**Read a car and push telemetry in a few lines.**

AutoTLM Core is the open-source Arduino C++ library at the heart of the
[AutoTLM](https://autotlm.com) car-telemetry platform: OBD-II PIDs, trouble
codes and VIN; GNSS; an IMU; and a road-proven cloud push — behind one clean
facade, on hardware you can buy (or breadboard) today.

```cpp
#include <AutoTLM.h>
AutoTLM car;

void setup() {
  car.begin();                                  // OBD + GNSS + IMU up
  car.wifi("MyHotspot", "password");
  car.cloud("http://yourserver.com/api/ingest", "TOKEN");
  car.onDTC([](const char* code){ Serial.println(code); });
}

void loop() {
  car.update();                                 // pump sensors + push
  if (car.obd().connected()) Serial.println(car.obd().rpm());
  AutoTLMGPS g = car.gps();
  if (g.fix) Serial.printf("%.5f, %.5f\n", g.lat, g.lng);
}
```

Works in the **Arduino IDE** and **PlatformIO**. MIT licensed.

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

## Supported boards

Board selection is one `#define` before the include — sketches are otherwise
identical on every board. AutoTLM runs on **your own hardware**: a plain ESP32
(and, soon, our own AutoTLM One board) is the first-class target.

| Board | Define | What you get |
|---|---|---|
| **Generic ESP32 + CAN transceiver** | `AUTOTLM_BOARD_GENERIC_ESP32` (default on ESP32; what the examples ship with) | The primary target. AutoTLM speaks ISO 15765-4 itself over the ESP32's TWAI controller (500 kbps, 11-bit, ISO-TP multi-frame for VIN/DTCs), GNSS on UART2, optional MPU-6050 IMU. No third-party libraries needed. Pins configurable via `BoardGenericEsp32::Pins`. |
| *AutoTLM One* | *(coming)* | Our own purpose-built OBD-II unit. Same sketches, no changes. |
| **Freematics ONE+** | `AUTOTLM_BOARD_FREEMATICS_ONEPLUS` | A compatibility board and capability benchmark — a commercial ESP32 OBD dongle we support so AutoTLM One can be measured against (and surpass) it. OBD via its co-processor, GNSS (BE-220) on RX 26 @ 38400, ICM-42627 IMU, battery-voltage sense, status LED. Needs the [FreematicsPlus library](https://github.com/stanleyhuangyc/Freematics) — **note:** upstream needs small patches to build on arduino-esp32 3.x (`soc/gpio_struct.h` include; `esp_task_wdt_reconfigure`; the `ledcAttach` API). |

Custom hardware: subclass `AutoTLMHAL` (one small interface: OBD transport,
GNSS bytes, IMU, LED) and pass it to `car.begin(yourHal)`. Nothing else in
the library knows what board it's on — which is exactly how our own boards
plug in.

Generic-board default wiring: CAN TX GPIO5 / RX GPIO4 → SN65HVD230 → CANH pin 6,
CANL pin 14, GND pin 5 of the OBD-II port; GNSS RX 16 / TX 17 @ 9600;
MPU-6050 on SDA 21 / SCL 22; LED GPIO2.

## Install

**Arduino IDE:** clone (or download) this repo into your sketchbook's
`libraries/` folder, restart the IDE.

```
git clone https://github.com/AcidAlchamy/autotlm-core.git ~/Documents/Arduino/libraries/AutoTLM
```

**arduino-cli:**

```
arduino-cli compile --fqbn esp32:esp32:esp32 --library /path/to/autotlm-core examples/01_HelloCar
```

The examples compile as-is for the generic ESP32 board. For the Freematics
ONE+, flip the define at the top of the sketch and also supply the (patched —
see the boards table) FreematicsPlus library:
`--libraries /path/to/Freematics/libraries`.

## Examples

| Sketch | Shows |
|---|---|
| `01_HelloCar` | RPM + coolant + speed on the Serial Monitor — the AutoTLM "blink". |
| `02_GpsToSerial` | Streaming a GNSS fix (with optional raw-NMEA echo). |
| `03_PushToCloud` | The full unit: OBD + GNSS + IMU → JSON frames → your HTTP ingest, 1/s. |
| `04_ReadDTCs` | Decoding the check-engine light, new-code callback, clearing codes. |

## API overview

**Facade (`AutoTLM car`)** — `begin()`, `begin(hal)`, `wifi(ssid, pass)`,
`cloud(url, token, intervalMs)`, `update()`, `gps()`, `motion()`,
`onDTC(cb)`, `frame()`, `deviceId(id)`, `printDiagnostics()`,
`statusLed(on)`, `setLogStream(s)`.

**OBD (`car.obd()`)** — `connected()`, `rpm()`, `speedKph()`, `coolantC()`,
`loadPct()`, `throttlePct()`, `volts()`, `vin()`, `hasPid(pid)`,
`pidValue(pid)`, `supportedCount()`, `dtcCount()`, `dtcAt(i)`, `mil()`,
`clearDTCs()`, `setPollInterval(ms)`.

**GNSS (`car.gnss()`)** — `data()` → `AutoTLMGPS {fix, lat, lng, altM,
speedKph, course, hdop, sats, ageMs}`, `alive()`, `echoTo(stream)`,
`onSentence(cb)`.

**IMU (`car.imu()`)** — `data()` → `AutoTLMMotion {ax..az (g), gx..gz
(deg/s)}`, `available()`, `setSampleInterval(ms)`.

**Net (`car.net()`)** — `wifiConnected()`, `rssi()`, `state()`, `pushOk()`,
`pushFail()`, `lastHttp()`, `wifiDrops()`, `setVerbose(on)`.

**Config (`car.config()`)** — persisted WiFi creds, session diagnostics, and
a small key/value store for your own sketch (`putString`/`getString`,
`putInt`/`getInt`).

Values use SI units end to end: km/h, °C, kPa, g/s. (Dashboards convert for
display; AutoTLM Dash shows mph/°F from the same frames.)

## The telemetry frame

`car.frame().toJson(...)` — and the cloud push — produce the AutoTLM frame
format (every field the dashboards understand):

```json
{"source":"device",
 "device":{"id":"A6445000","type":"16","mems":"ICM-42627","fw_gnss":"OK","rssi":-51},
 "obd":{"connected":true,"speed_kph":58,"rpm":1840,"coolant_c":88,"load_pct":23,
        "throttle_pct":14,"volts":14.2,"vin":"YV0EXAMPLE0000000",
        "pids":{"04":23,"05":88,"0C":1840,"0D":58,"11":14}},
 "dtc":{"mil":true,"codes":["P0171"]},
 "gps":{"fix":true,"lat":36.114647,"lng":-115.172813,"alt_m":610.0,
        "speed_kph":57.9,"course":271,"sats":12,"hdop":0.8},
 "imu":{"ax":0.02,"ay":-0.11,"az":1.00,"gx":0.4,"gy":-0.2,"gz":0.1}}
```

PID map keys are uppercase mode-01 hex; values are normalized integers
(RPM in rpm, temps in °C, percents 0–100 — see `src/core/AutoTLMPids.h`).

## Roadmap

- AutoTLM One board support (same sketches, our hardware)
- Teensy 4.x HAL (FlexCAN) for radio-less CAN tools like the bench ECU emulator
- Captive-portal WiFi provisioning module
- TLMscript on top of this API

## License

MIT — see [LICENSE](LICENSE).
