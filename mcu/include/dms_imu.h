#ifndef DMS_IMU_H
#define DMS_IMU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS IMU Interface - IMU 传感器预留接口。
 *
 * 当前只定义数据结构和接口。
 * 不实现完整的乘客舒适度算法。
 * 保证模块接口以后能接进来。
 */

/* ==================== IMU 采样数据 ==================== */
typedef struct {
    float    ax;            /* 加速度 X (m/s²) */
    float    ay;            /* 加速度 Y (m/s²) */
    float    az;            /* 加速度 Z (m/s²) */
    float    gx;            /* 角速度 X (rad/s) */
    float    gy;            /* 角速度 Y (rad/s) */
    float    gz;            /* 角速度 Z (rad/s) */
    uint32_t timestamp_ms;  /* 采样时间戳 */
} imu_sample_t;

/* ==================== 驾驶行为事件（预留） ==================== */
typedef struct {
    bool hard_brake;        /* 急刹车 */
    bool hard_accel;        /* 急加速 */
    bool hard_turn;         /* 急转弯 */
    bool bump;              /* 颠簸 */
    bool jerk;              /* 急冲/顿挫 */
} imu_driving_event_t;

/* ==================== API（当前全部为 stub） ==================== */

/* 初始化 IMU 接口 */
void dms_imu_init(void);

/* 获取最新 IMU 采样（stub） */
bool dms_imu_get_sample(imu_sample_t *sample);

/* 获取驾驶行为事件（stub，全部返回 false） */
bool dms_imu_get_driving_events(imu_driving_event_t *events);

#ifdef __cplusplus
}
#endif

#endif
