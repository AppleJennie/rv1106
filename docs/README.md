# docs/ —— Bus DMS 系统级文档

> 本目录由离线总装 TASK J/K/L/M 产出（2026-08-25），与 `代码说明文档/`（RV1106 工程内部文档）并列。
> 分工：`代码说明文档/` 描述 RV1106 工程"现在是什么"；本目录描述整机系统"约定是什么"。

## 文档索引

| 文件 | 内容 | 什么时候看 |
|------|------|-----------|
| `DATA_PRIVACY_AND_ETHICS.md` | 隐私与数据原则（产品红线：不做人脸识别、不自动处罚、Risk Score≠绩效、归责默认 UNKNOWN） | 任何新需求评审前 |
| `SYSTEM_INTERFACES.md` | 四个接口的统一契约：I1 RV1106→MCU（UART 二进制）、I2 RV1106→Server、I3 STM32→Server、I4 Server→Web（REST） | 写任何跨模块代码前 |
| `BUS_DMS_SYSTEM_ARCHITECTURE.md` | 整体架构：四层职责边界、四大核心域、数据流图、降级设计 | 新人上手 / 架构评审 |

部署相关见 `deploy/README_SERVER_DEPLOY.md`。

## 对其他模块的接口假设（重要）

本文档组编写时，以下模块由其他 Agent 并行开发、仓库中尚不存在，文档对其接口做了如下**假设**；模块落地后如有出入，以 `SYSTEM_INTERFACES.md` 为基准对齐并回填本节：

1. **Server（TASK D/E，`server/`）**：假设提供 FastAPI 应用，WSGI/ASGI 入口为 `app.main:app`；启动时自动建表或提供初始化入口；实现 `SYSTEM_INTERFACES.md` I4 全部端点及 I2/I3 接入端点；配置从环境变量读取（变量名见 `deploy/.env.example`）。
2. **bus_event_fusion（TASK C，`bus_event_fusion/`）**：假设输出含 `attribution`（默认 `UNKNOWN`）与 `review_status`（`SUSPECTED_*` 初始为 `PENDING`）字段，落库后经 `/api/v1/bus/events` 暴露。
3. **STM32 车辆运动节点（TASK B/B2，`vehicle_mcu/`）**：假设其事件模型与本目录 I3 字段表一致（event_type/confidence/三轴加速度/jerk/可空车速），坐标系 +X 前 / +Y 左 / +Z 上；上报路径（直连 DTU 或经 RV1106 转发）未定，文档对两条路径定义了相同载荷。
4. **舒适度引擎（TASK 13/14，`passenger_comfort.c/.h`）**：假设产出 `smoothness_{x,y,z}` RMS、窗口事件计数、`trip_comfort_index`（0~100 内部工程指标），经 `/api/v1/comfort/samples` 上行与下行。
5. **Web（TASK F，`web/`）**：假设 5 页面只消费 I4 的 GET 接口；人工复核的写回接口（PATCH review_status）留待下一阶段定义，本文档未覆盖。
6. **设备认证**：`X-Device-Key` 与 Web 会话鉴权均为占位方案，正式方案待部署阶段确定。

## 已修正的上游文档问题（未改动原文档，仅在此记录）

- `代码说明文档/07_DMS_MCU_Protocol.md` 第 3 节"最小帧 13 字节 / 最大帧 45 字节"与代码实现不符；以 `src/dms/dms_mcu_protocol.c` 及 `08_交接文档` 为准：**最小 15 / 最大 47 字节**。`SYSTEM_INTERFACES.md` I1 已按正确值书写，示例帧 CRC 经同算法核算。
