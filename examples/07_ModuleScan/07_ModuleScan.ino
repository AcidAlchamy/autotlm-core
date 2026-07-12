/*
 * 07_ModuleScan — see every module (ECU) in the car, like a scan tool does.
 *
 * A real car isn't one computer: engine, transmission, ABS, airbag, body...
 * each is a diagnosable module with its own trouble codes. AutoTLM
 * enumerates them automatically after connecting; this sketch prints the
 * roster every few seconds — module id, stored / pending / permanent code
 * counts, and the codes themselves — and announces new codes with the
 * module that stored them.
 *
 * Codes read as P0xxx (powertrain), C0xxx (chassis/ABS), B0xxx (body),
 * U0xxx (network — e.g. U0101 "lost communication with TCM").
 */
// No board define needed: select the AutoTLM One board in the IDE and the
// right HAL comes up on its own.
#include <AutoTLM.h>

AutoTLM car;

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== AutoTLM: Module Scan ===");

  car.begin();

  // The module-aware callback: which code, and which computer stored it.
  car.onDTC([](const char* code, uint32_t moduleId) {
    Serial.printf(">>> %s stored by module 0x%lX\n", code, (unsigned long)moduleId);
  });
}

uint32_t lastPrint = 0;

void loop() {
  car.update();

  if (millis() - lastPrint < 5000) return;
  lastPrint = millis();

  if (!car.obd().connected()) {
    Serial.println("waiting for the ECU... (is the ignition on?)");
    return;
  }

  const int n = car.modules();
  if (n == 0) {
    Serial.println("single-module view (enumeration unsupported on this transport)");
    return;
  }

  Serial.printf("--- %d module(s) ---\n", n);
  for (int i = 0; i < n; i++) {
    const AutoTLMModuleInfo m = car.obd().module(i);
    Serial.printf("0x%lX  stored:%u pending:%u permanent:%u  ",
                  (unsigned long)m.id, m.stored, m.pending, m.permanent);
    for (int j = 0; j < m.stored; j++) Serial.printf("%s ", car.obd().moduleDtcAt(i, j));
    for (int j = 0; j < m.pending; j++) Serial.printf("(%s) ", car.obd().modulePendingAt(i, j));
    Serial.println();
  }
}
