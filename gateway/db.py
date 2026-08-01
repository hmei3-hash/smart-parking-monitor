# Filename: db.py
# Author: Hongyi Mei
# Date: 2026-08-01
# Description: SQLite persistence layer for the gateway. Everything that
#              touches the database lives here; fusion.py owns the voting
#              policy and calls into this module.
#
#              The split matters for one reason beyond tidiness: the session
#              table is derived state. A session row is only correct if it
#              was opened and closed by the same transition logic that wrote
#              the fused rows around it. Keeping that in one function
#              (record_fused) makes the invariant enforceable instead of
#              something fusion.py has to remember.
#
#              Threading: paho-mqtt delivers on_message on its network
#              thread while fuse() runs on the main thread, so two threads
#              write here. One connection with check_same_thread=False,
#              serialised by _lock, rather than a connection per thread --
#              SQLite would otherwise serialise them anyway at the file
#              level, with lock contention surfacing as OperationalError.

import os
import sqlite3
import threading
import time

# ================== Config ====================
_HERE       = os.path.dirname(os.path.abspath(__file__))
SCHEMA_PATH = os.path.join(_HERE, "schema.sql")
DEFAULT_DB  = os.path.join(_HERE, "parking.db")

# ================== State ====================
_conn = None
_lock = threading.Lock()


# ================== Setup ====================
def init(db_path=DEFAULT_DB):
    """Open the database and apply schema.sql. Safe to call on an existing
    file: every statement in the schema is IF NOT EXISTS."""
    global _conn

    _conn = sqlite3.connect(db_path, check_same_thread=False)
    _conn.row_factory = sqlite3.Row

    with open(SCHEMA_PATH, "r", encoding="utf-8") as f:
        _conn.executescript(f.read())

    # WAL lets the dashboard read while we write. Set here as well as in the
    # schema because journal_mode is a property of the database file, not of
    # the schema -- executescript applying it once on creation is not enough
    # to guarantee it on a file created some other way.
    _conn.execute("PRAGMA journal_mode = WAL")

    # NORMAL instead of FULL: one fsync per commit at 18 commits/min is
    # avoidable SD-card wear on a Pi. The exposure is losing the last few
    # commits on power loss, which for occupancy samples is acceptable --
    # the next vote pass reconstructs current state anyway.
    _conn.execute("PRAGMA synchronous = NORMAL")
    _conn.commit()

    print(f"db: {db_path}")
    return _conn


def close():
    with _lock:
        if _conn is not None:
            _conn.commit()
            _conn.close()


# ================== Raw readings ====================
def record_reading(ts, spot_id, node_id, occupied, seq=None, uptime=None):
    """One row per node report, plus a last_seen touch on the node."""
    with _lock:
        _conn.execute(
            "INSERT INTO readings (ts, spot_id, node_id, occupied, seq, uptime) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            (int(ts), int(spot_id), int(node_id), int(bool(occupied)), seq, uptime),
        )

        # A node that is publishing is online regardless of what the retained
        # status topic says -- the status message only arrives on connect, so
        # after a gateway restart it may never be seen at all.
        _conn.execute(
            "INSERT INTO nodes (node_id, status, last_seen) VALUES (?, 'online', ?) "
            "ON CONFLICT(node_id) DO UPDATE SET last_seen = excluded.last_seen",
            (int(node_id), int(ts)),
        )
        _conn.commit()


# ================== Node health ====================
def record_node_status(ts, node_id, status):
    """Called on the retained status topic (online / Last Will offline).
    Appends to node_events only when the status actually changes, so the
    event log stays a transition log rather than a duplicate of every
    reconnect."""
    with _lock:
        row = _conn.execute(
            "SELECT status FROM nodes WHERE node_id = ?", (int(node_id),)
        ).fetchone()
        changed = (row is None) or (row["status"] != status)

        _conn.execute(
            "INSERT INTO nodes (node_id, status, last_seen) VALUES (?, ?, ?) "
            "ON CONFLICT(node_id) DO UPDATE SET status = excluded.status",
            (int(node_id), status, int(ts)),
        )
        if changed:
            _conn.execute(
                "INSERT INTO node_events (ts, node_id, status) VALUES (?, ?, ?)",
                (int(ts), int(node_id), status),
            )
        _conn.commit()
        return changed


