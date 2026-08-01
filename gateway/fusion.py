# Filename: fusion.py
# Author: Hongyi Mei
# Date: 07/29/2026
# Description: Gateway-side majority-vote fusion for the smart parking
#              monitor. Subscribes to all node reports for a spot, keeps each
#              node's most recent reading, and votes on a fixed interval.
#
#              Sliding-window rather than seq-aligned: real nodes publish on
#              independent timers and will never have matching sequence
#              numbers, so grouping by seq only works for the simulator.
#              Readings older than STALE_SEC are excluded from the vote,
#              which is what makes a dead node degrade the vote instead of
#              stalling it.
#
#              Results are published back to MQTT and persisted to SQLite
#              through db.py, which also derives parking sessions from fused
#              state transitions (see docs/dashboard_requirements.md).
#
#              Usage: python3 fusion.py [db_path]

import json
import sys
import time
import threading
import paho.mqtt.client as mqtt

import db

# ================== Config ====================
MQTT_HOST    = "localhost"
MQTT_PORT    = 1883

# MQTT wildcards must occupy a whole topic level, so "parking/spot+/node+"
# is invalid. This filter also catches parking/status/... and our own
# parking/spotN/fused, both of which are filtered in on_message.
DATA_TOPIC   = "parking/+/+"
RESULT_TOPIC = "parking/spot{}/fused"

VOTE_PERIOD  = 10.0   # seconds between fusion passes
STALE_SEC    = 30.0   # a reading older than this is not counted


# ================== State ====================
# spot_id -> node_id -> {"occupied": bool, "truth": bool|None, "ts": float}
readings = {}
node_status = {}      # node_id -> "online" | "offline"
lock = threading.Lock()

# spot_id -> bool. Last fused state, used both for the tie hysteresis below
# and for edge-triggering session open/close. Seeded from the database at
# startup so a gateway restart does not read as a state change.
last_state = {}

# Accuracy tracking against the simulator's ground truth. Only meaningful
# while nodes publish a "truth" field; real nodes will not.
stats = {"votes": 0, "correct": 0, "node_correct": 0, "node_total": 0}


def on_connect(client, userdata, flags, rc):
    if rc != 0:
        print(f"MQTT connect failed rc={rc}")
        return
    print("MQTT connected")
    client.subscribe(DATA_TOPIC)


def on_message(client, userdata, msg):
    topic = msg.topic

    if topic.startswith("parking/status/"):
        node = topic.rsplit("/", 1)[-1]        # "node1"
        status = msg.payload.decode()
        with lock:
            node_status[node] = status
        print(f"  [status] {node} -> {status}")

        # Topic segment is "nodeN"; the table keys on the integer. A topic
        # that does not match that shape is not ours, so skip the write
        # rather than guess.
        if node.startswith("node") and node[4:].isdigit():
            db.record_node_status(time.time(), int(node[4:]), status)
        return

    # Our own fused output matches the subscription wildcard. Ignore it here
    # rather than narrowing the filter, since node topics are the only other
    # thing under parking/+/+.
    if topic.endswith("/fused"):
        return

    try:
        data = json.loads(msg.payload)
        spot = int(data["spot"])
        node = int(data["node"])
        occ  = bool(data["occupied"])
    except (json.JSONDecodeError, KeyError, ValueError) as e:
        # A malformed payload from one node must not take down the gateway.
        print(f"  [drop] {topic}: {e}")
        return

    truth = bool(data["truth"]) if "truth" in data else None
    now   = time.time()

    with lock:
        readings.setdefault(spot, {})[node] = {
            "occupied": occ,
            "truth": truth,
            "ts": now,
        }

    # Outside the lock: a disk write must not block the next MQTT callback.
    # seq and uptime are optional so the simulator and older firmware still
    # work; missing values land as NULL.
    db.record_reading(now, spot, node, occ,
                      seq=data.get("seq"), uptime=data.get("uptime"))


