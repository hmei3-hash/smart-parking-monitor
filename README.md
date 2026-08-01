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
| 6 | [Three nodes + gateway fusion + persistence](docs/phase6-mqtt-integration.md) | Embedded complete; dashboard in development |
| 7 | FreeRTOS task architecture (dual-core, semaphore, queue) | Not started |
| 8 | Final documentation | Not started |

Phase 6 runs end to end: three nodes publish inference results over MQTT, the
gateway votes and writes to SQLite, and parking sessions are derived from the
fused state. The first full-system run also surfaced publish loss of up to
39% and correlated node reboots that are not yet resolved — measurements and
analysis in [docs/phase6-mqtt-integration.md](docs/phase6-mqtt-integration.md).

## Edge AI Model

| Property | Value |
|----------|-------|
| Architecture | Custom CNN: 3× Conv2D+MaxPool → GlobalAveragePooling → Dense |
| Input | 96 × 96 × 3 RGB |
| Parameters | 6 594 |
| Quantization | INT8 (TFLite Micro) |
| Model size | 13.8 KB |
| Inference time | 4.15 s on ESP32-S3 (reference kernels) |
| Tensor arena | 103 552 B used (120 KB allocated) |
| Classes | `empty`, `occupied` |

**ML pipeline:** collect from CAM stream → train in Jupyter → INT8 quantize → export C header → compile into firmware

## Phase 5 — On-Device Inference

TFLite Micro deployed on ESP32-S3. 96×96×3 INT8 CNN, 13.8 KB model in flash,
120 KB tensor arena in SRAM, 4.15 s inference, ~14 s worst-case observation
latency. Decision logic (temporal debounce, multi-node vote) is deferred to
the Raspberry Pi gateway; the node publishes raw observations only.

Two root causes were found and fixed during bring-up: a board-level
flash/PSRAM misconfiguration causing a boot loop, and a train/serve brightness
mismatch causing constant-OCCUPIED output. Neither was in the ML stack. See
[firmware/bringup/](firmware/bringup/) and
[docs/phase5-tflite-deployment.md](docs/phase5-tflite-deployment.md).

## Simulated Nodes and Gateway Fusion

Gateway development was decoupled from the still-blocked camera/TFLite
inference path. `node-sim/` publishes simulated occupancy over MQTT with no
camera and no inference, which let the broker, the majority-vote fusion, and
the Last Will node-health path all be built and validated while the inference
bug remained open.

- **`node-sim/`** — three ESP32-S3 boards, one PlatformIO env each
  (`pio run -e node1 -t upload`). Ground truth is derived from NTP epoch time
  so all three nodes agree, then each node flips its own report at a fixed
  rate to produce genuine 2–1 splits for the gateway to resolve. A
  single-board variant in `node-sim/variants/` runs the three nodes as three
  FreeRTOS tasks, used while hardware for three separate boards was
  unavailable.
- **`gateway/fusion.py`** — sliding-window majority vote. Keeps each node's
  latest reading and votes on a fixed interval; readings older than the stale
  threshold are dropped from the vote, so a dead node degrades the result
  instead of stalling it. Sliding-window rather than sequence-aligned because
  real nodes publish on independent timers and never share sequence numbers.

**Measured accuracy: a single node classifies correctly ~85% of the time;
the three-node majority vote reaches ~94%.** That gain is the reason for the
multi-node architecture.

### MQTT Topics

```
parking/spot{N}/node{M}    node occupancy reports (JSON)
parking/spot{N}/fused      fusion result for the spot
parking/status/node{M}     node online/offline — retained, set via MQTT Last Will
```

### Dashboard Data Contract

`gateway/schema.sql` defines the SQLite persistence layer, and
`gateway/tools/gen_sample_db.py` generates 12 hours of synthetic history so the
dashboard can be developed in parallel against a realistic database. The
dashboard itself is a teammate's scope and is in active development; this repo
defines only the data contract. See
[docs/dashboard_requirements.md](docs/dashboard_requirements.md) for the field
reference and UI requirements.

`fusion.py` writes live results through `gateway/db.py`, which also derives
parking sessions from fused state transitions. The database is opened
read-only from the dashboard side and WAL is enabled, so dashboard reads never
block gateway writes.

## Phase 6 — Multi-Node Integration

Node firmware in [`firmware/node/`](firmware/node/) merges the Phase 5
inference path with the MQTT layer proven in `node-sim/`. Three ESP32-S3
boards, one PlatformIO env each, `NODE_ID` set as a build flag.

An even split in the vote holds the previous state rather than resolving to
EMPTY. A tie is only reachable with two live nodes, and the demo deliberately
unplugs one, so it is a normal operating mode — under the old rule a single
misread on one survivor cut one parking session into two.

The first full-system run worked end to end and produced measurements worth
more than the passing result: up to 39% publish loss, and node2/node3
rebooting within 14 seconds of each other after ~18 minutes of uptime, which
points at shared supply sag rather than firmware. Both are open. Full analysis
in [docs/phase6-mqtt-integration.md](docs/phase6-mqtt-integration.md).

Inspect a live or sample database with:

```bash
python3 gateway/inspect_db.py --watch
```

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
| `firmware/node/` | [Node firmware](firmware/node/README.md) | Phase 6 production firmware: capture, inference, MQTT publish |
| `firmware/bringup/` | [Bringup tests](firmware/bringup/README.md) | Staged isolation tests (T1–T5) from Phase 5 |
| `model/` | [Model artifacts](model/README.md) | Training scripts, INT8 quantized model, dataset notes |
| `node-sim/` | Simulated nodes | Three MQTT publishers with no inference, for gateway bring-up |
| `gateway/` | [Gateway server](gateway/README.md) | Fusion, SQLite persistence, sample-data generator |
| `docs/` | Documentation | Setup guides, deployment notes, phase write-ups |

## Build & Flash

```bash
git clone https://github.com/hmei3-hash/smart-parking-monitor.git
cd smart-parking-monitor

# Production node firmware (one env per board)
cd firmware/node
cp src/secrets.h.example src/secrets.h    # SSID, password, broker IP
pio run -e node1 -t upload && pio device monitor

# Or a bringup isolation test (e.g. T4 camera + inference)
cd firmware/bringup/t4_camera_tflite
pio run -t upload
```

```bash
# Gateway, on the Raspberry Pi
cd gateway
pip3 install paho-mqtt
python3 fusion.py                 # subscribes, votes, writes parking.db
python3 inspect_db.py --watch     # read-only live view, separate shell
```

## Milestones — MVP

- [x] Single CAM HTTP stream verified
- [x] MQTT pipeline: ESP32 → Mosquitto → Python subscriber
- [x] Training data collection
- [x] CNN training + INT8 quantization
- [x] Single node edge inference
- [x] Three nodes with gateway decision fusion
- [x] SQLite persistence and session tracking
- [ ] Dashboard (teammate — in development)
- [ ] Resolve node supply stability / publish loss
- [ ] FreeRTOS dual-core task architecture

## Stretch Goals

- [ ] Triangular mutual surveillance
- [ ] FSR pressure sensors
- [ ] Embedded Linux kernel module on gateway
- [ ] Session management with timeout enforcement

## License

MIT
