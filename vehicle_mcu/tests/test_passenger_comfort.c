/*
 * test_passenger_comfort.c - passenger_comfort 模块单元测试（已知答案测试）
 *
 * 默认配置下的理论值（测试依据）：
 *   单方向得分 = 100 × (1 - (rms_a + 0.5×rms_j) / metric_full)
 *   纵向 metric_full=2.0，横向 2.0，垂向 3.0
 *   行程指数 = 0.4×纵向 + 0.3×横向 + 0.3×垂向 - min(事件影响合计, 30)
 *
 * 覆盖：
 *   P1 完全平顺：全部得分 100，计数为 0
 *   P2 恒定纵向加速度：纵向得分≈75，行程指数≈90
 *   P3 事件计数与行程指数向下调整
 *   P4 事件影响总量上限
 *   P5 小幅噪声下平顺度保持高位（对噪声不敏感）
 */
#include "passenger_comfort.h"
#include "test_common.h"

#include <string.h>

/* 构造一帧 IMU 输出 */
static vehicle_imu_output_t make_imu(float lon, float lat, float vert,
                                     float jlon, float jlat, float jvert,
                                     uint32_t t)
{
    vehicle_imu_output_t o;
    memset(&o, 0, sizeof(o));
    o.longitudinal = lon;
    o.lateral = lat;
    o.vertical_accel = vert;
    o.longitudinal_jerk = jlon;
    o.lateral_jerk = jlat;
    o.vertical_jerk = jvert;
    o.timestamp_ms = t;
    return o;
}

/* 连续喂 n 帧恒定数据（10ms 步进） */
static void feed_n(float lon, float lat, float vert,
                   float jlon, float jlat, float jvert,
                   uint32_t *t, int n)
{
    for (int i = 0; i < n; i++) {
        *t += 10u;
        vehicle_imu_output_t o = make_imu(lon, lat, vert, jlon, jlat, jvert, *t);
        passenger_comfort_update(&o);
    }
}

/* P1 完全平顺 */
static void test_perfectly_smooth(void)
{
    passenger_comfort_init(NULL);
    uint32_t t = 0;
    feed_n(0, 0, 0, 0, 0, 0, &t, 500);

    passenger_comfort_metrics_t m;
    passenger_comfort_get_metrics(&m);
    CHECK_NEAR(m.longitudinal_smoothness, 100.0f, 0.01f, "P1a 平顺行程纵向=100");
    CHECK_NEAR(m.lateral_smoothness,      100.0f, 0.01f, "P1b 平顺行程横向=100");
    CHECK_NEAR(m.vertical_comfort,        100.0f, 0.01f, "P1c 平顺行程垂向=100");
    CHECK_NEAR(m.trip_comfort_index,      100.0f, 0.01f, "P1d 平顺行程指数=100");
    CHECK_TRUE(m.hard_brake_count == 0 && m.hard_accel_count == 0
               && m.hard_turn_count == 0 && m.bump_count == 0,
               "P1e 无事件计数");
    CHECK_TRUE(m.sample_count == 500, "P1f 样本计数正确");
    CHECK_TRUE(m.trip_duration_ms > 0, "P1g 行程时长被记录");
}

/* P2 恒定纵向加速度：理论纵向得分 75，行程指数 90 */
static void test_constant_longitudinal(void)
{
    passenger_comfort_init(NULL);
    uint32_t t = 0;
    feed_n(0.5f, 0, 0, 0, 0, 0, &t, 1000);   /* 10s，EMA 充分收敛 */

    passenger_comfort_metrics_t m;
    passenger_comfort_get_metrics(&m);
    /* rms_a=0.5 → metric=0.5 → 得分 = 100×(1-0.5/2.0) = 75 */
    CHECK_NEAR(m.longitudinal_smoothness, 75.0f, 2.0f, "P2a 纵向得分≈75");
    CHECK_NEAR(m.lateral_smoothness, 100.0f, 0.01f, "P2b 横向不受影响");
    CHECK_NEAR(m.vertical_comfort, 100.0f, 0.01f, "P2c 垂向不受影响");
    /* 指数 = 0.4×75 + 0.3×100 + 0.3×100 = 90 */
    CHECK_NEAR(m.trip_comfort_index, 90.0f, 2.0f, "P2d 行程指数≈90");
}

