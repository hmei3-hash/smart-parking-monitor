# Phase 6 — MQTT Integration, Multi-Node Fusion, and Persistence

**Author:** Hongyi Mei
**Date:** 2026-08-01
**Status:** Embedded and gateway layers functional end-to-end. Dashboard in
development by a teammate against a published data contract.

---

## 1. Scope

Phase 6 connects the three pieces that until now had only been validated
separately:

- Phase 5 proved on-device inference on one node, printing to serial.
- `node-sim/` proved the MQTT path, majority-vote fusion, and Last Will
  node-health detection — with no camera and no inference.

Phase 6 merges them and adds the persistence layer the dashboard reads from.

```
3× ESP32-S3-EYE            Raspberry Pi 5
capture → INT8 CNN  ──MQTT──►  Mosquitto ──► fusion.py ──► SQLite
                                              majority vote   readings
                                              session state   fused
                                                              sessions
                                                              nodes
                                                                 │
                                                                 ▼
                                                          dashboard (teammate,
                                                          read-only, in dev)
```

---

## 2. What was built

### Node firmware — [`firmware/node/`](../firmware/node/)

The merge was mostly mechanical, with three changes that were not:

| Change | Why |
|--------|-----|
| `runDetection()` returns `int`, not `void` | Lets `loop()` skip publishing on a capture or `Invoke()` failure instead of sending a stale result |
| `loop()` uses `millis()` instead of `delay()` | The Phase 5 loop ended in `delay(INFER_PERIOD)`, which would block `mqtt.loop()` for the entire interval and stall keepalive |
| MQTT keepalive 15 s → 60 s | One cycle is `INFER_PERIOD` (10 s) + ~4 s `Invoke()`. The 15 s default leaves no margin and the broker drops the connection mid-inference |

Camera and model initialise before WiFi: the WiFi stack allocates ~100 KB of
internal SRAM, and the tensor arena is a 120 KB static buffer. Bringing WiFi
up first risks a heap layout where the arena no longer fits.

### Gateway persistence — [`gateway/db.py`](../gateway/db.py), [`gateway/schema.sql`](../gateway/schema.sql)

All SQL is isolated in `db.py`; `fusion.py` owns voting policy only. The split
is not cosmetic: `sessions` is derived state, and a session row is only
correct if it was opened and closed by the same transition logic that wrote
the `fused` rows around it. Keeping both in `record_fused()` makes that
invariant enforceable rather than something the caller must remember.

Four tables — `readings` (raw per-node reports), `fused` (one row per vote
pass), `sessions` (derived from fused transitions), `nodes` (current health).
Timestamps are gateway receive time throughout: Phase 6 node firmware has no
RTC and no NTP, so a node cannot stamp its own reports.

### Fusion change — tie hysteresis

Previously an even split resolved to EMPTY. An even split is only reachable
with two live nodes, and the demo script deliberately unplugs a node, so the
two-node path is a normal operating mode rather than a corner case. Under the
old rule a single misread on one of the two survivors flipped the state and
cut one parking session into two.

Ties now hold the previous state, which is what a tie actually means — no new
information, rather than a vote for empty.

Replaying a full sequence (three nodes agree → node 3 unplugged → two ties →
vehicle leaves):

```
[1,1,1] fused=1  session open
[1,0]   fused=1                  ← tie, held
[0,1]   fused=1                  ← tie, held
[0,0]   fused=0  session close

sessions = 1        (was 3 under the old rule)
```

### Read-only inspection — [`gateway/inspect_db.py`](../gateway/inspect_db.py)

Opens the database `mode=ro` so it can run against a live gateway without
contending for the write lock. Beyond dumping rows it flags three conditions
that are invisible in the raw tables: a stale last vote (fusion.py died — the
data still looks plausible, it just stops advancing), `votes_total < 3`
(degraded vote, otherwise indistinguishable from a healthy one), and a node
whose `last_seen` is stale while `status` still reads `online`.

---

## 3. First full-system run — measured

A 7-minute run with all three nodes, gateway, and persistence live.
78 readings, 33 fusion passes, 1 session. **The link works end to end.** The
data it produced is also the most useful output of this phase, because it
surfaced three problems that neither the simulator nor single-node testing
could have.

### 3.1 Publish loss — up to 39%

| Node | Received | Expected | Loss |
|------|----------|----------|------|
| node1 | 18 | 18 | 0% |
| node2 | 33 | 43 | 23% |
| node3 | 27 | 44 | 39% |

Median inter-arrival is 10 s on all three nodes, matching `INFER_PERIOD`, so
the firmware timer is not drifting. The loss is concentrated in outages:
node2 lost 10 consecutive publishes (`seq 5 → 16`, a 111 s silence) and node3
had a 160 s gap. The `seq` field made this measurable — without it the loss
would have looked like normal jitter.

### 3.2 Node reboots, and they correlate

```
05:06:27  node3  uptime 1132 → 14    reboot
05:06:41  node2  uptime 1094 → 14    reboot
05:07:01  node1  offline
05:09:14  node1  offline
```

