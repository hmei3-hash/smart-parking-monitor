# Parking Dashboard

FastAPI 后端 + 原生前端，只读 `gateway/` 写出的 SQLite。

数据库对本服务是**只读**的：以 `mode=ro` 打开，不建表、不写入，不影响网关。

## 运行

```bash
pip3 install fastapi uvicorn --break-system-packages   # Bookworm 起需要这个 flag
PARKING_DB=/home/pi/parking/parking.db python3 main.py
```

浏览器打开 `http://<树莓派IP>:8000`。

本地开发不需要真实网关，用样例库即可：

```bash
python3 ../gateway/tools/gen_sample_db.py
PARKING_DB=./parking_sample.db python3 main.py
```

## 开机自启

```bash
sudo cp parking-dashboard.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now parking-dashboard
```

`.service` 里的 `User=` 必须和运行 `fusion.py` 的用户一致。WAL 模式下读取需要访问 `-shm` 共享内存索引，用户不同会报 `unable to open database file` —— 只 `chmod 644` 主库文件是不够的。

## 接口

| 路径 | 说明 |
|------|------|
| `GET /api/realtime` | 当前状态、票数、已停时长、数据新鲜度 |
| `GET /api/history?hours=N` | 窗口内的 `fused` 采样点 + 窗口边界 |
| `GET /api/sessions?hours=N` | 停车会话列表（含跨窗口的进行中会话） |
| `GET /api/nodes` | 各节点在线状态与最后上报时间 |

## 和网关耦合的几个约定

**`OFFLINE_THRESHOLD` 必须等于 `gateway/fusion.py` 的 `STALE_SEC`。** 两者是同一个语义——「多久没上报就不再算数」。不一致会出现节点已被判离线、却仍被计入 `votes_total`，页面上同时显示「2 个在线」和「3/3 票」。节点上报周期是 10 秒，当前两边都取 30。

**票数打平时网关保持上一状态，不是投给空闲。** 平局只在 2 个存活节点时可能出现（演示会拔掉一个节点，所以这是常规工况而非边界情况）。因此会出现 `votes_occ=1, votes_total=2, occupied=1`，界面标为「持平 · 维持上一状态」。判定条件 `votes_occ * 2 === votes_total`。

**所有时间戳都是网关的接收时间**，unix epoch 秒。节点没有 RTC 也没有 NTP，无法自己打时间戳。

**已停时长由服务端计算后返回**，前端只做本地递增。树莓派做热点时通常没有外网也就没有 NTP，浏览器和树莓派的时钟可能差几小时，两边相减会得到负数或几万小时。

## 时间轴的「无数据」

相邻采样点间隔超过 `DATA_GAP_SEC`（3 个投票周期）时，中间渲染为灰色而不是沿用前一点的颜色。

网关停机 3 小时会在 `fused` 表里留下两条相隔 3 小时的相邻行；按颜色直接填充会在时间轴上凭空长出一条 3 小时的占用记录。目前节点丢包率实测 23%–39%，这个场景一定会出现。

## 端到端延迟

放入车辆到页面变红，**典型 15 秒，最坏约 27 秒**：

```
节点采样周期     10 s    最坏情况刚采完，要等下一轮
推理 Invoke()   4.15 s
网关投票周期     10 s    最坏再等一轮
页面轮询         3 s
```

3 秒是页面刷新频率，不是端到端延迟。
