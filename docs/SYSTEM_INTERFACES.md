# 系统接口统一文档（Bus DMS）

> 版本：v1.0（2026-08-25）
> 地位：四个子系统之间接口的**唯一权威定义**。各模块实现与本文档不一致时，以本文档为准并修订实现。
> 前置阅读：`代码说明文档/07_DMS_MCU_Protocol.md`（UART 协议原始文档）、`代码说明文档/05_通讯接口文档.md`（历史文件上传协议）。

## 0. 全局约定

### 0.1 接口总览

| 编号 | 接口 | 物理/传输层 | 编码 | 方向 |
|------|------|-------------|------|------|
| I1 | RV1106 → MCU（STM32） | UART 115200 8N1 | 二进制帧 + CRC16 | 单向（命令预留反向） |
| I2 | RV1106 → Server | TCP/HTTP（4G/WiFi） | JSON | 单向 POST |
| I3 | STM32 → Server/Gateway | UART→4G DTU 或经 RV1106 转发 | JSON | 单向 POST |
| I4 | Server → Web | HTTP | JSON（REST） | 请求/响应 |

### 0.2 时间戳基准（全局统一）

- 线上传输与存储统一使用 **Unix epoch 毫秒（UTC）**，字段名以 `_ms` 结尾；UART 协议因带宽限制使用 **epoch 秒（uint32）**。
- 设备时钟：RV1106 启动后需 NTP 校时；**未校时成功时**，请求体中 `clock_synced=false`，并携带 `uptime_ms`（开机毫秒），服务器以 `received_at_ms`（服务器接收时刻）为准入库并保留设备原始时间用于排序校正。
- MCU 侧无 RTC 概念：I1 帧的 Timestamp 由 RV1106 填写（epoch 秒或系统启动秒数，见 I1 字段表），MCU 只做透传/显示，不解释。

### 0.3 红线约束（所有接口遵守）

- 所有接口字段、错误码、示例中严禁出现 penalty/fine/punishment/deduct/罚款/扣分/自动处罚 概念。
- 归责字段 `attribution` 取值：`UNKNOWN`（默认）/ `SUSPECTED_DRIVER` / `SUSPECTED_ROAD` / `SUSPECTED_VEHICLE` / `CONFIRMED_*`（仅人工复核后写入）。无信息时**必须**为 `UNKNOWN`。详见 `docs/DATA_PRIVACY_AND_ETHICS.md` 第 5 节。
- 所有阈值均为**工程初始值，需实车标定**。

---

## I1. RV1106 → MCU（UART 二进制帧）

> 已实现并单测通过：`src/dms/dms_mcu_protocol.c`（编码/解码，42 项单测 + 10000 包压力测试）。
> 本节数值以**代码实现**为准。注意：`07_DMS_MCU_Protocol.md` 中"最小帧 13 / 最大帧 45 字节"与实现不符，正确值为**最小 15 / 最大 47 字节**（无 payload 帧 = 2+1+1+1+1+2+4+1+2 = 15 字节）。

### I1.1 物理层

| 项 | 值 |
|----|----|
| 接口 | UART，引脚由硬件设计定 |
| 波特率 | 115200（默认，可配置） |
| 数据位/停止位/校验/流控 | 8 / 1 / None / None |

### I1.2 帧格式（大端序多字节字段）

```
+--------+--------+-------+-------+-------+-------+------------+-----------+-------+---------+--------+
| 0xAA   | 0x55   | Ver   | Event | Risk  | Conf  | DurationMS | Timestamp | PayLen| Payload | CRC16  |
| 1B     | 1B     | 1B    | 1B    | 1B    | 1B    | 2B BE      | 4B BE     | 1B    | 0~32B   | 2B BE  |
+--------+--------+-------+-------+-------+-------+------------+-----------+-------+---------+--------+
```

### I1.3 字段表

