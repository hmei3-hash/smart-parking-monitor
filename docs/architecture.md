# Architecture

**Author:** Hongyi Mei
**Date:** 2026-07-28

---

## Responsibility Split

```
Node (ESP32-S3)    capture, inference, publish raw observation
Gateway (RPi)      temporal debounce, multi-node majority vote,
                   persistence, alerting
```

Keeping the node stateless means debounce windows and confidence thresholds
are gateway configuration rather than firmware constants, and temporal fusion
sits alongside the spatial fusion the gateway already performs. The cost is
that every frame is published rather than only state changes; at a 10 s
period and a sub-100-byte payload this is negligible. If deep-sleep low-power
operation is added later, publishing only on change would become worthwhile
and the debounce may need to move back to the node.

---

## MQTT Observation Payload (Planned)

Topic: `parking/<node_id>/observation`

```json
{
  "node": "node1",
  "seq": 142,
  "occupied": true,
  "p_empty": 0.016,
  "p_occupied": 0.996,
  "infer_ms": 4152,
  "mean_rgb": [16, 24, 15]
}
```

**This payload is planned, not implemented. MQTT integration is Phase 6.**

Dequantized scores are included so the gateway can weight by confidence
rather than treating every vote equally. `mean_rgb` is included so the
gateway can detect exposure drift on a node — the failure mode that caused
constant OCCUPIED output during bring-up — without needing image data.
