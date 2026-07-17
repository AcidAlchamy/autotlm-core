/*
 * 08_BleChangeWifi — change the unit's WiFi from a phone, over BLE.
 *
 * The phone app discovers the unit (it advertises its device id), proves
 * possession with the 8-char setup code from the unit's label, picks a
 * network from the unit's own scan, and writes credentials. The unit tries
 * them while KEEPING the old network and reports an honest outcome:
 * testing → connected (persisted) | reverted (old network kept, with a
 * reason). A wrong password can never strand the unit off-grid.
 *
 * Advertising policy (yours to own — the library never forces always-on):
 * this sketch advertises while unprovisioned, for 2 minutes after power-on,
 * and whenever WiFi has been lost for a while; it goes dark while happily
 * streaming, so a driving car is not a followable beacon.
 *
 * NOTE flash size: BLE + WiFi together need more than the classic 1.3 MB app
 * partition. On the AutoTLM One board this just works (its standard
 * partition table has 1.9 MB OTA slots); on a generic ESP32 select a big-app
 * partition scheme (e.g. "Huge App").
 *
 * BLE is compiled in by default only on the AutoTLM One. This define forces
 * it on for any board (a generic ESP32 build of this example needs it), and
 * is harmless on the One. It MUST precede the AutoTLM.h include.
 */
#define AUTOTLM_ENABLE_BLE 1
#include <AutoTLM.h>

AutoTLM car;

// Advertise for a couple of minutes after power-on, so a phone can always
// catch a freshly plugged-in unit.
const uint32_t ADVERTISE_AFTER_BOOT_MS = 120000;

void setup() {
  Serial.begin(115200);
  car.begin();

  // Bring BLE up FIRST — right after begin(), before provision(). The BT
  // stack needs a big contiguous allocation, and the captive portal (SoftAP +
  // DNS + web server) fragments the heap enough to starve it on a real
  // firmware. This ordering is the canonical one; get it wrong and bleBegin()
  // returns false (it won't take the unit down, but you won't have BLE).
  car.bleBegin();
  car.bleAdvertise(true);  // power-on window (policy below turns it off)

  car.provision();  // saved settings apply; fresh unit raises the portal

  // Optional: watch outcomes race-free (the BLE status characteristic uses
  // this same hook internally).
  car.onWifiChange([](int state, int reason, void*) {
    if (state == AUTOTLM_WIFI_OK) Serial.println("WiFi change: connected + saved");
    if (state == AUTOTLM_WIFI_REVERTED)
      Serial.printf("WiFi change: reverted (reason %d)\n", reason);
  });
}

void loop() {
  car.update();

  // The advertising policy: discoverable when a change is plausibly wanted,
  // dark while associated and streaming.
  const bool wantAdvertise =
      !car.config().hasWifi() ||                     // unprovisioned
      millis() < ADVERTISE_AFTER_BOOT_MS ||          // power-on window
      car.net().sinceConnectedMs() > 60000;          // network really gone
  car.bleAdvertise(wantAdvertise);
}