| 字段 | 类型 | 单位 | 可空 | 说明 |
|------|------|------|------|------|
| Header | u8×2 | — | 否 | 固定 0xAA 0x55，帧同步 |
| Version | u8 | — | 否 | 协议版本，当前 0x01；不匹配则整帧丢弃并记日志 |
| Event | u8 | — | 否 | 事件码，见 I1.4 |
| Risk Level | u8 | — | 否 | 0=NORMAL / 1=ATTENTION / 2=WARNING / 3=HIGH |
| Confidence | u8 | % | 否 | 置信度 0~100 |
| Duration MS | u16 BE | ms | 否 | 事件持续时间；心跳帧填 0 |
| Timestamp | u32 BE | s | 否 | epoch 秒；设备未校时时为系统启动秒数（接收方不得当作绝对时间展示） |
| Payload Len | u8 | B | 否 | 附加数据长度，0~32；>32 整帧丢弃 |
| Payload | u8[N] | — | 是 | 附加数据；仅心跳帧当前有定义（4 字节，见 I1.5） |
| CRC16 | u16 BE | — | 否 | CRC16-CCITT：多项式 0x1021、初值 0xFFFF、无反射、无异或输出；覆盖 Header 到 Payload（不含 CRC 自身）；已验证 CRC16("123456789")=0x29B1 |

- 最小帧 15 字节（无 payload），最大帧 47 字节（32 字节 payload）。
- 协议版本：0x01（2026-08-23 初始版本）。

### I1.4 事件码表

| 码 | 事件 | 方向 | 说明 |
|----|------|------|------|
| 0x01 | HEARTBEAT | RV1106→MCU | 心跳，每秒 1 帧 |
| 0x10 | EYE_CLOSED | RV1106→MCU | 闭眼 |
| 0x11 | LONG_EYE_CLOSED | RV1106→MCU | 长闭眼 |
| 0x12 | YAWN | RV1106→MCU | 哈欠 |
| 0x13 | HEAD_DOWN | RV1106→MCU | 低头 |
| 0x14 | FACE_LOST | RV1106→MCU | 人脸丢失 |
| 0x20 | FATIGUE_WARNING | RV1106→MCU | 疲劳警告（WARNING 级） |
| 0x21 | FATIGUE_HIGH | RV1106→MCU | 疲劳高危（HIGH 级） |

预留段（接收方收到未识别事件码：当前版本丢弃并计数，不得崩溃）：0x02~0x0F 系统预留；0x15~0x1F 检测预留；0x22~0x2F 风险预留；0x30~0x3F MCU→RV1106 命令预留；0x40~0xFF 未来扩展。

### I1.5 心跳 Payload（4 字节）

| 字节 | 字段 | 说明 |
|------|------|------|
| 0 | dms_alive | DMS 应用存活：0=异常 1=正常 |
| 1 | camera_alive | 摄像头存活：0=异常 1=正常 |
| 2 | ai_alive | AI 推理存活：0=异常 1=正常 |
| 3 | risk_level | 当前风险等级 0~3 |

MCU 侧链路监控（已实现于 `mcu/src/dms_heartbeat.c`）：<3s 未收到 = LINK_OK；3~5s = LINK_DEGRADED；>5s = LINK_LOST（看门狗策略：连续丢失 10s 请求复位 RV1106，当前为 stub）。

### I1.6 错误处理（接收侧）

| 错误 | 处理 |
|------|------|
| Header 不匹配 | 丢弃当前字节，继续搜索 0xAA 0x55（支持 0xAA 0xAA 连续头重同步） |
| Version 不匹配 | 丢弃整帧，记录日志 |
| Payload Len >32 | 丢弃整帧 |
| CRC 校验失败 | 丢弃整帧，统计错误计数 |
| 粘包/分包/截断/噪声 | 字节流状态机容错，自动重同步（已经 10000 包压力测试验证） |

### I1.7 示例帧（字节级，CRC 已经代码同算法核算）

心跳帧（全部正常，ts=1787616000 = 2026-08-25 00:00:00 UTC），共 19 字节：

```
AA 55 01 01 00 64 00 00 6A 8C DB 00 04 01 01 01 00 4B 36
```

