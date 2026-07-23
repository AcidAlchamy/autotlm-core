/*
 * 04_ReadDTCs — decode the check-engine light.
 *
 * Lists the stored diagnostic trouble codes ("P0171" etc.) and fires a
 * callback the moment a new one appears while driving. Type 'c' + Enter in
 * the Serial Monitor to clear the codes (turns the MIL off until the fault
 * recurs).
 */
// No board define needed: select the AutoTLM One board in the IDE and the
// right HAL comes up on its own.
#include <AutoTLM.h>

AutoTLM car;

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== AutoTLM: Read DTCs ===");
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
    Serial.println("requesting a clear (Mode $04)...");
    // clearDTCs() returns the vehicle's ACTUAL verdict — it never assumes the
    // clear worked. An ECU with the engine running normally refuses (NRC 0x22).
    AutoTLMClearResult r = car.obd().clearDTCs();
    if (r.responderCount < 0) {
      Serial.println("  can't clear — not connected to the ECU");
    } else if (r.responderCount == 0) {
      Serial.println("  no ECU answered — NOTHING was cleared (is the bus alive?)");
    } else {
      for (int i = 0; i < r.responderCount; i++) {
        const AutoTLMClearResponder& m = r.responders[i];
        if (m.verdict == AUTOTLM_CLEAR_CONFIRMED)
          Serial.printf("  0x%lX: cleared\n", (unsigned long)m.id);
        else
          Serial.printf("  0x%lX: REFUSED (NRC 0x%02X%s)\n", (unsigned long)m.id, m.nrc,
                        m.nrc == 0x22 ? " conditionsNotCorrect — engine running?" : "");
      }
      if (r.anyRefused && !r.anyConfirmed)
        Serial.println("  the car refused — codes are NOT cleared (try key on, engine off)");
      else if (r.anyRefused)
        Serial.println("  partial: some modules cleared, some refused");
      else
        Serial.println("  all responding modules cleared");
    }
  }
}
