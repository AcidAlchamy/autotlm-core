# AutoTLM Core — update log

Announce-worthy releases and milestones for the AutoTLM Core library
(github.com/AcidAlchamy/autotlm-core).

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