def fuse(client):
    now = time.time()
    results = []

    with lock:
        for spot in list(readings.keys()):
            fresh = {
                n: r for n, r in readings[spot].items()
                if now - r["ts"] <= STALE_SEC
            }
            stale = set(readings[spot]) - set(fresh)

            if not fresh:
                print(f"spot{spot}: no fresh readings, skipping")
                continue

            votes_occ = sum(1 for r in fresh.values() if r["occupied"])
            total     = len(fresh)

            # Strict majority, with hysteresis on an even split.
            #
            # An even split is only reachable with 2 live nodes. Previously a
            # tie fell through to EMPTY; that made a single misread on one of
            # the two survivors flip the state and cut one parking session
            # into two. Holding the previous state instead means a tie is
            # "no new information" rather than a vote for empty, which is
            # what it actually is.
            #
            # The demo deliberately unplugs a node, so the 2-node path is a
            # normal operating mode here, not a corner case.
            prev = last_state.get(spot)
            tie  = (votes_occ * 2 == total)

            if tie and prev is not None:
                fused = prev
            else:
                # No prior state to hold: fall back to strict majority, which
                # resolves a first-ever tie to EMPTY. Safer default, since a
                # false OCCUPIED would open a session on an empty spot.
                fused = votes_occ * 2 > total

            # Ground truth is only present in simulation.
            truths = [r["truth"] for r in fresh.values() if r["truth"] is not None]
            truth = truths[0] if truths else None

            if truth is not None:
                stats["votes"] += 1
                if fused == truth:
                    stats["correct"] += 1
                for r in fresh.values():
                    stats["node_total"] += 1
                    if r["occupied"] == truth:
                        stats["node_correct"] += 1

            detail = " ".join(
                f"n{n}={'1' if r['occupied'] else '0'}"
                for n, r in sorted(fresh.items())
            )
            line = (f"spot{spot}: {'OCCUPIED' if fused else 'EMPTY':8s} "
                    f"({votes_occ}/{total})  [{detail}]")

            if truth is not None:
                mark = "ok " if fused == truth else "MISS"
                line += f"  truth={'1' if truth else '0'} {mark}"
            if tie:
                line += "  TIE(held)"
            if stale:
                line += f"  stale: {sorted(stale)}"

            # Publishing and writing to disk under the readings lock would
            # stall on_message for the duration of the IO. Collect here,
            # act after the lock is released.
            results.append((spot, fused, votes_occ, total, prev, line))
            last_state[spot] = fused

    for spot, fused, votes_occ, total, prev, line in results:
        payload = json.dumps({
            "spot": spot,
            "occupied": int(fused),
            "votes": f"{votes_occ}/{total}",
            "ts": int(now),
        })
        client.publish(RESULT_TOPIC.format(spot), payload)

        # prev is the state before this pass, so record_fused sees the same
        # edge the hysteresis above did and opens/closes sessions on it.
        event = db.record_fused(now, spot, fused, votes_occ, total, prev)
        if event:
            line += f"  session:{event}"

        print(line)


def report_accuracy():
    if stats["votes"] == 0 or stats["node_total"] == 0:
        return
    fused_acc = 100.0 * stats["correct"] / stats["votes"]
    node_acc  = 100.0 * stats["node_correct"] / stats["node_total"]
    print(f"\n--- accuracy over {stats['votes']} votes ---")
    print(f"  single node : {node_acc:.1f}%")
    print(f"  fused       : {fused_acc:.1f}%\n")


def recover_state():
    """Seed last_state from the database so a gateway restart is not read as
    a state change. Without this, a restart while a spot is occupied would
    see prev=None, treat the first pass as a fresh arrival, and open a second
    session on top of the one still open."""
    for spot in db.open_spots():
        prev = db.last_fused_state(spot)
        if prev is not None:
            last_state[spot] = prev
            print(f"  recovered spot{spot} = {'OCCUPIED' if prev else 'EMPTY'}")


def main():
    db.init(sys.argv[1] if len(sys.argv) > 1 else db.DEFAULT_DB)
    recover_state()

    # paho-mqtt 2.x defaults to a new callback signature. Pinning VERSION1
    # keeps the handlers above valid on both 1.x and 2.x.
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_HOST, MQTT_PORT, 60)
    client.loop_start()

    print(f"fusing every {VOTE_PERIOD}s, stale threshold {STALE_SEC}s\n")

    last_report = time.time()
    try:
        while True:
            time.sleep(VOTE_PERIOD)
            fuse(client)
            if time.time() - last_report > 60:
                report_accuracy()
                last_report = time.time()
    except KeyboardInterrupt:
        report_accuracy()
        db.print_summary()
        client.loop_stop()
        client.disconnect()
        db.close()


if __name__ == "__main__":
    main()
