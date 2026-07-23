# AutoTLM Core — update log

Announce-worthy releases and milestones for the AutoTLM Core library
(github.com/AcidAlchamy/autotlm-core).

## 2026-07-23 — v0.9.1: clearing trouble codes now tells you the truth

- **The device no longer says "cleared" when it wasn't.** Asking the unit to
  clear stored trouble codes (turn the check-engine light off) used to report
  success no matter what the car actually did — even when the car refused (which
  is exactly what a car does when the engine is running) or never answered at
  all. It now reads the car's real response and tells you plainly: cleared,
  refused (with the reason), or no answer from the bus.
- **It only forgets a code the car confirmed it cleared.** A refused or unheard
  clear leaves the stored codes exactly as they were, so the app can never show a
  clean bill of health the unit didn't actually get. On cars with several
  computers each one is asked and answered for independently, and permanent
  emissions codes correctly survive a clear, as the standard requires.
- Related: reading codes now tells "no codes stored" apart from "the bus didn't
  answer," so a car that never replied is never mistaken for a healthy one.
- For builders: `clearDTCs()` returns an `AutoTLMClearResult` (per-responder
  verdict + raw refusal code); `obdReadDTC` is tri-state. See docs/API.md and
  example 04.

## 2026-07-18 — v0.9.0: live gauges straight from the car, and it always finds you

- **Local live data over Bluetooth.** When your phone is at the car, the app
  can now read live gauges *directly from the unit* over Bluetooth — no
  round-trip through the internet — so speed, RPM, temps and the rest update
  instantly and work even with no signal. The cloud stays the source when
  you're away (remote view, history, fleet); local is used when you're near.
- **Your phone can always find its unit.** The device now advertises with a
  private, rotating Bluetooth address that only *your* paired phone can
  recognize — so you can re-open the app and change WiFi (or anything else)
  anytime, without power-cycling the unit first, and a stranger still can't
  track it. Fixes the “can only change WiFi once” / “app can’t see my device”
  dead-ends.
- Under the hood: a new encrypted BLE telemetry characteristic streaming a
  compact live frame (same field names as the cloud frame), resolvable private
  addressing, and a larger BLE transfer size. `car.bleTelemetry(on, hz)`.
- **Hardened before shipping.** A pre-release security review of the live
  stream closed five issues found before any of it reached hardware: the
  telemetry feed is push-only and only ever reaches a proven-you-own-it phone
  (so a merely-paired phone can never read your location), the live frame is
  sized to your phone's actual connection so it always arrives complete, and
  GPS is never quietly dropped from a busy car's frame.

## 2026-07-18 — v0.8.1: no more reboot loop while pushing to the cloud

- **Fixed a crash that rebooted the unit every ~15 seconds** when it was on
  WiFi and trying to reach the cloud (especially with Bluetooth also running).
  The network task could wait too long on a slow or unanswered upload and trip
  the chip's safety timer, forcing a reset. Uploads are now time-boxed well
  under that limit — a slow upload is simply skipped and retried (drives never
  lose data; it buffers), and the unit keeps running instead of rebooting.
  Caught on real hardware the first time a unit ran the full stack — Bluetooth,
  WiFi and cloud push — at once.

## 2026-07-17 — v0.8.0: Bluetooth that shares the chip politely

- **Bluetooth and WiFi now truly coexist.** Real-hardware testing showed the
  previous Bluetooth stack claimed so much working memory that WiFi could fail
  to start alongside it — the worst possible timing for a feature whose whole
  job is switching WiFi networks. The service now runs on a much lighter
  stack, leaving WiFi its room. (The change-WiFi-from-your-phone feature
  returns with this release; the app-facing behavior is unchanged.)
- **Better pairing manners.** The phone pairs on first use exactly the way iOS
  expects, larger transfers happen in one piece, a stalled connection can no
  longer camp the unit unreachable, and a successful setup-code entry is now
  positively acknowledged. Factory-reset flows can forget previously paired
  phones (`ble().clearBonds()`) so re-pairing is never haunted by stale keys.
- **Privacy-hardened before shipping.** An in-depth security review of the new
  Bluetooth service tightened it end to end: the link keeps every message
  encrypted (network names and status are never readable in the clear), only
  one phone can talk to a unit at a time, a proven-you-own-it code is required
  before it will scan or accept a network — with limited attempts — and the
  unit only makes itself discoverable when you're actually setting it up, never
  as a broadcast a passer-by could track.
- Under the hood the BLE build switch became strictly build-wide, closing a
  subtle misconfiguration risk for custom generic-board builds.

## 2026-07-17 — v0.7.1 + boards 0.4.0: first-hardware fixes

- **Important board update.** The AutoTLM One board package 0.3.0 could leave a
  unit unable to start after uploading — its flash layout didn't line up with
  where the Arduino tools actually write. **Boards 0.4.0 fixes it and 0.3.0 has
  been withdrawn**; update the board package before your next upload (Tools →
  Boards Manager → AutoTLM Boards → 0.4.0). The two big update slots are
  unchanged, so nothing else about your unit changes.
- **Bluetooth setup is steadier.** Starting the BLE service when memory was
  already tight could stop a unit rather than simply reporting the problem — it
  now declines gracefully and says why. Bring Bluetooth up early in your sketch
  (right after `car.begin()`); the example and docs show the order.
- **Board recall made airtight.** The 0.4.0 download always carried the fixed
  flash layout, but the version tag still referenced the old source — corrected,
  and a clean install was verified end-to-end to write the correct layout. An
  automated check now guards the flash offsets on every change so this can't slip
  through a build again.

## 2026-07-16 — v0.7.0: change your WiFi from your phone

