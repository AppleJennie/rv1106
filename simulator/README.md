# simulator/ - 公交系统离线运营模拟器

> 在虚拟时钟下模拟一个公交车队 8 小时运营，向后台服务（`server/`）上报
> 注册信息、DMS 疲劳事件、车辆运动事件、舒适度采样与融合安全事件，
> 跑完自动自检。支持 `--offline-jsonl` 离线落盘模式（无服务器环境用）。
>
> 红线：本系统是**安全提示系统，不是处罚系统**。单次事件不归责司机；
> 无信息时 `attribution=UNKNOWN`；司机相关推断一律 SUSPECTED 措辞并保留人工复核。
> 脚本中所有事件概率/舒适度参数均为演示用工程初始值，需实车标定。

## 运行环境

- python3.8+；连服务器模式需要 `httpx`，直接用 `server/.venv` 的 python：
  `server/.venv/bin/python simulator/run_bus_simulation.py`
- 接口契约以 `server/app/schemas.py` 为准。注意 vehicle 事件
  `confidence` 取值 **0.0~1.0**（不是 0~100）。

## 用法

```bash
# 0) 先起后台服务（开发端口 8099；可用临时库：DMS_SERVER_DB=/tmp/sim_test.db）
cd server && .venv/bin/python -m uvicorn app.main:app --host 127.0.0.1 --port 8099 &

# 1) 默认：压缩时间跑完 8 小时虚拟运营，seed=42
server/.venv/bin/python simulator/run_bus_simulation.py

# 2) 完整参数
server/.venv/bin/python simulator/run_bus_simulation.py \
    --server http://127.0.0.1:8099 --hours 8 --seed 42 --start 06:00

# 3) 实时模式（虚拟时间按真实时间 × --speed 推进；例：8 小时 48 秒跑完）
server/.venv/bin/python simulator/run_bus_simulation.py --realtime --speed 600

# 4) 离线模式：不连服务器，全部 POST 请求落 JSONL（供无服务器环境回放/核对）
server/.venv/bin/python simulator/run_bus_simulation.py \
    --offline-jsonl /tmp/bus_sim.jsonl --hours 8 --seed 42
```

CLI 参数：

| 参数 | 默认 | 说明 |
|------|------|------|
| `--server` | `http://127.0.0.1:8099` | 后台服务地址 |
| `--hours` | `8` | 模拟时长（小时） |
| `--speed` | `1.0` | `--realtime` 下虚拟时间相对真实时间的倍速 |
| `--seed` | `42` | 随机种子，同 seed 同 start 背景事件可复现 |
| `--start` | `06:00` | 虚拟开始时间，`HH:MM`（当天）或完整 ISO 8601 |
| `--realtime` | 关 | 按真实时间推进；默认压缩时间一次跑完 |
| `--offline-jsonl PATH` | 关 | 离线落 JSONL，不写服务器 |

自检：跑完后在线模式自动 GET `/api/v1/events`（含 dms/motion/bus 分类计数）
与 `/api/v1/dashboard/overview`，打印汇总并与本地发送计数比对；
**服务器事件数为 0 或本地/服务器计数不一致时退出码为 1**。离线模式改用本地计数。

## 场景设计

车队：10 司机、5 车、3 线路。每辆车 2 个班次覆盖 8 小时：

- 早班 06:00-10:00：司机 1-5；晚班 10:00-14:00：司机 6-10。
- 班次含 `continuous_drive_minutes / rest_minutes / consecutive_work_days`
  （演示用排班初始值，需实车标定），走 `POST /api/v1/shifts` 注册。

背景随机事件（与 `--seed` 相关）：

- 运动事件：急刹/急加速/急转弯(左右)/颠簸，按权重随机，confidence∈[0.6,0.95]，
  带 accel/jerk 物理量；所有车辆参与。
- DMS 事件：哈欠/闭眼/低头/人脸丢失（ATTENTION/WARNING 级）。
- 疲劳序列：哈欠→哈欠→闭眼→长闭眼，8 分钟内演进至 HIGH。
- 舒适度采样：每 10 虚拟分钟/车一条，基准 92 分，随近 10 分钟运动事件数下降。
- **案例司机（司机 1/2/8）不参与随机背景 DMS**，其 DMS 事件只来自剧本，
  保证案例时序干净；其余 7 名司机参与背景随机。

## 典型案例（固定时间点，与 seed 无关；默认 --start 06:00）

| 案例 | 虚拟时间 | 司机/车辆/线路 | 事件序列 | 融合结论（POST /api/v1/bus/events） |
|------|----------|----------------|----------|--------------------------------------|
| CASE A | 07:30:00 | 司机01/车1/1路 | 单次 HARD_BRAKE（司机状态 NORMAL，无外部信息） | `attribution=UNKNOWN`：仅记录提示，不归责 |
| CASE B | 09:14:59.2 → 09:15:01 | 司机02/车2/2路 | HEAD_DOWN 后 0.8 秒内 HARD_BRAKE | `attribution=DRIVER_ATTENTION`，`ATTENTION_RELATED_BRAKE_SUSPECTED`（SUSPECTED 措辞，需人工复核） |
| CASE C | 12:30 → 13:36 | 司机08/车3/3路 | YAWN×3 + EYE_CLOSED + LONG_EYE_CLOSED×2（该班次连续驾驶 320 分钟/连班 5 天） | HIGH 疲劳（SUSPECTED），描述建议立即休息并检查排班，需人工复核 |

时间换算：`--start` 改变时，案例时刻按上表相对偏移（A=+1.5h，B=+3.25h，C=+6.5h 起）。

## 上报接口对照

| 数据 | 接口 |
|------|------|
| 司机/车辆/线路/班次注册 | `POST /api/v1/drivers|vehicles|routes|shifts` |
| DMS 疲劳事件 | `POST /api/v1/dms/events` |
| 车辆运动事件 | `POST /api/v1/vehicle/events` |
| 舒适度采样 | `POST /api/v1/comfort/samples` |
| 融合安全事件（CASE A/B/C） | `POST /api/v1/bus/events` |
| 自检查询 | `GET /api/v1/events`、`GET /api/v1/dashboard/overview` |
