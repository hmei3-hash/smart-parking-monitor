# Smart Parking Monitor — Multi-Node Edge AI System

**Author:** Hongyi Mei

A distributed smart parking monitoring system built on **ESP32-S3-CAM**
nodes with on-device INT8 TFLite Micro inference. Nodes publish JSON
detection results over MQTT to a Raspberry Pi 5 gateway running
Mosquitto, SQLite event logging, and a Flask dashboard. The gateway
performs majority-vote fusion across nodes. All vision compute runs on
the edge — the gateway never does inference.

---

## Architecture

```
Sensing Layer ──── 3× ESP32-S3-CAM (on-device INT8 inference)
       │
       ├── WiFi / MQTT  (JSON detection results)
       ▼
Gateway Layer ──── Raspberry Pi 5
                   ├── Mosquitto broker
                   ├── SQLite event log
                   ├── Flask dashboard
                   └── Majority-vote fusion
```

## Current Status

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Single camera bringup (HTTP stream → OpenCV client) | Complete |
| 2 | MQTT pipeline (ESP32 → Mosquitto on RPi) | Complete |
| 3 | Data collection (205 images: ~100 occupied, ~100 empty) | Complete |
| 4 | Model training and INT8 quantization | Complete |
| 5 | [On-device TFLite Micro deployment](docs/phase5-tflite-deployment.md) | Complete |
| 6 | Three nodes + gateway fusion (MQTT integration, majority vote) | Not started |
| 7 | FreeRTOS task architecture (dual-core, semaphore, queue) | Not started |
| 8 | Final documentation | Not started |

## Phase 5 — On-Device Inference

Deployed the INT8 parking-occupancy model onto the ESP32-S3-CAM using
TFLite Micro. Bringup was blocked by boot loops traced to a board-level
misconfiguration (incorrect flash size and PSRAM mode for the N16R8
module), not a memory-exhaustion issue as initially hypothesized.

| Metric             | Value          |
|--------------------|----------------|
| Model size (flash) | 13.5 KB        |
| Tensor arena used  | 103 552 B (120 KB allocated) |
| Inference latency  | 4.15 s         |

See the full bringup narrative in [docs/phase5-tflite-deployment.md](docs/phase5-tflite-deployment.md).

## Project Structure

| Folder | Topic | Description |
|--------|-------|-------------|
| `firmware/bringup/` | [Bringup tests](firmware/bringup/README.md) | Staged isolation tests (T1–T4) from Phase 5 |
| `model/` | [Model artifacts](model/README.md) | Training scripts, INT8 quantized model, dataset notes |
| `gateway_server/` | Gateway server | Mosquitto + Flask dashboard (Phase 6, not yet started) |
| `docs/` | Documentation | Setup guides, deployment notes |
| `app/` | Application | Dashboard app (future) |

## Build & Flash

```bash
git clone https://github.com/hmei3-hash/smart-parking-monitor.git
cd smart-parking-monitor

# Build a bringup test (e.g., T4 camera + inference)
cd firmware/bringup/t4_camera_tflite
pio run
pio run --target upload && pio device monitor
```

## Hardware

| Component | Qty | Notes |
|-----------|-----|-------|
| ESP32-S3-CAM (N16R8) | 3 | Edge inference nodes |
| Raspberry Pi 5 | 1 | Gateway (Mosquitto, SQLite, Flask) |
| Breadboard, jumpers, LEDs, resistors | — | Prototyping |

FSR pressure sensors are deferred to stretch goals.

## License

MIT
