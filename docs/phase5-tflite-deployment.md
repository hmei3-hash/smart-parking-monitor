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
| Model size (flash)  | 13.8 KB (6 594 params, INT8)                   |
| Tensor arena used   | 103 552 B measured (120 KB / 122 880 B allocated) |
| Inference latency   | 4.15 s per frame (reference kernels, ~7.3 M MACs) |
| Input               | 96 × 96 × 3 RGB, INT8 (scale 0.003922, zp −128) |
| TFLite library      | tanakamasayuki/Arduino_TensorFlowLite_ESP32    |
| Build system        | PlatformIO, Arduino framework                  |

---

## Bringup Narrative

### Symptom — Boot Loop

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

Once the PlatformIO configuration was corrected — setting the flash size
to 16 MB and enabling octal PSRAM (`qio_opi`) — the boot loop stopped
and inference ran successfully on the first attempt.

### Lesson

The failure mode (boot loop) looked identical to a memory-exhaustion
crash, which led to weeks of chasing the wrong hypothesis. The staged
isolation approach — testing board config, camera, and TFLite
independently — would have surfaced the real cause much sooner.

---

## Train/Serve Distribution Mismatch

The deployed classifier returned OCCUPIED on every frame with the output
pinned at the INT8 rail (0.996), regardless of scene. Two hypotheses: a
broken model, or an input path outside the training distribution.

The discriminating test was to bypass the camera and feed a known-EMPTY
training image compiled into the firmware as a C array. It classified
correctly at 0.984. The same model on live frames returned 0.996 OCCUPIED.
One model, two inputs, opposite results — the model and the deployment were
both correct, and the fault was in the input.

Channel means confirmed it:

```
training set    uint8 [144, 152, 143]
device (AEC)    uint8 [ 62,  66,  66]
```

The sensor's auto-exposure was underexposing by more than a factor of two at
96×96. Every live frame fell outside the brightness range the model was fitted
on, so the classifier saturated.

Fix: disable AEC and AGC, set exposure manually, and flush five frames so the
sensor settles before the first inference. Per-frame channel means are now
logged so drift away from the training distribution is visible immediately
rather than surfacing later as an unexplained prediction.

The durable fix is to capture training data through the deployment pipeline
itself — same sensor, same 96×96 RGB565 path, same conversion — so the two
distributions are identical by construction rather than by tuning. Tracked in
[model/README.md](../model/README.md).

---

## Detection Latency

End-to-end latency is dominated by inference, not capture:

| Stage                    | Time     |
|--------------------------|----------|
| Capture                  | ~0.1 s   |
| RGB565 → INT8 conversion | 0.001 s  |
| Inference                | 4.15 s   |
| Loop period              | 10 s     |
| Worst-case observation lag | ~14.2 s |

Frames captured mid-transition — a vehicle partially in the space — are a
pose absent from the training set, and are misclassified roughly once per
state change. This is inherent to sampling a continuous event at a fixed
period, not a model defect, and it is deliberately not corrected on the node.

Field accuracy is currently unmeasured. The dataset issues recorded in
[model/README.md](../model/README.md) mean the reported validation accuracy
is not representative of deployment performance.

---

## Planned Next Steps (Not Yet Executed)

- **Retrain at 48 × 48 grayscale** to reduce inference time and free SRAM
  headroom for the WiFi/MQTT stack needed in Phase 6.
- **Evaluate esp-tflite-micro with ESP-NN kernels** for hardware-accelerated
  INT8 operations on the ESP32-S3.

These are planned optimizations; neither has been started.
