# Filename: gen_sample_db.py
# Author: Hongyi Mei
# Date: 07/29/2026
# Description: Generates a sample parking.db for dashboard development.
#              Produces 12 hours of synthetic history matching the real
#              gateway's write pattern, so the dashboard can be built and
#              tested without access to the live Raspberry Pi.
#
#              The data is synthetic but structurally identical to what
#              fusion.py writes: same tables, same cadence, same failure
#              modes (node dropout, split votes, occasional misreads).

import sqlite3
import random
import time

DB_PATH        = "parking_sample.db"

HOURS          = 12
NODE_PERIOD    = 10        # seconds between node reports
VOTE_PERIOD    = 10        # seconds between fusion passes
STALE_SEC      = 30        # readings older than this are excluded from a vote

SPOT_ID        = 1
NODE_IDS       = [1, 2, 3]
NODE_ERROR_PCT = 8         # per-node misclassification rate

# A node is taken offline for one stretch so the dashboard has a real
# example of degraded voting to render.
OUTAGE_NODE    = 2
OUTAGE_START_H = 6.0
OUTAGE_END_H   = 7.5

# Occupancy must persist this many consecutive votes before a session
# opens or closes. Without it, a single split vote produces a 10-second
# phantom session.
DEBOUNCE_ROUNDS = 3

random.seed(475)


# ================== Ground Truth Timeline ====================
# Alternating occupied/empty stretches with plausible durations rather
# than uniform random, so the history view shows something car-like.
def build_timeline(start_ts, end_ts):
    spans = []
    t = start_ts
    occupied = False
    while t < end_ts:
        if occupied:
            dur = random.randint(15 * 60, 110 * 60)     # parked 15-110 min
        else:
            dur = random.randint(5 * 60, 45 * 60)       # vacant 5-45 min
        spans.append((t, min(t + dur, end_ts), occupied))
        t += dur
        occupied = not occupied
    return spans


def truth_at(spans, ts):
    for start, end, occ in spans:
        if start <= ts < end:
            return occ
    return spans[-1][2]


def node_online(node_id, ts, start_ts):
    if node_id != OUTAGE_NODE:
        return True
    h = (ts - start_ts) / 3600.0
    return not (OUTAGE_START_H <= h < OUTAGE_END_H)


# ================== Schema ====================
DDL = """
CREATE TABLE IF NOT EXISTS readings (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    ts        INTEGER NOT NULL,
    spot_id   INTEGER NOT NULL,
    node_id   INTEGER NOT NULL,
    occupied  INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_readings_ts   ON readings(ts);
CREATE INDEX IF NOT EXISTS idx_readings_node ON readings(node_id, ts);

CREATE TABLE IF NOT EXISTS fused (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    ts          INTEGER NOT NULL,
    spot_id     INTEGER NOT NULL,
    occupied    INTEGER NOT NULL,
    votes_occ   INTEGER NOT NULL,
    votes_total INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_fused_ts   ON fused(ts);
CREATE INDEX IF NOT EXISTS idx_fused_spot ON fused(spot_id, ts);

CREATE TABLE IF NOT EXISTS nodes (
    node_id    INTEGER PRIMARY KEY,
    status     TEXT NOT NULL,
    last_seen  INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS node_events (
    id       INTEGER PRIMARY KEY AUTOINCREMENT,
    ts       INTEGER NOT NULL,
    node_id  INTEGER NOT NULL,
    status   TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_node_events_ts ON node_events(node_id, ts);

CREATE TABLE IF NOT EXISTS sessions (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    spot_id    INTEGER NOT NULL,
    started_at INTEGER NOT NULL,
    ended_at   INTEGER
);
CREATE INDEX IF NOT EXISTS idx_sessions_spot ON sessions(spot_id, started_at);
"""


