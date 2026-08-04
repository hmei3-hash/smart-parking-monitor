# Smart Parking Monitor — Multi-Node Edge AI System

**Author:** Hongyi Mei

**[▶ Demo video](https://youtu.be/OTSbgnwH_kA)** — three nodes, gateway fusion,
and dashboard running end to end on hardware.

A distributed smart parking monitoring system built on **ESP32-S3-CAM**
nodes with on-device INT8 TFLite Micro inference. Nodes publish JSON
detection results over MQTT to a Raspberry Pi 5 gateway running
Mosquitto, SQLite persistence, and a FastAPI dashboard. The gateway
performs majority-vote fusion across nodes. All vision compute runs on
the edge — the gateway never does inference.

---

## Architecture
Edge Layer ─────── 3× ESP32-S3-CAM
(camera + TFLite INT8 inference + MQTT publish)
│
│ WiFi / MQTT (JSON only)
▼
Gateway Layer ──── Raspberry Pi 5
├── Mosquitto broker
├── Majority-vote decision fusion
├── SQLite persistence (readings, fused, sessions, nodes)
└── FastAPI dashboard (read-only on the same database)

## Current Status

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Single camera bringup (HTTP stream → OpenCV client) | Complete |
| 2 | MQTT pipeline (ESP32 → Mosquitto on RPi) | Complete |
| 3 | Data collection (205 images: ~100 occupied, ~100 empty) | Complete |
| 4 | Model training and INT8 quantization | Complete |
| 5 | [On-device TFLite Micro deployment](docs/phase5-tflite-deployment.md) | Complete |
| 6 | [Three nodes + gateway fusion + persistence](docs/phase6-mqtt-integration.md) | Complete; long-run stability open |
| 7 | FreeRTOS task architecture (dual-core, semaphore, queue) | Not started |
| 8 | Final documentation | Not started |

**Verified on hardware** — see the demo video above. A full-chain run exercises
three nodes publishing inference results, majority-vote fusion on the gateway,
SQLite persistence, session derivation from fused state, and the dashboard
reading that database. Both `empty` and `occupied` states are reached during
the run, so the classifier does discriminate in deployment rather than pinning
to a constant as it did in the first attempt.

**Open items**, tracked so the status above isn't read as more finished than
it is:

- **Long-run stability is unverified.** The demo run is under 10 minutes. The
  earlier failure — two boards rebooting within 14 seconds of each other after
  ~18 minutes of uptime each, pointing at shared supply sag — has not been
  reproduced *or* ruled out, because no run since has been long enough to
  reach that point. Uptime beyond ~20 minutes should be treated as untested.
- **Publish loss is still the 39% figure.** Loss has not been re-measured
  since the run documented in
  [docs/phase6-mqtt-integration.md](docs/phase6-mqtt-integration.md), so the
  per-node numbers there (0% / 23% / 39%) remain the current best estimate.
  Whether the demo run improved on them is unknown.
- **Classifier accuracy in deployment is qualitative only.** The demo shows
  correct transitions in both directions, which the earlier run could not.
  It is not a measured accuracy: no labelled empty/occupied trial set has been
  run against gateway output, so there is no in-situ confusion matrix and no
  false-positive or false-negative rate.
- **No temporal debounce on the gateway**, although
  `gateway/tools/gen_sample_db.py` models one. Live sessions are therefore
  noisier than the sample data suggests.
- **No runtime resource measurements.** Per-task stack high-water marks, heap
  headroom, and CPU utilisation during inference are unmeasured. These are
  needed as a baseline before the Phase 7 task restructure, since without a
  "before" the restructure cannot be shown to have improved anything.

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

**On the 95% held-out figure.** It is optimistic. Images were burst-captured
and then split randomly, so near-duplicate frames from the same burst appear
in both the training and validation sets — the model is scored partly on
frames it has effectively seen. The planned fix is per-session capture with
`GroupShuffleSplit`, so that no session spans the split. Until then, treat 95%
as an upper bound rather than an estimate.

## Phase 5 — On-Device Inference

TFLite Micro deployed on ESP32-S3. 96×96×3 INT8 CNN, 13.8 KB model in flash,
120 KB tensor arena in SRAM, 4.15 s inference, ~14 s worst-case observation
latency. Inference is deliberately unoptimised: parking occupancy changes on a
timescale of minutes, so seconds of latency cost nothing, and reference kernels
keep the build simple. Decision logic (temporal debounce, multi-node vote) is
deferred to the Raspberry Pi gateway; the node publishes raw observations only.

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

**On the simulated accuracy numbers.** The simulator injects a fixed 15%
disagreement rate per node (`DISAGREE_PCT`), so a single simulated node is
correct 85% of the time by construction and the three-node majority vote lands
at 94% — `3(0.85²)(0.15) + 0.85³ = 93.9%`, which the measured result matches.
That confirms the vote logic is implemented correctly, but it says nothing
about the real classifier: the numbers are a design parameter and its
arithmetic consequence, not a measurement of the CNN.

Real-classifier accuracy in deployment remains unquantified — see open items
above.

### MQTT Topics
parking/spot{N}/node{M} node occupancy reports (JSON)
parking/spot{N}/fused fusion result for the spot
parking/status/node{M} node online/offline — retained, set via MQTT Last Will

### Dashboard Data Contract

The SQLite database is the only interface between the gateway and the
dashboard. There is no shared code and no API between them: `fusion.py` writes
through `gateway/db.py`, and `dashboard/` opens the same file read-only. WAL is
enabled, so dashboard reads never block gateway writes.

Defining that boundary up front is what let the dashboard be built in parallel
with hardware that was still unstable:

- **[`gateway/schema.sql`](gateway/schema.sql)** — table definitions, treated
  as a contract. Columns are added, never renamed or removed.
- **[`gateway/tools/gen_sample_db.py`](gateway/tools/gen_sample_db.py)** —
  generates 12 hours of synthetic history structurally identical to what
  `fusion.py` writes, including node dropout and split votes, so the dashboard
  can be developed and demoed without access to the Raspberry Pi.
- **[`docs/dashboard_requirements.md`](docs/dashboard_requirements.md)** — UI
  requirements plus a field-by-field mapping from each requirement to the table
  that satisfies it.

Two coupling points are easy to get wrong and are documented on both sides:

**`dashboard/config.py: OFFLINE_THRESHOLD` must equal `gateway/fusion.py:
STALE_SEC`.** Both answer the same question — how long before a node's report
stops counting. If they disagree, a node can be excluded from the dashboard's
online list while still being counted in `votes_total`, and the page shows two
nodes online alongside a 3/3 vote.

**A tied vote holds the previous state rather than resolving to empty.** Ties
are only reachable with two live nodes, which the demo deliberately induces by
unplugging one. So `votes_occ=1, votes_total=2, occupied=1` is a legitimate
row, and the dashboard labels it rather than rendering an apparent minority
vote.

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
points at shared supply sag rather than firmware. The demo run since then
exercises the full chain in both occupancy states, but is too short to bear on
either issue. Both remain open. Full analysis in
[docs/phase6-mqtt-integration.md](docs/phase6-mqtt-integration.md).

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

Baseline resource measurements are a prerequisite for this phase — see open
items.

## Gateway Features

- **Mosquitto MQTT broker** — central message bus
- **Sliding-window majority vote** — each node's latest reading is kept and
  readings older than `STALE_SEC` are dropped, so a dead node degrades the
  vote from 3 to 2 instead of stalling it
- **Tie hysteresis** — an even split holds the previous state, since a tie is
  absence of new information rather than a vote for empty
- **MQTT Last Will** — node disconnect detection, though `last_seen` is
  treated as authoritative because a node that loses WiFi without a clean TCP
  close stays "online" until the broker keepalive expires
- **SQLite persistence** — raw readings, fused results, derived parking
  sessions, and node health; sessions survive a gateway restart
- **FastAPI dashboard** — real-time status, 10-hour history, node health
- **`inspect_db.py`** — read-only console view, safe to run against a live
  gateway

Not yet implemented: temporal debounce, timeout alerts, violation flagging.

## Hardware BOM

| Component | Qty | Notes |
|-----------|-----|-------|
| ESP32-S3-CAM (N16R8) | 3 | Edge inference nodes |
| Raspberry Pi 5 | 1 | Gateway (Mosquitto, SQLite, FastAPI) |
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
| `dashboard/` | [Web dashboard](dashboard/README.md) | FastAPI + vanilla JS, read-only on the gateway database |
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
pip3 install paho-mqtt --break-system-packages       # PEP 668 on Bookworm
python3 fusion.py /home/pi/parking/parking.db        # subscribe, vote, persist
python3 inspect_db.py --watch                        # read-only live view
```

```bash
# Dashboard, same Pi, separate service
cd dashboard
pip3 install fastapi uvicorn --break-system-packages
PARKING_DB=/home/pi/parking/parking.db python3 main.py
```

Both processes must run as the same user. Reading a WAL database requires
access to the `-shm` index, so a mismatch fails with
`unable to open database file` even when the main file is world-readable.

To develop the dashboard without hardware:

```bash
python3 gateway/tools/gen_sample_db.py               # 12 h of synthetic history
PARKING_DB=./parking_sample.db python3 dashboard/main.py
```

## Milestones — MVP

- [x] Single CAM HTTP stream verified
- [x] MQTT pipeline: ESP32 → Mosquitto → Python subscriber
- [x] Training data collection
- [x] CNN training + INT8 quantization
- [x] Single node edge inference
- [x] Three nodes with gateway decision fusion
- [x] SQLite persistence and session tracking
- [x] Dashboard reading the gateway database
- [x] Full-chain integration run on real hardware, both occupancy states
- [ ] Resolve node supply stability (no run past ~20 min since the reboot issue)
- [ ] Re-measure publish loss
- [ ] Quantify classifier accuracy in situ (labelled trial set → confusion matrix)
- [ ] Baseline runtime resource measurements (stack, heap, CPU)
- [ ] Temporal debounce on the gateway
- [ ] FreeRTOS dual-core task architecture

## Stretch Goals

- [ ] Triangular mutual surveillance
- [ ] FSR pressure sensors
- [ ] Embedded Linux kernel module on gateway
- [ ] Session management with timeout enforcement

## License

MIT
