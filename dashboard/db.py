import sqlite3
import time

from config import DB_PATH, OFFLINE_THRESHOLD, SPOT_ID


def get_db_conn():
    # 只读模式打开，适配 WAL。
    #
    # immutable=0 是默认值，这里不能设成 1：网关正在写这个文件，
    # immutable 会让 SQLite 缓存页面而看不到后续写入。
    uri = f"file:{DB_PATH}?mode=ro"
    conn = sqlite3.connect(uri, uri=True, timeout=5)
    conn.row_factory = sqlite3.Row
    return conn


# 1. 获取最新融合状态
def get_latest_fused():
    conn = get_db_conn()
    try:
        row = conn.execute("""
            SELECT ts, occupied, votes_occ, votes_total
            FROM fused
            WHERE spot_id = ?
            ORDER BY ts DESC, id DESC
            LIMIT 1
        """, (SPOT_ID,)).fetchone()
    finally:
        conn.close()
    return dict(row) if row else None


# 2. 获取历史融合数据
def get_history_fused(hours=10):
    conn = get_db_conn()
    start_ts = int(time.time()) - hours * 3600
    try:
        rows = conn.execute("""
            SELECT ts, occupied, votes_occ, votes_total
            FROM fused
            WHERE spot_id = ? AND ts >= ?
            ORDER BY ts ASC
        """, (SPOT_ID, start_ts)).fetchall()
    finally:
        conn.close()
    return [dict(r) for r in rows]


# 3. 获取停车会话列表
def get_sessions(hours=10):
    """按开始时间倒序返回窗口内的会话。

    过滤条件不能只写 started_at >= start_ts：一辆车 11 小时前停进来
    到现在还没走，用 10 小时窗口就查不到它，而这恰恰是最该显示的
    一条。所以额外把所有未结束的会话也算进来。
    """
    conn = get_db_conn()
    start_ts = int(time.time()) - hours * 3600
    try:
        rows = conn.execute("""
            SELECT id, started_at, ended_at
            FROM sessions
            WHERE spot_id = ?
              AND (started_at >= ? OR ended_at IS NULL)
            ORDER BY started_at DESC
        """, (SPOT_ID, start_ts)).fetchall()
    finally:
        conn.close()
    return [dict(r) for r in rows]


# 4. 获取当前进行中的会话
def get_active_session():
    conn = get_db_conn()
    try:
        row = conn.execute("""
            SELECT id, started_at, ended_at
            FROM sessions
            WHERE spot_id = ? AND ended_at IS NULL
            ORDER BY started_at DESC
            LIMIT 1
        """, (SPOT_ID,)).fetchone()
    finally:
        conn.close()
    return dict(row) if row else None


# 5. 获取所有节点状态
def get_nodes():
    """节点在线判定以 last_seen 为准，不用 nodes.status。

    status 来自 MQTT 的 retained Last Will，只在连接/断开时更新。
    节点掉 WiFi 但没干净关闭 TCP 时，status 会一直停在 online 直到
    broker keepalive 超时，所以最后上报时间才是可靠依据。
    """
    conn = get_db_conn()
    try:
        rows = conn.execute("""
            SELECT node_id, status, last_seen
            FROM nodes
            ORDER BY node_id ASC
        """).fetchall()
    finally:
        conn.close()

    now = int(time.time())
    result = []
    for r in rows:
        d = dict(r)
        last_seen = d.get("last_seen")
        age = None if last_seen is None else now - last_seen
        d["age"] = age
        d["status"] = "online" if (age is not None and age < OFFLINE_THRESHOLD) else "offline"
        result.append(d)
    return result
