from fastapi import FastAPI, Query
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse
from fastapi.middleware.cors import CORSMiddleware
import db
from config import PORT, DEFAULT_HISTORY_HOURS

app = FastAPI(title="Parking Dashboard API")

# 允许跨域，方便本地调试
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# 挂载静态页面
app.mount("/static", StaticFiles(directory="static"), name="static")

@app.get("/")
def root():
    return FileResponse("static/index.html")

# 实时状态
@app.get("/api/realtime")
def api_realtime():
    fused = db.get_latest_fused()
    active = db.get_active_session()
    return {
        "occupied": fused["occupied"] if fused else 0,
        "votes_occ": fused["votes_occ"] if fused else 0,
        "votes_total": fused["votes_total"] if fused else 0,
        "ts": fused["ts"] if fused else 0,
        "active_session": active
    }

# 历史占用数据
@app.get("/api/history")
def api_history(hours: int = Query(DEFAULT_HISTORY_HOURS)):
    return {"data": db.get_history_fused(hours)}

# 停车会话列表
@app.get("/api/sessions")
def api_sessions(hours: int = Query(DEFAULT_HISTORY_HOURS)):
    return {"data": db.get_sessions(hours)}

# 节点健康状态
@app.get("/api/nodes")
def api_nodes():
    return {"data": db.get_nodes()}

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host="0.0.0.0", port=PORT, reload=False)