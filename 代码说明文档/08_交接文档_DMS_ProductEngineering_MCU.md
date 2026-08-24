# DMS 项目交接文档

> **日期**：2026-08-23
> **阶段**：Product Engineering V1 + MCU Integration V1 完成
> **状态**：两个 Agent 独立开发完成，等待 Integration Phase

---

## 目录

- [1. 项目全景](#1-项目全景)
- [2. 双 Agent 分工与当前状态](#2-双-agent-分工与当前状态)
- [3. 已验收通过的算法模块](#3-已验收通过的算法模块)
- [4. Agent B 交付物清单（本次完成）](#4-agent-b-交付物清单本次完成)
- [5. RV1106 侧新增模块详解](#5-rv1106-侧新增模块详解)
- [6. MCU 侧固件模块详解](#6-mcu-侧固件模块详解)
- [7. 协议联调工具链](#7-协议联调工具链)
- [8. 自启动与异常恢复](#8-自启动与异常恢复)
- [9. 测试结果汇总](#9-测试结果汇总)
- [10. 严格禁止修改的文件](#10-严格禁止修改的文件)
- [11. 风险阈值声明](#11-风险阈值声明)
- [12. Integration Phase 接口清单](#12-integration-phase-接口清单)
- [13. 后续路线建议](#13-后续路线建议)

---

## 1. 项目全景

```
┌─────────────────────────────────────────────────────────────┐
│                    DMS 疲劳驾驶检测系统                       │
│                                                             │
│  ┌──────────────┐    UART     ┌──────────────┐              │
│  │   RV1106     │◄───────────►│     MCU      │              │
│  │              │  二进制协议  │              │              │
│  │  看懂司机     │             │  可靠执行     │              │
│  │              │             │              │              │
│  │  RetinaFace  │             │  蜂鸣器       │              │
│  │  106 Landmark│             │  心跳监控     │              │
│  │  EAR/MAR     │             │  看门狗       │              │
│  │  Risk Manager│             │  IMU(预留)    │              │
│  │  Event Logger│             │  CAN(预留)    │              │
│  │  MCU Protocol│             │  电源管理     │              │
│  └──────┬───────┘             └──────────────┘              │
│         │                                                   │
│         │  SD 卡                                            │
│         ▼                                                   │
│  /mnt/sdcard/dms/                                           │
│  ├── events/events.csv                                      │
│  ├── log/console.log                                        │
│  └── sessions/                                              │
└─────────────────────────────────────────────────────────────┘
```

**核心理念**：RV1106 负责"看懂司机"，MCU 负责"可靠执行"。复杂视觉算法不下放到 MCU。

---

## 2. 双 Agent 分工与当前状态

```
Agent A（RV1106 视觉性能线）          Agent B（MCU 安全执行线）
═══════════════════════════════       ════════════════════════════════

VI / ISP / RGA / VENC / NPU           ✅ Risk Manager（已完成）
        │                             ✅ Event Logger（已完成）
        │                             ✅ MCU Protocol 编解码（已完成）
        │                             ✅ Alarm Policy（已完成）
        │                             ✅ MCU 固件模块（已完成）
        │                             ✅ 协议模拟器 + 监控器（已完成）
        │                             ✅ 压力测试（已完成）
        │                             ✅ 自启动脚本（已完成，未部署）
        ▼
   最终输出 dms_result
        │
        ▼
   ┌─────────────────────────┐
   │   Integration Phase     │  ← 等待 Agent A 完成后开始
   │   （薄层 glue code）     │
   └─────────────────────────┘
```

| Agent | 状态 | 说明 |
|-------|------|------|
| Agent A | 🔨 进行中 | 硬件视频链 V2 重构（VI/RGA/VENC/NPU） |
| Agent B | ✅ 完成 | 所有独立模块已编译、测试、就绪 |

---

## 3. 已验收通过的算法模块

以下算法已验收通过，**不要重新修改**：

| 算法 | 状态 | 验收目录 |
|------|------|----------|
| RetinaFace 人脸检测 | ✅ PASS | `acceptance/p0_*` |
| 106 点 Landmark | ✅ PASS | `acceptance/lm106_*` |
| EAR（眼睛纵横比） | ✅ PASS | `acceptance/fatigue_*` |
| MAR（嘴巴纵横比） | ✅ PASS | `acceptance/fatigue_*` |
| 闭眼检测 | ✅ PASS | `acceptance/fatigue_*` |
| 长闭眼检测 | ✅ PASS | `acceptance/fatigue_*` |
| 哈欠检测 | ✅ PASS | `acceptance/fatigue_*` |
| 低头检测 | ✅ PASS | `acceptance/fatigue_*` |
| 疲劳基础状态机 | ✅ PASS | `acceptance/fatigue_*` |

---

## 4. Agent B 交付物清单（本次完成）

### 4.1 新增文件总览

```
项目根目录/
│
├── include/                          # RV1106 侧头文件
│   ├── dms_risk_manager.h            # 139 行 - 风险评估
│   ├── dms_event_logger.h            #  78 行 - 事件日志
│   ├── dms_mcu_protocol.h            # 111 行 - MCU 协议编解码
│   └── dms_alarm_policy.h            #  74 行 - 报警策略
│
├── src/dms/                          # RV1106 侧源文件
│   ├── dms_risk_manager.c            # 478 行
│   ├── dms_event_logger.c            # 190 行
│   ├── dms_mcu_protocol.c            # 207 行
│   └── dms_alarm_policy.c            # 127 行
│
├── mcu/                              # MCU 侧固件（独立工程）
│   ├── include/
│   │   ├── dms_protocol.h            # 144 行 - UART 字节流 parser
│   │   ├── dms_event_handler.h       #  47 行 - 事件分发
│   │   ├── dms_heartbeat.h           #  65 行 - 心跳监控
│   │   ├── dms_alarm.h               #  87 行 - 蜂鸣器非阻塞控制
│   │   ├── dms_watchdog.h            #  59 行 - 看门狗
│   │   ├── dms_vehicle.h             #  57 行 - CAN 预留接口
│   │   └── dms_imu.h                 #  54 行 - IMU 预留接口
│   ├── src/
│   │   ├── dms_protocol.c            # 241 行
│   │   ├── dms_event_handler.c       # 105 行
│   │   ├── dms_heartbeat.c           #  79 行
│   │   ├── dms_alarm.c               # 180 行
│   │   ├── dms_watchdog.c            #  91 行
│   │   ├── dms_vehicle.c             #  54 行
│   │   └── dms_imu.c                 #  28 行
│   ├── tests/
│   │   └── test_dms_protocol_stress.c # 288 行 - 10000 包压力测试
│   └── README.md
│
├── tools/                            # PC 侧联调工具
│   ├── dms_mcu_simulator.py          # 315 行 - RV1106 协议模拟器
│   └── dms_protocol_monitor.py       # 263 行 - 协议抓包监控
│
├── tests/                            # RV1106 侧单元测试
│   ├── test_dms_risk_manager.c       # 364 行 - 24 个测试
│   └── test_dms_mcu_protocol.c       # 289 行 - 42 个测试
│
├── scripts/                          # 自启动脚本
│   ├── S99dms                        #  47 行 - init.d 脚本
│   └── dms_start.sh                  #  95 行 - 启动脚本（有限重启）
│
└── 代码说明文档/
    └── 07_DMS_MCU_Protocol.md        # MCU 协议文档
```

### 4.2 代码量统计

| 类别 | 文件数 | 总行数 |
|------|--------|--------|
| RV1106 头文件 | 4 | 402 |
| RV1106 源文件 | 4 | 1,002 |
| MCU 头文件 | 7 | 513 |
| MCU 源文件 | 7 | 778 |
| Python 工具 | 2 | 578 |
| 测试文件 | 3 | 941 |
| 脚本 | 2 | 142 |
| **合计** | **29** | **4,356** |

### 4.3 CMakeLists.txt 修改

仅最小追加 4 行到 `add_executable`：

```cmake
src/dms/dms_risk_manager.c      # 第 57 行
src/dms/dms_event_logger.c      # 第 58 行
src/dms/dms_mcu_protocol.c      # 第 59 行
src/dms/dms_alarm_policy.c      # 第 60 行
```

**未修改其他任何 CMake 配置。**

---

## 5. RV1106 侧新增模块详解

### 5.1 Risk Manager（风险评估器）

**文件**：`include/dms_risk_manager.h` + `src/dms/dms_risk_manager.c`

**职责**：基于疲劳特征事件，在滑动时间窗口内统计事件频次，输出统一的风险等级。

**风险等级**：

```
NORMAL → ATTENTION → WARNING → HIGH
  │         │          │         │
  无风险   单次哈欠   长闭眼    重复长闭眼
           短暂低头   持续低头   组合高风险
```

**滑动窗口规则**：

| 窗口 | 统计 | 阈值 | 结果 |
|------|------|------|------|
| 60s 短窗口 | 长闭眼次数 | ≥2 | HIGH |
| 60s 短窗口 | 哈欠次数 | ≥3 | WARNING |
| 60s 短窗口 | 事件种类数 | ≥2 | WARNING |
| 5min 长窗口 | 哈欠次数 | ≥5 | WARNING |
| 5min 长窗口 | 疲劳事件总数 | ≥8 | HIGH |
| 实时 | 长闭眼持续 | ≥1500ms | WARNING |
| 实时 | 持续低头 | ≥3000ms | WARNING |

**恢复滞后**：降级需保持 5 秒无事件后才执行，防止等级抖动。

**关键设计决策**：
- 长闭眼时不重复记录 eye_closed 事件（避免 combo 误判）
- 所有阈值集中在 `dms_risk_config_t`，不散落 magic number
- 评分 0~100，每秒衰减 2 分

**API**：

```c
bool dms_risk_manager_init(void);
dms_risk_result_t dms_risk_manager_update(const dms_risk_input_t *input);
void dms_risk_manager_reset(void);
dms_risk_level_t dms_risk_manager_get_level(void);
```

### 5.2 Event Logger（事件日志器）

**文件**：`include/dms_event_logger.h` + `src/dms/dms_event_logger.c`

**输出路径**：`/mnt/sdcard/dms/events/events.csv`

**CSV 格式**：

```csv
timestamp,event_type,risk_level,risk_score,duration_ms,ear,ear_baseline,mar,mar_baseline,head_down_score,face_score,vehicle_speed,route_id,driver_id,shift_id
2026-08-23 16:30:00.123,LONG_EYE_CLOSED,WARNING,40,1600,0.1023,0.2850,0.2100,0.2500,0.0500,0.9500,0.0,,,
```

**规则**：
- NORMAL 事件不记录
- 预留字段（vehicle_speed/route_id/driver_id/shift_id）当前无数据时填 0 或空，不编造
- 快照接口已定义：`dms_event_logger_save_snapshot()`，文件名格式 `YYYYMMDD_HHMMSS_EVENT_TYPE.jpg`

### 5.3 MCU Protocol（协议编解码）

**文件**：`include/dms_mcu_protocol.h` + `src/dms/dms_mcu_protocol.c`

**帧格式**：

```
[0xAA] [0x55] [Ver] [Event] [Risk] [Conf] [DurH] [DurL] [Ts0..3] [PayLen] [Payload..] [CRCH] [CRCL]
  2B     2B     1B     1B      1B     1B     2B BE       4B BE      1B       0~32B       2B
```

- 最小帧：15 字节（无 payload）
- 最大帧：47 字节（32 字节 payload）
- CRC16-CCITT，多项式 0x1021，初值 0xFFFF
- 已验证：CRC16("123456789") = 0x29B1 ✓

**事件码**：

| 码 | 事件 | 码 | 事件 |
|----|------|----|------|
| 0x01 | HEARTBEAT | 0x13 | HEAD_DOWN |
| 0x10 | EYE_CLOSED | 0x14 | FACE_LOST |
| 0x11 | LONG_EYE_CLOSED | 0x20 | FATIGUE_WARNING |
| 0x12 | YAWN | 0x21 | FATIGUE_HIGH |

### 5.4 Alarm Policy（报警策略）

**文件**：`include/dms_alarm_policy.h` + `src/dms/dms_alarm_policy.c`

| 风险等级 | 报警级别 | 说明 |
|----------|----------|------|
| NORMAL | ALARM_NONE | 不响 |
| ATTENTION | ALARM_NONE | 不响 |
| WARNING | ALARM_WARNING | 警告音 |
| HIGH | ALARM_HIGH | 高危警报 |

- 冷却时间 3 秒，防止连续蜂鸣
- 当前只输出报警级别，不直接调用 GPIO
- 现有 `beep_warn` 保留作为实验 fallback

---

## 6. MCU 侧固件模块详解

### 6.1 架构

```
UART RX 中断
    │
    ▼
dms_protocol_feed_byte()     ← 逐字节状态机 parser
    │
    ▼ (完整帧)
dms_event_handler_on_frame()  ← 事件分发
    │
    ├─→ dms_heartbeat_on_received()  ← 心跳监控
    ├─→ dms_alarm_set_level()        ← 蜂鸣器控制
    └─→ 更新 dms_mcu_state_t         ← 全局安全状态

系统定时器 (1ms)
    │
    ▼
dms_event_handler_tick()
    ├─→ dms_heartbeat_tick()   ← 检查心跳超时
    ├─→ dms_alarm_tick()       ← 驱动蜂鸣器状态机
    └─→ dms_watchdog_tick()    ← 看门狗状态更新
```

### 6.2 UART Parser（字节流状态机）

**文件**：`mcu/include/dms_protocol.h` + `mcu/src/dms_protocol.c`

```
PARSER_SYNC_0 ──0xAA──▶ PARSER_SYNC_1 ──0x55──▶ READ_FIXED ──▶ READ_PAYLOAD ──▶ READ_CRC ──▶ dispatch
      ▲                                                                                       │
      │                                                                                 CRC 错误
      └─────────────────────────────────────────────────────────────────────────────────────┘
```

**容错**：分包、粘包、CRC 错误、噪声字节、截断帧、0xAA 0xAA 连续头。

### 6.3 心跳监控

**文件**：`mcu/include/dms_heartbeat.h` + `mcu/src/dms_heartbeat.c`

| 状态 | 条件 | 含义 |
|------|------|------|
| DMS_LINK_OK | < 3 秒未收到 | 正常 |
| DMS_LINK_DEGRADED | 3~5 秒未收到 | 降级 |
| DMS_LINK_LOST | > 5 秒未收到 | 丢失 |

### 6.4 蜂鸣器非阻塞状态机

**文件**：`mcu/include/dms_alarm.h` + `mcu/src/dms_alarm.c`

```
BUZZER_IDLE → BUZZER_ON → BUZZER_OFF → (repeat N) → BUZZER_PAUSE → BUZZER_ON → ...
```

| 级别 | ON | OFF | 重复 | 暂停 |
|------|-----|-----|------|------|
| SHORT | 100ms | - | 1 | - |
| WARNING | 200ms | 300ms | 3 | 1000ms |
| HIGH | 100ms | 100ms | 5 | 500ms |

**禁止 `delay_ms()`**。完全由系统 tick 驱动。

**集成时唯一需要实现的硬件接口**：

```c
void dms_alarm_buzzer_hw_set(bool on);  // 由 BSP 层实现 GPIO 控制
```

### 6.5 看门狗

**文件**：`mcu/include/dms_watchdog.h` + `mcu/src/dms_watchdog.c`

- 心跳丢失超过 10 秒后调用 `dms_watchdog_request_rv1106_reset()`
- 当前为 stub，只记录请求计数，不实际控制硬件
- 未来由 MCU GPIO 控制 RV1106 复位引脚

### 6.6 Driver Safety Output

```c
typedef struct {
    uint8_t  link_ok;             // 通信链路正常
    uint8_t  risk_level;          // 当前风险等级 (0~3)
    uint8_t  alarm_level;         // 当前报警级别 (0~3)
    uint8_t  last_event;          // 最后收到的事件码
    uint32_t event_duration_ms;   // 最后事件的持续时间
    uint32_t last_heartbeat_ms;   // 最后心跳时间
} dms_mcu_state_t;  // 16 bytes
```

通过 `dms_event_handler_get_state()` 获取只读指针。未来蜂鸣器、仪表、CAN、继电器统一读取此状态。

### 6.7 CAN / IMU 预留接口

| 模块 | 文件 | 状态 |
|------|------|------|
| CAN | `mcu/include/dms_vehicle.h` + `mcu/src/dms_vehicle.c` | stub，不编造 CAN ID |
| IMU | `mcu/include/dms_imu.h` + `mcu/src/dms_imu.c` | stub，接口已定义 |

### 6.8 MCU 资源占用

| 项目 | 大小 |
|------|------|
| Flash (text) | ~4 KB |
| RAM (bss) | ~88 bytes |
| RAM (parser ctx) | ~76 bytes |
| RAM (栈) | ~100 bytes |
| **RAM 总计** | **~264 bytes** |

对 STM32F4（512KB Flash / 128KB RAM）：**Flash <1%，RAM <1%**。

---

## 7. 协议联调工具链

### 7.1 RV1106 协议模拟器

**文件**：`tools/dms_mcu_simulator.py`

```bash
# 发送单个事件
python3 tools/dms_mcu_simulator.py --port /dev/ttyUSB0 \
    --event LONG_EYE_CLOSED --risk WARNING --duration 1800

# 持续心跳
python3 tools/dms_mcu_simulator.py --port /dev/ttyUSB0 --heartbeat

# 场景测试
python3 tools/dms_mcu_simulator.py --port /dev/ttyUSB0 --scenario warning

# 虚拟串口（无硬件，输出到 stdout）
python3 tools/dms_mcu_simulator.py --virtual --scenario high
```

**支持场景**：`normal`, `attention`, `warning`, `high`, `heartbeat_loss`

### 7.2 协议抓包监控器

**文件**：`tools/dms_protocol_monitor.py`

```bash
# 串口监控
python3 tools/dms_protocol_monitor.py --port /dev/ttyUSB0

# 管道联调（模拟器 → 监控器）
python3 tools/dms_mcu_simulator.py --virtual --scenario warning | \
    python3 tools/dms_protocol_monitor.py --stdin

# 离线分析
python3 tools/dms_protocol_monitor.py --file capture.bin
```

**输出示例**：

```
[18:00:24.620]
  LONG_EYE_CLOSED
  risk=WARNING confidence=92
  duration=1800ms
  timestamp=1787479224
```

### 7.3 无硬件联调验证

```bash
# 终端 1：启动模拟器发送 warning 场景
python3 tools/dms_mcu_simulator.py --virtual --scenario warning | \
    python3 tools/dms_protocol_monitor.py --stdin
```

已验证通过：心跳帧、事件帧、场景序列均可正确编码 → 传输 → 解析 → 显示。

---

## 8. 自启动与异常恢复

### 8.1 脚本文件

| 文件 | 说明 |
|------|------|
| `scripts/S99dms` | init.d 自启动脚本（等待 20s 系统稳定） |
| `scripts/dms_start.sh` | 启动脚本（有限重启策略） |

### 8.2 重启策略

```
程序退出
    │
    ▼
计算延迟 = 5s + 重启次数 × 2s（最大 60s）
    │
    ▼
检查 5 分钟窗口内重启次数
    │
    ├─ < 10 次 → 等待延迟后重启
    │
    └─ ≥ 10 次 → 等待 60s，重置计数，重启
```

### 8.3 部署注意事项

**当前仅生成脚本，未部署到板端。**

部署步骤（等 Agent A 完成后统一执行）：

```bash
# 1. 复制启动脚本
cp scripts/dms_start.sh /usr/bin/dms_start.sh
chmod +x /usr/bin/dms_start.sh

# 2. 复制 init.d 脚本
cp scripts/S99dms /etc/init.d/S99dms
chmod +x /etc/init.d/S99dms

# 3. 确认不冲突（如果 S99hand_capture 存在）
# rm /etc/init.d/S99hand_capture  # 或重命名

# 4. 重启验证
reboot
```

---

## 9. 测试结果汇总

### 9.1 Risk Manager 单元测试

```
=== Results: 24 passed, 0 failed ===
```

| 测试场景 | 预期 | 结果 |
|----------|------|------|
| 正常状态 | NORMAL | ✅ |
| 一次哈欠 | 不得 HIGH | ✅ |
| 普通眨眼 | NORMAL（含恢复滞后） | ✅ |
| 长闭眼 | WARNING | ✅ |
| 短时间重复长闭眼 | HIGH | ✅ |
| 持续低头 | WARNING | ✅ |
| 恢复正常 | 有滞后后回 NORMAL | ✅ |
| 多次哈欠 | WARNING | ✅ |
| 无脸 | 不得 HIGH | ✅ |
| 组合事件 | ≥ATTENTION | ✅ |

### 9.2 MCU Protocol 单元测试

```
=== Results: 42 passed, 0 failed ===
```

| 测试场景 | 结果 |
|----------|------|
| 基本编码/解码一致性 | ✅ |
| 心跳帧编码/解码 | ✅ |
| CRC 错误检测 | ✅ |
| Header 错误检测 | ✅ |
| 缓冲区太小 | ✅ |
| NULL 参数 | ✅ |
| 所有 8 种事件类型 | ✅ |
| CRC16 已知值 (0x29B1) | ✅ |
| 短缓冲区解码 | ✅ |

### 9.3 协议压力测试（10000 包）

```
Test Composition:
  normal_packets      = 3925    (39%)
  split_packets       = 995     (10%)  随机拆包
  merged_packets      = 1025    (10%)  粘包
  crc_error_packets   = 995     (10%)  CRC 损坏
  noise_packets       = 1037    (10%)  噪声字节
  truncated_packets   = 1017    (10%)  截断帧
  unknown_event       = 519     (5%)   未知事件
  unknown_version     = 487     (5%)   未知版本

Parser Results:
  valid_frames        = 10527   ✓
  crc_errors          = 995     ✓（与注入一致）
  resync_count        = 2516    ✓
  parser_errors       = 1504    ✓
  callback_count      = 10527   ✓（与 valid_frames 一致）

=== STRESS TEST PASSED ===
```

### 9.4 编译验证

| 编译目标 | 结果 |
|----------|------|
| RV1106 新模块（交叉编译器语法检查） | ✅ 4/4 通过 |
| MCU 全部模块（gcc -Wall -Wextra） | ✅ 7/7 通过，零警告 |
| 单元测试（gcc -Wall -Wextra） | ✅ 3/3 通过，零警告 |

---

## 10. 严格禁止修改的文件

以下文件属于 Agent A 的工作范围，**Agent B 完全未修改**：

| 文件 | 说明 |
|------|------|
| `src/hal/hal_camera_rkmpi.c` | 摄像头 HAL（RKMPI） |
| `src/dms/dms_rga_preprocess.c` | RGA 硬件预处理 |
| `src/dms/dms_retinaface.c` | RetinaFace 推理 |
| `src/dms/dms_visualize.c` | 视频 OSD/可视化 |
| `src/app/state_machine.c` | 主状态机 |
| `src/app/main.c` | 主入口 |
| `src/dms/dms_fatigue_logic.c` | 疲劳规则状态机 |
| `src/dms/dms_fatigue_features.c` | 疲劳特征计算 |
| `src/dms/dms_ai_thread.c` | AI 推理线程 |
| `src/dms/dms_stream_server.c` | 视频流服务器 |

---

## 11. 风险阈值声明

> **⚠️ 重要：以下阈值是 engineering_default，不是最终标准。**

当前 Risk Manager 中的阈值（60s 窗口、5min 窗口、次数阈值等）是**第一版工程默认值**。

它们**不是**：
- 公交司机处罚标准
- 医学疲劳诊断标准
- 最终运营考核标准

后续必须通过**真实公交场景数据采集**和**运营数据校准**来标定。

代码中**严禁出现** `penalty`、`driver_deduct`、`fine`、`punishment` 等自动处罚概念。

---

## 12. Integration Phase 接口清单

等 Agent A 视频链验收完成后，在 RV1106 主程序中添加以下 glue code：

### 12.1 RV1106 侧需要调用的接口

```c
// ========== 初始化（main 或 state_machine 启动时） ==========
dms_risk_manager_init();
dms_event_logger_init();
dms_alarm_policy_init();

// ========== 每帧 AI 推理完成后 ==========
// 从 dms_result 构造输入
dms_risk_input_t input = {
    .face_found     = result->face_found,
    .eye_closed     = (result->state == EYE_CLOSED),
    .long_eye_closed = (result->state == LONG_EYE_CLOSED),
    .yawn           = (result->state == YAWN),
    .head_down      = (result->state == HEAD_DOWN),
    .ear            = result->ear,
    .mar            = result->mar,
    .head_down_score = result->head_down_score,
    .timestamp_ms   = get_system_ms(),
};

// 风险评估
dms_risk_result_t risk = dms_risk_manager_update(&input);

// 事件记录
if (risk.save_event_requested) {
    dms_event_record_t record = { ... };
    dms_event_logger_write(&record);
}

// 报警策略
dms_alarm_level_t alarm = dms_alarm_policy_update(&risk, input.timestamp_ms);

// MCU 事件发送（如果需要）
if (risk.level >= DMS_RISK_WARNING) {
    dms_mcu_frame_t frame;
    dms_mcu_build_event_frame(&frame, event_code, risk.level,
                               confidence, duration, timestamp);
    uint8_t buf[DMS_MCU_ENCODE_BUF_SIZE];
    size_t len = dms_mcu_encode(&frame, buf, sizeof(buf));
    uart_send(buf, len);  // 需要实现 UART 发送
}

// ========== 每秒心跳 ==========
dms_mcu_heartbeat_t hb = {
    .dms_alive    = 1,
    .camera_alive = camera_ok ? 1 : 0,
    .ai_alive     = ai_ok ? 1 : 0,
    .risk_level   = (uint8_t)risk.level,
};
dms_mcu_build_heartbeat_frame(&frame, &hb, timestamp);
// ... encode + uart_send
```

### 12.2 MCU 侧已就绪的接口

| 接口 | 调用位置 | 说明 |
|------|----------|------|
| `dms_protocol_feed_byte()` | UART RX 中断 | 逐字节喂入 |
| `dms_event_handler_on_frame()` | parser 回调 | 帧解析完成 |
| `dms_event_handler_tick(ms)` | 系统定时器 | 周期驱动 |
| `dms_event_handler_get_state()` | 任意位置 | 获取安全状态 |
| `dms_alarm_buzzer_hw_set()` | **需 BSP 实现** | 蜂鸣器 GPIO |

### 12.3 集成时需要的胶水代码量

预计 **< 100 行** glue code，不修改任何现有视频/算法模块。

---

## 13. 后续路线建议

```
当前状态                    下一步                      更远
═══════════               ═══════════                ═══════════

Agent A: 视频链 V2    →   Integration V1          →  车辆适配
  （进行中）              （薄层 glue code）            （CAN DBC）
                            ↓
Agent B: MCU 闭环     →   RV1106 + MCU 联调       →  司机/线路/班次
  ✅ 完成                   （UART 实测）               管理后台
                            ↓
                         真人实测数据采集          →  云端数据分析
                            ↓
                         风险阈值标定
```

**建议**：
1. Agent B 当前阶段完成后**先停**，不要马上做 Web 后台
2. 等 Agent A 视频链验收 → Integration V1 → UART 联调
3. 有了车辆 ID、司机 ID、线路 ID、班次 ID、GPS/4G 方案后，再开云端 Agent
4. 风险阈值需要真人实测数据后再校准

---

## 附录 A：快速验证命令

```bash
# 进入项目目录
cd /home/jennie/hhh/embed_complication/hand_capture_right

# 编译并运行 Risk Manager 单元测试
gcc -Wall -Wextra -std=c11 -I include \
    -o tests/test_dms_risk_manager \
    tests/test_dms_risk_manager.c src/dms/dms_risk_manager.c
./tests/test_dms_risk_manager

# 编译并运行 MCU Protocol 单元测试
gcc -Wall -Wextra -std=c11 -I include \
    -o tests/test_dms_mcu_protocol \
    tests/test_dms_mcu_protocol.c src/dms/dms_mcu_protocol.c
./tests/test_dms_mcu_protocol

# 编译并运行协议压力测试
gcc -Wall -Wextra -std=c11 -I mcu/include \
    -o mcu/tests/test_dms_protocol_stress \
    mcu/tests/test_dms_protocol_stress.c mcu/src/dms_protocol.c
./mcu/tests/test_dms_protocol_stress

# 模拟器 + 监控器管道联调
python3 tools/dms_mcu_simulator.py --virtual --scenario warning | \
    python3 tools/dms_protocol_monitor.py --stdin
```

## 附录 B：MCU 最小集成示例

```c
#include "dms_protocol.h"
#include "dms_event_handler.h"
#include "dms_alarm.h"
#include "dms_heartbeat.h"
#include "dms_watchdog.h"

static dms_parser_t g_parser;

// UART 接收中断
void uart_rx_isr(uint8_t byte) {
    dms_protocol_feed_byte(&g_parser, byte,
                            dms_event_handler_on_frame, NULL);
}

// 系统定时器（1ms）
void systick_handler(void) {
    static uint32_t tick = 0;
    tick++;
    dms_event_handler_tick(tick);
    dms_watchdog_tick(tick);
}

// 蜂鸣器 GPIO（需要 BSP 实现）
void dms_alarm_buzzer_hw_set(bool on) {
    gpio_write(BUZZER_PIN, on);
}

int main(void) {
    dms_protocol_parser_init(&g_parser);
    dms_event_handler_init();
    dms_heartbeat_init();
    dms_alarm_init();
    dms_watchdog_init();

    while (1) {
        // 主循环
        const dms_mcu_state_t *state = dms_event_handler_get_state();
        // state->risk_level, state->alarm_level, state->link_ok ...
    }
}
```

---

*文档版本：v1.0 | 2026-08-23 | Agent B (DMS Product Engineering + MCU Integration)*
