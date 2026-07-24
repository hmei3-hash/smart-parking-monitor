# Smart Parking Monitor — Multi-Node Edge AI System

A distributed smart parking monitoring system using **ESP32-S3-CAM** nodes with on-device TFLite Micro inference, a **Raspberry Pi** gateway running Mosquitto MQTT broker, SQLite event logging, and a Flask web dashboard. Designed around an edge-first principle: nodes publish only JSON detection results via MQTT — the gateway never performs vision compute.

---

## Architecture

```
Edge Layer ─────── 3x ESP32-S3-CAM
                   (camera + TFLite INT8 inference + FreeRTOS + MQTT publish)
       │
       │  WiFi / MQTT (JSON only)
       ▼
Gateway Layer ──── 1x Raspberry Pi
                   (Mosquitto broker + majority-vote decision fusion
                    + SQLite event log + Flask web dashboard)
```

## Edge AI Model

| Property | Value |
|----------|-------|
| Architecture | Custom CNN: 3x Conv2D+MaxPool → GlobalAveragePooling → Dense |
| Input | 96x96x3 RGB |
| Parameters | ~5,000 |
| Quantization | INT8 (TFLite Micro) |
| Model size | ~20–50 KB |
| Inference time | ~100–300 ms on ESP32-S3 |
| Classes | `empty`, `occupied` |

**ML pipeline:** collect from CAM stream → train in Colab → INT8 quantize → export C header → compile into firmware

## Firmware Design

Each CAM node runs **FreeRTOS** on the ESP32-S3 dual-core architecture:

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
| ESP32-S3-CAM | 3 | Edge inference nodes |
| Raspberry Pi | 1 | Gateway |
| USB cables | 3 | Power + serial |
| LED + 220Ω resistor | 3 | Alarm indicator |
| Breadboard + jumpers | — | Prototyping |

## Project Structure

```
smart-parking-monitor/
├── firmware/
│   ├── cam_node/            # ESP32 Arduino firmware
│   │   ├── CamNode.ino
│   │   └── parking_model.h  # auto-generated TFLite model
│   └── common/              # shared constants
├── model/
│   ├── collect_data.py
│   ├── train_and_quantize.py
│   └── export_header.py
├── gateway/
│   ├── gateway.py           # MQTT subscriber + fusion
│   ├── dashboard.py         # Flask
│   ├── db.py                # SQLite
│   └── templates/
├── docs/
│   ├── setup.md
│   ├── architecture.md
│   └── calibration.md
└── README.md
```

## Milestones — MVP

- [x] Single CAM HTTP stream verified
- [x] MQTT pipeline: ESP32 → Mosquitto → Python subscriber
- [ ] Training data collection
- [ ] CNN training + INT8 quantization
- [ ] Single node edge inference + MQTT publish
- [ ] Three nodes with gateway decision fusion
- [ ] FreeRTOS dual-core task architecture

## Stretch Goals

- [ ] Triangular mutual surveillance
- [ ] Flask web dashboard
- [ ] FSR pressure sensors
- [ ] Embedded Linux kernel module on gateway
- [ ] Session management with timeout enforcement

## Skills Demonstrated

- Edge AI deployment (TFLite Micro INT8 on MCU)
- Multi-node embedded system design (WiFi, MQTT)
- FreeRTOS multi-task scheduling with inter-core sync
- Embedded Linux gateway (Mosquitto, SQLite, Flask)
- Full-stack integration (MCU → Linux gateway → web dashboard)
- ML pipeline (collection → training → quantization → deployment)

## License

MIT
