/*
 * test_vehicle_imu.c - vehicle_imu 模块单元测试（已知答案测试）
 *
 * 覆盖：
 *   T1 静止标定：完成后线加速度≈0，roll/pitch≈0
 *   T2 恒定重力分量：倾斜角估计正确（pitch≈30°）
 *   T3 安装方向矩阵：传感器旋转 90° 时加速度正确映射到车辆坐标系
 *   T4 陀螺零偏标定：长时间运行姿态不漂移
 *   T5 标定失败：长期非静止判 IMU_ERROR，重新标定可恢复
 *   T6 jerk 低通：对高频噪声不敏感
 */
#include "vehicle_imu.h"
#include "test_common.h"

#include <string.h>

#define G       VEHICLE_IMU_GRAVITY_MS2
#define RAD2DEG 57.29577951308232f

/* 连续喂入 n 帧恒定数据，步进 10ms，返回最后一帧状态 */
static vehicle_imu_state_t feed_const(float ax, float ay, float az,
                                      float gx, float gy, float gz,
                                      uint32_t *t_ms, int n)
{
    vehicle_imu_state_t st = VEHICLE_IMU_CALIBRATING;
    for (int i = 0; i < n; i++) {
        *t_ms += 10u;
        st = vehicle_imu_update(ax, ay, az, gx, gy, gz, *t_ms);
    }
    return st;
}

/* T1 静止标定：线加速度≈0 */
static void test_stationary_calibration(void)
{
    vehicle_imu_init(NULL);
    uint32_t t = 0;

    feed_const(0, 0, G, 0, 0, 0, &t, 150);
    CHECK_TRUE(vehicle_imu_get_state() == VEHICLE_IMU_CALIBRATING,
               "T1a 标定样本不足时保持 CALIBRATING");

    feed_const(0, 0, G, 0, 0, 0, &t, 100);   /* 累计 250 > 200，完成标定 */
    CHECK_TRUE(vehicle_imu_get_state() == VEHICLE_IMU_READY,
               "T1b 静止标定完成后进入 READY");

    vehicle_imu_output_t out;
    CHECK_TRUE(vehicle_imu_get_output(&out), "T1c READY 后输出有效");
    CHECK_NEAR(out.longitudinal,   0.0f, 0.02f, "T1d 静止线加速度 X≈0");
    CHECK_NEAR(out.lateral,        0.0f, 0.02f, "T1e 静止线加速度 Y≈0");
    CHECK_NEAR(out.vertical_accel, 0.0f, 0.02f, "T1f 静止线加速度 Z≈0");
    CHECK_NEAR(out.roll_rad  * RAD2DEG, 0.0f, 0.5f, "T1g 静止 roll≈0°");
    CHECK_NEAR(out.pitch_rad * RAD2DEG, 0.0f, 0.5f, "T1h 静止 pitch≈0°");
}

/* T2 恒定重力分量：倾斜角估计正确 */
static void test_tilt_angle_from_gravity(void)
{
    vehicle_imu_init(NULL);
    uint32_t t = 0;

    feed_const(0, 0, G, 0, 0, 0, &t, 250);   /* 水平静止标定 */

    /* 车体绕 Y 轴抬头 30°：静止比力 = (-g·sin30, 0, g·cos30) */
    float ax = -G * 0.5f;
    float az = G * 0.8660254037844386f;
    feed_const(ax, 0, az, 0, 0, 0, &t, 1000);   /* 10s 充分收敛 */

    vehicle_imu_output_t out;
    CHECK_TRUE(vehicle_imu_get_output(&out), "T2a 保持 READY");
    CHECK_NEAR(out.pitch_rad * RAD2DEG, 30.0f, 1.5f, "T2b pitch 收敛到 ≈30°");
    CHECK_NEAR(out.roll_rad * RAD2DEG, 0.0f, 1.0f, "T2c roll≈0°");
    /* 姿态收敛后重力补偿应把线加速度重新压回 0 */
    CHECK_NEAR(out.longitudinal,   0.0f, 0.10f, "T2d 收敛后线加速度 X≈0");
    CHECK_NEAR(out.vertical_accel, 0.0f, 0.10f, "T2e 收敛后线加速度 Z≈0");
}

/* T3 安装方向矩阵：传感器绕 Z 旋转 90° */
static void test_mount_matrix(void)
{
    vehicle_imu_config_t cfg;
    vehicle_imu_get_default_config(&cfg);

    /* 传感器相对车辆绕 Z 轴旋转 +90°：veh_x = -sen_y, veh_y = sen_x */
    float m[3][3] = {{0.0f, -1.0f, 0.0f},
                     {1.0f,  0.0f, 0.0f},
                     {0.0f,  0.0f, 1.0f}};
    memcpy(cfg.mount_matrix, m, sizeof(m));
    /* 关小加速度可信窗：持续 1m/s² 加速时不允许姿态被带偏 */
    cfg.accel_trust_band_ms2 = 0.02f;

    vehicle_imu_init(&cfg);
    uint32_t t = 0;
    feed_const(0, 0, G, 0, 0, 0, &t, 250);
    CHECK_TRUE(vehicle_imu_get_state() == VEHICLE_IMU_READY, "T3a READY");

    /* 车辆前向加速 1m/s²：veh a=(1,0,g) → 传感器读数 sen=M^T·veh=(0,-1,g) */
    feed_const(0.0f, -1.0f, G, 0, 0, 0, &t, 50);

    vehicle_imu_output_t out;
    CHECK_TRUE(vehicle_imu_get_output(&out), "T3b READY");
    CHECK_NEAR(out.longitudinal, 1.0f, 0.10f, "T3c 安装矩阵映射后纵向≈+1m/s²");
    CHECK_NEAR(out.lateral,      0.0f, 0.05f, "T3d 横向≈0");
    CHECK_NEAR(out.pitch_rad * RAD2DEG, 0.0f, 0.5f,
               "T3e 可信窗关闭时姿态不被加速拖偏");
}

