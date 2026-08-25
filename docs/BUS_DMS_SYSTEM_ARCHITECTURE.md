# 公交驾驶员安全与乘客舒适度智能系统 —— 整体架构

> 版本：v1.0（2026-08-25）
> 系统名：Bus DMS（Bus Driver Monitoring & Passenger Comfort System）
> 产品定位：**安全提示系统，不是处罚系统**（红线见 `docs/DATA_PRIVACY_AND_ETHICS.md`）
> 前置阅读：`代码说明文档/00_交接文档.md`（RV1106 视觉链）、`08_交接文档_DMS_ProductEngineering_MCU.md`（产品化与 MCU 模块）、`docs/SYSTEM_INTERFACES.md`（接口契约）

---

## 1. 一句话架构

**RV1106 负责"看懂司机"，STM32 负责"感知车辆、可靠执行"，Server 负责"数据融合与运营洞察"，Web 负责"给运营人员看的界面"。复杂算法不下放，归责推断不上车。**

## 2. 系统全景数据流图

```
┌────────────────────────────────────── 车 内 边 缘 侧 ──────────────────────────────────────┐
│                                                                                            │
│   MIS5001 摄像头                                                                           │
│       │ NV12                                                                               │
│       ▼                                                                                    │
│   ┌─────────────────────────────┐   I1: UART 115200      ┌──────────────────────────────┐  │
│   │ RV1106 驾驶员状态节点        │   0xAA55 二进制帧       │ STM32 车辆运动与乘客舒适度节点 │  │
│   │ Driver State Node           │◄──────────────────────►│ Vehicle Motion Node          │  │
│   │                             │   +CRC16（命令码预留）  │                              │  │
│   │ ISP→VI→RGA→RetinaFace(NPU)  │                        │ IMU 100Hz（标定/姿态/重力补偿）│  │
│   │ →106点landmark(NPU)         │                        │ → jerk → 车辆运动事件状态机    │  │
│   │ →EAR/MAR/低头→疲劳状态机     │                        │ CAN（stub，预留车速）          │  │
│   │ →Risk Manager（滑动窗口）    │                        │ 蜂鸣器（非阻塞状态机）          │  │
│   │ →Alarm Policy（冷却防连响）  │ ── 风险等级/事件 ──────►│ 心跳监控 / 看门狗（stub）      │  │
│   │ →Event Logger（事件CSV/快照）│                        │ 电源管理                      │  │
│   │ →Product Bridge（胶水层）    │                        │                              │  │
│   └──────────┬──────────────────┘                        └──────────────┬───────────────┘  │
│              │ SD 卡本地缓存（events.csv / 快照 JPEG，循环覆盖）           │                  │
└──────────────┼──────────────────────────────────────────────────────────┼──────────────────┘
               │ I2: HTTP/JSON POST /api/v1/dms/events                     │ I3: HTTP/JSON
               │ （4G/WiFi，离线缓存补传）                                   │ POST /api/v1/vehicle/events
               │                                                           │ POST /api/v1/comfort/samples
               │                     （经 4G DTU 或经 RV1106 转发）          │
               ▼                                                           ▼
              ┌────────────────────────────────────────────────────────────────┐
              │ Server —— 数据融合层（FastAPI + SQLite，1 worker，<300MB）       │
              │                                                                │
              │  接入层：设备认证 / 幂等去重 / 时钟校正（clock_synced）            │
              │  bus_event_fusion：30s 时间线融合 DMS+车辆运动 → Bus Safety Event│
              │    （attribution: UNKNOWN 默认 / SUSPECTED_* 措辞 / 人工复核闭环） │
              │  rule engines：排班风险（SCHEDULE_REVIEW 建议）/ Driver Care 建议 │
              │  聚合：线路风险 / 车辆健康 / 舒适度指标                           │
              └──────────────────────────────┬─────────────────────────────────┘
                                             │ I4: REST /api/v1（JSON）
                                             ▼
              ┌────────────────────────────────────────────────────────────────┐
              │ Web —— 公交运营后台（Bus Operations）                            │
              │  5 页面：安全总览 / Driver Care / Passenger Comfort /            │
              │          Route Risk / Event Timeline                            │
              │  只读展示 + 人工复核入口；展示事件证据，不提供全程监控              │
              └────────────────────────────────────────────────────────────────┘
```

