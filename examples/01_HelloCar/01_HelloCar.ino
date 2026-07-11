/*
 * 01_HelloCar — the AutoTLM "blink": read RPM + coolant and print them.
 *
 * Plug the unit into the OBD-II port, open the Serial Monitor @115200 and
 * watch the engine wake up. The ECU connection is brought up lazily in the
 * background, so the sketch starts instantly even before the ignition is on.
 */
// Board: the generic define compiles with no extra libraries; the ONE+ needs
// the FreematicsPlus library installed (see README "Supported boards").
#define AUTOTLM_BOARD_GENERIC_ESP32
// #define AUTOTLM_BOARD_FREEMATICS_ONEPLUS
#include <AutoTLM.h>

AutoTLM car;

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== AutoTLM: Hello, Car ===");

  car.begin();  // board + GNSS + IMU up; OBD connects in the background
}

uint32_t lastPrint = 0;

void loop() {
  car.update();  // pump everything (self-throttling; never blocks for long)

  if (millis() - lastPrint > 1000) {
    lastPrint = millis();
    if (car.obd().connected()) {
      Serial.printf("RPM %d  |  coolant %d degC  |  speed %d km/h  |  battery %.1f V\n",
                    car.obd().rpm(), car.obd().coolantC(),
                    car.obd().speedKph(), car.obd().volts());
    } else {
      Serial.println("waiting for the ECU... (is the ignition on?)");
    }
  }
}
