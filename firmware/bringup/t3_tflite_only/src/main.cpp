// Filename: main.cpp
// Author: Hongyi Mei
// Date: 07/28/2026
// Description: Bring-up T3 — TFLite Micro only, no camera. Loads the model,
//              reports arena residency and real usage, quantization params,
//              and per-inference latency on synthetic input.
//
// Requires parking_model.h (INT8 model as a C array) alongside this file.
//
// Measured on ESP32-S3 N16R8:
//   arena addr   = 0x3fc96bfc (SRAM)
//   ARENA USED   = 103552 bytes
//   input        = [1,96,96,3] INT8, scale 0.003922, zero_point -128
//   operators    = 10
//   Invoke       = 4.15 s   (reference kernels, ~7.3M MACs)

#include <Arduino.h>
#include "esp_heap_caps.h"
#include "parking_model.h"

#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

// ================== TFLite Config ====================
#define ARENA_SIZE (160 * 1024)   // oversized on purpose: measure, then shrink

// ESP32-S3 maps PSRAM at 0x3C000000-0x3DFFFFFF; internal SRAM starts at
// 0x3FC00000. Used to prove where the arena actually landed.
#define PSRAM_BASE  0x3C000000UL
#define PSRAM_LIMIT 0x3E000000UL

static uint8_t tensor_arena[ARENA_SIZE];
static tflite::MicroInterpreter *interpreter = nullptr;
static TfLiteTensor *input  = nullptr;
static TfLiteTensor *output = nullptr;

// ================== Helpers ====================
static const char *regionOf(const void *p) {
  uint32_t a = (uint32_t)p;
  return (a >= PSRAM_BASE && a < PSRAM_LIMIT) ? "PSRAM" : "SRAM";
}

// ================== Setup ====================
bool setupModel() {
  Serial.println("--- model setup ---");
  Serial.printf("arena addr   = %p  (%s)\n", tensor_arena, regionOf(tensor_arena));
  Serial.printf("arena size   = %d bytes\n", ARENA_SIZE);
  Serial.printf("dram before  = %d\n", ESP.getFreeHeap());

  const tflite::Model *model = tflite::GetModel(model_data);
  Serial.printf("model addr   = %p  (%s)\n", model_data, regionOf(model_data));
  Serial.printf("schema       = %d (expect %d)\n",
                model->version(), TFLITE_SCHEMA_VERSION);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println(">>> schema mismatch: re-export with matching TF version");
    return false;
  }

  // This library predates the 4-argument constructor; ErrorReporter required.
  static tflite::MicroErrorReporter micro_error_reporter;
  static tflite::AllOpsResolver resolver;
  static tflite::MicroInterpreter static_interpreter(
      model, resolver, tensor_arena, ARENA_SIZE, &micro_error_reporter);
  interpreter = &static_interpreter;

  Serial.printf("dram post-ctor = %d\n", ESP.getFreeHeap());

  if (interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println(">>> AllocateTensors FAILED");
    return false;
  }

  // The number that decides the production arena size.
  Serial.printf("ARENA USED   = %d / %d bytes  (%.1f%%)\n",
                interpreter->arena_used_bytes(), ARENA_SIZE,
                100.0 * interpreter->arena_used_bytes() / ARENA_SIZE);

  input  = interpreter->input(0);
  output = interpreter->output(0);

  Serial.printf("input  addr  = %p  (%s)\n",
                input->data.int8, regionOf(input->data.int8));
  Serial.printf("input  dims  = [%d,%d,%d,%d] %s, %d bytes\n",
                input->dims->data[0], input->dims->data[1],
                input->dims->data[2], input->dims->data[3],
                input->type == kTfLiteInt8 ? "INT8" : "NON-INT8",
                input->bytes);
  Serial.printf("input  quant = scale %.6f, zero_point %d\n",
                input->params.scale, input->params.zero_point);
  Serial.printf("output dims  = [%d,%d] %s, %d bytes\n",
                output->dims->data[0], output->dims->data[1],
                output->type == kTfLiteInt8 ? "INT8" : "NON-INT8",
                output->bytes);
  Serial.printf("output quant = scale %.6f, zero_point %d\n",
                output->params.scale, output->params.zero_point);
  Serial.printf("operators    = %d\n",
                model->subgraphs()->Get(0)->operators()->size());
  return true;
}

// ================== Main ====================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== T3: tflite diagnostics ===");
  Serial.printf("CPU freq     = %d MHz\n", getCpuFrequencyMhz());
  Serial.printf("dram free    = %d / %d\n", ESP.getFreeHeap(), ESP.getHeapSize());
  Serial.printf("largest dram = %d\n",
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  Serial.printf("psram free   = %d\n\n", ESP.getFreePsram());

  if (!setupModel()) while (1) delay(1000);
  Serial.println("\nModel ready\n");
}

void loop() {
  static uint8_t pattern = 0;

  // Synthetic input with a changing value: confirms the output responds at
  // all. Constant output across patterns means inference is not really running.
  memset(input->data.int8, (int8_t)(pattern - 128), input->bytes);
  pattern += 40;

  unsigned long t = micros();
  TfLiteStatus r = interpreter->Invoke();
  unsigned long us = micros() - t;

  if (r != kTfLiteOk) {
    Serial.println("Invoke FAILED");
  } else {
    Serial.printf("pat=%3d out=[%4d,%4d] %lu us (%.2f s) | dram=%d\n",
                  pattern, output->data.int8[0], output->data.int8[1],
                  us, us / 1000000.0, ESP.getFreeHeap());
  }
  delay(2000);
}