## 3. 各层职责边界

### 3.1 RV1106 —— Driver State Node（驾驶员状态）

**做**：摄像头取帧 → NPU 推理（RetinaFace + 106 点 landmark）→ 疲劳特征（EAR/MAR/低头）→ 疲劳状态机 → Risk Manager 滑动窗口风险评估 → 本地报警策略 → 事件落盘（CSV+快照）→ MCU 帧编码与待发队列（Product Bridge）→ 服务器事件上报（离线缓存补传）。

**不做**：车辆运动判断（那是 STM32 的 IMU/CAN）；跨源归责推断（那是服务器融合层）；人脸身份识别（红线，见隐私文档 §2）；直接控制蜂鸣器 GPIO（经 MCU 执行）。

关键事实：AI 15FPS 满帧，单帧 33ms，RSS ~10MB；已实现并经单测的模块见 `08_交接文档` 第 5 节。所有疲劳/风险阈值集中在 `dms_risk_config_t` 等 config 结构体，**仅工程初始值，需实车标定**。

### 3.2 STM32 —— Vehicle Motion & Passenger Comfort Node（车辆运动与乘客舒适度）

**做**：
- IMU 100Hz 处理：开机静止标定（gyro bias/accel offset）→ 互补滤波姿态（roll/pitch）→ 重力补偿线加速度 → jerk（滤波微分+低通）→ 车辆运动事件状态机（进入/退出阈值+持续+cooldown，全部在 `vehicle_motion_config_t`，仅工程初始值）。
- CAN 抽象（`vehicle_can_state_t`，stub，**不编造 CAN ID**）+ IMU/CAN 置信度融合；车速不可得时上报 null。
- 可靠执行：蜂鸣器非阻塞状态机（禁止 `delay_ms()`）、心跳监控（3s/5s 阈值）、看门狗（心跳丢失 10s 请求复位 RV1106，当前 stub）、电源管理。
- 乘客舒适度的**原始数据源**（三轴加速度/事件计数），舒适度指标计算可位于 MCU 或 Server，以 `passenger_comfort` 模块实现为准。

**不做**：视觉算法；司机归责；长期存储（事件经网关上送，本地只保持必要状态）。资源预算：Flash <1%、RAM <1%（对 STM32F4，详见 `08_交接文档` 第 6.8 节）。

### 3.3 Server —— Data Fusion（数据融合）

**做**：设备接入与认证、幂等去重、时钟校正；**30 秒时间线事件融合**（bus_event_fusion：DMS 疲劳事件 × 车辆运动事件 → Bus Safety Event，`attribution` 默认 `UNKNOWN`，多源时序证据充分时给出 `SUSPECTED_*` 并强制 `review_status=PENDING`）；聚合计算（线路风险、车辆健康、班次风险）；规则引擎输出**建议**（`REST_RECOMMENDED` / `SCHEDULE_REVIEW` / Driver Care 建议）——全部是建议，人工决策。

**不做**：实时车内控制（服务器不在安全闭环内，断网不影响车上报警）；原始视频存储（只存事件证据引用）；自动产出 `CONFIRMED_*` 归责（只有人工复核能写）。

部署形态：FastAPI + SQLite，1 worker，内存目标 <300MB，详见 `deploy/README_SERVER_DEPLOY.md`。

### 3.4 Web —— Bus Operations（公交运营后台）