长闭眼事件帧（WARNING，置信度 90，持续 1500ms，无 payload），共 15 字节：

```
AA 55 01 11 02 5A 05 DC 6A 8C DB 00 00 E4 E1
```

### I1.8 升级兼容策略

- Version 单调递增；MCU 收到未知 Version 当前策略是**丢帧记日志**——因此**升级必须先发 MCU 固件、再发 RV1106**，或保证新旧 Version 帧结构前向一致（仅追加事件码、不改字段布局）。
- 新增信息优先放入 Payload（≤32B）或新增事件码，**不得改动既有字段的顺序与语义**。
- 风险等级语义只增不改：0~3 含义固定，新增等级必须 ≥4 并同步升级 MCU 显示逻辑。

---

## I2. RV1106 → Server（HTTP/JSON 事件上报）

> 状态：契约定义（v1）。Server 端（TASK D/E）与 RV1106 上报端按本节实现。
> 与历史协议的关系：`05_通讯接口文档.md` 的 TCP `FILE/OK/ERR` 协议**保留用于采集模式的批量文件回传**；DMS 安全事件实时上报一律走本节 HTTP/JSON。

### I2.1 传输约定

| 项 | 值 |
|----|----|
| 方法/路径 | `POST /api/v1/dms/events` |
| 编码 | JSON，UTF-8，`Content-Type: application/json` |
| 认证 | `X-Device-Key` 头（设备预共享密钥，占位——正式方案待部署阶段定） |
| 批量 | 单请求最多携带 50 条事件；设备离线期间事件在 SD 卡 CSV 缓存，网络恢复后批量补传 |
| 超时 | 连接 5s / 读写 10s；失败指数退避重试（初始 5s，最大 300s） |
| 时间戳基准 | 见 §0.2；每条事件携带 `timestamp_ms` 与 `clock_synced` |

### I2.2 请求体字段表

顶层：

| 字段 | 类型 | 单位 | 可空 | 说明 |
|------|------|------|------|------|
| device_id | string | — | 否 | 设备唯一 ID，如 `rv1106-bus-0001` |
| protocol_version | int | — | 否 | 当前为 1 |
| sent_at_ms | int64 | ms | 否 | 本请求发出时刻（设备时钟） |
| clock_synced | bool | — | 否 | 设备时钟是否已 NTP 同步；false 时服务器以接收时刻为准 |
| events | array | — | 否 | 事件数组，可空数组（心跳式保活上报） |

`events[]` 元素（与 RV1106 端 `dms_event_record_t` / events.csv 一一对应）：

| 字段 | 类型 | 单位 | 可空 | 说明 |
|------|------|------|------|------|
| event_id | string | — | 否 | 设备侧唯一 ID（建议 `<device_id>-<seq>`），用于幂等去重 |
| timestamp_ms | int64 | ms | 否 | 事件发生时刻（设备时钟） |
| event_type | string | — | 否 | `EYE_CLOSED` / `LONG_EYE_CLOSED` / `YAWN` / `HEAD_DOWN` / `FACE_LOST` / `FATIGUE_WARNING` / `FATIGUE_HIGH` |
| risk_level | string | — | 否 | `NORMAL` / `ATTENTION` / `WARNING` / `HIGH`（NORMAL 不记录不上报） |
| risk_score | int | 分 | 否 | 0~100 安全风险分（**非绩效分**，见隐私文档 §4） |
| duration_ms | int64 | ms | 否 | 事件持续时间 |
| confidence | int | % | 否 | 0~100 |
| ear | float | — | 是 | 眼睛纵横比（无量纲）；无数据为 null |
| ear_baseline | float | — | 是 | EAR 个人基线 |
| mar | float | — | 是 | 嘴部纵横比 |
| mar_baseline | float | — | 是 | MAR 个人基线 |
| head_down_score | float | — | 是 | 低头分数 0~1 |
| face_score | float | — | 是 | 人脸检测置信度 0~1 |
| vehicle_speed_kmh | float | km/h | 是 | 车速；CAN 未接入时为 null，**不得编造** |
| route_id | string | — | 是 | 线路 ID（运营系统绑定；无信息为 null） |
| driver_id | string | — | 是 | 司机 ID（排班绑定，**非人脸识别结果**；无信息为 null） |
| shift_id | string | — | 是 | 班次 ID |
| attribution | string | — | 否 | 归责，默认 `UNKNOWN`；取值见 §0.3 |
| snapshot_ref | string | — | 是 | 事件快照对象存储引用；未上传为 null |