def main():
    now      = int(time.time())
    start_ts = now - HOURS * 3600
    spans    = build_timeline(start_ts, now)

    conn = sqlite3.connect(DB_PATH)
    conn.executescript("PRAGMA journal_mode = WAL;")
    conn.executescript(DDL)
    cur = conn.cursor()

    # ---- Node reports ----
    # Each node's latest reading is tracked so fusion can apply the same
    # staleness rule the live gateway uses.
    latest = {n: None for n in NODE_IDS}
    prev_online = {n: True for n in NODE_IDS}
    readings_rows = []
    event_rows = []

    for ts in range(start_ts, now, NODE_PERIOD):
        truth = truth_at(spans, ts)
        for n in NODE_IDS:
            up = node_online(n, ts, start_ts)
            if up != prev_online[n]:
                event_rows.append((ts, n, "online" if up else "offline"))
                prev_online[n] = up
            if not up:
                continue
            occ = truth if random.randint(0, 99) >= NODE_ERROR_PCT else not truth
            readings_rows.append((ts, SPOT_ID, n, int(occ)))

    cur.executemany(
        "INSERT INTO readings (ts, spot_id, node_id, occupied) VALUES (?,?,?,?)",
        readings_rows)
    cur.executemany(
        "INSERT INTO node_events (ts, node_id, status) VALUES (?,?,?)",
        event_rows)

    # ---- Fusion ----
    by_ts = {}
    for ts, _spot, n, occ in readings_rows:
        by_ts.setdefault(ts, {})[n] = occ

    fused_rows = []
    session_rows = []
    latest = {}
    state = False            # last committed occupancy
    pending = None           # candidate new state
    pending_count = 0
    open_start = None

    for ts in range(start_ts, now, VOTE_PERIOD):
        for rts, per_node in by_ts.items():
            if rts == ts:
                for n, occ in per_node.items():
                    latest[n] = (occ, rts)

        fresh = {n: v for n, (v, rts) in latest.items() if ts - rts <= STALE_SEC}
        if not fresh:
            continue

        votes_occ = sum(fresh.values())
        total = len(fresh)
        # Strict majority; an even split falls through to empty, the safer
        # default when a false "occupied" would start a billing session.
        occ = votes_occ * 2 > total

        fused_rows.append((ts, SPOT_ID, int(occ), votes_occ, total))

        # ---- Session state machine with debounce ----
        if occ != state:
            if pending == occ:
                pending_count += 1
            else:
                pending = occ
                pending_count = 1
            if pending_count >= DEBOUNCE_ROUNDS:
                state = occ
                pending = None
                pending_count = 0
                if state:
                    open_start = ts - (DEBOUNCE_ROUNDS - 1) * VOTE_PERIOD
                elif open_start is not None:
                    session_rows.append(
                        (SPOT_ID, open_start,
                         ts - (DEBOUNCE_ROUNDS - 1) * VOTE_PERIOD))
                    open_start = None
        else:
            pending = None
            pending_count = 0

    # A session still open at the end of the window stays open: this is what
    # the dashboard renders as "currently parked".
    if state and open_start is not None:
        session_rows.append((SPOT_ID, open_start, None))

    cur.executemany(
        "INSERT INTO fused (ts, spot_id, occupied, votes_occ, votes_total) "
        "VALUES (?,?,?,?,?)", fused_rows)
    cur.executemany(
        "INSERT INTO sessions (spot_id, started_at, ended_at) VALUES (?,?,?)",
        session_rows)

    # ---- Current node status ----
    for n in NODE_IDS:
        last_seen = max((ts for ts, _s, nn, _o in readings_rows if nn == n),
                        default=start_ts)
        status = "online" if prev_online[n] else "offline"
        cur.execute(
            "INSERT OR REPLACE INTO nodes (node_id, status, last_seen) "
            "VALUES (?,?,?)", (n, status, last_seen))

    conn.commit()

    # ---- Summary ----
    print(f"{DB_PATH}")
    print(f"  window      : {HOURS} h ending {time.strftime('%Y-%m-%d %H:%M', time.localtime(now))}")
    print(f"  readings    : {len(readings_rows)}")
    print(f"  fused       : {len(fused_rows)}")
    print(f"  sessions    : {len(session_rows)} "
          f"({'1 open' if session_rows and session_rows[-1][2] is None else 'none open'})")
    print(f"  node_events : {len(event_rows)}")

    truth_hits = 0
    for ts, _s, occ, _vo, _vt in fused_rows:
        if bool(occ) == truth_at(spans, ts):
            truth_hits += 1
    node_hits = sum(1 for ts, _s, _n, occ in readings_rows
                    if bool(occ) == truth_at(spans, ts))
    print(f"  single node accuracy : {100.0*node_hits/len(readings_rows):.1f}%")
    print(f"  fused accuracy       : {100.0*truth_hits/len(fused_rows):.1f}%")

    conn.close()


if __name__ == "__main__":
    main()
