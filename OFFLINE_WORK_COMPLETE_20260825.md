# OFFLINE_WORK_COMPLETE_20260825 - 离线总装完成报告

> 日期：2026-08-25
> 范围：公交 DMS 系统离线总装（Agent C）——所有不依赖实机硬件的工作
> 验证：`./run_all_offline_tests.sh` → **ALL OFFLINE TEST SUITES PASSED**（6/6 套件全绿）

---

## 1. OFFLINE DONE（已离线完成并验证）

### 1.1 RV1106 产品胶水层（TASK A）✅

| 交付物 | 说明 |
|--------|------|
| `include/dms_product_bridge.h` / `src/dms/dms_product_bridge.c` | dms_result_t → Risk Manager → Alarm Policy → Event Logger → MCU 帧队列。事件持续时间跟踪、边沿检测（不按帧刷日志）、MCU 待发队列（16 帧环形，满丢最旧并计数） |
| `dms_event_logger_init_with_dir()` | 对既有模块的最小增量：PC 可指定事件目录（板端默认路径不变） |
| `tests/test_dms_product_bridge.c` | **28/28 PASS**：正常不记日志、长闭眼→WARNING、60s 内两次长闭眼→HIGH、滑窗老化+恢复滞后降级、心跳帧、缓冲区保护、CSV 落盘校验 |
| `tools/bridge_replay.c` + `tools/dms_result_replay.py` | CSV/JSONL/内置场景回放，输出 risk/alarm/MCU hex 帧 + 事件 CSV |
| CMakeLists.txt | 追加 1 行；**交叉编译零警告通过**（`build/hand_capture_right` 已含 bridge） |

**验证**：`python3 tools/dms_result_replay.py --scenario fatigue` 真实跑通 NORMAL→ATTENTION→WARNING→HIGH 全链路。

### 1.2 STM32 车辆运动与乘客舒适度节点（TASK B/B2/13/14）✅

`vehicle_mcu/`（自包含纯 C，仅标准库 + libm，可直接移植 STM32）：

| 模块 | 内容 |
|------|------|
| `vehicle_imu` | 100Hz 处理、安装方向矩阵（+X前/+Y左/+Z上）、开机静止标定（不假设水平）、互补滤波 roll/pitch、重力补偿线加速度、jerk（滤波微分+低通） |
| `vehicle_motion` | 7 事件状态机（HARD_ACCEL/HARD_BRAKE/HARD_TURN_L/R/BUMP/HIGH_LONG_JERK/HIGH_LAT_JERK），进入/退出/最短持续/冷却四要素，阈值全在 `vehicle_motion_config_t` |
| `vehicle_can` | `vehicle_can_state_t`（speed/brake/accel_pedal/door/soc/valid_flags），分路独立过期，HAL stub，**未编造任何 CAN ID** |
| 置信度融合 | 急刹+CAN 佐证 0.9 / 纯 IMU 0.6 / 矛盾 0.4；CAN 缺失不阻塞 |
| `passenger_comfort` | 三向平顺度 + 事件计数 + `trip_comfort_index`（0~100，内部工程指标） |

**测试：118/118 PASS（gcc -Wall -Wextra -Werror 零警告）**，含已知答案核对（静止≈0、30° 倾斜收敛、急刹 300ms 触发、冷却不抖动、CAN 有无置信度分层）。

**本次修复的真实算法缺陷**：原互补滤波用 `|a|≈g` 判机动——急刹 −2.5m/s² 时 |a| 仅变化 0.3，姿态会被拖偏导致持续急刹漏检。已改为"当前姿态预测的线加速度估计"判机动 + **机动逃逸**（`maneuver_max_ms=5000`：等效原理下无陀螺旋转的恒定"加速度"超时视为安装/姿态变化，重新允许收敛）。修复后 T2 倾斜收敛与急刹检出同时成立。

### 1.3 公交事件融合引擎（TASK C）✅

`bus_event_fusion/`（自包含 C11）：30 秒事件时间线、三个 CASE：

- CASE 1：NORMAL + HARD_BRAKE → `EMERGENCY_BRAKE`，attribution=UNKNOWN（不归责）
- CASE 2：HEAD_DOWN 后 0~2s 内 HARD_BRAKE → `ATTENTION_RELATED_BRAKE_SUSPECTED`（置信度随间隔 90→40 衰减；**反向时序永不触发**，实测验证）
- CASE 3：LONG_EYE_CLOSED + 车辆运动中 → `FATIGUE_HIGH_RISK`

