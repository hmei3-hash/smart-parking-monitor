import os

# 数据库文件路径。
# 默认值与部署文档一致；本地开发用环境变量覆盖，避免相对路径被带上生产：
#     PARKING_DB=./parking_sample.db python3 main.py
DB_PATH = os.environ.get("PARKING_DB", "/home/pi/parking/parking.db")

# 节点离线阈值（秒）。
#
# 必须与 gateway/fusion.py 的 STALE_SEC 保持一致，两者是同一个语义：
# 「多久没上报就不再算数」。不一致会出现节点已被判离线、却仍被计入
# votes_total 的情况，页面上会同时显示「2 个在线」和「3/3 票」。
#
# 节点上报周期是 10 秒（固件 INFER_PERIOD = 10000），30 秒相当于允许
# 连续丢 2 次。改这个值时记得同步改 fusion.py。
OFFLINE_THRESHOLD = 30

# 服务端口
PORT = 8000

# 历史视图默认时长（小时）
DEFAULT_HISTORY_HOURS = 10

# 当前监测的车位。MVP 只有一个车位，但所有查询都按它过滤，
# 避免将来加车位时 ORDER BY ts LIMIT 1 跨车位串数据。
SPOT_ID = 1

# 时间轴上相邻两点间隔超过这个秒数，就认为中间是数据缺口（网关停机、
# 节点全部离线），渲染为「无数据」而不是沿用前一个状态的颜色。
# 取 3 倍投票周期。
DATA_GAP_SEC = 30
