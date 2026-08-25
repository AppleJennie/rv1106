# TODO_OFFLINE - 离线总装任务清单（Agent C）

> 规则：完成一项就把 [ ] 改成 [x]。硬件相关阻塞项不记在这里，记 `TODO_HARDWARE.md`。

## 产品红线（所有模块遵守）
- AI 是安全提示系统，不是处罚系统；禁止 penalty/fine/punishment/deduct 概念
- Risk Score = 安全风险，≠ Driver Penalty Score
- 单次事件不自动归责司机；无信息时 attribution=UNKNOWN

## 任务清单

- [x] TASK A: RV1106 Product Glue Layer
  - [x] `include/dms_product_bridge.h` / `src/dms/dms_product_bridge.c`
  - [x] `dms_event_logger_init_with_dir()`（PC 可运行）
  - [x] `tests/test_dms_product_bridge.c`（28/28 PASS）
  - [x] `tools/bridge_replay.c` + `tools/dms_result_replay.py`（CSV/JSONL/场景回放）
  - [x] 加入 CMakeLists，交叉编译零警告通过
- [x] TASK B: `vehicle_mcu/` STM32 车辆运动节点
  - [x] IMU 100Hz 处理 + 安装方向矩阵（+X前/+Y左/+Z上）
  - [x] 开机静止标定（gyro bias / accel offset，IMU_CALIBRATING/READY/ERROR）
  - [x] 互补滤波姿态（roll/pitch）+ 重力补偿线加速度
  - [x] jerk（滤波后微分 + 低通）
  - [x] 车辆运动事件状态机（进入/退出阈值+持续+cooldown，全在 vehicle_motion_config_t）
  - [x] 单元测试
- [x] TASK B2: CAN 抽象 `vehicle_can_state_t`（stub，不编造 CAN ID）+ IMU/CAN 置信度融合
- [x] TASK 13/14: `passenger_comfort.c/.h` 舒适度引擎（smoothness×3 + 事件计数 + trip_comfort_index）
- [x] TASK C: `bus_event_fusion/` 30s 时间线事件融合（SUSPECTED 措辞，时序方向敏感）
- [x] TASK D/E: `server/` FastAPI+SQLite+pytest（10 张表、API 全集、开发用 SQLite）
- [x] TASK F: Web 5 页面（web/ 5 页 + server /web/ 静态挂载，curl 全 200）（总览/Driver Care/Comfort/Route Risk/Timeline）
- [x] TASK G/H: 排班风险 rule engine + Driver Care 建议引擎（mock 数据测试）
- [x] TASK I: `simulator/` 10司机5车3线路 8 小时模拟 + CASE A/B/C + POST 到 FastAPI（40 事件进库实测）
- [x] TASK J: `docs/DATA_PRIVACY_AND_ETHICS.md`
- [x] TASK K: `deploy/` 部署包（只准备不部署；不动 xiaozhi；不占 8000/8003）
- [x] TASK L: `docs/SYSTEM_INTERFACES.md`
- [x] TASK M: `docs/BUS_DMS_SYSTEM_ARCHITECTURE.md`
- [x] TASK N: `run_all_offline_tests.sh` 一条命令全绿
- [x] TASK O: `tools/replay_dms_csv.py` / `replay_vehicle_imu.py` / `replay_bus_events.py`
- [x] TASK P: `OFFLINE_WORK_COMPLETE_20260825.md`（OFFLINE DONE / REQUIRES HARDWARE 分明）
