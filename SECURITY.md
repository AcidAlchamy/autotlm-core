# Security notes — AutoTLM Core

## Telemetry transport is plain HTTP, by design

Frames push over **plain HTTP** (`car.cloud(url, token)` → `Authorization:
Bearer` on port 80). This is a deliberate trade-off: TLS handshakes routinely
stall on the weak, high-latency cellular/hotspot links a drive actually rides
on, and a telemetry stream that dies in every dead zone is worse than one that
is readable in transit.

What that means concretely — anyone who can observe the network path can:

- **read the telemetry** (VIN, live position, speed, DTCs);
- **capture the bearer token** and replay it to submit frames as your device.

Mitigations in place:

- Tokens are **per-device** and revocable server-side — a leaked token
  compromises one device's ingest, not an account.
- The ingest endpoint accepts writes only; a captured token does not grant
  reads of stored history (query auth is a separate credential).
- Provisioning is protected independently: the setup AP is WPA2 with a
  per-device password and the portal save step is CSRF-guarded (v0.5.0), so
  the token can't be stolen or replaced at onboarding time by a bystander.

Planned: an **opt-in TLS mode** for devices on good connectivity, so users
who prefer confidentiality over dead-zone robustness can choose it.

If your threat model includes network observers, treat the device token as
sniffable and rotate it periodically.

## Reporting

Found a vulnerability in the library? Open a GitHub security advisory on
`AcidAlchamy/autotlm-core` (preferred) or email dev@autotlm.com.
