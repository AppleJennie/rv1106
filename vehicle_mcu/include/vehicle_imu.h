#ifndef VEHICLE_IMU_H
#define VEHICLE_IMU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * vehicle_imu.h - 车辆 IMU 处理模块（100Hz）
 *
 * 功能：
 *   1. 安装方向矩阵：传感器坐标系 → 车辆坐标系
 *      （车辆坐标系约定：+X = 车前方，+Y = 车左方，+Z = 车上方）
 *   2. 开机静止标定：陀螺零偏 + 加速度计零偏（第一版基础实现，
 *      不假设传感器安装绝对水平，水平参考由重力向量实测获得）
 *   3. 互补滤波估计 roll / pitch（yaw 需要磁力计，本版本不估计）
 *   4. 重力补偿输出线加速度；线加速度微分 + 低通得到 jerk
 *
 * 安全约束：
 *   - 严禁直接使用原始加速度判定急刹/急加速，必须使用本模块
 *     重力补偿后的线加速度（longitudinal/lateral/vertical_accel）。
 *   - 本模块只描述车辆运动状态，不对驾驶员做任何归责。
 *
 * 依赖：仅 C 标准库 + libm，自包含，可独立移植到 STM32。
 */

/* 重力加速度常数 (m/s²) */
#define VEHICLE_IMU_GRAVITY_MS2   9.81f

/* IMU 模块运行状态 */
typedef enum {
    VEHICLE_IMU_CALIBRATING = 0,  /* 开机静止标定中，输出无效 */
    VEHICLE_IMU_READY,            /* 标定完成，输出有效 */
    VEHICLE_IMU_ERROR             /* 标定失败（长期非静止或传感器输出异常） */
} vehicle_imu_state_t;

/*
 * IMU 配置（阈值集中管理）。
 * 注意：以下默认值仅为工程初始值，需实车标定。
 */
typedef struct {
    /* 安装方向矩阵 M（3x3 旋转矩阵）：v_vehicle = M × v_sensor。
     * 默认单位阵（传感器坐标系与车辆坐标系重合）。
     * 实际安装存在旋转时必须按实测填写，需实车标定。 */
    float mount_matrix[3][3];

    /* --- 开机静止标定 --- */
    uint16_t calib_min_samples;      /* 连续静止样本数要求，默认 200（100Hz 约 2s） */
    uint16_t calib_max_violations;   /* 标定期允许的非静止样本总数，超出判 IMU_ERROR，默认 1000 */
    float    gyro_still_rads;        /* 静止判定：各轴 |gyro| 上限 (rad/s)，默认 0.2 */
    float    accel_still_band_ms2;   /* 静止判定：| |a| - g | 上限 (m/s²)，默认 0.5 */

    /* --- 互补滤波 --- */
    float comp_alpha;                /* 陀螺积分权重 (0~1)，默认 0.98 */
    float accel_trust_band_ms2;      /* 机动可信窗 (m/s²)：用当前姿态预测重力分量，
                                        实测与预测之差（线加速度估计）超过该值时
                                        判定为机动，冻结加速度姿态修正，防止
                                        急刹/急加速把姿态拖偏。默认 1.0 */

    uint32_t maneuver_max_ms;        /* 机动逃逸时间 (ms)：线加速度估计持续超过
                                        可信窗超过该时长时，认为是姿态/安装变化
                                        而非真实机动（真实路面机动不会长时间
                                        恒定），重新允许加速度修正收敛姿态。
                                        默认 5000。仅工程初始值，需实车标定 */

    /* --- jerk 低通 --- */
    float jerk_lpf_alpha;            /* 一阶低通系数 (0~1)，越小越平滑，默认 0.2 */

    float sample_dt_s;               /* 标称采样周期 (s)，默认 0.01；时间戳异常时回退使用 */
} vehicle_imu_config_t;

/* IMU 输出（仅 READY 状态有效） */
typedef struct {
    /* 姿态（弧度）：roll 绕 +X 轴，pitch 绕 +Y 轴；yaw 本版本不估计 */
    float roll_rad;
    float pitch_rad;

    /* 重力补偿后的线加速度（车辆坐标系，m/s²） */
    float longitudinal;    /* +X 车前方：正值=加速，负值=减速 */
    float lateral;         /* +Y 车左方：正值=左转（向心加速度向左） */
    float vertical_accel;  /* +Z 车上方：正值=向上颠簸 */

    /* 线加速度微分 + 低通后的 jerk（m/s³） */
    float longitudinal_jerk;
    float lateral_jerk;
    float vertical_jerk;

    uint32_t timestamp_ms;
} vehicle_imu_output_t;

/* 获取默认配置（工程初始值，需实车标定） */
void vehicle_imu_get_default_config(vehicle_imu_config_t *config);

/* 初始化并进入标定状态。config 传 NULL 使用默认配置。返回 0 成功，-1 参数非法 */
int vehicle_imu_init(const vehicle_imu_config_t *config);

/* 100Hz 喂入一帧传感器原始数据（传感器坐标系），返回当前模块状态 */
vehicle_imu_state_t vehicle_imu_update(float ax, float ay, float az,
                                       float gx, float gy, float gz,
                                       uint32_t timestamp_ms);

/* 查询模块状态 */
vehicle_imu_state_t vehicle_imu_get_state(void);

/* 获取最新输出。READY 时返回 true，其余状态返回 false（输出无效） */
bool vehicle_imu_get_output(vehicle_imu_output_t *out);

/* 请求重新标定（例如检测到零偏漂移）。调用时车辆必须处于静止状态 */
void vehicle_imu_request_recalibration(void);

#ifdef __cplusplus
}
#endif

#endif /* VEHICLE_IMU_H */