# ================== Fused state + sessions ====================
def last_fused_state(spot_id):
    """Most recent fused occupancy for a spot, or None if there is no
    history. Read at startup so hysteresis and session tracking survive a
    gateway restart instead of resetting to EMPTY and closing a session that
    is still physically occupied."""
    row = _conn.execute(
        "SELECT occupied FROM fused WHERE spot_id = ? ORDER BY ts DESC, id DESC LIMIT 1",
        (int(spot_id),),
    ).fetchone()
    return None if row is None else bool(row["occupied"])


def open_spots():
    """Every spot the database has ever seen a fused row for. Used at startup
    to decide which spots need their state recovered."""
    rows = _conn.execute("SELECT DISTINCT spot_id FROM fused").fetchall()
    return [r["spot_id"] for r in rows]


def open_session_id(spot_id):
    row = _conn.execute(
        "SELECT id FROM sessions WHERE spot_id = ? AND ended_at IS NULL",
        (int(spot_id),),
    ).fetchone()
    return None if row is None else row["id"]


def record_fused(ts, spot_id, occupied, votes_occ, votes_total, prev_occupied):
    """Write one fusion pass and apply the resulting session transition.

    prev_occupied is passed in rather than re-read here so that the caller's
    hysteresis decision and the session transition are driven by the same
    value -- reading it back from the table would let them disagree if a
    write failed.

    Returns "open" | "close" | None describing the session transition, for
    logging.
    """
    ts       = int(ts)
    spot_id  = int(spot_id)
    occupied = bool(occupied)
    event    = None

    with _lock:
        _conn.execute(
            "INSERT INTO fused (ts, spot_id, occupied, votes_occ, votes_total) "
            "VALUES (?, ?, ?, ?, ?)",
            (ts, spot_id, int(occupied), int(votes_occ), int(votes_total)),
        )

        # Session transitions are edge-triggered on the fused state.
        if prev_occupied is None:
            # First pass ever for this spot. If it comes up occupied, the
            # vehicle was already there and we have no arrival time, so the
            # session starts now and this first duration is a lower bound.
            if occupied:
                _open_session(spot_id, ts)
                event = "open"

        elif occupied and not prev_occupied:
            _open_session(spot_id, ts)
            event = "open"

        elif prev_occupied and not occupied:
            cur = _conn.execute(
                "UPDATE sessions SET ended_at = ? "
                "WHERE spot_id = ? AND ended_at IS NULL",
                (ts, spot_id),
            )
            # No open row to close means the gateway restarted mid-session
            # and lost it, or a previous open failed. Not fatal, but it
            # means the session list has a gap.
            event = "close" if cur.rowcount else "close(orphan)"

        _conn.commit()

    return event


def _open_session(spot_id, ts):
    """Caller must hold _lock. The unique partial index in schema.sql makes a
    duplicate open raise rather than silently corrupt the session list; that
    can only happen if a previous close was lost, so recover by closing the
    stale row at its last known time."""
    try:
        _conn.execute(
            "INSERT INTO sessions (spot_id, started_at) VALUES (?, ?)",
            (spot_id, ts),
        )
    except sqlite3.IntegrityError:
        print(f"  [db] spot{spot_id}: session already open, closing stale row")
        _conn.execute(
            "UPDATE sessions SET ended_at = ? WHERE spot_id = ? AND ended_at IS NULL",
            (ts, spot_id),
        )
        _conn.execute(
            "INSERT INTO sessions (spot_id, started_at) VALUES (?, ?)",
            (spot_id, ts),
        )


# ================== Diagnostics ====================
def summary():
    """Row counts and current state. Printed on shutdown as a sanity check
    that the run actually persisted something."""
    out = {}
    for t in ("readings", "fused", "sessions", "node_events"):
        out[t] = _conn.execute(f"SELECT COUNT(*) AS n FROM {t}").fetchone()["n"]

    open_rows = _conn.execute(
        "SELECT spot_id, started_at FROM sessions WHERE ended_at IS NULL"
    ).fetchall()
    out["open_sessions"] = [(r["spot_id"], r["started_at"]) for r in open_rows]
    return out


def print_summary():
    s = summary()
    print(f"\n--- db ---")
    print(f"  readings={s['readings']} fused={s['fused']} "
          f"sessions={s['sessions']} node_events={s['node_events']}")
    for spot, started in s["open_sessions"]:
        print(f"  spot{spot}: session open for {int(time.time()) - started}s")
    print()
