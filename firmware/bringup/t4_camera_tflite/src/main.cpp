// Filename: main.cpp
// Author: Hongyi Mei
// Date: 07/28/2026
// Description: Phase 5 camera node — capture, RGB565-to-INT8 conversion,
//              and on-device CNN inference for parking occupancy.
//              Camera frame buffer in PSRAM, tensor arena in internal SRAM.
//
//              Exposure is fixed manually rather than left to AEC/AGC. The
//              sensor's auto-exposure produced frames at ~64/255 mean
//              brightness while the training set sits at ~144/255; that gap
//              alone was enough to saturate the classifier. See
//              docs/phase5-tflite-deployment.md.
//
// Requires parking_model.h (INT8 model as a C array).

#include <Arduino.h>
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "parking_model.h"

#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

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

// ================== Config ====================
#define IMG_SIZE     96
#define EXPECTED_LEN (IMG_SIZE * IMG_SIZE * 2)   // RGB565, 2 bytes/pixel
#define ARENA_SIZE   (120 * 1024)                // measured 103552 used
#define INFER_PERIOD 10000                       // ms between inferences

// ---- Exposure ----
// Tuned so the captured channel mean matches the training distribution.
// Target: int8 mean ~ +16  (uint8 ~ 144), verified via reportChannelStats().
// AEC_VALUE is the primary lever; raise AGC_GAIN only if AEC saturates,
// since gain amplifies sensor noise along with signal.
#define AEC_VALUE  800    // 0..1200
#define AGC_GAIN   20     // 0..30
#define BRIGHTNESS 1      // -2..2
#define WARMUP_FRAMES 5   // sensor needs a few frames after a manual change

// ---- Diagnostics ----
// Per-frame channel statistics. Cheap, and the fastest way to spot an
// exposure or white-balance drift away from the training distribution.
#define REPORT_CHANNEL_STATS 1

// Hex dump of the exact input tensor every N frames, for host-side
// reconstruction. ~55 KB per dump at 115200 baud (~5 s). 0 disables.
#define DUMP_EVERY 0

// ================== TFLite State ====================
static uint8_t tensor_arena[ARENA_SIZE];
static tflite::MicroInterpreter *interpreter = nullptr;
static TfLiteTensor *input  = nullptr;
static TfLiteTensor *output = nullptr;

static uint32_t frame_count = 0;

// ================== Camera Setup ====================
bool setupCamera() {
  // Zero-init is mandatory: esp_camera_init() reads fields this function
  // never assigns. Leftover stack garbage in those fields crashes init.
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

  // Disable the auto loops before writing manual values, otherwise AEC/AGC
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

  // Flush stale frames captured under the previous exposure settings.
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

  // This library version predates the 4-argument constructor.
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

  // The conversion loop writes exactly this many bytes. Assert rather than
  // let a model swap turn into a silent buffer overrun.
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

// ================== Diagnostics ====================
#if REPORT_CHANNEL_STATS
// Training-set reference is int8 [+16, +24, +15]. A large drift here means
// the input has left the distribution the model was fitted on, which shows
// up as a saturated, input-independent prediction.
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
// Emits the tensor shifted back to unsigned so the host can reshape it to
// (96,96,3) and view it as an image.
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
void runDetection() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Capture FAILED");
    return;
  }

  if (fb->len != EXPECTED_LEN) {
    Serial.printf("Frame length %d, expected %d - skipping\n",
                  fb->len, EXPECTED_LEN);
    esp_camera_fb_return(fb);
    return;
  }

  // ---- RGB565 (little-endian) -> INT8 ----
  // Input quantization is scale = 1/255, zero_point = -128, so the mapping
  // reduces to (uint8 channel value) - 128.
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
    return;
  }
  t_inf = micros() - t_inf;

  int8_t empty_score    = output->data.int8[0];
  int8_t occupied_score = output->data.int8[1];
  bool occupied = (occupied_score > empty_score);

  // Dequantize: both scores pinned at the INT8 rails is itself a symptom,
  // and it is not visible from the raw values alone.
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
}

// ================== Main ====================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== Phase 5: parking node ===");
  Serial.printf("dram=%d psram=%d largest=%d\n",
                ESP.getFreeHeap(), ESP.getFreePsram(),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  if (!setupCamera()) while (1) delay(1000);
  if (!setupModel())  while (1) delay(1000);

  Serial.printf("dram after setup=%d psram=%d\n\n",
                ESP.getFreeHeap(), ESP.getFreePsram());
}

void loop() {
  runDetection();
  delay(INFER_PERIOD);
}
