#ifndef VEHICLE_MOTION_H
#define VEHICLE_MOTION_H

#include <stdint.h>
#include <stdbool.h>

#include "vehicle_can.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * vehicle_motion.h - 车辆运动事件状态机
 *
 * 输入：重力补偿后的线加速度 + jerk（来自 vehicle_imu 输出），
 *       以及可选的 CAN 信号快照（用于置信度融合，可为 NULL）。
 * 输出：当前主运动事件（单状态）+ 全部激活事件 bitmask
 *       + 本拍新确认事件边沿 + 检测置信度。
 *
 * 每个事件独立跟踪，具备：进入阈值 / 退出阈值（滞回）/
 * 最短持续时间 / 冷却防抖 四要素。
 *
 * 重要（产品红线）：
 *   - 事件仅描述"车辆运动"本身，不自动归责驾驶员；无额外信息时
 *     责任判定必须为 UNKNOWN，任何与驾驶员相关的推断必须使用
 *     SUSPECTED 措辞并保留人工复核（human review）。
 *   - confidence 仅表示"该运动事件被正确检出的可信度"，与责任无关。
 *   - CAN 缺失不得阻塞任何功能：纯 IMU 独立判定，仅置信度较低。
 */

/* 车辆运动事件（同时用作状态与事件类型） */
typedef enum {
    VEHICLE_NORMAL = 0,      /* 正常行驶（无事件） */
    VEHICLE_HARD_ACCEL,      /* 急加速 */
    VEHICLE_HARD_BRAKE,      /* 急刹车 */
    VEHICLE_HARD_TURN_LEFT,  /* 急左转 */
    VEHICLE_HARD_TURN_RIGHT, /* 急右转 */
    VEHICLE_BUMP,            /* 颠簸 */
    VEHICLE_HIGH_LONG_JERK,  /* 纵向 jerk 过大（顿挫） */
    VEHICLE_HIGH_LAT_JERK    /* 横向 jerk 过大（晃动） */
} vehicle_motion_event_t;

/* active_flags / entered_flags 位定义：bit i = (1u << 事件枚举值)，bit0 保留不用 */

/*
 * 运动检测配置（阈值集中管理）。
 * 注意：以下全部阈值仅为工程初始值，需实车标定；
 *       不是公交处罚标准，不构成任何考核依据。
 */
typedef struct {
    /* 急加速：longitudinal > enter 持续 min_ms 触发，回落到 exit 以下解除 */
    float    hard_accel_enter_ms2;     /* 默认 +1.5 */
    float    hard_accel_exit_ms2;      /* 默认 +1.0 */
    uint32_t hard_accel_min_ms;        /* 默认 300 */
    uint32_t hard_accel_cooldown_ms;   /* 默认 1000 */

    /* 急刹车：longitudinal < enter 持续 min_ms 触发，回升到 exit 以上解除 */
    float    hard_brake_enter_ms2;     /* 默认 -2.0 */
    float    hard_brake_exit_ms2;      /* 默认 -1.2 */
    uint32_t hard_brake_min_ms;        /* 默认 300 */
    uint32_t hard_brake_cooldown_ms;   /* 默认 1000 */

    /* 急转弯：|lateral| > enter 持续 min_ms 触发（+Y=左转为正） */
    float    hard_turn_enter_ms2;      /* 默认 1.8 */
    float    hard_turn_exit_ms2;       /* 默认 1.2 */
    uint32_t hard_turn_min_ms;         /* 默认 300 */
    uint32_t hard_turn_cooldown_ms;    /* 默认 1000 */

    /* 颠簸：|vertical_accel| > enter 持续 min_ms 触发 */
    float    bump_enter_ms2;           /* 默认 2.5 */
    float    bump_exit_ms2;            /* 默认 1.5 */
    uint32_t bump_min_ms;              /* 默认 20 */
    uint32_t bump_cooldown_ms;         /* 默认 500 */

    /* jerk 过大：|jerk| > enter 持续 min_ms 触发（纵向/横向共用阈值） */
    float    high_jerk_enter_ms3;      /* 默认 2.5 */
    float    high_jerk_exit_ms3;       /* 默认 1.5 */
    uint32_t high_jerk_min_ms;         /* 默认 50 */
    uint32_t high_jerk_cooldown_ms;    /* 默认 500 */

    /* --- 置信度融合（0~1，仅描述检测可信度，与责任无关） --- */
    float conf_imu_only;            /* 无 CAN 时纯 IMU 独立判定，默认 0.6 */
    float conf_can_agree;           /* CAN 信号与 IMU 判定一致，默认 0.9 */
    float conf_can_conflict;        /* CAN 信号与 IMU 判定矛盾，默认 0.4 */
    float conf_other_event;         /* 无 CAN 融合逻辑的事件基础置信度，默认 0.7 */
    float brake_speed_drop_kph_s;   /* 判定"减速中"的车速下降速率阈值 (km/h/s)，默认 2.0 */
    float accel_pedal_confirm_pct;  /* 急加速佐证的油门开度下限 (%)，默认 5.0 */
} vehicle_motion_config_t;

/* 运动检测输入（从 vehicle_imu_output_t 拷贝对应字段） */
typedef struct {
    float    longitudinal;
    float    lateral;
    float    vertical_accel;
    float    longitudinal_jerk;
    float    lateral_jerk;
    float    vertical_jerk;
    uint32_t timestamp_ms;
} vehicle_motion_input_t;

/* 运动检测输出 */
typedef struct {
    vehicle_motion_event_t state;          /* 当前主状态（优先级最高的激活事件） */
    uint32_t               active_flags;   /* 全部激活事件 bitmask */
    vehicle_motion_event_t entered_event;  /* 本拍新确认事件代表；无 = VEHICLE_NORMAL */
    uint32_t               entered_flags;  /* 本拍全部新确认事件 bitmask */
    float                  confidence;     /* 主状态检测置信度 0~1；NORMAL 时恒为 0 */
    bool                   can_corroborated; /* 本拍主状态是否有 CAN 信号佐证 */
} vehicle_motion_output_t;

/* 获取默认配置（工程初始值，需实车标定） */
void vehicle_motion_get_default_config(vehicle_motion_config_t *config);

/* 初始化。config 传 NULL 使用默认配置。返回 0 成功，-1 参数非法 */
int vehicle_motion_init(const vehicle_motion_config_t *config);

/*
 * 100Hz 更新一拍。
 * can 为 NULL 表示无 CAN（纯 IMU 独立判定，功能不受阻）。
 */
void vehicle_motion_update(const vehicle_motion_input_t *in,
                           const vehicle_can_state_t *can,
                           vehicle_motion_output_t *out);

/* 复位全部事件跟踪状态（保留配置） */
void vehicle_motion_reset(void);

/* 事件名称（日志/上报用） */
const char *vehicle_motion_event_name(vehicle_motion_event_t event);

#ifdef __cplusplus
}
#endif

#endif /* VEHICLE_MOTION_H */
