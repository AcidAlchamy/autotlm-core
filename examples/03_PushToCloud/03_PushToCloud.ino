/*
 * 03_PushToCloud — the full unit: OBD + GNSS + IMU streamed to your server.
 *
 * POSTs one JSON telemetry frame per second to an HTTP ingest endpoint with
 * a bearer token. The push loop runs on its own CPU core with the
 * road-proven recipe (plain HTTP, cached DNS, fresh connection per push), so
 * a stalled car bus or a weak hotspot never take each other down.
 *
 * The status LED tells the story from across the garage:
 *   fast blink = no WiFi · slow blink = WiFi up but pushes not landing ·
 *   brief pulse every second = streaming.
 */
// No board define needed: select the AutoTLM One board in the IDE and the
// right HAL comes up on its own.
#include <AutoTLM.h>

// Your hotspot + ingest endpoint. Plain http:// by design — TLS handshakes
// stall on weak cellular; the bearer token still authenticates every POST.
#define WIFI_SSID   "YourHotspot"
#define WIFI_PASS   "your-password"
#define INGEST_URL  "http://yourserver.com/api/telemetry_ingest.php"
#define INGEST_TOKEN "your-ingest-token"

AutoTLM car;

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== AutoTLM: Push to Cloud ===");

  car.begin();
  car.wifi(WIFI_SSID, WIFI_PASS);        // reconnects forever on its own
  car.cloud(INGEST_URL, INGEST_TOKEN);   // 1 frame/second on core 0

  car.onDTC([](const char* code) {
    Serial.printf("Check-engine code appeared: %s\n", code);
  });
}

void loop() {
  car.update();
  // That's it. Watch the Serial Monitor for PUSH:200 lines and the
  // DIAG NOW heartbeat; car.printDiagnostics() dumps the same on demand.
}
