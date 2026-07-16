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
 */
#include <AutoTLM.h>

AutoTLM car;

// Advertise for a couple of minutes after power-on, so a phone can always
// catch a freshly plugged-in unit.
const uint32_t ADVERTISE_AFTER_BOOT_MS = 120000;

void setup() {
  Serial.begin(115200);
  car.begin();
  car.provision();  // saved settings apply; fresh unit raises the portal

  car.bleBegin();
  car.bleAdvertise(true);  // power-on window (policy below turns it off)

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
