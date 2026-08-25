# VEHICLE_MCU 设计说明

STM32 车辆运动与乘客舒适度节点。纯 C11，自包含（仅标准库 + libm），
不依赖 RV1106 工程任何头文件。所有算法在主机侧以单测验证后再上板。

## 1. 坐标系

车辆坐标系：**+X = 车前方，+Y = 车左方，+Z = 车上方**。

传感器安装存在旋转时，用 3x3 安装方向矩阵 `mount_matrix` 把传感器坐标系
向量变换到车辆坐标系：`v_vehicle = M × v_sensor`（加速度与角速度同矩阵）。
默认单位阵。静止水平参考不假设安装绝对水平，由开机标定期间的重力向量实测获得。

姿态只估计 roll（绕 +X）/ pitch（绕 +Y）；yaw 需要磁力计，本版本不估计。

## 2. 数据流

```
IMU 原始数据(ax..gz) ──► vehicle_imu ──► 线加速度3轴 + jerk3轴 + roll/pitch
                              │                    │
CAN 解码注入 ──► vehicle_can ─┤(信号快照,可缺失)    │
                              ▼                    ▼
                       vehicle_motion ──► 运动事件 + 置信度
                              │  (entered_flags 边沿)
                              ▼
                     passenger_comfort ──► 三向平顺度 + 事件计数
                                          + trip_comfort_index(0~100)
```

调用节奏 100Hz。依赖方向单向：imu → motion → comfort，can 被 motion 读取。

## 3. IMU 状态机

```
                ┌────────────────────────────────────┐
                ▼                                    │
        ┌───────────────┐   连续静止样本满 N(200)     │
        │  CALIBRATING  │ ──────────────┐            │
        └───────────────┘               ▼            │
                │                ┌───────────┐       │
                │ 非静止样本累计  │   READY   │       │
                │ 超过 1000      └───────────┘       │
                ▼                     │              │
        ┌───────────────┐             │ request_     │
        │     ERROR     │ ◄───────────┘ recalibration│
        └───────────────┘  ──────────────────────────┘
```

- 静止判定：`| |a| - g | ≤ 0.5` 且各轴 `|gyro| ≤ 0.2 rad/s`；出现非静止样本
  则累加器清零重新计数。
- 标定产出：陀螺零偏（静止均值）、加速度计零偏（静止均值 − 实测姿态下的
  理论重力分量）、初始 roll/pitch（由重力向量 atan2 实测，不假设水平）。

## 4. 姿态与重力补偿

互补滤波（小角度近似，第一版实现）：

```
roll  += gx·dt ;  pitch += gy·dt                     (陀螺积分)
若 lin_est = 测量比力 − g_v(当前姿态)，|lin_est| ≤ accel_trust_band(1.0)：
    roll  = α·roll  + (1-α)·atan2(ay, az)
    pitch = α·pitch + (1-α)·atan2(-ax, √(ay²+az²))   (α=0.98)
重力分量 g_v = (-g·sin p, g·sin r·cos p, g·cos r·cos p)
线加速度 = 测量比力 − 零偏 − g_v        ← 急刹判定必须用这个
jerk = LPF( d(线加速度)/dt )，一阶低通 α=0.2
```

可信窗的作用：机动时关闭姿态修正，防止持续加速度被误当成倾斜。
**注意**：不能用 |a|≈g 判定机动——急刹 −2.5m/s² 时 |a| 仅变化约 0.3，
会漏判；因此用"当前姿态预测的线加速度估计"判机动。
**机动逃逸**（maneuver_max_ms=5000）：等效原理决定无陀螺旋转时恒定
"加速度"与倾斜不可区分，故超窗持续 5 秒以上视为姿态/安装变化，
重新允许修正收敛（测试 T2 覆盖：10s 倾斜收敛到 30°；T3 覆盖：可信窗 0.02 时 1m/s² 加速不拖偏）。

## 5. 运动事件状态机

