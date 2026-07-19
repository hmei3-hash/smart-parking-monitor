# Smart Parking Monitor — Multi-Node Edge AI System

A distributed smart parking monitoring system built on **ESP32-S3-CAM** nodes with on-device AI inference, pressure-sensor fusion, and triangular mutual surveillance for tamper detection.

> **Course:** UW EE 475 / CSE 475 — Embedded Systems Capstone

---

## Architecture

```
Sensing Layer ──── 3x ESP32-S3-CAM + per-spot FSR pressure sensors
       │
       ├── ESP-NOW  (inter-node heartbeat & alert relay)
       ├── WiFi     (image upload & session data)
       ▼
Gateway Layer ──── 1x ESP32 DevKit (aggregation + rule engine)
       │
       ├── MQTT
       ▼
Cloud / Server ─── Database · Rule Engine · Violation Logs
       │
       ▼
Applications ───── Manager Dashboard · User App
```

## Features

- **Edge AI vehicle detection** — MobileNet-SSD / YOLO-lite (INT8, TFLite Micro) running entirely on ESP32-S3, triggered by pressure events
- **Sensor fusion** — pressure sensor ground truth cross-validated with vision; discrepancies logged and flagged
- **Triangular mutual surveillance** — three CAM nodes arranged so each monitors the other two; lost heartbeat + person detection = tampering alert
- **Parking session management** — automatic session tracking, time-based enforcement, soft reminders, violation flagging
- **Soft vehicle ID** — color histogram + bounding box features for re-identification without license plate dependency

## Firmware Design

Each CAM node runs **FreeRTOS** on the ESP32-S3 dual-core architecture:

| Task | Core | Description |
|------|------|-------------|
| Pressure Sensor Polling | 0 | FSR sampling, occupancy event generation |
| Image Capture & Inference | 0 | Camera trigger, TFLite Micro inference |
| ESP-NOW Heartbeat | 1 | Peer health monitoring, alert relay |
| WiFi Upload & MQTT | 1 | Session data + image upload to gateway |

**Synchronization:** binary semaphore (pressure → capture), queue (inference → upload), mutex (shared feature buffer).

## Hardware

| Component | Qty | Notes |
|-----------|-----|-------|
| ESP32-S3-CAM | 3 | Edge inference nodes |
| ESP32 DevKit | 1 | Gateway |
| FSR pressure sensor | 4 | Per-spot occupancy |
| Breadboard, jumpers, LEDs, resistors | — | Prototyping |

**Estimated BOM: ~$110**

## Build & Flash

```bash
# Clone
git clone https://github.com/<username>/smart-parking-monitor.git
cd smart-parking-monitor

# Build with ESP-IDF (v5.x)
cd firmware/cam_node
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

## Project Structure

```
smart-parking-monitor/
├── firmware/
│   ├── cam_node/          # CAM node firmware (FreeRTOS tasks)
│   ├── gateway/           # Gateway aggregation + MQTT
│   └── common/            # Shared drivers & utilities
├── model/
│   ├── training/          # Model training scripts
│   └── tflite/            # Quantized INT8 model artifacts
├── gateway_server/        # MQTT broker + rule engine
├── app/                   # Manager & User app
├── docs/                  # Setup guides, calibration, architecture
└── README.md
```

## License

MIT
