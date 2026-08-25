/*
 * vehicle_imu.c - 车辆 IMU 处理模块实现（100Hz）
 *
 * 处理流水线（每帧）：
 *   原始数据 → 安装矩阵变换到车辆坐标系 → (标定中：静止判定与累加)
 *   → 去零偏 → 互补滤波姿态(roll/pitch) → 重力补偿 → 线加速度
 *   → 微分 + 一阶低通 → jerk
 *
 * 详见 vehicle_imu.h 头部注释。仅依赖 C 标准库 + libm。
 */
#include "vehicle_imu.h"

#include <math.h>
#include <string.h>

/* ==================== 内部状态 ==================== */

typedef struct {
    vehicle_imu_config_t cfg;
    vehicle_imu_state_t  state;

    /* 开机静止标定累加器（已变换到车辆坐标系） */
    float    sum_a[3];
    float    sum_g[3];
    uint16_t still_count;
    uint16_t violation_count;

    /* 标定结果 */
    float gyro_bias[3];     /* 陀螺零偏（车辆坐标系） */
    float accel_offset[3];  /* 加速度计零偏（车辆坐标系） */

    /* 姿态估计（弧度） */
    float roll_rad;
    float pitch_rad;

    /* jerk 计算状态 */
    float prev_lin[3];
    bool  prev_lin_valid;
    float jerk_lpf[3];

    /* 机动可信窗逃逸计时 */
    uint32_t maneuver_since_ms;   /* 线加速度估计持续超窗的起点时间戳 */
    bool     maneuver_active;     /* 当前是否处于机动（冻结修正）状态 */

    vehicle_imu_output_t out;
    uint32_t last_ts;
    bool     last_ts_valid;
    bool     initialized;
} imu_state_t;

static imu_state_t s_imu;

/* ==================== 默认配置 ==================== */

void vehicle_imu_get_default_config(vehicle_imu_config_t *config)
{
    if (!config) return;

    memset(config, 0, sizeof(*config));

    /* 安装方向矩阵：默认单位阵 */
    config->mount_matrix[0][0] = 1.0f;
    config->mount_matrix[1][1] = 1.0f;
    config->mount_matrix[2][2] = 1.0f;

    /* 静止标定 */
    config->calib_min_samples    = 200;
    config->calib_max_violations = 1000;
    config->gyro_still_rads      = 0.2f;
    config->accel_still_band_ms2 = 0.5f;

    /* 互补滤波 */
    config->comp_alpha            = 0.98f;
    config->accel_trust_band_ms2  = 1.0f;
    config->maneuver_max_ms       = 5000;

    /* jerk 低通 */
    config->jerk_lpf_alpha = 0.2f;

    config->sample_dt_s = 0.01f;
}

/* ==================== 内部辅助函数 ==================== */

/* 根据姿态角计算重力在车辆坐标系中的分量（Z 向上为正） */
static void gravity_vector(float roll, float pitch, float g_out[3])
{
    float sr = sinf(roll);
    float cr = cosf(roll);
    float sp = sinf(pitch);
    float cp = cosf(pitch);

    g_out[0] = -VEHICLE_IMU_GRAVITY_MS2 * sp;
    g_out[1] =  VEHICLE_IMU_GRAVITY_MS2 * sr * cp;
    g_out[2] =  VEHICLE_IMU_GRAVITY_MS2 * cr * cp;
}

/*
 * 喂入一帧标定数据（已变换到车辆坐标系）。
 * 返回 true 表示本帧使标定完成（状态已切到 READY）。
 */
