# Documentation

| Document | Contents |
|----------|----------|
| [setup.md](setup.md) | Toolchain, PlatformIO, N16R8 board configuration |
| [architecture.md](architecture.md) | Node/gateway responsibility split, observation payload |
| [phase5-tflite-deployment.md](phase5-tflite-deployment.md) | On-device INT8 inference: boot loop and train/serve brightness mismatch |
| [phase6-mqtt-integration.md](phase6-mqtt-integration.md) | Multi-node MQTT, fusion, SQLite persistence, and the first full-system run |
| [dashboard_requirements.md](dashboard_requirements.md) | Data contract and UI requirements for the dashboard (teammate scope) |

The two phase write-ups are the substantive ones. Both are structured around
what failed and how it was diagnosed rather than around what the final code
does — in Phase 5 the two root causes were a board-level flash/PSRAM
misconfiguration and a train/serve brightness mismatch, neither of which was
in the ML stack; in Phase 6 the integration passed on the first run, but the
data it produced exposed publish loss and correlated node reboots that the
simulator could not have surfaced.
