# bus_event_fusion —— 公交事件融合引擎

纯 C11 自包含模块（只依赖标准库），融合 **DMS 驾驶员监测事件** 与 **车辆运动事件**，
在最近 30 秒事件时间线上做时间关联，输出"公交安全事件"（Bus Safety Event）。

> **产品红线**：本模块是安全提示系统，不是处罚系统。所有输出均为 AI 安全风险提示，
> **不代表最终责任认定**；`human_review_required` 恒为 `true`；无信息时归因一律
> `UNKNOWN`；涉及驾驶员的推断一律使用 **SUSPECTED**（疑似）措辞。
> 本模块不存在 penalty / fine / punishment / deduct / 罚款 / 扣分 / 自动处罚 语义。

## 目录结构

```
bus_event_fusion/
├── include/bus_event_fusion.h      # 对外头文件（类型 / 配置 / API）
├── src/bus_event_fusion.c          # 实现（静态状态 + 环形缓冲时间线）
├── tests/test_bus_event_fusion.c   # 单元测试（12 个用例）
├── tests/run_tests.sh              # 编译运行脚本（gcc -Wall -Wextra -std=c11）
└── README.md
```

## 构建与测试

```bash
cd bus_event_fusion/tests
./run_tests.sh
```

要求零警告；测试末尾打印 `TOTAL: PASS=n FAIL=0`，全绿返回 0。

## 输入

| 输入 | 结构体 | 字段 |
|---|---|---|
| DMS 事件 | `fusion_dms_event_t` | `type`(EYE_CLOSED/LONG_EYE_CLOSED/YAWN/HEAD_DOWN/FACE_LOST)、`risk_level`(NORMAL/ATTENTION/WARNING/HIGH)、`timestamp_ms`、`duration_ms` |
| 运动事件 | `fusion_motion_event_t` | `type`(HARD_ACCEL/HARD_BRAKE/HARD_TURN_LEFT/HARD_TURN_RIGHT/BUMP/HIGH_LONG_JERK/HIGH_LAT_JERK)、`timestamp_ms`、`confidence`(0~100)、`longitudinal_accel`、`lateral_accel` |
| 车速（可选） | `bus_event_fusion_update_speed(speed_mps, ts)` | 无 CAN 时可以不调用，系统照常工作 |

## 输出（Bus Safety Event）

`bus_safety_event_t`：`event_type`、`attribution`、`correlation_confidence`(0~100)、
`evidence`（关联证据字符串）、`message`（人类可读提示）、`human_review_required`（恒 true）、`timestamp_ms`。

| CASE | 触发条件 | 输出 event_type | attribution |
|---|---|---|---|
| 1 | HARD_BRAKE 且前 2 秒内无 HEAD_DOWN | `EMERGENCY_BRAKE` | `UNKNOWN` |
| 2 | HEAD_DOWN 之后 0~2 秒内 HARD_BRAKE（方向敏感） | `ATTENTION_RELATED_BRAKE_SUSPECTED` | `DRIVER_ATTENTION`（message 必含 SUSPECTED） |
| 3 | LONG_EYE_CLOSED 且车辆仍在运动（车速>0 或时间线内有运动事件） | `FATIGUE_HIGH_RISK` | `DRIVER_ATTENTION`（message 必含 SUSPECTED） |

- CASE 2 关联置信度随"低头→急刹"间隔线性衰减（默认 90 → 40，端点含 0 与 2000ms）。
- 单次 HARD_BRAKE 永不产生驾驶员归责（CASE 1 恒 UNKNOWN）。
- `PEDESTRIAN_AVOIDANCE / TRAFFIC / ROAD_CONDITION / VEHICLE` 为保留归因，
  当前没有对应数据源，不会产出；无信息时一律 `UNKNOWN`。

## 对外接口摘要

