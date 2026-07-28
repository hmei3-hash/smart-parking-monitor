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
Edge Layer ─────── 3× ESP32-S3-CAM
                   (camera + TFLite INT8 inference + MQTT publish)
       │
       │  WiFi / MQTT (JSON only)
       ▼
Gateway Layer ──── Raspberry Pi 5
                   ├── Mosquitto broker
                   ├── Majority-vote decision fusion
                   ├── SQLite event log
                   └── Flask web dashboard
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

## Edge AI Model

| Property | Value |
|----------|-------|
| Architecture | Custom CNN: 3× Conv2D+MaxPool → GlobalAveragePooling → Dense |
| Input | 96 × 96 × 3 RGB |
| Parameters | 6 594 |
| Quantization | INT8 (TFLite Micro) |
| Model size | 13.5 KB |
| Inference time | 4.15 s on ESP32-S3 (reference kernels) |
| Tensor arena | 103 552 B used (120 KB allocated) |
| Classes | `empty`, `occupied` |

**ML pipeline:** collect from CAM stream → train in Jupyter → INT8 quantize → export C header → compile into firmware

## Phase 5 — On-Device Inference

Deployed the INT8 parking-occupancy model onto the ESP32-S3-CAM using
TFLite Micro. Bringup was blocked by boot loops traced to a board-level
misconfiguration (incorrect flash size and PSRAM mode for the N16R8
module), not a memory-exhaustion issue as initially hypothesized.

See the full bringup narrative in [docs/phase5-tflite-deployment.md](docs/phase5-tflite-deployment.md).

## Planned FreeRTOS Design

| Core | Task | Role |
|------|------|------|
| 0 | Camera capture + TFLite inference | Queue producer |
| 1 | MQTT publish + status heartbeat (timer) | Queue consumer |

**Synchronization:** FreeRTOS queue for detection results between cores. No shared globals between cores.

## Gateway Features

- **Mosquitto MQTT broker** — central message bus
- **Majority-vote decision fusion** — 2 of 3 nodes agree = final decision
- **MQTT Last Will & Testament** — automatic node disconnect detection
- **SQLite event logging** — persistent occupancy history
- **Flask web dashboard** — real-time parking status
- **Timeout alert / violation flagging**

## Hardware BOM

| Component | Qty | Notes |
|-----------|-----|-------|
| ESP32-S3-CAM (N16R8) | 3 | Edge inference nodes |
| Raspberry Pi 5 | 1 | Gateway (Mosquitto, SQLite, Flask) |
| USB cables | 3 | Power + serial |
| LED + 220 Ω resistor | 3 | Alarm indicator |
| Breadboard + jumpers | — | Prototyping |

FSR pressure sensors are deferred to stretch goals.

## Project Structure

| Folder | Topic | Description |
|--------|-------|-------------|
| `firmware/bringup/` | [Bringup tests](firmware/bringup/README.md) | Staged isolation tests (T1–T4) from Phase 5 |
| `firmware/cam_node/` | CAM node firmware | In development (Phase 6) |
| `firmware/common/` | Shared constants | Common configuration |
| `model/` | [Model artifacts](model/README.md) | Training scripts, INT8 quantized model, dataset notes |
| `gateway/` | Gateway server | Mosquitto + Flask dashboard (Phase 6) |
| `docs/` | Documentation | Setup guides, deployment notes |

## Build & Flash

```bash
git clone https://github.com/hmei3-hash/smart-parking-monitor.git
cd smart-parking-monitor

# Build a bringup test (e.g., T4 camera + inference)
cd firmware/bringup/t4_camera_tflite
pio run
pio run --target upload && pio device monitor
```

## Milestones — MVP

- [x] Single CAM HTTP stream verified
- [x] MQTT pipeline: ESP32 → Mosquitto → Python subscriber
- [x] Training data collection
- [x] CNN training + INT8 quantization
- [x] Single node edge inference
- [ ] Three nodes with gateway decision fusion
- [ ] FreeRTOS dual-core task architecture

## Stretch Goals

- [ ] Triangular mutual surveillance
- [ ] FSR pressure sensors
- [ ] Embedded Linux kernel module on gateway
- [ ] Session management with timeout enforcement

## License

MIT
