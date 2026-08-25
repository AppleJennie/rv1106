# vehicle_mcu — STM32 车辆运动与乘客舒适度节点

纯 C11 自包含固件模块（仅标准库 + libm），未来的 STM32 固件。
**不依赖 RV1106 工程任何头文件**，可直接复制 `include/` + `src/` 进 MCU 工程。

## 模块清单

| 文件 | 职责 |
|---|---|
| `include/vehicle_imu.h` / `src/vehicle_imu.c` | 100Hz IMU：安装矩阵坐标变换、开机静止标定（陀螺零偏+加速度计零偏）、互补滤波 roll/pitch、重力补偿线加速度、jerk 微分+低通。状态机 CALIBRATING/READY/ERROR |
| `include/vehicle_motion.h` / `src/vehicle_motion.c` | 运动事件状态机：HARD_ACCEL/HARD_BRAKE/HARD_TURN_LEFT/HARD_TURN_RIGHT/BUMP/HIGH_LONG_JERK/HIGH_LAT_JERK；每事件四要素（进入/退出/最短持续/冷却）；CAN 置信度融合 |
| `include/vehicle_can.h` / `src/vehicle_can.c` | CAN 信号抽象：speed/brake/accelerator/door/soc + valid_flags + 独立过期管理；HAL stub，不编造任何 CAN ID |
| `include/passenger_comfort.h` / `src/passenger_comfort.c` | 乘客舒适度引擎：三向平顺度、事件计数、trip_comfort_index(0~100) |
| `tests/` | 4 个模块单测 + `run_tests.sh`（gcc -Wall -Wextra -Werror -std=c11，零警告，118 用例全绿） |
| `docs/VEHICLE_MCU.md` | 设计说明：坐标系、状态机图、配置表、融合规则 |

## 构建与测试

```bash
vehicle_mcu/tests/run_tests.sh
# 末尾打印 总 PASS / 总 FAIL，全绿退出码 0
```

## 集成方式（100Hz 定时任务）

```c
#include "vehicle_imu.h"
#include "vehicle_can.h"
#include "vehicle_motion.h"
#include "passenger_comfort.h"

/* 初始化（NULL = 默认配置，阈值均为工程初始值，需实车标定） */
vehicle_imu_init(NULL);
vehicle_can_init(NULL);
vehicle_motion_init(NULL);
passenger_comfort_init(NULL);

/* 100Hz 周期任务 */
void task_100hz(uint32_t now_ms)
{
    /* 1. BSP 读取 IMU 寄存器得到 ax..gz（传感器坐标系） */
    vehicle_imu_update(ax, ay, az, gx, gy, gz, now_ms);

    vehicle_imu_output_t imu;
    if (!vehicle_imu_get_output(&imu)) return;   /* 标定未完成/出错 */

    /* 2. CAN：BSP 解码真实报文后调用 vehicle_can_update_*() 注入；
     *    CAN 缺失时 get_state 返回 false，传 NULL 即可，功能不受阻 */
    vehicle_can_state_t can;
    bool has_can = vehicle_can_get_state(&can, now_ms);

    /* 3. 运动事件检测（含置信度融合） */
    vehicle_motion_input_t mi = {
        imu.longitudinal, imu.lateral, imu.vertical_accel,
        imu.longitudinal_jerk, imu.lateral_jerk, imu.vertical_jerk,
        imu.timestamp_ms
    };
    vehicle_motion_output_t mo;
    vehicle_motion_update(&mi, has_can ? &can : NULL, &mo);

    /* 4. 舒适度累计 */
    passenger_comfort_update(&imu);
    if (mo.entered_flags) {
        passenger_comfort_on_motion_events(mo.entered_flags);
    }
}
```

## 对外接口摘要（其他模块对接所需）

- **运动事件**：`vehicle_motion_update()` → `vehicle_motion_output_t`
  - `state` 主状态（优先级最高激活事件）；`active_flags` 全部激活事件 bitmask（bit i = `1u<<事件枚举值`）
  - `entered_flags` 本拍新确认事件边沿 bitmask（只报一次，供计数/上报）
  - `confidence` 0~1 检测置信度（NORMAL 时恒 0）；`can_corroborated` 是否有 CAN 佐证
  - 事件名：`vehicle_motion_event_name()`
- **IMU 数据**：`vehicle_imu_get_output()` → 线加速度 3 轴 (m/s²)、jerk 3 轴 (m/s³)、roll/pitch (rad)、时间戳；`vehicle_imu_get_state()` 查状态机
- **舒适度**：`passenger_comfort_get_metrics()` → 三向平顺度 (0~100)、5 类事件计数、`trip_comfort_index` (0~100)、行程时长/样本数；`passenger_comfort_reset_trip()` 开始新行程
- **CAN**：`vehicle_can_get_state(&st, now)` → false 表示 CAN 完全缺失（不阻塞）

## 与其他模块的接口假设（待确认，不阻塞）

1. **上报 RV1106/主机侧**：假设未来经 UART 上报运动事件边沿（事件 ID + 时间戳 +
   confidence）与行程舒适度摘要；具体帧格式未定，建议复用既有
   `mcu/include/dms_protocol.h` 风格自定义帧，本目录暂不提供协议实现。
2. **时间戳**：假设上层提供单调递增的 uint32 ms 时钟（回绕安全已用无符号
   差值/有符号比较处理）。
3. **调度**：假设 100Hz 周期任务由定时器中断或 RTOS 任务保证；
   时间戳异常（非 10ms 间隔）时 IMU 内部回退到标称 dt=10ms。
4. **单实例**：与 `src/dms/dms_risk_manager.c` 同风格，全部模块使用静态
   状态结构体单实例，不支持多实例。

## REQUIRES HARDWARE（需真实硬件验证项）

- [ ] 安装方向矩阵实车测量（传感器实际安装姿态）
- [ ] 开机静止标定在发动机怠速/车门振动环境下的鲁棒性（可能需放宽 still 阈值或改用车辆静止位）
- [ ] 互补滤波 α、可信窗宽度与实车振动谱匹配
- [ ] 全部运动事件阈值（急刹/急加速/急转弯/颠簸/jerk）按车型、悬架、载重实车标定
- [ ] CAN 报文 ID 与信号布局：必须取自整车厂 DBC/通讯矩阵（当前为 stub，未编造任何 ID）
- [ ] 置信度阈值（减速佐证速率 2km/h/s 等）实车验证
- [ ] trip_comfort_index 与乘客主观反馈的相关性标定

## 红线声明

- 本模块是安全提示系统的一部分：事件只描述车辆运动，不自动归责驾驶员；
  无信息时责任 = UNKNOWN，任何驾驶员相关推断必须 SUSPECTED + 人工复核。
- Comfort Index 为内部工程指标，不构成驾驶员评价/考核依据。
- 代码与文档中不出现 penalty/fine/punishment/deduct/罚款/扣分/自动处罚 概念。