/* P3 事件计数与行程指数 */
static void test_event_counting(void)
{
    passenger_comfort_init(NULL);
    uint32_t t = 0;
    feed_n(0, 0, 0, 0, 0, 0, &t, 200);   /* 完全平顺基准 100 */

    for (int i = 0; i < 3; i++) {
        passenger_comfort_on_motion_events(1u << VEHICLE_HARD_BRAKE);
    }
    passenger_comfort_on_motion_events(1u << VEHICLE_HARD_TURN_LEFT);
    passenger_comfort_on_motion_events(1u << VEHICLE_HARD_TURN_RIGHT);
    passenger_comfort_on_motion_events((1u << VEHICLE_BUMP) | (1u << VEHICLE_HIGH_LONG_JERK));
    passenger_comfort_on_motion_events(1u << VEHICLE_BUMP);

    passenger_comfort_metrics_t m;
    passenger_comfort_get_metrics(&m);
    CHECK_TRUE(m.hard_brake_count == 3, "P3a 急刹计数=3");
    CHECK_TRUE(m.hard_turn_count == 2,  "P3b 急转弯计数=2（左右合计）");
    CHECK_TRUE(m.bump_count == 2,       "P3c 颠簸计数=2");
    CHECK_TRUE(m.high_jerk_count == 1,  "P3d jerk 计数=1");
    CHECK_TRUE(m.hard_accel_count == 0, "P3e 急加速计数=0");
    /* 影响 = 3×1.5 + 2×1.2 + 2×0.8 + 1×0.5 = 9.0 → 指数 = 100-9.0 = 91.0 */
    CHECK_NEAR(m.trip_comfort_index, 91.0f, 0.2f, "P3f 行程指数按事件向下调整");
}

/* P4 事件影响总量上限 */
static void test_impact_cap(void)
{
    passenger_comfort_init(NULL);
    uint32_t t = 0;
    feed_n(0, 0, 0, 0, 0, 0, &t, 100);

    for (int i = 0; i < 100; i++) {
        passenger_comfort_on_motion_events(1u << VEHICLE_HARD_BRAKE);
    }

    passenger_comfort_metrics_t m;
    passenger_comfort_get_metrics(&m);
    CHECK_TRUE(m.hard_brake_count == 100, "P4a 计数不封顶");
    /* 原始影响 100×1.5=150，封顶 30 → 指数 = 100-30 = 70 */
    CHECK_NEAR(m.trip_comfort_index, 70.0f, 0.2f, "P4b 事件影响总量封顶 30");
}

/* P5 小幅噪声不敏感 */
static void test_noise_insensitivity(void)
{
    passenger_comfort_init(NULL);
    uint32_t t = 0;

    /* 交替 ±0.05m/s² 加速度 + ±0.3m/s³ jerk（零均值小幅噪声） */
    for (int i = 0; i < 2000; i++) {
        t += 10u;
        float a = (i % 2 == 0) ? 0.05f : -0.05f;
        float j = (i % 2 == 0) ? 0.3f  : -0.3f;
        vehicle_imu_output_t o = make_imu(a, 0, 0, j, 0, 0, t);
        passenger_comfort_update(&o);
    }

    passenger_comfort_metrics_t m;
    passenger_comfort_get_metrics(&m);
    /* rms_a=0.05, rms_j=0.3 → metric=0.05+0.5×0.3=0.2 → 得分 = 100×(1-0.2/2.0) = 90 */
    CHECK_NEAR(m.longitudinal_smoothness, 90.0f, 2.0f,
               "P5a 小幅噪声下纵向平顺度≈90");
    CHECK_TRUE(m.trip_comfort_index > 85.0f,
               "P5b 小幅噪声下行程指数保持高位");
}

int main(void)
{
    printf("==== test_passenger_comfort ====\n");
    test_perfectly_smooth();
    test_constant_longitudinal();
    test_event_counting();
    test_impact_cap();
    test_noise_insensitivity();
    TEST_SUMMARY("passenger_comfort");
}