static bool calibration_feed(const float a[3], const float g[3])
{
    const vehicle_imu_config_t *cfg = &s_imu.cfg;

    float amag = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    bool still = (fabsf(amag - VEHICLE_IMU_GRAVITY_MS2) <= cfg->accel_still_band_ms2)
              && (fabsf(g[0]) <= cfg->gyro_still_rads)
              && (fabsf(g[1]) <= cfg->gyro_still_rads)
              && (fabsf(g[2]) <= cfg->gyro_still_rads);

    if (!still) {
        /* 非静止样本：清空累加器重新计数，保证标定数据连续静止 */
        s_imu.violation_count++;
        s_imu.still_count = 0;
        memset(s_imu.sum_a, 0, sizeof(s_imu.sum_a));
        memset(s_imu.sum_g, 0, sizeof(s_imu.sum_g));
        if (s_imu.violation_count > cfg->calib_max_violations) {
            s_imu.state = VEHICLE_IMU_ERROR;
        }
        return false;
    }

    for (int i = 0; i < 3; i++) {
        s_imu.sum_a[i] += a[i];
        s_imu.sum_g[i] += g[i];
    }
    s_imu.still_count++;

    if (s_imu.still_count < cfg->calib_min_samples) {
        return false;
    }

    /* ---- 标定完成：计算零偏与初始姿态 ---- */
    float n = (float)s_imu.still_count;
    float mean_a[3], mean_g[3];
    for (int i = 0; i < 3; i++) {
        mean_a[i] = s_imu.sum_a[i] / n;
        mean_g[i] = s_imu.sum_g[i] / n;
    }

    /* 加速度均值模长异常：视为传感器故障 */
    float mean_amag = sqrtf(mean_a[0] * mean_a[0] + mean_a[1] * mean_a[1]
                          + mean_a[2] * mean_a[2]);
    if (mean_amag < 3.0f) {
        s_imu.state = VEHICLE_IMU_ERROR;
        return false;
    }

    /* 陀螺零偏 = 静止期间均值（此时真实角速度≈0） */
    for (int i = 0; i < 3; i++) {
        s_imu.gyro_bias[i] = mean_g[i];
    }

    /* 初始姿态：由重力向量实测获得，不假设安装绝对水平 */
    s_imu.roll_rad  = atan2f(mean_a[1], mean_a[2]);
    s_imu.pitch_rad = atan2f(-mean_a[0],
                             sqrtf(mean_a[1] * mean_a[1] + mean_a[2] * mean_a[2]));

    /* 加速度计零偏 = 静止均值 - 当前姿态下的理论重力分量。
     * 这样标定姿态下线加速度严格为 0，同时吸收小幅安装倾斜与零偏。 */
    float gvec[3];
    gravity_vector(s_imu.roll_rad, s_imu.pitch_rad, gvec);
    for (int i = 0; i < 3; i++) {
        s_imu.accel_offset[i] = mean_a[i] - gvec[i];
    }

    /* 复位 jerk 状态，避免标定边界产生尖峰 */
    s_imu.prev_lin_valid = false;
    memset(s_imu.jerk_lpf, 0, sizeof(s_imu.jerk_lpf));
    memset(&s_imu.out, 0, sizeof(s_imu.out));

    s_imu.state = VEHICLE_IMU_READY;
    return true;
}

/* ==================== 对外接口 ==================== */

int vehicle_imu_init(const vehicle_imu_config_t *config)
{
    vehicle_imu_config_t cfg;
    if (config) {
        cfg = *config;
    } else {
        vehicle_imu_get_default_config(&cfg);
    }

    /* 参数合法性检查 */
    if (cfg.comp_alpha <= 0.0f || cfg.comp_alpha >= 1.0f) return -1;
    if (cfg.jerk_lpf_alpha <= 0.0f || cfg.jerk_lpf_alpha > 1.0f) return -1;
    if (cfg.calib_min_samples == 0) return -1;
    if (cfg.sample_dt_s <= 0.0f) return -1;

    memset(&s_imu, 0, sizeof(s_imu));
    s_imu.cfg = cfg;
    s_imu.state = VEHICLE_IMU_CALIBRATING;
    s_imu.initialized = true;
    return 0;
}