- **The headline: no button, no cable, no laptop.** Your phone finds the unit
  over Bluetooth, you prove it's yours with the code on its label, pick a
  network from the unit's own scan, type the password — and watch it honestly
  report *testing… → connected* or *couldn't connect — kept your old network*
  (with the reason). The safety net underneath is the same validate-and-
  rollback engine: a wrong password can never strand the unit.
- **Private by design.** The Bluetooth link is encrypted and bonded, the
  credentials write requires physical possession of the unit's setup code,
  and the unit is only discoverable when a change is plausibly wanted — a
  driving car is never a followable beacon.
- For builders: `car.bleBegin()` / `car.bleAdvertise(on)`, a race-free
  `car.onWifiChange()` observer, and revert reasons
  (`car.wifiChangeReason()`) — full GATT contract in docs/API.md.

## 2026-07-15 — v0.6.0: every sensor your car offers, freeze frames, and drives that keep their timeline

- **The whole sensor menu.** Frames now tell every dashboard exactly which
  sensors YOUR car offers (`obd.supported`), and the polled set nearly doubles
  to the full standard OBD-II decode range — fuel trims, O2 sensors, timing
  advance, EGR, evap, catalyst temps, pedal positions, torque, fuel rate and
  more. Sensor pickers can finally show the car's real capabilities.
- **Freeze frames.** When a trouble code sets, the car stores the sensor
  snapshot from that exact moment — and now it rides in telemetry
  (`dtc.freeze`), so "what was happening when the light came on" is a data
  point, not a guess.
- **Offline drives keep their timeline.** Buffered catch-up frames now carry
  how old each one is (`age_ms`), so a tunnel or parking-garage gap uploads as
  the real minute-by-minute history instead of one merged blob.
- **Location provenance** (`gps.source`) and tougher bus parsing: corrupted
  VINs are rejected instead of believed, and a glitchy reconnect can never
  shrink the discovered sensor map mid-drive.

## 2026-07-15 — v0.5.0: change your WiFi without fear, re-pair without tools

- **A wrong WiFi password can never strand your unit.** Changing networks now
  tries the new one while keeping the old — if the new password is wrong or
  the network's not there, the unit quietly stays on its working connection
  instead of dropping off the grid. (`car.changeWifi()`.)
- **Lost your network? The unit offers itself back.** A provisioned unit that
  can't find its WiFi for a couple of minutes automatically raises its setup
  network again, so re-pairing from your phone needs no button and no cable.
  Opt-in, and conservative — a brief dead zone mid-drive won't trigger it.
- **The setup network is now private.** The onboarding access point comes up
  as a secured (WPA2) network with a per-device password instead of open, and
  its save step is protected against cross-site abuse — so nobody nearby can
  point your unit's telemetry somewhere else during setup.

## 2026-07-12 — v0.4.0: drives survive dead spots

- **Offline catch-up.** Lose WiFi in a parking garage or a tunnel mid-drive?
  The unit keeps capturing frames and replays them in one batched upload the
  moment the connection returns — the trip's story arrives complete, not
  with a hole in it. Rate-limited by the server? It backs off politely and
  catches up after.
- **Cleaner frames.** Telemetry now omits what isn't there (no GPS fix = no
  gps object) instead of sending zeroes — dashboards can trust every field
  they receive.
- **Multi-module reliability fix** from the first two-device bench runs: on
  cars where several modules answer every broadcast request, responses are
  now matched to their request — no more misattributed answers under heavy
  bus chatter.

## 2026-07-12 — v0.3.0: your car is more than one computer

- **Multi-module awareness.** Real cars carry several diagnosable computers —
  engine, transmission, ABS, airbag and more. AutoTLM Core now finds them all
  automatically and reads each one's trouble codes, including pending and
  permanent codes, just like a professional scan tool: `car.modules()`,
  per-module code accessors, and DTC alerts that tell you *which* module
  stored the code (P, C, B and U codes all decode).
- New example: **07_ModuleScan** — the whole roster, live on the Serial
  Monitor.
- **`car.pushNow()`** — push a frame the moment something happens (a new
  trouble code, a script event) instead of waiting for the next scheduled
  upload. Safe to call from anywhere; the network core does the work.

## 2026-07-11 — v0.2.0 tagged + CI

- **v0.2.0 released** on GitHub — the AutoTLM One board package, browser
  provisioning, six examples and the full API reference, now under a proper
  version tag.
- **Continuous integration**: every push now compiles all six examples on
  both the standard toolchain and the published AutoTLM One board package —
  a regression can't reach users unnoticed.

## 2026-07-11 — docs pass: developer-experience first

- README and reference docs streamlined to focus purely on the developer
  experience: select the AutoTLM One board, install the library, write your
  sketch. Capability-oriented board section (what the library speaks for
  you: ISO 15765-4/CAN, GNSS, motion, raw CAN), tightened example headers,
  and the same story now told consistently on autotlm.com/docs.

## 2026-07-11 — v0.2.0: the AutoTLM One era

- **"AutoTLM One" is now a real board in the Arduino IDE.** Add our boards
  URL, install AutoTLM Boards, and pick Tools → Board → AutoTLM One — pins,
  defaults and wiring come with it. No more "generic ESP32" setup.
- **Browser-based first-boot setup.** A fresh unit raises its own WiFi
  hotspot; join it from a phone and a setup page pops up — pick your
  network, paste your cloud endpoint, choose units, save. The unit reboots
  and streams. Zero code edits: `car.provision()` is the whole onboarding.
- **Two new examples**: phone-based provisioning and a live serial gauge
  dashboard — six examples total, all compiling out of the box.
- **Full API reference published** (`docs/API.md`): ~118 public functions
  documented with units and defaults.
- PlatformIO manifest added; the Freematics ONE+ compatibility path is
  deprecated (the AutoTLM One needs no third-party libraries).
