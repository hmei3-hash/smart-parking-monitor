# Model — Parking Occupancy Classifier

**Author:** Hongyi Mei
**Date:** 2026-07-28

---

## Task

Binary classification: **occupied** vs. **empty** parking spot, from a
single camera frame captured by an ESP32-S3-CAM.

---

## Dataset (Phase 3)

- 205 images total (≈100 occupied, ≈100 empty).
- Captured from the ESP32-S3-CAM HTTP stream using `collect.py`, which
  connects to the camera's MJPEG endpoint and saves frames on keypress.
- `test.py` is a simpler stream viewer used to verify connectivity before
  collection.

---

## Training Setup (Phase 4)

Training was done in a Jupyter notebook (`colab.ipynb`), run locally with
TensorFlow 2.21.0.

| Parameter          | Value                                  |
|--------------------|----------------------------------------|
| Framework          | TensorFlow 2.21.0 / Keras              |
| Architecture       | 3-block CNN (Conv2D → MaxPool × 3, GAP, Dense) |
| Total params       | 6 594 (25.8 KB float32)                |
| Input size         | 96 × 96 × 3 (RGB)                     |
| Output             | 2-class softmax (empty, occupied)      |
| Optimizer          | Adam, lr = 0.0002                      |
| Epochs             | 80                                     |
| Batch size         | 8                                      |
| Augmentation       | Random horizontal flip, brightness ±0.2, contrast ±0.2 |
| Train/val split    | 80 / 20 random split (`seed=42`)       |

### Model Architecture

```
Conv2D(8, 3×3)  → MaxPool → 48×48×8
Conv2D(16, 3×3) → MaxPool → 24×24×16
Conv2D(32, 3×3) → MaxPool → 12×12×32
GlobalAveragePooling2D   → 32
Dense(16, relu)          → 16
Dense(2, softmax)        → 2
```

### Quantization

Post-training INT8 quantization using the TensorFlow Lite converter with
a representative dataset (50 training batches).

| Detail                  | Value                          |
|-------------------------|--------------------------------|
| Quantized model size    | 13.8 KB                        |
| Input quantization      | scale = 0.003922, zero_point = −128 |
| Output quantization     | scale = 0.003906, zero_point = −128 |
| Input/output dtype      | INT8                           |

The quantized model is exported both as a `.tflite` file
(`model/tflite/parking_model.tflite`) and as a C header array
(`parking_model.h`) for inclusion in ESP32 firmware.

---

## Known Issues

### Issue 1 — Train/Validation Leakage

The dataset (≈100 occupied / ≈100 empty) was captured in bursts from a
fixed camera position. The train/val split (`validation_split=0.2,
seed=42`) partitions individual images at random, not by capture session.
Near-duplicate frames from the same burst end up in both training and
validation sets.

The reported 95.12 % INT8 validation accuracy measured **memorization**
rather than generalization. True generalization accuracy is **unknown**.

### Issue 2 — Narrow Lighting Distribution

Training images sit at ~144/255 mean brightness. The deployed sensor under
auto-exposure produced ~64/255, far outside anything the model had seen,
which saturated the output to constant OCCUPIED (0.996). Worked around
on-device by fixing exposure manually; the dataset itself still covers only
one lighting condition.

### Planned Fix

Recapture through the deployment pipeline (ESP32-S3-EYE, 96×96 RGB565,
same conversion as the firmware), grouped by capture session with a
different variable changed per session (lighting, vehicle, angle),
deduplicated by frame correlation, and split with `GroupShuffleSplit`
grouped by session. This has not been started.

---

## Directory Layout

```
model/
├── training/
│   ├── colab.ipynb           # Training notebook (TF 2.21.0)
│   ├── collect.py            # Data collection from ESP32-CAM stream
│   └── test.py               # Stream connectivity test
├── tflite/
│   ├── parking_model.tflite  # INT8 quantized model (13.8 KB)
│   └── parking_model.h       # C array header for ESP32 firmware
└── README.md
```
