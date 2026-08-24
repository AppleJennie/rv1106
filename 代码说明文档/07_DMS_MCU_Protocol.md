# DMS MCU 通讯协议 V1

## 1. 概述

RV1106（DMS 主控）与 MCU 之间通过 UART 进行二进制通讯。
RV1106 负责 AI 推理和风险评估，MCU 负责执行器控制（蜂鸣器、LED、CAN、电源管理等）。

### 职责划分

| 模块 | 职责 |
|------|------|
| RV1106 | 人脸检测、疲劳特征提取、风险评估、事件记录 |
| MCU | 蜂鸣器、IMU、CAN 总线、车辆 IO、硬件看门狗、电源管理 |

### 设计原则

- 简单、稳定、二进制协议
- 不依赖 JSON 作为车内实时协议
- 包含 CRC16 校验
- 可扩展（预留 payload）

---

## 2. 物理层

- 接口：UART（具体引脚由硬件设计决定）
- 波特率：115200（默认，可配置）
- 数据位：8
- 停止位：1
- 校验位：None
- 流控：None

---

## 3. 帧格式

```
+--------+--------+---------+-------+------------+------------+-------------+-----------+-------------+----------+----------+
| Header | Header | Version | Event | Risk Level | Confidence | Duration MS | Timestamp | Payload Len | Payload  | CRC16    |
| 0xAA   | 0x55   | 1 byte  | 1 B   | 1 byte     | 1 byte     | 2 bytes BE  | 4 bytes BE| 1 byte      | N bytes  | 2 bytes  |
+--------+--------+---------+-------+------------+------------+-------------+-----------+-------------+----------+----------+
```

### 字段说明

| 字段 | 长度 | 说明 |
|------|------|------|
| Header | 2 bytes | 固定 0xAA 0x55，用于帧同步 |
| Version | 1 byte | 协议版本，当前 0x01 |
| Event | 1 byte | 事件码，见事件码表 |
| Risk Level | 1 byte | 风险等级：0=NORMAL, 1=ATTENTION, 2=WARNING, 3=HIGH |
| Confidence | 1 byte | 置信度 0~100 |
| Duration MS | 2 bytes | 事件持续时间（毫秒），大端序 |
| Timestamp | 4 bytes | 时间戳（秒级），大端序 |
| Payload Len | 1 byte | 附加数据长度（0~32） |
| Payload | N bytes | 附加数据（心跳状态等） |
| CRC16 | 2 bytes | CRC16-CCITT，多项式 0x1021，初值 0xFFFF，覆盖 Header 到 Payload |

### 最小帧长

无 Payload 时：13 字节

### 最大帧长

Payload 32 字节时：45 字节

---

## 4. 事件码表

| 事件码 | 名称 | 说明 | 方向 |
|--------|------|------|------|
| 0x01 | HEARTBEAT | 心跳 | RV1106 → MCU |
| 0x10 | EYE_CLOSED | 闭眼 | RV1106 → MCU |
| 0x11 | LONG_EYE_CLOSED | 长闭眼 | RV1106 → MCU |
| 0x12 | YAWN | 哈欠 | RV1106 → MCU |
| 0x13 | HEAD_DOWN | 低头 | RV1106 → MCU |
| 0x14 | FACE_LOST | 人脸丢失 | RV1106 → MCU |
| 0x20 | FATIGUE_WARNING | 疲劳警告 | RV1106 → MCU |
| 0x21 | FATIGUE_HIGH | 疲劳高危 | RV1106 → MCU |

### 预留事件码

| 事件码范围 | 用途 |
|------------|------|
| 0x02~0x0F | 系统事件预留 |
| 0x15~0x1F | 检测事件预留 |
| 0x22~0x2F | 风险事件预留 |
| 0x30~0x3F | MCU → RV1106 命令预留 |
| 0x40~0xFF | 未来扩展 |

---

## 5. 心跳协议

### 发送频率

RV1106 每 **1 秒** 向 MCU 发送一次 HEARTBEAT 帧。

### 心跳 Payload 格式（4 字节）

| 字节 | 字段 | 说明 |
|------|------|------|
| 0 | dms_alive | DMS 应用存活：0=异常，1=正常 |
| 1 | camera_alive | 摄像头存活：0=异常，1=正常 |
| 2 | ai_alive | AI 推理存活：0=异常，1=正常 |
| 3 | risk_level | 当前风险等级：0~3 |

### MCU 看门狗策略（未来实现）

- MCU 超过 **5 秒** 未收到心跳 → 认为 RV1106 异常
- MCU 可执行硬件复位 RV1106
- 当前版本只完成协议定义和软件接口，不控制 MCU 硬件

---

## 6. 风险等级映射

| Risk Level 值 | 枚举 | 说明 | MCU 建议动作 |
|---------------|------|------|-------------|
| 0 | NORMAL | 无风险 | 无动作 |
| 1 | ATTENTION | 轻度注意 | 可选：LED 黄色闪烁 |
| 2 | WARNING | 警告 | 蜂鸣器警告音 |
| 3 | HIGH | 高危 | 蜂鸣器急促警报 + LED 红色 |

---

## 7. 示例帧

### 心跳帧（正常状态）

```
AA 55 01 01 00 64 00 00 00 00 10 04 01 01 01 00 [CRC_H] [CRC_L]
```

- Event = 0x01 (HEARTBEAT)
- Risk Level = 0 (NORMAL)
- Confidence = 100 (0x64)
- Duration = 0
- Timestamp = 0x00001000 (示例)
- Payload = 01 01 01 00 (dms_alive=1, camera_alive=1, ai_alive=1, risk=0)

### 长闭眼事件帧

```
AA 55 01 11 02 5A 05 DC 00 00 10 00 00 [CRC_H] [CRC_L]
```

- Event = 0x11 (LONG_EYE_CLOSED)
- Risk Level = 2 (WARNING)
- Confidence = 90 (0x5A)
- Duration = 1500ms (0x05DC)
- Timestamp = 0x00001000
- Payload Len = 0

---

## 8. 错误处理

| 错误类型 | 处理方式 |
|----------|----------|
| Header 不匹配 | 丢弃当前字节，继续搜索 0xAA 0x55 |
| CRC 校验失败 | 丢弃整帧，统计错误计数 |
| Payload Len 超限 | 丢弃整帧 |
| Version 不匹配 | 丢弃整帧，记录日志 |

---

## 9. 版本历史

| 版本 | 日期 | 说明 |
|------|------|------|
| 0x01 | 2026-08-23 | 初始版本 |