### I2.3 响应与错误码

成功：`200 OK`

```json
{ "accepted": 2, "rejected": 0, "errors": [] }
```

部分拒绝仍为 200，`errors[]` 给出逐条原因；整体错误使用统一错误信封：

```json
{ "error": { "code": "INVALID_PAYLOAD", "message": "events[0].timestamp_ms 缺失", "details": {} } }
```

| HTTP | code | 含义 | 设备行为 |
|------|------|------|----------|
| 400 | INVALID_PAYLOAD | 字段缺失/类型错误/取值越界 | 记日志，不重试（修正数据后随下一批） |
| 401 | UNAUTHORIZED | 设备密钥缺失或无效 | 停止重试，告警待人工处理 |
| 404 | UNKNOWN_DEVICE | device_id 未注册 | 停止重试，等待设备注册 |
| 409 | DUPLICATE_EVENT | event_id 重复（幂等） | 视为成功，不重试 |
| 413 | PAYLOAD_TOO_LARGE | 超过批量/体积限制 | 拆分批次后重试 |
| 429 | RATE_LIMITED | 服务端限流 | 按 `Retry-After` 退避 |
| 500 | INTERNAL_ERROR | 服务器内部错误 | 指数退避重试 |

### I2.4 升级兼容策略

- URL 主版本 `/api/v1` 与 body 的 `protocol_version` 双重标识；不兼容变更必须升主版本（`/api/v2`），v1 至少保留一个版本周期。
- **只增不改**：新增字段必须为可空/有默认值；服务器必须忽略未知字段；删除或改义字段视为不兼容变更。
- 枚举值只增不减；客户端/服务器对未知枚举值的默认行为：按 `UNKNOWN` 处理并记日志，不得报错丢弃整批。

---

## I3. STM32 → Server/Gateway（车辆运动事件）

> 状态：契约定义（v1）。STM32 车辆运动节点（TASK B/B2）与舒适度引擎（TASK 13/14）按本节上报。
> 传输路径二选一（实现时确定）：① STM32 → 4G DTU → Server 直连；② STM32 →（车内链路）→ RV1106 → Server 转发。无论哪条路径，**HTTP/JSON 载荷格式一致**，服务器不感知差异。

### I3.1 传输约定

| 项 | 值 |
|----|----|
| 方法/路径 | 事件：`POST /api/v1/vehicle/events`；舒适度采样：`POST /api/v1/comfort/samples` |
| 编码/认证/批量/超时 | 同 I2.1 |
| 时间戳基准 | 见 §0.2；STM32 无 RTC，时间戳由网关（DTU 或 RV1106）在打点时写入 `timestamp_ms`，STM32 侧只提供 `uptime_ms`（可空） |

### I3.2 车辆运动事件字段表（`POST /api/v1/vehicle/events`）

顶层结构同 I2.2（`device_id` 如 `stm32-bus-0001`）。`events[]` 元素：