**测试：50/50 PASS**，含时序方向敏感（800ms 触发 / 反向 8s 不触发）、单次急刹永不归责司机、窗口外不关联、无 CAN 照常工作。所有输出 `human_review_required=true`，司机相关推断必含 SUSPECTED。

### 1.4 服务器后台（TASK D/E/G/H）✅

`server/`（FastAPI 0.124.4 + SQLite，py3.8 兼容，`server/.venv` 已建好）：

- 10 张表（drivers/vehicles/routes/shifts/dms_events/vehicle_motion_events/bus_safety_events/comfort_trips/risk_daily_summary/schedule_risk_summary），ID 全部可空，无 penalty 类字段
- 全部 11 个规定端点 + 注册辅助端点 + 日汇总刷新端点
- 排班风险引擎 + 司机关怀引擎（输出 REST_RECOMMENDED/SCHEDULE_REVIEW，文案为关怀建议风格）
- **测试：36/36 pytest PASS**（ASGI transport，不起真实端口）
- `server/README.md`（启动/API/JSON schema）、`server/requirements.txt`（已 freeze）

### 1.5 文档与部署包（TASK J/K/L/M）✅

- `docs/DATA_PRIVACY_AND_ETHICS.md`、`docs/SYSTEM_INTERFACES.md`（四接口契约，含经 CRC 核算的真实示例帧）、`docs/BUS_DMS_SYSTEM_ARCHITECTURE.md`
- `deploy/`：requirements.txt、.env.example、systemd 单元模板（MemoryMax=300M、1 worker）、backup_db.sh、logrotate 配置、README_SERVER_DEPLOY.md（显著警示 xiaozhi 占用 8000/8003，**未启动任何服务**）
- 发现并记录上游文档勘误（07 协议文档最小/最大帧 13/45 应为 15/47），正确值写入 SYSTEM_INTERFACES.md

### 1.6 自动测试与回归工具（TASK N/O）✅

- **`./run_all_offline_tests.sh` 一条命令全绿**：risk_manager 24 + mcu_protocol 42 + product_bridge 28 + 协议 10000 包压力 + vehicle_mcu 118 + fusion 50 + server pytest 36
- 回归工具三件套：`tools/replay_dms_csv.py`、`tools/replay_vehicle_imu.py`（含 --demo 仿真）、`tools/replay_bus_events.py`——真实 CSV 进来直接跑，无需改代码

---

## 2. 未完成（本次按指示只收尾已开工部分）

| 项 | 状态 | 说明 |
|----|------|------|
| TASK F：Web 5 页面原型 | **未开工** | 列入下次会话；接口以 `server/README.md` 为准 |
| TASK I：simulator 8 小时运营模拟 | **未开工** | 列入下次会话；CASE A/B/C 已由 fusion 单测覆盖等效逻辑 |

---

## 3. REQUIRES HARDWARE（PENDING HARDWARE VALIDATION，未伪造完成）

完整清单见 `TODO_HARDWARE.md`，要点：

- RV1106↔MCU 真实 UART、蜂鸣器 GPIO、开机自启实机、看门狗、供电整改
- Product Bridge 合入 RV1106 主程序（Integration glue code，接口已就绪）
- STM32 真实 IMU 接入 + 安装方向实车标定；公交 CAN 真实 DBC（当前 stub）
- 全部阈值实车标定（Risk Manager / 运动状态机 / 融合关联窗 / 舒适度）
- 服务器正式部署（端口待腾讯云放行；deploy/ 仅准备未启动）

---

## 4. 已知不一致（下次开工先处理）

- `server` 的 vehicle `confidence` 为 **0.0~1.0**，`docs/SYSTEM_INTERFACES.md` 契约写 0~100 —— 以 `server/app/schemas.py` 为准，或下次统一改契约文档（记录在 `docs/README.md` 假设清单与 `server/README.md`）。
- fusion 输出的事件类型字符串与 server `bus_safety_events` 表的 `event_type` 字段对接，需在模拟器/Integration 时对齐。

## 5. 产品红线自查

全量 grep 新建代码：无 penalty/fine/punishment/deduct/罚款/扣分/自动处罚 的功能性使用（仅红线声明中的否定语境）。单次事件不归责司机、SUSPECTED 措辞、human review 保留，均已落实到代码与测试断言。

---

> 快速验证：`./run_all_offline_tests.sh`（全绿即为准）。下次会话建议顺序：TASK I 模拟器 → TASK F Web → Integration V1 上板。
