-- Filename: schema.sql
-- Author: Hongyi Mei
-- Date: 07/29/2026
-- Description: Gateway persistence for the smart parking monitor.
--              Written by fusion.py, read-only for the dashboard backend.
--
--              Two tables rather than one: raw node reports are kept so
--              fusion can be re-evaluated offline and per-node reliability
--              measured, while fused results carry the authoritative
--              occupancy state the UI displays.

PRAGMA journal_mode = WAL;   -- reader and writer are separate processes

-- ================== Raw node reports ====================
-- One row per MQTT message from a node. 3 nodes x 1 msg/10s.
-- ts is gateway receive time, not node time: Phase 6 node firmware has no
-- RTC and no NTP sync, so a node cannot stamp its own reports. LAN delay is
-- negligible against the 10 s publish period.
CREATE TABLE IF NOT EXISTS readings (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    ts        INTEGER NOT NULL,        -- unix epoch, gateway receive time
    spot_id   INTEGER NOT NULL,
    node_id   INTEGER NOT NULL,
    occupied  INTEGER NOT NULL,        -- 0 or 1
    seq       INTEGER,                 -- node counter; gaps = lost publishes
    uptime    INTEGER                  -- node uptime (s); a drop = it rebooted
);

CREATE INDEX IF NOT EXISTS idx_readings_ts   ON readings(ts);
CREATE INDEX IF NOT EXISTS idx_readings_node ON readings(node_id, ts);

-- ================== Fused results ====================
-- One row per fusion pass per spot. This is the authoritative state.
-- votes_occ / votes_total records the margin, so a 2/3 result is
-- distinguishable from 3/3 in the UI.
CREATE TABLE IF NOT EXISTS fused (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    ts          INTEGER NOT NULL,
    spot_id     INTEGER NOT NULL,
    occupied    INTEGER NOT NULL,
    votes_occ   INTEGER NOT NULL,
    votes_total INTEGER NOT NULL       -- < 3 means a node was stale
);

CREATE INDEX IF NOT EXISTS idx_fused_ts   ON fused(ts);
CREATE INDEX IF NOT EXISTS idx_fused_spot ON fused(spot_id, ts);

-- ================== Node health ====================
-- Current state only, one row per node, upserted. History of transitions
-- lives in node_events.
CREATE TABLE IF NOT EXISTS nodes (
    node_id    INTEGER PRIMARY KEY,
    status     TEXT NOT NULL,          -- 'online' | 'offline'
    last_seen  INTEGER NOT NULL
);

-- Append-only log of online/offline transitions, for uptime reporting.
CREATE TABLE IF NOT EXISTS node_events (
    id       INTEGER PRIMARY KEY AUTOINCREMENT,
    ts       INTEGER NOT NULL,
    node_id  INTEGER NOT NULL,
    status   TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_node_events_ts ON node_events(node_id, ts);

-- ================== Parking sessions ====================
-- Derived from fused state transitions: a session opens when a spot goes
-- empty -> occupied and closes on the reverse. ended_at NULL means the
-- session is still open. Populated by fusion.py, not by the dashboard.
CREATE TABLE IF NOT EXISTS sessions (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    spot_id    INTEGER NOT NULL,
    started_at INTEGER NOT NULL,
    ended_at   INTEGER                 -- NULL while occupied
);

CREATE INDEX IF NOT EXISTS idx_sessions_spot ON sessions(spot_id, started_at);

-- At most one open session per spot. A second open row would mean the
-- transition logic double-fired; silently accepting it would corrupt the
-- history the dashboard reads, so let the INSERT fail instead.
CREATE UNIQUE INDEX IF NOT EXISTS idx_sessions_one_open
    ON sessions(spot_id) WHERE ended_at IS NULL;
