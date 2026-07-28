// Filename: main.cpp
// Author: Hongyi Mei
// Date: 07/28/2026
// Description: Bring-up T1 — validate board configuration only.
//              No camera, no TFLite. Confirms flash size, PSRAM detection,
//              and available contiguous DRAM before anything else is trusted.
//
// Expected on ESP32-S3-DevKitC-1 N16R8:
//   Flash size    : 16777216
//   PSRAM found   : YES
//   PSRAM total   : ~8386279
//   PSRAM r/w     : PASS
//   Largest DRAM  : ~335860

#include <Arduino.h>
#include "esp_heap_caps.h"

// ================== Config ====================
#define PSRAM_TEST_SIZE (1024 * 1024)   // 1 MB write/read verification

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== T1: N16R8 board check ===");

  Serial.printf("Chip          : %s rev%d, %d core(s)\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
  Serial.printf("Flash size    : %d bytes  (expect 16777216)\n", ESP.getFlashChipSize());
  Serial.printf("PSRAM found   : %s\n", psramFound() ? "YES" : "NO");
  Serial.printf("PSRAM total   : %d bytes  (expect ~8388608)\n", ESP.getPsramSize());
  Serial.printf("PSRAM free    : %d bytes\n", ESP.getFreePsram());
  Serial.printf("DRAM free/tot : %d / %d\n", ESP.getFreeHeap(), ESP.getHeapSize());
  Serial.printf("Largest DRAM  : %d bytes\n",
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

  // psramFound() only confirms the chip answered its ID. Wrong octal/quad
  // timing can pass ID readback and still corrupt data, which shows up later
  // as random camera DMA corruption. Verify with an actual write/read pass.
  if (psramFound()) {
    uint8_t *p = (uint8_t *)ps_malloc(PSRAM_TEST_SIZE);
    if (!p) {
      Serial.println("ps_malloc(1MB) FAILED");
    } else {
      Serial.printf("ps_malloc(1MB) at %p\n", p);
      for (size_t i = 0; i < PSRAM_TEST_SIZE; i++) p[i] = (uint8_t)(i & 0xFF);

      bool ok = true;
      for (size_t i = 0; i < PSRAM_TEST_SIZE; i++) {
        if (p[i] != (uint8_t)(i & 0xFF)) {
          Serial.printf("MISMATCH at %u: got %d\n", (unsigned)i, p[i]);
          ok = false;
          break;
        }
      }
      Serial.printf("PSRAM write/read: %s\n", ok ? "PASS" : "FAIL");
      free(p);
    }
  }
}

void loop() {
  Serial.printf("alive | dram=%d psram=%d\n", ESP.getFreeHeap(), ESP.getFreePsram());
  delay(3000);
}