每个事件独立跟踪四要素：**进入阈值 / 退出阈值（滞回）/ 最短持续 / 冷却防抖**。
冷却期内暂停条件计时；冷却结束后需重新连续满足最短持续才再触发。
事件边沿只上报一次（`entered_flags`）。

| 事件 | 进入条件 | 退出条件 | 最短持续 | 冷却 |
|---|---|---|---|---|
| HARD_ACCEL | longitudinal > +1.5 m/s² | < +1.0 | 300ms | 1000ms |
| HARD_BRAKE | longitudinal < −2.0 m/s² | > −1.2 | 300ms | 1000ms |
| HARD_TURN_LEFT | lateral > +1.8 m/s² | < +1.2 | 300ms | 1000ms |
| HARD_TURN_RIGHT | lateral < −1.8 m/s² | > −1.2 | 300ms | 1000ms |
| BUMP | \|vertical\| > 2.5 m/s² | < 1.5 | 20ms | 500ms |
| HIGH_LONG_JERK | \|long_jerk\| > 2.5 m/s³ | < 1.5 | 50ms | 500ms |
| HIGH_LAT_JERK | \|lat_jerk\| > 2.5 m/s³ | < 1.5 | 50ms | 500ms |

主状态优先级（高→低）：HARD_BRAKE > HARD_ACCEL > HARD_TURN_LEFT >
HARD_TURN_RIGHT > BUMP > HIGH_LONG_JERK > HIGH_LAT_JERK。
（减速类对站立乘客安全影响最大故急刹最高；该顺序为工程设计选择。）
全部激活事件以 `active_flags` bitmask 并行给出，不受优先级影响。

以上阈值**仅为工程初始值，需实车标定；不是公交处罚标准**。

## 6. 置信度融合（仅描述检测可信度，与责任无关）

| 情形 | confidence | can_corroborated |
|---|---|---|
| 急刹 + CAN(刹车踏板 ON 且车速下降 ≥ 2 km/h/s) | 0.9 | true |
| 急刹 + CAN 信号矛盾（未踩踏板或车速不降） | 0.4 | false |
| 急刹 + 无 CAN（纯 IMU 独立判定） | 0.6 | false |
| 急加速 + 油门开度 ≥ 5% | 0.9 | true |
| 其余事件（转弯/颠簸/jerk） | 0.7 | false |

**CAN 缺失不阻塞任何功能**：纯 IMU 照常判定，仅置信度较低。

## 7. CAN 抽象

`vehicle_can_state_t { speed_kph, brake_pedal, accelerator_pedal(%),
door_state, soc(%), valid_flags }`。每路信号独立时间戳与过期阈值
（车速/踏板 500ms、车门 2s、SOC 5s）；过期信号 flag 清零、字段清零
（door 置 UNKNOWN）。本模块不解析原始报文，**禁止编造 CAN ID**；
真实 ID 必须来自整车厂 DBC，由 BSP 解码后 `vehicle_can_update_*()` 注入。

## 8. 乘客舒适度

```
metric_axis = rms(accel_axis) + jerk_weight(0.5) × rms(jerk_axis)   (二阶矩 EMA, α=0.05)
score_axis  = 100 × clamp(1 − metric/metric_full)                   (纵2.0/横2.0/垂3.0)
base        = 0.4×纵向 + 0.3×横向 + 0.3×垂向
impact      = min( Σ次数×单次影响(急刹1.5/急加1.0/转弯1.2/颠簸0.8/jerk0.5), 30 )
trip_comfort_index = clamp(base − impact, 0, 100)
```

Comfort Index 是**内部工程指标**，用于算法调参与横向对比；
需实车数据与乘客主观反馈标定；不构成对驾驶员的评价或考核。

## 9. 已知限制

- yaw 不估计；急转弯由横向加速度间接判断。
- roll/pitch 积分采用小角度近似，大角度耦合未处理。
- 加速度计零偏只在开机静止标定时估计一次，运行中不在线更新。
- 全部默认阈值/系数为工程初始值，需实车标定（见 REQUIRES HARDWARE）。