vehicle_imu_state_t vehicle_imu_update(float ax, float ay, float az,
                                       float gx, float gy, float gz,
                                       uint32_t timestamp_ms)
{
    if (!s_imu.initialized) {
        return VEHICLE_IMU_ERROR;
    }

    /* 1. 坐标变换：v_vehicle = M × v_sensor */
    const float (*m)[3] = s_imu.cfg.mount_matrix;
    const float as[3] = {ax, ay, az};
    const float gs[3] = {gx, gy, gz};
    float a[3], g[3];
    for (int i = 0; i < 3; i++) {
        a[i] = m[i][0] * as[0] + m[i][1] * as[1] + m[i][2] * as[2];
        g[i] = m[i][0] * gs[0] + m[i][1] * gs[1] + m[i][2] * gs[2];
    }

    /* 2. 采样周期：优先用时间戳差，异常时回退标称值 */
    float dt = s_imu.cfg.sample_dt_s;
    if (s_imu.last_ts_valid) {
        float d = (float)(timestamp_ms - s_imu.last_ts) / 1000.0f;
        if (d > 0.0005f && d < 0.1f) {
            dt = d;
        }
    }
    s_imu.last_ts = timestamp_ms;
    s_imu.last_ts_valid = true;

    if (s_imu.state == VEHICLE_IMU_ERROR) {
        return s_imu.state;
    }

    /* 3. 标定阶段：未完成则直接返回 */
    if (s_imu.state == VEHICLE_IMU_CALIBRATING) {
        if (!calibration_feed(a, g)) {
            return s_imu.state;
        }
        /* 标定刚完成，继续用本帧走正常处理，产出第一帧有效输出 */
    }

    /* 4. READY：去零偏 */
    float gxc = g[0] - s_imu.gyro_bias[0];
    float gyc = g[1] - s_imu.gyro_bias[1];
    float axc = a[0] - s_imu.accel_offset[0];
    float ayc = a[1] - s_imu.accel_offset[1];
    float azc = a[2] - s_imu.accel_offset[2];

    /* 5. 互补滤波：陀螺积分 + 加速度修正（带机动可信窗）。
     *    注：小角度近似，roll/pitch 独立积分，第一版实现。
     *    机动判定：用当前姿态预测重力分量，实测比力与其之差即线加速度估计。
     *    持续机动（急刹/急加速/急转弯）时线加速度显著非零 → 冻结加速度修正，
     *    防止姿态被机动拖偏。不能用 |a|≈g 判定：-2.5m/s² 急刹时
     *    |a|=√(2.5²+9.81²)≈10.12，与 g 仅差 0.3，会漏判。 */
    s_imu.roll_rad  += gxc * dt;
    s_imu.pitch_rad += gyc * dt;

    float gvec_pred[3];
    gravity_vector(s_imu.roll_rad, s_imu.pitch_rad, gvec_pred);
    float lex = axc - gvec_pred[0];
    float ley = ayc - gvec_pred[1];
    float lez = azc - gvec_pred[2];
    float linmag = sqrtf(lex * lex + ley * ley + lez * lez);

    /* 机动可信窗 + 逃逸：持续超窗超过 maneuver_max_ms 认为是姿态/安装
     * 变化（等效原理：无陀螺旋转时恒定"加速度"与倾斜不可区分），
     * 重新允许加速度修正收敛姿态；短时真实机动（急刹/转弯，秒级）保持冻结。 */
    bool allow_correction;
    if (linmag <= s_imu.cfg.accel_trust_band_ms2) {
        s_imu.maneuver_active = false;
        allow_correction = true;
    } else {
        if (!s_imu.maneuver_active) {
            s_imu.maneuver_active   = true;
            s_imu.maneuver_since_ms = timestamp_ms;
        }
        allow_correction =
            (timestamp_ms - s_imu.maneuver_since_ms) >= s_imu.cfg.maneuver_max_ms;
    }

    if (allow_correction) {
        float roll_acc  = atan2f(ayc, azc);
        float pitch_acc = atan2f(-axc, sqrtf(ayc * ayc + azc * azc));
        float al = s_imu.cfg.comp_alpha;
        s_imu.roll_rad  = al * s_imu.roll_rad  + (1.0f - al) * roll_acc;
        s_imu.pitch_rad = al * s_imu.pitch_rad + (1.0f - al) * pitch_acc;
    }

    /* 6. 重力补偿：线加速度 = 测量比力 - 零偏 - 重力分量 */
    float gvec[3];
    gravity_vector(s_imu.roll_rad, s_imu.pitch_rad, gvec);
    float lin[3];
    lin[0] = axc - gvec[0];
    lin[1] = ayc - gvec[1];
    lin[2] = azc - gvec[2];

    /* 7. jerk：微分 + 一阶低通 */
    float jraw[3] = {0.0f, 0.0f, 0.0f};
    if (s_imu.prev_lin_valid) {
        jraw[0] = (lin[0] - s_imu.prev_lin[0]) / dt;
        jraw[1] = (lin[1] - s_imu.prev_lin[1]) / dt;
        jraw[2] = (lin[2] - s_imu.prev_lin[2]) / dt;
    }
    for (int i = 0; i < 3; i++) {
        s_imu.jerk_lpf[i] += s_imu.cfg.jerk_lpf_alpha * (jraw[i] - s_imu.jerk_lpf[i]);
        s_imu.prev_lin[i] = lin[i];
    }
    s_imu.prev_lin_valid = true;

    /* 8. 输出 */
    s_imu.out.roll_rad          = s_imu.roll_rad;
    s_imu.out.pitch_rad         = s_imu.pitch_rad;
    s_imu.out.longitudinal      = lin[0];
    s_imu.out.lateral           = lin[1];
    s_imu.out.vertical_accel    = lin[2];
    s_imu.out.longitudinal_jerk = s_imu.jerk_lpf[0];
    s_imu.out.lateral_jerk      = s_imu.jerk_lpf[1];
    s_imu.out.vertical_jerk     = s_imu.jerk_lpf[2];
    s_imu.out.timestamp_ms      = timestamp_ms;

    return s_imu.state;
}

vehicle_imu_state_t vehicle_imu_get_state(void)
{
    if (!s_imu.initialized) {
        return VEHICLE_IMU_ERROR;
    }
    return s_imu.state;
}

bool vehicle_imu_get_output(vehicle_imu_output_t *out)
{
    if (!out || !s_imu.initialized || s_imu.state != VEHICLE_IMU_READY) {
        return false;
    }
    *out = s_imu.out;
    return true;
}

void vehicle_imu_request_recalibration(void)
{
    if (!s_imu.initialized) return;

    s_imu.state = VEHICLE_IMU_CALIBRATING;
    s_imu.still_count = 0;
    s_imu.violation_count = 0;
    memset(s_imu.sum_a, 0, sizeof(s_imu.sum_a));
    memset(s_imu.sum_g, 0, sizeof(s_imu.sum_g));
    memset(s_imu.gyro_bias, 0, sizeof(s_imu.gyro_bias));
    memset(s_imu.accel_offset, 0, sizeof(s_imu.accel_offset));
    s_imu.roll_rad = 0.0f;
    s_imu.pitch_rad = 0.0f;
    s_imu.prev_lin_valid = false;
    memset(s_imu.jerk_lpf, 0, sizeof(s_imu.jerk_lpf));
}