/* T4 陀螺零偏标定 */
static void test_gyro_bias_calibration(void)
{
    vehicle_imu_init(NULL);
    uint32_t t = 0;

    /* 传感器静止，但陀螺带固定零偏 (0.05, 0.08, 0) rad/s */
    feed_const(0, 0, G, 0.05f, 0.08f, 0.0f, &t, 250);
    CHECK_TRUE(vehicle_imu_get_state() == VEHICLE_IMU_READY,
               "T4a 带零偏仍可完成静止标定");

    feed_const(0, 0, G, 0.05f, 0.08f, 0.0f, &t, 2000);   /* 20s */
    vehicle_imu_output_t out;
    CHECK_TRUE(vehicle_imu_get_output(&out), "T4b READY");
    /* 若零偏未标定，0.08rad/s 的 pitch 零偏 20s 将积分出巨大漂移 */
    CHECK_NEAR(out.roll_rad * RAD2DEG,  0.0f, 0.5f, "T4c 零偏标定后 roll 不漂移");
    CHECK_NEAR(out.pitch_rad * RAD2DEG, 0.0f, 0.5f, "T4d 零偏标定后 pitch 不漂移");
}

/* T5 标定失败与恢复 */
static void test_calibration_error_and_retry(void)
{
    vehicle_imu_init(NULL);
    uint32_t t = 0;

    /* 持续剧烈晃动：|a| 在 6.81/12.81 间跳变，永远不满足静止条件 */
    vehicle_imu_state_t st = VEHICLE_IMU_CALIBRATING;
    for (int i = 0; i < 1100; i++) {
        t += 10u;
        float az = (i % 2 == 0) ? (G + 3.0f) : (G - 3.0f);
        st = vehicle_imu_update(0, 0, az, 0, 0, 0, t);
    }
    CHECK_TRUE(st == VEHICLE_IMU_ERROR, "T5a 长期非静止判 IMU_ERROR");
    CHECK_TRUE(vehicle_imu_get_state() == VEHICLE_IMU_ERROR,
               "T5b 状态保持 ERROR");

    vehicle_imu_output_t out;
    CHECK_TRUE(!vehicle_imu_get_output(&out), "T5c ERROR 时输出无效");

    /* 重新标定：恢复静止后应能回到 READY */
    vehicle_imu_request_recalibration();
    CHECK_TRUE(vehicle_imu_get_state() == VEHICLE_IMU_CALIBRATING,
               "T5d 请求重标定后回到 CALIBRATING");
    feed_const(0, 0, G, 0, 0, 0, &t, 250);
    CHECK_TRUE(vehicle_imu_get_state() == VEHICLE_IMU_READY,
               "T5e 重新标定后恢复 READY");
}

/* T6 jerk 低通：对高频噪声不敏感 */
static void test_jerk_lowpass_noise(void)
{
    vehicle_imu_init(NULL);
    uint32_t t = 0;
    feed_const(0, 0, G, 0, 0, 0, &t, 250);

    /* 交替 ±0.5m/s² 的高频噪声：原始微分幅值 = 1.0/0.01 = 100 m/s³ */
    float max_jerk = 0.0f;
    for (int i = 0; i < 200; i++) {
        t += 10u;
        float ax = (i % 2 == 0) ? 0.5f : -0.5f;
        vehicle_imu_update(ax, 0, G, 0, 0, 0, t);
        if (i > 100) {   /* 跳过初始瞬态 */
            vehicle_imu_output_t out;
            vehicle_imu_get_output(&out);
            float a = fabsf(out.longitudinal_jerk);
            if (a > max_jerk) max_jerk = a;
        }
    }
    /* 一阶低通 α=0.2 的理论稳态幅值 ≈ α·raw/(2-α) ≈ 11 m/s³，
     * 远小于原始微分 100 m/s³ */
    CHECK_TRUE(max_jerk < 30.0f, "T6a jerk 低通显著抑制高频噪声");
    CHECK_TRUE(max_jerk > 1.0f,  "T6b jerk 仍有输出（未被完全滤死）");
}

int main(void)
{
    printf("==== test_vehicle_imu ====\n");
    test_stationary_calibration();
    test_tilt_angle_from_gravity();
    test_mount_matrix();
    test_gyro_bias_calibration();
    test_calibration_error_and_retry();
    test_jerk_lowpass_noise();
    TEST_SUMMARY("vehicle_imu");
}
