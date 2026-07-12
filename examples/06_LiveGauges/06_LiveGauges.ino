/*
 * 06_LiveGauges — a live text dashboard over serial.
 *
 * Renders RPM / speed / coolant / throttle as bar gauges that redraw in
 * place, 5x a second, using ANSI escape codes. Open a terminal that speaks
 * ANSI (PuTTY, minicom, VS Code's serial monitor) @115200 and watch the
 * engine move. The plain Arduino Serial Monitor works too — it just scrolls
 * instead of redrawing.
 */
// No board define needed: select the AutoTLM One board in the IDE and the
// right HAL comes up on its own.
#include <AutoTLM.h>

AutoTLM car;

// One bar gauge line: label, value, unit, bar filled value/max.
static void gauge(const char* label, int value, const char* unit, int max) {
  const int width = 30;
  int fill = (max > 0) ? (value * width) / max : 0;
  if (fill < 0) fill = 0;
  if (fill > width) fill = width;
  Serial.printf("%-9s [", label);
  for (int i = 0; i < width; i++) Serial.print(i < fill ? '#' : '.');
  Serial.printf("] %5d %s\x1b[K\n", value, unit);
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== AutoTLM: Live Gauges ===");

  car.begin();
  car.setLogStream(nullptr);  // keep the dashboard clean of library logs
}

uint32_t lastDraw = 0;
bool everDrew = false;

void loop() {
  car.update();

  if (millis() - lastDraw < 200) return;
  lastDraw = millis();

  if (!car.obd().connected()) {
    Serial.println("waiting for the ECU... (is the ignition on?)");
    everDrew = false;
    return;
  }

  // Cursor up over the previous frame so the gauges redraw in place.
  if (everDrew) Serial.print("\x1b[5A");
  everDrew = true;

  gauge("RPM",      car.obd().rpm(),         "rpm",  7000);
  gauge("Speed",    car.obd().speedKph(),    "km/h", 240);
  gauge("Coolant",  car.obd().coolantC(),    "degC", 130);
  gauge("Throttle", car.obd().throttlePct(), "%",    100);
  Serial.printf("Battery %.1f V   DTCs %d %s\x1b[K\n", car.obd().volts(),
                car.obd().dtcCount(), car.obd().mil() ? "(MIL ON)" : "");
}
