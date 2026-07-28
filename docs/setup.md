# Setup Guide

**Author:** Hongyi Mei
**Date:** 2026-07-28

---

## Prerequisites

- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation.html)
  or the PlatformIO IDE extension for VS Code
- Python 3.8+ (for data collection and training scripts)
- ESP32-S3-WROOM-1 N16R8 module (16 MB flash, 8 MB octal PSRAM)

## Board Configuration

The N16R8 module requires specific PlatformIO settings. These are already
set in each bringup test's `platformio.ini`:

```ini
board_build.arduino.memory_type = qio_opi
board_build.flash_mode = qio
board_build.psram_type = opi
board_upload.flash_size = 16MB
board_upload.maximum_size = 16777216
board_build.partitions = default_16MB.csv

build_flags =
    -DBOARD_HAS_PSRAM
```

Getting these wrong causes boot loops. See
[Phase 5 deployment notes](phase5-tflite-deployment.md) for the full
root-cause analysis.

## Building and Flashing

```bash
# Example: build and flash the T4 camera + inference test
cd firmware/bringup/t4_camera_tflite
pio run --target upload && pio device monitor
```

All bringup tests (T1–T4) follow the same pattern. See
[firmware/bringup/README.md](../firmware/bringup/README.md) for what each
test does.

## Data Collection

Connect to the ESP32-S3-CAM's HTTP stream and run:

```bash
cd model/training
python collect.py
```

Controls: Space = save frame, `o` = label occupied, `e` = label empty,
`q` = quit. Edit `STREAM_URL` in the script to match your camera's IP.

Use `test.py` to verify stream connectivity before collecting.

## Model Training

Open `model/training/colab.ipynb` in Jupyter or VS Code. The notebook
trains the CNN, quantizes to INT8, and exports both `parking_model.tflite`
and `parking_model.h`. See [model/README.md](../model/README.md) for
details on the architecture and known dataset leakage issue.
