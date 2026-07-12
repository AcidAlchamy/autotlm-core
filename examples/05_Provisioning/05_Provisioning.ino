/*
 * 05_Provisioning — the zero-code onboarding path.
 *
 * No SSID in the sketch, no token pasted into code. On a fresh unit,
 * car.provision() raises a WiFi access point ("AutoTLM-XXXX"); join it from
 * a phone or laptop and the setup page pops up on its own (captive portal —
 * or browse to http://192.168.4.1). Pick your network, paste your ingest
 * URL + token, choose GPS and units, hit save: the unit reboots and starts
 * streaming. Every boot after that, the same line applies the saved
 * settings and the portal never appears.
 *
 * To re-provision, either call car.beginPortal() (wire it to a button
 * hold), or just submit new settings from a sketch: car.wifi(...) always
 * persists what it's given.
 */
// No board define needed: select "AutoTLM One" in the IDE (or any plain
// ESP32 devkit while breadboarding) and the right HAL comes up on its own.
#include <AutoTLM.h>

AutoTLM car;

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== AutoTLM: Provisioning ===");

  car.begin();

  if (car.provision()) {
    Serial.println("Provisioned — coming up on the saved settings.");
  } else {
    Serial.printf("First boot: join WiFi \"%s\" to set this unit up.\n",
                  car.provisioning().apName());
  }
}

void loop() {
  car.update();  // pumps sensors — and the portal, while it's up

  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 5000) {
    lastPrint = millis();
    if (!car.provisioning().active() && car.obd().connected())
      Serial.printf("RPM %d | %d km/h | net:%d\n", car.obd().rpm(),
                    car.obd().speedKph(), (int)car.net().state());
  }
}
