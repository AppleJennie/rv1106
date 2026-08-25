#ifndef PASSENGER_COMFORT_H
#define PASSENGER_COMFORT_H

#include <stdint.h>
#include <stdbool.h>

#include "vehicle_imu.h"
#include "vehicle_motion.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * passenger_comfort.h - 乘客舒适度引擎
 *
 * 输入：三轴线加速度 + 三轴 jerk（vehicle_imu 输出）
 *       + 运动事件边沿（vehicle_motion 的 entered_flags）。
 * 输出：三向平顺度、各类事件计数、行程舒适度指数 (0~100)。
 *
 * 重要（产品红线）：
 *   - Comfort Index 仅为内部工程指标，用于算法调参与横向对比；
 *     后续需实车数据与乘客主观反馈标定。
 *   - 不构成对驾驶员的评价或考核，不与任何处罚/绩效机制挂钩。
 *   - 单次急刹/颠簸不自动归责驾驶员。
 */

/*
 * 舒适度配置（阈值集中管理）。
 * 注意：以下全部系数仅为工程初始值，需实车与乘客反馈标定。
 */
typedef struct {
    float ema_alpha;             /* 二阶矩 EMA 系数 (0~1)，默认 0.05
                                    （100Hz 下时间常数约 0.2s） */

    /* 各方向"不可接受"参考值：等效指标达到该值时该向得分降为 0 */
    float long_metric_full;      /* 纵向，默认 2.0 (m/s² 等效) */
    float lat_metric_full;       /* 横向，默认 2.0 */
    float vert_metric_full;      /* 垂向，默认 3.0 */

    float jerk_weight;           /* jerk 在等效指标中的权重，默认 0.5
                                    （将 m/s³ 折算为 m/s² 等效） */

    /* 行程指数合成权重（三者之和应为 1.0） */
    float w_long;                /* 默认 0.4 */
    float w_lat;                 /* 默认 0.3 */
    float w_vert;                /* 默认 0.3 */

    /* 单次事件对行程指数的向下调整量（内部工程指标，非考核非处罚） */
    float impact_hard_brake;     /* 默认 1.5 */
    float impact_hard_accel;     /* 默认 1.0 */
    float impact_hard_turn;      /* 默认 1.2 */
    float impact_bump;           /* 默认 0.8 */
    float impact_high_jerk;      /* 默认 0.5 */
    float impact_max_total;      /* 事件影响总量上限，默认 30.0 */
} passenger_comfort_config_t;

/* 舒适度指标输出 */
typedef struct {
    float    longitudinal_smoothness;  /* 纵向平顺度 0~100 */
    float    lateral_smoothness;       /* 横向平顺度 0~100 */
    float    vertical_comfort;         /* 垂向舒适度 0~100 */

    uint32_t hard_brake_count;         /* 本行程急刹次数 */
    uint32_t hard_accel_count;         /* 本行程急加速次数 */
    uint32_t hard_turn_count;          /* 本行程急转弯次数（左右合计） */
    uint32_t bump_count;               /* 本行程颠簸次数 */
    uint32_t high_jerk_count;          /* 本行程 jerk 过大次数（纵/横合计） */

    float    trip_comfort_index;       /* 行程舒适度指数 0~100 */
    uint32_t trip_duration_ms;         /* 本行程累计时长 */
    uint32_t sample_count;             /* 本行程累计样本数 */
} passenger_comfort_metrics_t;

/* 获取默认配置（工程初始值，需实车标定） */
void passenger_comfort_get_default_config(passenger_comfort_config_t *config);

/* 初始化。config 传 NULL 使用默认配置。返回 0 成功，-1 参数非法 */
int passenger_comfort_init(const passenger_comfort_config_t *config);

/* 100Hz 喂入一帧 IMU 输出，累计三向加速度/jerk 统计量 */
void passenger_comfort_update(const vehicle_imu_output_t *imu_out);

/* 喂入运动事件边沿（vehicle_motion_output_t.entered_flags），累计事件计数 */
void passenger_comfort_on_motion_events(uint32_t entered_flags);

/* 读取当前舒适度指标（实时计算，无副作用） */
void passenger_comfort_get_metrics(passenger_comfort_metrics_t *out);

/* 清零重新开始一个行程（保留配置） */
void passenger_comfort_reset_trip(void);

#ifdef __cplusplus
}
#endif

#endif /* PASSENGER_COMFORT_H */
