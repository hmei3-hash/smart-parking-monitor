// Filename: main.cpp
// Author: Hongyi Mei
// Date: 2026-07-31
// Description: Phase 6 parking node — on-device CNN inference (Phase 5)
//              merged with MQTT publish (validated in node-sim).
//              Publishes occupancy to parking/spot{SPOT_ID}/node{NODE_ID}
//              after each inference. Last Will signals gateway on disconnect.
//
//              Key differences from node-sim:
//                - "truth" and "sim" fields are not sent (real inference)
//                - No NTP (ground truth is the model, not shared clock)
//                - loop() uses millis() pattern so mqtt.loop() runs between
//                  inferences; blocking delay() is gone
//
// Build: pio run -e node1 -t upload   (node2, node3 for other boards)

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "parking_model.h"

#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

// ================== Build Config ====================
#ifndef NODE_ID
#error "NODE_ID not defined - build with -D NODE_ID=1|2|3"
#endif

// Network credentials live in secrets.h, which is gitignored. Copy
// secrets.h.example to secrets.h and fill it in before building.
#include "secrets.h"

#define MQTT_PORT    1883

#define SPOT_ID      1
#define INFER_PERIOD 10000               // ms between inferences

// ================== Camera Pins (ESP32-S3-EYE) ====================
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5
#define Y9_GPIO_NUM    16
#define Y8_GPIO_NUM    17
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y4_GPIO_NUM    8
#define Y3_GPIO_NUM    9
#define Y2_GPIO_NUM    11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13

// ================== TFLite Config ====================
#define IMG_SIZE      96
#define EXPECTED_LEN  (IMG_SIZE * IMG_SIZE * 2)   // RGB565, 2 bytes/pixel
#define ARENA_SIZE    (120 * 1024)                // measured 103552 used

// ---- Exposure ----
// Tuned so captured channel mean matches training distribution.
// Target: int8 mean ~+16 (uint8 ~144). See docs/phase5-tflite-deployment.md.
#define AEC_VALUE     800    // 0..1200
#define AGC_GAIN      20     // 0..30
#define BRIGHTNESS    1      // -2..2
#define WARMUP_FRAMES 5

// ---- Diagnostics ----
#define REPORT_CHANNEL_STATS 1
#define DUMP_EVERY           0   // hex dump every N frames; 0 = off

// ================== TFLite State ====================
static uint8_t              tensor_arena[ARENA_SIZE];
static tflite::MicroInterpreter *interpreter = nullptr;
static TfLiteTensor         *input  = nullptr;
static TfLiteTensor         *output = nullptr;

static uint32_t frame_count = 0;

// ================== MQTT State ====================
static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

static char clientId[16];
static char dataTopic[32];
static char statusTopic[32];

static uint32_t seq = 0;

// ================== Camera Setup ====================
bool setupCamera() {
  camera_config_t config = {};

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size   = FRAMESIZE_96X96;
  config.fb_count     = 1;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  config.fb_location  = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init FAILED: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    Serial.println("Sensor handle NULL");
    return false;
  }
  Serial.printf("Sensor PID: 0x%02x\n", s->id.PID);

  // Disable auto loops before writing manual values; otherwise AEC/AGC
  // immediately overwrite them.
  s->set_exposure_ctrl(s, 0);
  s->set_aec2(s, 0);
  s->set_gain_ctrl(s, 0);

  s->set_aec_value(s, AEC_VALUE);
  s->set_agc_gain(s, AGC_GAIN);
  s->set_brightness(s, BRIGHTNESS);

  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);

  Serial.printf("Exposure: aec=%d agc=%d brightness=%d\n",
                AEC_VALUE, AGC_GAIN, BRIGHTNESS);

  // Flush stale frames captured under old exposure settings.
  for (int i = 0; i < WARMUP_FRAMES; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(100);
  }

  Serial.println("Camera ready");
  return true;
}

