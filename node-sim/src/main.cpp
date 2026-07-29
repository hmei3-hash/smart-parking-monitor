// Filename: main.cpp
// Author: Hongyi Mei
// Date: 07/29/2026
// Description: Simulated parking node for gateway bring-up. Publishes fake
//              occupancy over MQTT without running inference, so the broker,
//              majority-vote fusion, and Last Will paths can be validated
//              independently of the camera/TFLite layer.
//
//              All three nodes derive the same ground truth from NTP epoch
//              time, then each independently flips its own report with a
//              fixed probability. Without that disagreement the gateway's
//              vote logic is never exercised.
//
// Build with -D NODE_ID=1|2|3 (see platformio.ini).

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>

// ================== Build Config ====================
#ifndef NODE_ID
#error "NODE_ID not defined - build with -D NODE_ID=1|2|3"
#endif

#define WIFI_SSID     "YOUR_HOTSPOT"
#define WIFI_PASS     "YOUR_PASS"
#define MQTT_HOST     "192.168.137.100"   // Raspberry Pi running Mosquitto
#define MQTT_PORT     1883

#define SPOT_ID       1                 // all three nodes watch one spot
#define PUBLISH_MS    10000             // report interval
#define DWELL_SEC     30                // ground truth holds this long
#define DISAGREE_PCT  15                // % of reports this node flips

#define NTP_SERVER    "pool.ntp.org"
#define NTP_TIMEOUT_MS 15000

// ================== Status LED ====================
#define RGB_LED_PIN   38                // DevKitC-1 v1.1; v1.0 clone use 48
#define LED_LEVEL     32

// ================== Globals ====================
static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

static char clientId[16];
static char dataTopic[32];
static char statusTopic[32];

static uint32_t seq = 0;

// ================== Status LED ====================
// Red = reported occupied, green = empty, blue = link fault.
// neopixelWrite is the ESP32 Arduino core 2.0.14+ name; core 3.x renamed it
// to rgbLedWrite.
static void setStatusLed(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(RGB_LED_PIN, r, g, b);
}

// ================== Ground Truth ====================
// Deterministic function of wall-clock time only. Every node with a synced
// clock computes an identical result, which is what makes the simulated
// disagreement below meaningful.
static bool groundTruthOccupied() {
  uint32_t slot = (uint32_t)(time(nullptr) / DWELL_SEC);

  // Knuth multiplicative hash - cheap way to get a stable pseudo-random
  // bit per slot without carrying RNG state across reboots.
  uint32_t h = slot * 2654435761u;
  return (h >> 16) & 1;
}

// Flip the truth occasionally so the gateway sees a genuine 2-1 split.
static bool nodeReport(bool truth) {
  if ((esp_random() % 100) < DISAGREE_PCT) return !truth;
  return truth;
}

// ================== Connectivity ====================
static void connectWiFi() {
  Serial.printf("WiFi connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf(" ok, ip=%s\n", WiFi.localIP().toString().c_str());
}

// A node whose clock never synced would compute a different ground truth and
// look permanently broken to the gateway, so block until time is valid.
static void syncTime() {
  configTime(0, 0, NTP_SERVER);
  Serial.print("NTP syncing");
  uint32_t start = millis();
  while (time(nullptr) < 1700000000) {
    if (millis() - start > NTP_TIMEOUT_MS) {
      Serial.println(" TIMEOUT - restarting");
      ESP.restart();
    }
    delay(500);
    Serial.print(".");
  }
  Serial.printf(" ok, epoch=%ld\n", (long)time(nullptr));
}

static void connectMQTT() {
  while (!mqtt.connected()) {
    Serial.printf("MQTT connecting as %s ... ", clientId);

    // Last Will: broker publishes "offline" if this node drops without a
    // clean disconnect. This is the node-health signal the gateway uses.
    if (mqtt.connect(clientId, nullptr, nullptr,
                     statusTopic, 1, true, "offline")) {
      Serial.println("ok");
      mqtt.publish(statusTopic, "online", true);
    } else {
      Serial.printf("failed rc=%d, retry in 2s\n", mqtt.state());
      setStatusLed(0, 0, LED_LEVEL);
      delay(2000);
    }
  }
}

// ================== Publish ====================
static void publishReading() {
  bool truth    = groundTruthOccupied();
  bool reported = nodeReport(truth);

  // "truth" and "sim" are simulator-only fields. Real inference nodes will
  // not send them, and the gateway must not persist them.
  char payload[160];
  snprintf(payload, sizeof(payload),
           "{\"node\":%d,\"spot\":%d,\"occupied\":%d,"
           "\"seq\":%lu,\"uptime\":%lu,\"truth\":%d,\"sim\":true}",
           NODE_ID, SPOT_ID, reported ? 1 : 0,
           (unsigned long)seq, millis() / 1000, truth ? 1 : 0);

  bool ok = mqtt.publish(dataTopic, payload);

  setStatusLed(reported ? LED_LEVEL : 0,
               reported ? 0 : LED_LEVEL, 0);

  Serial.printf("#%lu %-8s (truth %-8s)%s%s\n",
                (unsigned long)seq,
                reported ? "OCCUPIED" : "EMPTY",
                truth ? "OCCUPIED" : "EMPTY",
                (reported != truth) ? " <-DISAGREE" : "",
                ok ? "" : " [publish failed]");

  seq++;
}

// ================== Main ====================
void setup() {
  Serial.begin(115200);
  delay(2000);

  snprintf(clientId,    sizeof(clientId),    "node%d", NODE_ID);
  snprintf(dataTopic,   sizeof(dataTopic),   "parking/spot%d/node%d", SPOT_ID, NODE_ID);
  snprintf(statusTopic, sizeof(statusTopic), "parking/status/node%d", NODE_ID);

  Serial.printf("\n=== Simulated parking node %d ===\n", NODE_ID);
  Serial.printf("data   : %s\n", dataTopic);
  Serial.printf("status : %s\n", statusTopic);

  setStatusLed(0, 0, 0);

  connectWiFi();
  syncTime();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  connectMQTT();
}

void loop() {
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();

  static uint32_t last = 0;
  if (millis() - last >= PUBLISH_MS) {
    last = millis();
    publishReading();
  }
}
