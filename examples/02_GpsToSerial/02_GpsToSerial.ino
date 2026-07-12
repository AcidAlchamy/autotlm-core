/*
 * 02_GpsToSerial — stream a GNSS fix.
 *
 * Prints position/speed/satellites once a second as soon as the module gets
 * a fix (works indoors near a window with a decent antenna). Uncomment the
 * echo line to also see the raw NMEA stream, handy when checking wiring.
 */
// No board define needed: select the AutoTLM One board in the IDE and the
// right HAL comes up on its own.
#include <AutoTLM.h>

AutoTLM car;

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== AutoTLM: GPS to Serial ===");

  car.begin();
  // car.gnss().echoTo(&Serial);  // raw NMEA passthrough for debugging
}

uint32_t lastPrint = 0;

void loop() {
  car.update();

  if (millis() - lastPrint > 1000) {
    lastPrint = millis();
    AutoTLMGPS g = car.gps();
    if (g.fix) {
      Serial.printf("%.6f, %.6f  alt %.0f m  %.1f km/h  course %.0f  sats %d  hdop %.1f\n",
                    g.lat, g.lng, g.altM, g.speedKph, g.course, g.sats, g.hdop);
    } else if (car.gnss().alive()) {
      Serial.printf("GNSS alive, no fix yet (%d sats)...\n", g.sats);
    } else {
      Serial.println("no NMEA from the GNSS module yet — check wiring/baud");
    }
  }
}
