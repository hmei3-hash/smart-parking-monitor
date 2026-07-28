# Phase 5 — On-Device TFLite Micro Deployment

**Author:** Hongyi Mei
**Date:** 2026-07-28

---

## Overview

Phase 5 deployed the INT8 quantized parking-occupancy model (from Phase 4)
onto the ESP32-S3-CAM using TFLite Micro. The goal was binary
occupied/empty inference running entirely on-device.

---

## Final Numbers

| Metric              | Value                                          |
|---------------------|------------------------------------------------|
| Model size (flash)  | 13.5 KB (6 594 params, INT8)                   |
| Tensor arena used   | 103 552 B measured (120 KB / 122 880 B allocated) |
| Inference latency   | 4.15 s per frame (reference kernels, ~7.3 M MACs) |
| Input               | 96 × 96 × 3 RGB, INT8 (scale 0.003922, zp −128) |
| TFLite library      | tanakamasayuki/Arduino_TensorFlowLite_ESP32    |
| Build system        | PlatformIO, Arduino framework                  |

---

## Bringup Narrative

### Symptom

After integrating TFLite Micro with the camera firmware, the ESP32-S3
entered a boot loop — restarting repeatedly before reaching `app_main`.

### Initial Hypothesis (Wrong)

The first hypothesis was **SRAM exhaustion**: the static tensor arena
(120 KB) plus the camera frame buffer might exceed available internal RAM,
causing the bootloader to fail allocation and reset.

Multiple TFLite library ports were tried in an attempt to reduce memory
overhead or work around the issue:

1. **Adafruit TFLite** — boot loop persisted
2. **TensorFlowLite_ESP32** — boot loop persisted
3. **EloquentTinyML** — boot loop persisted
4. **nicbytes tflite-micro-esp32-arduino** — boot loop persisted

Swapping libraries did not help because the problem was not in the
inference runtime. The working library turned out to be
[tanakamasayuki/Arduino_TensorFlowLite_ESP32](https://github.com/tanakamasayuki/Arduino_TensorFlowLite_ESP32),
but only after the board configuration was fixed.

### Isolation Method

To narrow the failure, a set of staged isolation tests was developed
(kept in `firmware/bringup/`):

| Test | Purpose |
|------|---------|
| T1   | Board configuration validation — verify flash size, PSRAM mode, serial output |
| T2   | Camera-only capture, no TFLite — confirm camera driver works in isolation |
| T3   | TFLite isolation — load model and run inference with no camera, measure arena usage |
| T4   | Camera + INT8 inference combined in a single firmware |

T1 revealed the root cause before the other tests were needed.

### Root Cause

**Board-level misconfiguration for the ESP32-S3-WROOM-1 N16R8 module:**

- Flash size was set incorrectly (default 4 MB instead of 16 MB).
- Octal PSRAM mode was not enabled. The N16R8 has 8 MB of octal PSRAM,
  but the build configuration was using the default quad-SPI PSRAM setting,
  which either failed to initialize PSRAM or made it too slow for the
  bootloader to proceed.

Once `sdkconfig` was corrected — setting the flash size to 16 MB and
enabling octal PSRAM — the boot loop stopped and inference ran
successfully on the first attempt.

### Lesson

The failure mode (boot loop) looked identical to a memory-exhaustion
crash, which led to weeks of chasing the wrong hypothesis. The staged
isolation approach — testing board config, camera, and TFLite
independently — would have surfaced the real cause much sooner.

---

## Planned Next Steps (Not Yet Executed)

- **Retrain at 48 × 48 grayscale** to reduce inference time and free SRAM
  headroom for the WiFi/MQTT stack needed in Phase 6.
- **Evaluate esp-tflite-micro with ESP-NN kernels** for hardware-accelerated
  INT8 operations on the ESP32-S3.

These are planned optimizations; neither has been started.
