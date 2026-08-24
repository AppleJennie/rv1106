# DMS MCU Firmware Module

## 概述

MCU 侧 DMS 安全执行模块。负责接收 RV1106 的风险评估结果，执行蜂鸣器报警、心跳监控、看门狗等安全功能。

## 职责划分

| 模块 | 职责 |
|------|------|
| RV1106 | 看懂司机：RetinaFace、106 landmark、EAR/MAR、Risk Manager |
| MCU | 可靠执行：蜂鸣器、车辆IO、IMU、CAN、心跳监控、看门狗、电源管理 |

## 目录结构

```
mcu/
├── include/
│   ├── dms_protocol.h        # UART 协议解析器（字节流状态机）
│   ├── dms_event_handler.h   # 事件分发 + Driver Safety Output
│   ├── dms_heartbeat.h       # 心跳监控状态机
│   ├── dms_alarm.h           # 蜂鸣器非阻塞控制
│   ├── dms_watchdog.h        # 看门狗接口
│   ├── dms_vehicle.h         # CAN 预留接口（stub）
│   └── dms_imu.h             # IMU 预留接口（stub）
├── src/
│   ├── dms_protocol.c
│   ├── dms_event_handler.c
│   ├── dms_heartbeat.c
│   ├── dms_alarm.c
│   ├── dms_watchdog.c
│   ├── dms_vehicle.c
│   └── dms_imu.c
├── tests/
│   └── test_dms_protocol_stress.c  # 10000 包压力测试
└── README.md
```

## 快速开始

### 编译（主机测试）

```bash
# 编译压力测试
gcc -Wall -Wextra -std=c11 -I mcu/include \
    -o mcu/tests/test_dms_protocol_stress \
    mcu/tests/test_dms_protocol_stress.c \
    mcu/src/dms_protocol.c

# 运行
./mcu/tests/test_dms_protocol_stress
```

### 集成到 MCU 工程

1. 将 `mcu/include/` 和 `mcu/src/` 复制到你的 MCU 工程
2. 实现 `dms_alarm_buzzer_hw_set(bool on)` 函数（GPIO 控制）
3. 在 UART 接收中断中调用 `dms_protocol_feed_byte()`
4. 在系统定时器中调用 `dms_event_handler_tick(ms)`

### 最小集成示例

```c
#include "dms_protocol.h"
#include "dms_event_handler.h"
#include "dms_alarm.h"

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
    }
}
```

## 重要说明

### 风险阈值

当前 Risk Manager 中的阈值（60s 窗口、5min 窗口等）是 **engineering_default**，
不是公交司机处罚标准，不是医学疲劳诊断标准，不是最终运营考核标准。
后续需要真实公交场景数据校准。

### 禁止事项

- 不要把复杂视觉算法移到 MCU
- 不要阻塞 MCU（禁止 delay_ms 控制蜂鸣器）
- 不要编造 CAN ID
- 不要实现自动处罚扣分
