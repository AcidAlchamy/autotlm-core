# AutoTLM Core — update log

Announce-worthy releases and milestones for the AutoTLM Core library
(github.com/AcidAlchamy/autotlm-core).

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
