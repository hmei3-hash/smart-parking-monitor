import os
import sqlite3
import time

from fastapi import FastAPI, Query
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse, JSONResponse

import db
from config import PORT, DEFAULT_HISTORY_HOURS, DATA_GAP_SEC, DB_PATH

app = FastAPI(title="Parking Dashboard API")

# 静态目录用基于本文件位置的绝对路径，不用相对路径。
# 相对路径依赖启动时的工作目录，systemd 不设 WorkingDirectory 时
# 会从 / 启动，直接找不到 static/。
BASE_DIR   = os.path.dirname(os.path.abspath(__file__))
STATIC_DIR = os.path.join(BASE_DIR, "static")

app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


@app.get("/")
def root():
    return FileResponse(os.path.join(STATIC_DIR, "index.html"))


@app.exception_handler(sqlite3.Error)
def sqlite_error_handler(request, exc):
    """数据库打不开时返回结构化错误而不是裸 500。

    最常见的两种情况：网关还没跑起来所以文件不存在，或者
    dashboard 和网关跑在不同用户下，WAL 的 -shm 文件没有权限。
    把路径带回去，省得到服务器上翻日志。
    """
    return JSONResponse(
        status_code=503,
        content={"error": "database_unavailable",
                 "detail": str(exc),
                 "db_path": DB_PATH},
    )


# 实时状态
@app.get("/api/realtime")
def api_realtime():
    fused  = db.get_latest_fused()
    active = db.get_active_session()
    now    = int(time.time())

    # 已停时长在服务端算好再返回。
    #
    # 之前由前端用 Date.now() 减 started_at，混用了浏览器和树莓派两个
    # 时钟。树莓派做热点时没有外网也就没有 NTP，时间可能偏几小时，
    # 相减会得到负数或几万小时。全部用树莓派的时钟算。
    elapsed = None
    if active:
        elapsed = max(0, now - active["started_at"])

    # 数据新鲜度。前端据此判断网关是否还活着 —— 表里有数据但不再增长
    # 的情况，光看 occupied 是看不出来的。
    age = None if not fused else now - fused["ts"]

    votes_occ   = fused["votes_occ"]   if fused else 0
    votes_total = fused["votes_total"] if fused else 0

    # 票数打平时融合逻辑保持上一状态（见 gateway/fusion.py 的迟滞处理），
    # 会出现 1/2 却显示占用的情况。标出来让前端渲染成「持平」而不是
    # 让人以为算错了。平局只在 2 个存活节点时可能出现。
    is_tie = votes_total > 0 and votes_occ * 2 == votes_total

    return {
        "occupied":       fused["occupied"] if fused else 0,
        "votes_occ":      votes_occ,
        "votes_total":    votes_total,
        "is_tie":         is_tie,
        "ts":             fused["ts"] if fused else 0,
        "age":            age,
        "server_now":     now,
        "stale":          age is not None and age > DATA_GAP_SEC,
        "active_session": active,
        "elapsed":        elapsed,
    }


# 历史占用数据
@app.get("/api/history")
def api_history(hours: int = Query(DEFAULT_HISTORY_HOURS, ge=1, le=72)):
    now = int(time.time())
    return {
        "data":     db.get_history_fused(hours),
        # 时间轴锚定到请求的窗口，而不是数据的首尾。网关只跑了 20 分钟时，
        # 用数据首尾会把这 20 分钟拉满整个宽度，标题却写着「过去 10 小时」。
        "window_start": now - hours * 3600,
        "window_end":   now,
        "gap_sec":      DATA_GAP_SEC,
    }


# 停车会话列表
@app.get("/api/sessions")
def api_sessions(hours: int = Query(DEFAULT_HISTORY_HOURS, ge=1, le=72)):
    return {"data": db.get_sessions(hours), "server_now": int(time.time())}


# 节点健康状态
@app.get("/api/nodes")
def api_nodes():
    return {"data": db.get_nodes(), "server_now": int(time.time())}


if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host="0.0.0.0", port=PORT, reload=False)
