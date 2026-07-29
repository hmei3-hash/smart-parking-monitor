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
#              Observation only at this stage: results are published back to
#              MQTT and printed, but not persisted. SQLite writes are the
#              next step (see docs/dashboard_requirements.md).

import json
import time
import threading
import paho.mqtt.client as mqtt

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
        node = topic.rsplit("/", 1)[-1]
        with lock:
            node_status[node] = msg.payload.decode()
        print(f"  [status] {node} -> {msg.payload.decode()}")
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

    with lock:
        readings.setdefault(spot, {})[node] = {
            "occupied": occ,
            "truth": truth,
            "ts": time.time(),
        }


def fuse(client):
    now = time.time()

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

            # Strict majority. An even split (possible with 2 live nodes)
            # falls through to EMPTY, the safer default here: a false
            # "occupied" would start a billing session on an empty spot.
            #
            # KNOWN ISSUE: with only 2 live nodes this makes a single
            # misread flip the state, which splits one parking session in
            # two. Hysteresis (hold previous state on a tie) is the likely
            # fix - see the sample-data generator where this shows up.
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

            payload = json.dumps({
                "spot": spot,
                "occupied": int(fused),
                "votes": f"{votes_occ}/{total}",
                "ts": int(now),
            })
            client.publish(RESULT_TOPIC.format(spot), payload)

            detail = " ".join(
                f"n{n}={'1' if r['occupied'] else '0'}"
                for n, r in sorted(fresh.items())
            )
            line = (f"spot{spot}: {'OCCUPIED' if fused else 'EMPTY':8s} "
                    f"({votes_occ}/{total})  [{detail}]")

            if truth is not None:
                mark = "ok " if fused == truth else "MISS"
                line += f"  truth={'1' if truth else '0'} {mark}"
            if stale:
                line += f"  stale: {sorted(stale)}"

            print(line)


def report_accuracy():
    if stats["votes"] == 0 or stats["node_total"] == 0:
        return
    fused_acc = 100.0 * stats["correct"] / stats["votes"]
    node_acc  = 100.0 * stats["node_correct"] / stats["node_total"]
    print(f"\n--- accuracy over {stats['votes']} votes ---")
    print(f"  single node : {node_acc:.1f}%")
    print(f"  fused       : {fused_acc:.1f}%\n")


def main():
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
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