// ================== TFLite Setup ====================
bool setupModel() {
  const tflite::Model *model = tflite::GetModel(model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.printf("Schema mismatch: %d vs %d\n",
                  model->version(), TFLITE_SCHEMA_VERSION);
    return false;
  }

  static tflite::MicroErrorReporter micro_error_reporter;
  static tflite::AllOpsResolver resolver;
  static tflite::MicroInterpreter static_interpreter(
      model, resolver, tensor_arena, ARENA_SIZE, &micro_error_reporter);
  interpreter = &static_interpreter;

  if (interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("AllocateTensors FAILED");
    return false;
  }

  input  = interpreter->input(0);
  output = interpreter->output(0);

  if (input->bytes != IMG_SIZE * IMG_SIZE * 3) {
    Serial.printf("Unexpected input size: %d (expected %d)\n",
                  input->bytes, IMG_SIZE * IMG_SIZE * 3);
    return false;
  }

  Serial.printf("Model ready | arena %d/%d\n",
                interpreter->arena_used_bytes(), ARENA_SIZE);
  Serial.printf("  input  quant: scale %.6f, zero_point %d\n",
                input->params.scale, input->params.zero_point);
  Serial.printf("  output quant: scale %.6f, zero_point %d\n",
                output->params.scale, output->params.zero_point);
  return true;
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

static void connectMQTT() {
  while (!mqtt.connected()) {
    Serial.printf("MQTT connecting as %s ... ", clientId);

    // Last Will: broker publishes "offline" if this node drops without a
    // clean disconnect. Gateway uses this as the node-health signal.
    if (mqtt.connect(clientId, nullptr, nullptr,
                     statusTopic, 1, true, "offline")) {
      Serial.println("ok");
      mqtt.publish(statusTopic, "online", true);
    } else {
      Serial.printf("failed rc=%d, retry in 2s\n", mqtt.state());
      delay(2000);
    }
  }
}

// ================== Diagnostics ====================
#if REPORT_CHANNEL_STATS
// Training-set reference is int8 [+16, +24, +15]. A large drift means the
// input has left the distribution the model was fitted on.
static void reportChannelStats() {
  long sr = 0, sg = 0, sb = 0;
  for (int i = 0; i < IMG_SIZE * IMG_SIZE; i++) {
    sr += input->data.int8[i * 3 + 0];
    sg += input->data.int8[i * 3 + 1];
    sb += input->data.int8[i * 3 + 2];
  }
  int n = IMG_SIZE * IMG_SIZE;
  Serial.printf("   channel mean (int8): R=%ld G=%ld B=%ld  (train ref +16/+24/+15)\n",
                sr / n, sg / n, sb / n);
}
#endif

#if DUMP_EVERY
static void dumpInputTensor() {
  Serial.println("---IMG_START---");
  for (int i = 0; i < input->bytes; i++) {
    Serial.printf("%02x", (uint8_t)(input->data.int8[i] + 128));
    if ((i + 1) % (IMG_SIZE * 3) == 0) Serial.println();
  }
  Serial.println("---IMG_END---");
}
#endif

// ================== Inference ====================
// Returns 1=occupied, 0=empty, -1=capture/inference error.
// Returning an int lets loop() skip publishReading() on failure.
int runDetection() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Capture FAILED");
    return -1;
  }

  if (fb->len != EXPECTED_LEN) {
    Serial.printf("Frame length %d, expected %d - skipping\n",
                  fb->len, EXPECTED_LEN);
    esp_camera_fb_return(fb);
    return -1;
  }

  // ---- RGB565 (little-endian) -> INT8 ----
  // Quantization: scale=1/255, zero_point=-128  =>  out = uint8_val - 128
  unsigned long t_conv = micros();
  int idx = 0;
  for (int i = 0; i < IMG_SIZE * IMG_SIZE; i++) {
    uint16_t pixel = (fb->buf[i * 2 + 1] << 8) | fb->buf[i * 2];
    uint8_t r = ((pixel >> 11) & 0x1F) << 3;
    uint8_t g = ((pixel >> 5)  & 0x3F) << 2;
    uint8_t b = ( pixel        & 0x1F) << 3;

    input->data.int8[idx++] = (int8_t)(r - 128);
    input->data.int8[idx++] = (int8_t)(g - 128);
    input->data.int8[idx++] = (int8_t)(b - 128);
  }
  t_conv = micros() - t_conv;

  esp_camera_fb_return(fb);   // release before the multi-second Invoke

  frame_count++;

  // ---- Inference ----
  unsigned long t_inf = micros();
  if (interpreter->Invoke() != kTfLiteOk) {
    Serial.println("Invoke FAILED");
    return -1;
  }
  t_inf = micros() - t_inf;

  int8_t empty_score    = output->data.int8[0];
  int8_t occupied_score = output->data.int8[1];
  bool occupied = (occupied_score > empty_score);

  float s  = output->params.scale;
  int   zp = output->params.zero_point;

  Serial.printf("#%lu [%-8s] empty=%4d(%.3f) occ=%4d(%.3f) | conv %lu us, infer %lu ms | dram=%d\n",
                frame_count,
                occupied ? "OCCUPIED" : "EMPTY",
                empty_score,    s * (empty_score - zp),
                occupied_score, s * (occupied_score - zp),
                t_conv, t_inf / 1000, ESP.getFreeHeap());

#if REPORT_CHANNEL_STATS
  reportChannelStats();
#endif

#if DUMP_EVERY
  if (frame_count % DUMP_EVERY == 0) dumpInputTensor();
#endif

  return occupied ? 1 : 0;
}

// ================== Publish ====================
static void publishReading(int occupied) {
  char payload[128];
  snprintf(payload, sizeof(payload),
           "{\"node\":%d,\"spot\":%d,\"occupied\":%d,\"seq\":%lu,\"uptime\":%lu}",
           NODE_ID, SPOT_ID, occupied,
           (unsigned long)seq, millis() / 1000);

  bool ok = mqtt.publish(dataTopic, payload);
  Serial.printf("mqtt #%lu %s%s\n",
                (unsigned long)seq,
                occupied ? "OCCUPIED" : "EMPTY",
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

  Serial.printf("\n=== Phase 6: parking node %d ===\n", NODE_ID);
  Serial.printf("dram=%d psram=%d\n", ESP.getFreeHeap(), ESP.getFreePsram());

  // Camera and model must init before WiFi: the WiFi stack allocates ~100KB
  // of internal SRAM; tensor arena (120KB) must already be placed before that
  // to avoid a layout where neither fits.
  if (!setupCamera()) while (1) delay(1000);
  if (!setupModel())  while (1) delay(1000);

  Serial.printf("dram after model=%d psram=%d\n", ESP.getFreeHeap(), ESP.getFreePsram());

  connectWiFi();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  // Default keepalive is 15s. Each inference cycle is INFER_PERIOD(10s) +
  // Invoke time (~2-3s). Raise keepalive so the broker doesn't drop the
  // connection mid-inference.
  mqtt.setKeepAlive(60);
  connectMQTT();

  Serial.printf("dram after mqtt=%d\n\n", ESP.getFreeHeap());
}

void loop() {
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();   // must be called regularly for keepalive

  static uint32_t last = 0;
  if (millis() - last >= INFER_PERIOD) {
    last = millis();
    int result = runDetection();
    if (result >= 0) publishReading(result);
  }
}