```c
bool bus_event_fusion_init(void);                                   /* 默认配置初始化 */
bool bus_event_fusion_init_with_config(const fusion_config_t *);    /* 自定义配置 */
void bus_event_fusion_get_default_config(fusion_config_t *);        /* 取默认阈值 */
bool bus_event_fusion_feed_dms(const fusion_dms_event_t *, bus_safety_event_t *out);
bool bus_event_fusion_feed_motion(const fusion_motion_event_t *, bus_safety_event_t *out);
void bus_event_fusion_update_speed(float speed_mps, uint64_t ts);   /* 可选车速 */
void bus_event_fusion_reset(void);                                  /* 清空时间线与车速 */
const char* bus_safety_event_type_to_string(...);                   /* 调试 */
const char* bus_attribution_to_string(...);                         /* 调试 */
```

- feed 返回 `true` 表示产生了一条融合输出（写入 `out`）；返回 `false` 表示仅记录时间线。
- `out` 可传 `NULL`（只关心是否触发）。
- 阈值集中在 `fusion_config_t`：`timeline_window_ms`(30000)、
  `head_down_brake_correlate_max_ms`(2000)、`brake_correlation_conf_near`(90)、
  `brake_correlation_conf_far`(40)、`fatigue_high_risk_confidence`(80)。
  **均为仅工程初始值，需实车标定。**

## 与其他模块的接口假设（对接方需知）

1. **时间戳同源假设**：DMS 事件与运动事件的 `timestamp_ms` 必须使用同一时间基准
   （建议单调毫秒时钟）。关联只在数值上比较，不做时钟校准。
2. **DMS 侧事件映射**：RV1106 疲劳状态机输出 → 本模块类型的映射为
   `EYE_CLOSED→EYE_CLOSED`、`LONG_EYE_CLOSED→LONG_EYE_CLOSED`、`YAWN→YAWN`、
   `HEAD_DOWN→HEAD_DOWN`、`NO_FACE→FACE_LOST`；风险等级直接透传。
   DMS 事件应在**状态沿**（开始）时喂入一次，而不是每帧重复喂入，否则时间线会被刷掉
   （模块对重复事件不报错，但 evidence 会以最新记录为准）。
3. **运动事件来源假设**：运动事件由 IMU/车辆运动检测模块产生（本模块不检测），
   事件粒度为"检测沿"，急刹事件触发时喂入一次即可。
4. **车速语义假设**：`update_speed` 传入的是"最新已知车速"，模块不做时效过期判断；
   实车集成时建议 ≥1Hz 更新。无 CAN 时完全不调用即可（CASE 3 自动退化为
   依据时间线内是否存在运动事件判断车辆是否在动）。
5. **消费方式假设**：输出事件建议交给事件日志/上报模块（如 dms_event_logger 类似物）
   持久化，并进入人工复核队列；本模块不做存储、不做网络发送。
6. **线程模型**：与 dms_risk_manager 一致，模块内部为静态状态、非线程安全，
   假设由单线程（或持锁）调用。

## REQUIRES HARDWARE（需真实硬件验证项）

- 全部阈值（2 秒关联窗、置信度衰减曲线、30 秒时间线）需在实车数据上标定。
- CASE 3 的"车辆仍在运动"判定：实车上应接入真实车速（CAN），当前 stub 语义未做车速过期处理。
- 运动事件（HARD_BRAKE 等）的检测灵敏度/误报率依赖 IMU 或 CAN 实车数据，
  会直接影响 CASE 1/2 的触发频率。
- 端到端时延：摄像头帧 → DMS 事件 → 本模块输出的时间戳精度，需要实板验证关联窗是否够用。

## 设计要点

- **30 秒时间线**：环形缓冲（容量 128 条，仅限制内存），关联时按时间戳 cutoff
  （`now - 30000`）过滤，窗口外事件不参与任何关联。
- **时序方向敏感**：CASE 2 只向前回溯（head_down 必须先于 brake），反向永不关联。
- **保守原则**：无法确认车辆在动时 CASE 3 不触发；无法确认原因时归因 UNKNOWN。
- **零依赖**：只用 `stdint/stdbool/string/stdio`，可脱离 RV1106 工程独立编译。
