// Filename: main_singleboard.cpp
// Author: Hongyi Mei
// Date: 07/29/2026
// Description: Single-board stand-in for three simulated parking nodes.
//              Runs three FreeRTOS tasks, each with its own MQTT client and
//              topic, so the gateway sees an identical picture to three
//              physical boards. Used to validate majority-vote fusion before
//              hardware for three separate nodes was available.
//
//              NOT a drop-in equivalent of three boards:
//                - all three tasks read the same clock, so NTP sync between
//                  nodes is untestable here
//                - all three share one TCP stack and one power rail, so
//                  Last Will fires for all three at once
//
//              To use, swap this file into src/main.cpp.

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>

// ================== Config ====================
#define WIFI_SSID     "YOUR_HOTSPOT"
#define WIFI_PASS     "YOUR_PASS"
#define MQTT_HOST     "192.168.137.100"
#define MQTT_PORT     1883

#define NUM_NODES     3
#define SPOT_ID       1
#define PUBLISH_MS    10000
#define DWELL_SEC     30
#define DISAGREE_PCT  15

#define NTP_SERVER    "pool.ntp.org"
#define NTP_TIMEOUT_MS 15000

#define RGB_LED_PIN   38
#define LED_LEVEL     32

// ================== Per-Node State ====================
// Each simulated node needs its own WiFiClient: PubSubClient holds a
// reference to one socket, and sharing a socket across three clients
// silently corrupts the streams.
typedef struct {
  int          id;
  WiFiClient   wifi;
  PubSubClient mqtt;
  char         clientId[16];
  char         dataTopic[32];
  char         statusTopic[32];
  uint32_t     seq;
} SimNode;

static SimNode nodes[NUM_NODES];

// The three tasks all publish concurrently. Serial.printf is not reentrant,
// so interleaved output would be unreadable without this.
static SemaphoreHandle_t serialMutex;

// ================== Status LED ====================
static void setStatusLed(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(RGB_LED_PIN, r, g, b);
}

// ================== Ground Truth ====================
static bool groundTruthOccupied() {
  uint32_t slot = (uint32_t)(time(nullptr) / DWELL_SEC);
  uint32_t h = slot * 2654435761u;   // Knuth multiplicative hash
  return (h >> 16) & 1;
}

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

static bool connectNode(SimNode *n) {
  if (n->mqtt.connect(n->clientId, nullptr, nullptr,
                      n->statusTopic, 1, true, "offline")) {
    n->mqtt.publish(n->statusTopic, "online", true);
    return true;
  }
  return false;
}

// ================== Node Task ====================
static void nodeTask(void *arg) {
  SimNode *n = (SimNode *)arg;

  n->mqtt.setClient(n->wifi);
  n->mqtt.setServer(MQTT_HOST, MQTT_PORT);

  // Stagger startup so three CONNECT packets don't land simultaneously.
  vTaskDelay(pdMS_TO_TICKS(n->id * 300));

  for (;;) {
    if (!n->mqtt.connected()) {
      xSemaphoreTake(serialMutex, portMAX_DELAY);
      Serial.printf("[node%d] MQTT connecting ... ", n->id);
      xSemaphoreGive(serialMutex);

      bool ok = connectNode(n);

      xSemaphoreTake(serialMutex, portMAX_DELAY);
      Serial.printf(ok ? "[node%d] ok\n" : "[node%d] failed rc=%d\n",
                    n->id, n->mqtt.state());
      xSemaphoreGive(serialMutex);

      if (!ok) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
    }

    n->mqtt.loop();

    bool truth    = groundTruthOccupied();
    bool reported = nodeReport(truth);

    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"node\":%d,\"spot\":%d,\"occupied\":%d,"
             "\"seq\":%lu,\"uptime\":%lu,\"truth\":%d,\"sim\":true}",
             n->id, SPOT_ID, reported ? 1 : 0,
             (unsigned long)n->seq, millis() / 1000, truth ? 1 : 0);

    bool ok = n->mqtt.publish(n->dataTopic, payload);

    xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.printf("[node%d] #%lu %-8s (truth %-8s)%s%s\n",
                  n->id, (unsigned long)n->seq,
                  reported ? "OCCUPIED" : "EMPTY",
                  truth ? "OCCUPIED" : "EMPTY",
                  (reported != truth) ? " <-DISAGREE" : "",
                  ok ? "" : " [publish failed]");
    xSemaphoreGive(serialMutex);

    n->seq++;

    // Node 1 drives the LED with ground truth. Showing one node's report
    // would be misleading when that node is the one disagreeing.
    if (n->id == 1) {
      setStatusLed(truth ? LED_LEVEL : 0, truth ? 0 : LED_LEVEL, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(PUBLISH_MS));
  }
}

// ================== Main ====================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.printf("\n=== %d simulated nodes on one board ===\n", NUM_NODES);

  setStatusLed(0, 0, 0);

  connectWiFi();
  syncTime();

  serialMutex = xSemaphoreCreateMutex();

  for (int i = 0; i < NUM_NODES; i++) {
    SimNode *n = &nodes[i];
    n->id  = i + 1;
    n->seq = 0;
    snprintf(n->clientId,    sizeof(n->clientId),    "node%d", n->id);
    snprintf(n->dataTopic,   sizeof(n->dataTopic),   "parking/spot%d/node%d", SPOT_ID, n->id);
    snprintf(n->statusTopic, sizeof(n->statusTopic), "parking/status/node%d", n->id);

    char taskName[12];
    snprintf(taskName, sizeof(taskName), "node%d", n->id);

    // 4 KB stack: PubSubClient plus the payload buffer fits comfortably.
    xTaskCreatePinnedToCore(nodeTask, taskName, 4096, n, 2, nullptr, 1);

    Serial.printf("node%d -> %s\n", n->id, n->dataTopic);
  }
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