| 字段 | 类型 | 单位 | 可空 | 说明 |
|------|------|------|------|------|
| event_id | string | — | 否 | 幂等 ID（网关生成） |
| timestamp_ms | int64 | ms | 否 | 事件时刻（网关时钟） |
| uptime_ms | int64 | ms | 是 | STM32 开机毫秒，用于与 I1 时间线对齐 |
| event_type | string | — | 否 | `HARSH_BRAKE` / `HARSH_ACCEL` / `SHARP_TURN` / `BUMP` / `VIBRATION`（枚举可扩展） |
| confidence | int | % | 否 | 0~100；IMU/CAN 融合置信度 |
| accel_x_ms2 | float | m/s² | 否 | 重力补偿后线加速度，车体系 +X 前 |
| accel_y_ms2 | float | m/s² | 否 | 车体系 +Y 左 |
| accel_z_ms2 | float | m/s² | 否 | 车体系 +Z 上 |
| jerk_ms3 | float | m/s³ | 是 | 加加速度（滤波微分+低通）；未启用为 null |
| speed_kmh | float | km/h | 是 | 车速（来自 CAN）；CAN 未接入为 null，**不得编造 CAN ID 或车速** |
| source | string | — | 否 | `IMU` / `CAN` / `FUSED` |
| attribution | string | — | 否 | 默认 `UNKNOWN`；车端不做司机归责推断（融合在服务器侧，见 I4 bus/events） |

坐标系约定与 TASK B 一致：+X 车头方向、+Y 左、+Z 上；安装方向矩阵在 STM32 侧完成变换后再上报。

### I3.3 舒适度采样字段表（`POST /api/v1/comfort/samples`）

聚合采样（默认每 10s 一条，仅工程初始值，需实车标定）：

| 字段 | 类型 | 单位 | 可空 | 说明 |
|------|------|------|------|------|
| timestamp_ms | int64 | ms | 否 | 采样窗口结束时刻 |
| window_ms | int | ms | 否 | 窗口长度（默认 10000） |
| smoothness_x / smoothness_y / smoothness_z | float | m/s² | 否 | 三轴线加速度 RMS（平稳度，越小越平稳） |
| event_count | int | 次 | 否 | 窗口内车辆运动事件数 |
| trip_comfort_index | float | 分 | 是 | 行程舒适度指数 0~100（**内部工程指标**，未标定前不对外展示） |
| route_id / vehicle_id / shift_id | string | — | 是 | 运营绑定信息 |

### I3.4 响应与错误码

与 I2.3 相同（统一错误信封 + 相同错误码表）。

### I3.5 升级兼容策略

与 I2.4 相同；另外：STM32 固件升级周期长，服务器必须**容忍旧固件缺失新字段**（全部新增字段可空），不得以缺字段为由拒收。

---

## I4. Server → Web（REST API）

> 状态：契约定义（v1）。Server（TASK D/E）与 Web（TASK F）按本节对接。
> 通用约定：
> - 所有列表接口支持公共查询参数：`from_ms`、`to_ms`（int64，epoch 毫秒）、`limit`（默认 100，最大 1000）、`offset`（默认 0）。
> - 成功列表响应信封：`{ "data": [...], "meta": { "count": n, "limit": l, "offset": o } }`；单对象响应直接返回对象。
> - 错误响应统一错误信封（格式同 I2.3）。
> - 所有时间字段为 epoch 毫秒 UTC；Web 侧负责本地化展示。
> - 协议版本：URL 主版本 `/api/v1`；兼容策略同 I2.4。

### I4.1 端点总表

| 方法 | 路径 | 用途 | 主要查询参数 |
|------|------|------|--------------|
| GET | `/health` | 健康检查（无需鉴权） | — |
| GET | `/api/v1/dms/events` | DMS 疲劳事件列表 | `driver_id` `vehicle_id` `risk_level` `event_type` |
| GET | `/api/v1/vehicle/events` | 车辆运动事件列表 | `vehicle_id` `event_type` |
| GET | `/api/v1/comfort/samples` | 舒适度采样时间序列 | `vehicle_id` `route_id` |
| GET | `/api/v1/bus/events` | 融合后公交安全事件（时间线） | `vehicle_id` `route_id` `attribution` |
| GET | `/api/v1/dashboard/overview` | 运营总览聚合 | — |
| GET | `/api/v1/drivers/{id}/safety` | 司机安全摘要 + 关怀建议 | `days`（默认 7） |
| GET | `/api/v1/routes/{id}/risk` | 线路风险聚合 | `days` |
| GET | `/api/v1/vehicles/{id}/health` | 车辆/设备健康 | — |
| GET | `/api/v1/shifts/{id}/risk` | 班次风险 + 排班复核建议 | — |
| GET | `/api/v1/events` | 统一事件流（全类型，Timeline 页用） | `source`（`dms`/`vehicle`/`bus`） |

