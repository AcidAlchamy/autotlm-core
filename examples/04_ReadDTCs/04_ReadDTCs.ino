/*
 * 04_ReadDTCs — decode the check-engine light.
 *
 * Lists the stored diagnostic trouble codes ("P0171" etc.) and fires a
 * callback the moment a new one appears while driving. Type 'c' + Enter in
 * the Serial Monitor to clear the codes (turns the MIL off until the fault
 * recurs).
 */
// Board: the generic define compiles with no extra libraries; the ONE+ needs
// the FreematicsPlus library installed (see README "Supported boards").
#define DRIVON_BOARD_GENERIC_ESP32
// #define DRIVON_BOARD_FREEMATICS_ONEPLUS
#include <Drivon.h>

Drivon car;

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== Drivon: Read DTCs ===");
  Serial.println("(send 'c' to clear stored codes)");

  car.begin();

  car.onDTC([](const char* code) {
    Serial.printf(">>> new trouble code: %s\n", code);
  });
}

uint32_t lastPrint = 0;

void loop() {
  car.update();

  if (millis() - lastPrint > 5000) {
    lastPrint = millis();
    if (!car.obd().connected()) {
      Serial.println("waiting for the ECU...");
    } else if (car.obd().dtcCount() == 0) {
      Serial.println("no stored codes — the check-engine light is off");
    } else {
      Serial.printf("MIL on, %d stored code(s): ", car.obd().dtcCount());
      for (int i = 0; i < car.obd().dtcCount(); i++) {
        Serial.printf("%s%s", i ? ", " : "", car.obd().dtcAt(i));
      }
      Serial.println();
    }
  }

  if (Serial.available() && Serial.read() == 'c') {
    Serial.println("clearing stored codes...");
    car.obd().clearDTCs();
  }
}