`node_events` recorded 14 transitions in 7 minutes. node2 and node3 rebooted
within 14 seconds of each other after ~18 minutes of uptime each. Two
independent boards failing near-simultaneously points at a shared cause, not
at firmware: most likely supply sag on the shared USB hub under combined
camera and WiFi-transmit peak current, with an AP-side glitch as the
alternative.

This is unresolved and is the top priority. It also contaminates the other
two findings — measuring classifier accuracy over a link that keeps dropping
is not worth doing first.

### 3.3 Every reading was `occupied=1`

All 78 readings from all three nodes agreed on occupied, across the whole run.
The resulting "100% inter-node agreement" is therefore vacuous: three nodes
agreeing on a constant is not evidence that the classifier discriminates.

Either the spot was genuinely occupied for the full 7 minutes, or this is the
train/serve brightness saturation from Phase 5 recurring. Distinguishing them
requires a run with the vehicle removed, watching for `occupied=0` in
`readings` and for `channel mean (int8)` drifting off the `+16/+24/+15`
training reference on serial. Not yet done.

### 3.4 What did work

15 of 33 fusion passes ran at `votes_total = 2`. The gateway correctly
excluded stale nodes and kept voting instead of stalling — the degraded path
was exercised accidentally and behaved as designed. Session open/close,
Last Will, and restart recovery all held.

---

## 4. Known gaps

**Supply stability (blocking).** See 3.2. Next step is powering the three
boards from independent supplies rather than one hub, or adding bulk
capacitance on 5 V, then re-running and counting `node_events`.

**Classifier not validated in situ.** See 3.3. No `empty` observation exists
in the production dataset yet.

The ~85% / ~94% figures quoted elsewhere are not measurements of the
classifier at all. `node-sim` injects a fixed 15% per-node disagreement rate,
so 85% is that parameter by construction and 94% is the arithmetic of a
three-way majority vote over it — `3(0.85²)(0.15) + 0.85³ = 93.9%`. Matching
that number confirms the vote logic is correct and nothing more. The CNN's own
held-out INT8 accuracy is ~95%; its accuracy in deployment remains unmeasured.

**No temporal debounce.** `architecture.md` assigns debounce to the gateway
and it is not implemented. Tie hysteresis covers the two-node case, but with
all three nodes healthy a single-frame flip still changes state immediately.

Note that `gateway/tools/gen_sample_db.py` *does* debounce
(`DEBOUNCE_ROUNDS = 3`). The sample database the dashboard is being developed
against therefore has cleaner sessions than the live gateway currently
produces. This is a real contract mismatch and should be closed by
implementing debounce in `fusion.py` rather than by removing it from the
generator.

**Node clocks.** Nodes carry no time source, so all timestamps are gateway
receive time. Acceptable at a 10 s cadence on a LAN, but it means per-node
latency cannot be measured.

**Schema migration.** `schema.sql` is replayed on every startup and every
statement is `IF NOT EXISTS`, which creates but never alters. The `seq` and
`uptime` columns added this phase will not appear in a `parking.db` created
before them; such a file must be deleted and recreated.

---

## 5. Team split and current status

The embedded and gateway layers are mine and are functionally complete for
the MVP. The dashboard is a teammate's scope and is in active development.

To let that work proceed in parallel rather than waiting on hardware, two
things were published as a contract:

- [`docs/dashboard_requirements.md`](dashboard_requirements.md) — UI
  requirements, the demo script, and a field-by-field mapping from each
  requirement to the table that satisfies it. Explicitly scoped: everything
  from SQLite outward is the dashboard's; MQTT, firmware, and fusion are not.
- [`gateway/tools/gen_sample_db.py`](../gateway/tools/gen_sample_db.py) —
  generates 12 hours of synthetic history structurally identical to what
  `fusion.py` writes, including node dropout and split votes, so the
  dashboard can be built and demoed without access to the live Pi.

The database is opened read-only (`file:...?mode=ro`) from the dashboard side
and WAL is enabled, so dashboard reads never block gateway writes.

| Layer | Owner | Status |
|-------|-------|--------|
| Node firmware (capture, inference, MQTT) | me | Complete |
| Gateway fusion + persistence | me | Complete |
| Data contract (schema, sample DB, requirements) | me | Published |
| Dashboard backend / frontend | teammate | In development |
| Supply-stability fix | me | Open — blocking |
| Temporal debounce | me | Open |
| In-situ classifier validation | me | Open |

---

## 6. Reproducing

```bash
# Gateway (Raspberry Pi)
cd gateway
pip3 install paho-mqtt
python3 fusion.py                      # writes gateway/parking.db
python3 inspect_db.py --watch          # read-only live view, separate shell

# Nodes
cd firmware/node
cp src/secrets.h.example src/secrets.h
pio run -e node1 -t upload             # repeat for node2, node3
```

Dashboard development without hardware:

```bash
python3 gateway/tools/gen_sample_db.py     # 12 h of synthetic history
```
