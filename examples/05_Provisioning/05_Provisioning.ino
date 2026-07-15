/*
 * 05_Provisioning — THE reference sketch: provision → read the car → push.
 *
 * No SSID in the sketch, no token pasted into code. On a fresh unit,
 * car.provision() raises a WiFi access point ("AutoTLM-XXXX"); join it from
 * a phone or laptop and the setup page pops up on its own (captive portal —
 * or browse to http://192.168.4.1). Pick your network, paste your cloud
 * URL + device token, choose GPS and units, hit save: the unit reboots and
 * starts streaming. Every boot after that, the same line applies the saved
 * settings and the portal never appears.
 *
 * RE-PROVISIONING (new WiFi, rotated token): three ways, no wrong answers —
 *   - hold the SETUP button while powering on (below), or
 *   - just move: setReprovisionOnLostWifi() offers a re-pair AP on its own
 *     once the unit has been offline a while (network changed / hotspot
 *     gone), so a phone is all you ever need, or
 *   - car.changeWifi(ssid, pass) from your own code — it validates the new
 *     network and auto-reverts to the old one if the password is wrong, so a
 *     WiFi change can never strand the unit off-network.
 * A wrong WiFi password never boot-loops: the unit keeps retrying and every
 * re-pair path stays open.
 */
// No board define needed: select the AutoTLM One board in the IDE and the
// right HAL comes up on its own.
#include <AutoTLM.h>

// The SETUP button (the boot-mode button on the AutoTLM One; LOW = held).
#define SETUP_BTN 0

AutoTLM car;

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== AutoTLM: Provisioning ===");

  car.begin();

  // If WiFi goes missing for a couple of minutes on a provisioned unit, offer
  // a re-pair AP automatically (WPA2 — password from car.config().apPassword)
  // so re-pairing needs nothing but a phone. Conservative + off by default.
  car.setReprovisionOnLostWifi(true);

  pinMode(SETUP_BTN, INPUT_PULLUP);
  if (digitalRead(SETUP_BTN) == LOW) {
    // Held at power-on: the owner wants the setup portal back.
    car.beginPortal();
    Serial.printf("Setup requested: join WiFi \"%s\" to reconfigure.\n",
                  car.provisioning().apName());
  } else if (car.provision()) {
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
