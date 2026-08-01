# Filename: inspect_db.py
# Author: Hongyi Mei
# Date: 2026-08-01
# Description: Read-only console view of the gateway database. Meant for
#              bring-up and demo debugging: shows current state, recent
#              fusion history, parking sessions, and node health in one
#              pass, with epoch timestamps rendered as local time.
#
#              Opens the file with mode=ro so it can be run against a live
#              gateway without contending for the write lock. WAL means the
#              read never blocks and never blocks the writer.
#
#              Usage: python3 inspect_db.py [db_path] [--watch]

import bisect
import os
import sqlite3
import sys
import time

_HERE      = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DB = os.path.join(_HERE, "parking.db")

HISTORY_ROWS = 15
WATCH_PERIOD = 3.0


def connect(path):
    if not os.path.exists(path):
        sys.exit(f"no database at {path}")
    # uri=True is required for the mode=ro query parameter to be honoured;
    # without it the string is treated as a literal filename.
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True, timeout=5)


def ts(v):
    return "-" if v is None else time.strftime("%H:%M:%S", time.localtime(v))


def dur(seconds):
    if seconds is None:
        return "-"
    m, s = divmod(int(seconds), 60)
    h, m = divmod(m, 60)
    return f"{h}:{m:02d}:{s:02d}" if h else f"{m}:{s:02d}"


# ================== Sections ====================
def show_current(c):
    row = c.execute(
        "SELECT * FROM fused ORDER BY ts DESC, id DESC LIMIT 1"
    ).fetchone()
    if row is None:
        print("current : no fused rows yet")
        return

    state = "OCCUPIED" if row["occupied"] else "EMPTY"
    age   = int(time.time() - row["ts"])

    # votes_total below 3 means a node was excluded as stale. Flagging it
    # here because an unnoticed 2-node vote looks identical to a healthy one
    # in the state column alone.
    degraded = " [DEGRADED]" if row["votes_total"] < 3 else ""

    # A tie can only occur at 2 live nodes; the gateway holds the previous
    # state in that case, so the displayed votes will look like a minority.
    tie = "  (tie, holding)" if row["votes_occ"] * 2 == row["votes_total"] else ""

    print(f"current : spot{row['spot_id']}  {state:8s} "
          f"{row['votes_occ']}/{row['votes_total']}{degraded}{tie}")
    print(f"          last vote {ts(row['ts'])} ({age}s ago)")

    # A stale last vote means fusion.py is not running, which is otherwise
    # invisible: the table still has plausible-looking data in it.
    if age > 30:
        print(f"          WARNING: no fusion pass in {age}s - is fusion.py running?")


def show_sessions(c):
    rows = c.execute(
        "SELECT id, spot_id, started_at, ended_at FROM sessions "
        "ORDER BY started_at DESC LIMIT 10"
    ).fetchall()
    if not rows:
        print("\nsessions: none")
        return

    print("\nsessions:")
    now = time.time()
    for r in rows:
        if r["ended_at"] is None:
            print(f"  #{r['id']:<3} spot{r['spot_id']}  {ts(r['started_at'])} -> "
                  f"{'(open)':10s} {dur(now - r['started_at']):>8s}  ACTIVE")
        else:
            print(f"  #{r['id']:<3} spot{r['spot_id']}  {ts(r['started_at'])} -> "
                  f"{ts(r['ended_at']):10s} {dur(r['ended_at'] - r['started_at']):>8s}")


def show_nodes(c):
    rows = c.execute("SELECT * FROM nodes ORDER BY node_id").fetchall()
    if not rows:
        print("\nnodes   : none seen")
        return

    print("\nnodes:")
    now = time.time()
    for r in rows:
        age = now - r["last_seen"] if r["last_seen"] else None

        # status comes from the retained Last Will and only updates on
        # connect/disconnect. A node that lost WiFi without a clean close
        # still reads "online" until the broker keepalive expires, so treat
        # a stale last_seen as authoritative over the status string.
        if age is not None and age > 30:
            verdict = "SILENT"
        else:
            verdict = r["status"].upper()

        line = f"  node{r['node_id']}  {verdict:8s} last seen {ts(r['last_seen'])}"
        if age is not None:
            line += f" ({int(age)}s ago)"
        print(line)