### I4.2 端点响应要点

- `GET /health` → `{ "status": "ok", "version": "x.y.z", "uptime_s": 123, "db": "ok" }`；异常时 503 + `status:"degraded"`。
- `GET /api/v1/dms/events` / `vehicle/events` / `comfort/samples`：列表元素字段同 I2.2 / I3.2 / I3.3 的入库字段，另加服务器侧 `received_at_ms`（int64，可空否=否）。
- `GET /api/v1/bus/events`：融合事件（bus_event_fusion 输出）：

| 字段 | 类型 | 单位 | 可空 | 说明 |
|------|------|------|------|------|
| event_id | string | — | 否 | 融合事件 ID |
| timestamp_ms | int64 | ms | 否 | 时间线锚点时刻 |
| vehicle_id / route_id / shift_id / driver_id | string | — | 是 | 运营绑定 |
| dms_event_ids | array[string] | — | 否 | 关联的 DMS 事件（可为空数组） |
| vehicle_event_ids | array[string] | — | 否 | 关联的车辆运动事件（可为空数组） |
| attribution | string | — | 否 | `UNKNOWN` / `SUSPECTED_DRIVER` / `SUSPECTED_ROAD` / `SUSPECTED_VEHICLE` / `CONFIRMED_*`（仅人工写入） |
| review_status | string | — | 否 | `PENDING` / `REVIEWED`；`SUSPECTED_*` 初始必须为 `PENDING` |
| summary | string | — | 否 | 人类可读摘要（措辞遵守红线） |

- `GET /api/v1/dashboard/overview` → `{ "active_vehicles": n, "alerts_24h": { "WARNING": n, "HIGH": n }, "comfort_avg_24h": f|null, "top_risk_routes": [...] }`（聚合口径见各实现模块 README）。
- `GET /api/v1/drivers/{id}/safety` → 安全风险摘要 + Driver Care 建议（休息建议、复核提示）；**响应中禁止出现任何绩效/处罚字段**。
- `GET /api/v1/routes/{id}/risk` → 线路聚合风险（事件密度、高发时段），用于线路整改而非司机追责。
- `GET /api/v1/vehicles/{id}/health` → `{ "vehicle_id", "rv1106": { "link": "OK|DEGRADED|LOST", "last_heartbeat_ms": t|null }, "stm32": { ... }, "uptime_s": n|null }`。
- `GET /api/v1/shifts/{id}/risk` → 班次风险与 `schedule_suggestion`（如 `REST_RECOMMENDED` / `SCHEDULE_REVIEW`，仅为建议，人工决策）。
- `GET /api/v1/events` → 跨源统一事件流，元素含 `source` 字段区分 `dms`/`vehicle`/`bus`，按 `timestamp_ms` 倒序。

### I4.3 错误码

| HTTP | code | 含义 |
|------|------|------|
| 400 | INVALID_QUERY | 查询参数非法（如 limit>1000、时间区间倒置） |
| 401 | UNAUTHORIZED | 未登录或令牌失效（Web 会话鉴权方案待部署阶段定） |
| 403 | FORBIDDEN | 无权访问该资源（如越权查看其他线路） |
| 404 | NOT_FOUND | `{id}` 对应资源不存在 |
| 500 | INTERNAL_ERROR | 服务器内部错误 |
| 503 | SERVICE_UNAVAILABLE | 依赖不可用（如数据库） |

---

## 附：版本历史

| 版本 | 日期 | 说明 |
|------|------|------|
| v1.0 | 2026-08-25 | 初始统一版本；I1 对齐已实现代码（修正 07 文档帧长笔误：15/47 字节）；I2/I3/I4 为契约定义 |
