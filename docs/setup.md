# Setup Guide

**Author:** Hongyi Mei
**Date:** 2026-07-28

---

## Build System

**PlatformIO** with the **Arduino framework**. Not ESP-IDF.

Each bringup test under `firmware/bringup/` is an independently flashable
PlatformIO project. Run them in order T1 → T2 → T3 → T4 to verify the
hardware before running inference.

## Board: ESP32-S3-DevKitC-1 N16R8

16 MB flash, 8 MB octal PSRAM.

Required `platformio.ini` settings:

```ini
[env:esp32s3]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino

board_build.arduino.memory_type = qio_opi
board_build.flash_mode = qio
board_build.psram_type = opi
board_upload.flash_size = 16MB
board_upload.maximum_size = 16777216
board_build.partitions = default_16MB.csv

build_flags =
    -DBOARD_HAS_PSRAM
```

### Why These Settings Matter

`CONFIG_SPIRAM_*` build flags do **not** work under the Arduino framework.
The Arduino core ships precompiled, so those macros never reach the IDF
build system. Only `board_build.arduino.memory_type` changes the PSRAM
mode.

Setting `flash_size` incorrectly (e.g., leaving it at the default 4 MB)
causes a boot loop with no serial output at all — the same symptom as SRAM
exhaustion, which misdirected debugging for weeks. See
[phase5-tflite-deployment.md](phase5-tflite-deployment.md).

## Building and Flashing

```bash
# Install PlatformIO CLI
pip install platformio

# Build and flash a bringup test (e.g., T4)
cd firmware/bringup/t4_camera_tflite
pio run --target upload && pio device monitor
```

## Data Collection

Connect to the ESP32-S3-CAM's HTTP stream and run:

```bash
cd model/training
python test.py          # verify stream connectivity first
python collect.py       # save labeled frames
```

Controls: Space = save frame, `o` = label occupied, `e` = label empty,
`q` = quit. Edit `STREAM_URL` in each script to match your camera's IP.

## Model Training

Open `model/training/colab.ipynb` in Jupyter or VS Code. The notebook
trains the CNN, quantizes to INT8, and exports both `parking_model.tflite`
and `parking_model.h`.

Requirements: Python 3.8+, TensorFlow/Keras. Training runs locally (not
Colab, despite the notebook name).

See [model/README.md](../model/README.md) for architecture details and
known dataset issues.