def show_history(c):
    rows = c.execute(
        "SELECT ts, spot_id, occupied, votes_occ, votes_total FROM fused "
        "ORDER BY ts DESC, id DESC LIMIT ?", (HISTORY_ROWS,)
    ).fetchall()
    if not rows:
        return

    print(f"\nlast {len(rows)} votes (newest first):")
    for r in reversed(rows):
        bar = "#" if r["occupied"] else "."
        print(f"  {ts(r['ts'])}  spot{r['spot_id']}  {bar}  "
              f"{r['votes_occ']}/{r['votes_total']}  "
              f"{'OCCUPIED' if r['occupied'] else 'EMPTY'}")


def show_counts(c):
    parts = []
    for t in ("readings", "fused", "sessions", "node_events"):
        n = c.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0]
        parts.append(f"{t}={n}")
    print("\nrows    : " + "  ".join(parts))


def show_node_accuracy(c):
    """Per-node agreement with the fused result. This is the number that
    justifies majority voting existing, and it is the reason the readings
    table is retained even though the dashboard never shows it.

    Matches each reading to the fused row nearest in time rather than by
    sequence: nodes publish on independent timers, so there is no shared
    index to join on."""
    # Matched in Python rather than SQL. The nearest-row lookup needs a
    # correlated subquery referencing the outer readings row, and SQLite
    # would not resolve that alias in either the JOIN ... ON or the
    # select-list position. A bisect over the fused timestamps is O(log n)
    # per reading and far easier to read than the SQL would have been.
    fused = {}
    for r in c.execute("SELECT spot_id, ts, occupied FROM fused ORDER BY ts"):
        fused.setdefault(r["spot_id"], ([], []))
        fused[r["spot_id"]][0].append(r["ts"])
        fused[r["spot_id"]][1].append(r["occupied"])

    if not fused:
        return

    tally = {}   # node_id -> [agree, total]
    for r in c.execute("SELECT node_id, spot_id, ts, occupied FROM readings"):
        entry = fused.get(r["spot_id"])
        if entry is None:
            continue
        times, states = entry

        i = bisect.bisect_left(times, r["ts"])
        # bisect gives the insertion point; the nearest row is on one side or
        # the other, so compare both neighbours.
        cands = [j for j in (i - 1, i) if 0 <= j < len(times)]
        best  = min(cands, key=lambda j: abs(times[j] - r["ts"]))

        t = tally.setdefault(r["node_id"], [0, 0])
        t[1] += 1
        if bool(states[best]) == bool(r["occupied"]):
            t[0] += 1

    if sum(v[1] for v in tally.values()) < 10:
        return   # too few samples for the number to mean anything

    print("\nagreement with fused result:")
    for node in sorted(tally):
        agree, n = tally[node]
        print(f"  node{node}  {100.0 * agree / n:5.1f}%  ({agree}/{n})")


# ================== Main ====================
def dump(path):
    c = connect(path)
    c.row_factory = sqlite3.Row
    print(f"=== {path} ===")
    show_current(c)
    show_nodes(c)
    show_sessions(c)
    show_history(c)
    show_node_accuracy(c)
    show_counts(c)
    c.close()


def main():
    args  = [a for a in sys.argv[1:] if not a.startswith("--")]
    watch = "--watch" in sys.argv
    path  = args[0] if args else DEFAULT_DB

    if not watch:
        dump(path)
        return

    try:
        while True:
            # Reconnecting each pass rather than holding one connection: a
            # long-lived reader can pin an old WAL snapshot and stop the
            # file from checkpointing.
            os.system("clear")
            dump(path)
            time.sleep(WATCH_PERIOD)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