**做**：5 个页面——安全总览（dashboard/overview）、Driver Care（司机安全摘要+关怀建议，**非绩效**）、Passenger Comfort（舒适度时间序列）、Route Risk（线路风险聚合，用于线路整改）、Event Timeline（统一事件流 + 融合事件 + 人工复核入口）。

**不做**：实时视频全程监控页面（红线，隐私文档 §6）；任何"一键处罚"类操作入口。

## 4. 四大核心域

| 核心域 | 数据源 | 计算位置 | 关键输出 | Web 页面 |
|--------|--------|----------|----------|----------|
| Driver Safety（驾驶员安全） | RV1106 视觉链（EAR/MAR/低头） | RV1106（实时）+ Server（聚合/融合） | 风险等级 NORMAL/ATTENTION/WARNING/HIGH、风险分 0~100、DMS 事件 | 安全总览、Driver Care、Timeline |
| Passenger Comfort（乘客舒适度） | STM32 IMU（三轴加速度/jerk） | STM32 事件状态机 + comfort 引擎 + Server 聚合 | 车辆运动事件、smoothness RMS、trip_comfort_index（内部工程指标，未标定前不对外） | Passenger Comfort |
| Vehicle Health（车辆健康） | 心跳（I1/I2/I3）、CAN（预留）、设备运行状态 | MCU 链路监控 + Server 聚合 | link OK/DEGRADED/LOST、设备存活、uptime | 安全总览、vehicles/{id}/health |
| Operation Health（运营健康） | 上述全部 + 排班数据 | Server 规则引擎 | 班次风险、线路风险、排班复核建议（SCHEDULE_REVIEW） | Route Risk、shifts/{id}/risk |

四个域共用的红线：单源单次事件不归责；融合结论默认 `UNKNOWN`；建议必须经 human review 才可执行。

## 5. 可靠性与降级设计

| 故障 | 行为 |
|------|------|
| RV1106 应用崩溃 | `dms_start.sh` 有限重启（5s+2s×次数，5min 窗口 ≥10 次则降频）；MCU 心跳超时 → LINK_DEGRADED/LOST |
| MCU 收不到心跳 >10s | 看门狗请求复位 RV1106（当前 stub，待硬件） |
| 断网 | RV1106 事件 SD 卡 CSV 缓存，恢复后批量补传（幂等 event_id 去重）；**车上报警不受影响**（服务器不在安全闭环） |
| 时钟未同步 | 事件携带 `clock_synced=false` + `uptime_ms`，服务器以接收时刻入库 |
| 服务器宕机 | 仅影响展示与融合，车上实时安全功能完全自治 |

## 6. 与既有资产的关系

- 视觉链（V2-A，已验收）：`src/hal`、`src/dms` 的取帧/推理/预处理模块**不在本次架构变更范围内**，Product Bridge 仅以 glue code 方式（预计 <100 行）挂接每帧结果。
- 历史文件上传协议（`05_通讯接口文档.md`）：保留用于采集模式批量回传；DMS 安全事件实时上报走 `docs/SYSTEM_INTERFACES.md` I2。
- MCU 协议（`07_DMS_MCU_Protocol.md`）：本文档 I1 以其为准并对齐代码实现（修正帧长笔误：最小 15 / 最大 47 字节）。

## 7. 待硬件验证项（PENDING HARDWARE VALIDATION）

完整清单见根目录 `TODO_HARDWARE.md`，架构层面关键项：RV1106↔MCU 真实 UART 联调、蜂鸣器 GPIO、STM32 真实 IMU 接入与安装方向标定、公交 CAN 真实 DBC、GPS 实车接入、真人疲劳阈值与舒适度阈值标定、服务器正式部署（端口待腾讯云控制台放行）。

---

## 附：版本历史

| 版本 | 日期 | 说明 |
|------|------|------|
| v1.0 | 2026-08-25 | 离线总装阶段初始版本；Server/Web/融合层为契约定义，以各模块实现落地后回填 |
