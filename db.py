import sqlite3
import time
from config import DB_PATH, OFFLINE_THRESHOLD

def get_db_conn():
    # 只读模式打开，适配 WAL
    uri = f"file:{DB_PATH}?mode=ro"
    conn = sqlite3.connect(uri, uri=True)
    conn.row_factory = sqlite3.Row
    return conn

# 1. 获取最新融合状态
def get_latest_fused():
    conn = get_db_conn()
    row = conn.execute("""
        SELECT ts, occupied, votes_occ, votes_total
        FROM fused
        ORDER BY ts DESC
        LIMIT 1
    """).fetchone()
    conn.close()
    return dict(row) if row else None

# 2. 获取历史融合数据
def get_history_fused(hours=10):
    conn = get_db_conn()
    start_ts = int(time.time()) - hours * 3600
    rows = conn.execute("""
        SELECT ts, occupied, votes_occ, votes_total
        FROM fused
        WHERE ts >= ?
        ORDER BY ts ASC
    """, (start_ts,)).fetchall()
    conn.close()
    return [dict(r) for r in rows]

# 3. 获取停车会话列表
def get_sessions(hours=10):
    conn = get_db_conn()
    start_ts = int(time.time()) - hours * 3600
    rows = conn.execute("""
        SELECT id, started_at, ended_at
        FROM sessions
        WHERE started_at >= ?
        ORDER BY started_at DESC
    """, (start_ts,)).fetchall()
    conn.close()
    return [dict(r) for r in rows]

# 4. 获取当前进行中的会话
def get_active_session():
    conn = get_db_conn()
    row = conn.execute("""
        SELECT id, started_at, ended_at
        FROM sessions
        WHERE ended_at IS NULL
        ORDER BY started_at DESC
        LIMIT 1
    """).fetchone()
    conn.close()
    return dict(row) if row else None

# 5. 获取所有节点状态
def get_nodes():
    conn = get_db_conn()
    rows = conn.execute("""
        SELECT node_id, last_seen
        FROM nodes
        ORDER BY node_id ASC
    """).fetchall()
    conn.close()
    now = int(time.time())
    result = []
    for r in rows:
        d = dict(r)
        d["status"] = "online" if (now - d["last_seen"]) < OFFLINE_THRESHOLD else "offline"
        result.append(d)
    return result