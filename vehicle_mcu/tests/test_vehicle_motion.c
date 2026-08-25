/*
 * test_vehicle_motion.c - vehicle_motion 模块单元测试
 *
 * 覆盖：
 *   M1  急刹：阈值穿越 + 最短持续 300ms
 *   M2  最短持续不足不触发
 *   M3  滞回：进入/退出阈值之间保持激活
 *   M4  冷却防抖：冷却期内不重复触发，冷却结束需重新计时
 *   M5  急转弯左右方向区分
 *   M6  急加速 + 滞回
 *   M7  纵向/横向 jerk 事件
 *   M8  颠簸事件（最短持续 20ms）
 *   M9  置信度融合：CAN 有无/一致/矛盾
 *   M10 优先级：急刹优先于急转弯
 */
#include "vehicle_motion.h"
#include "test_common.h"

#include <string.h>

/* 构造输入：未列字段清零 */
static vehicle_motion_input_t make_input(float lon, float lat, float vert,
                                         float jlon, float jlat, uint32_t t)
{
    vehicle_motion_input_t in;
    memset(&in, 0, sizeof(in));
    in.longitudinal = lon;
    in.lateral = lat;
    in.vertical_accel = vert;
    in.longitudinal_jerk = jlon;
    in.lateral_jerk = jlat;
    in.timestamp_ms = t;
    return in;
}

/* 连续运行 n 拍（10ms 步进），返回最后一拍输出 */
static vehicle_motion_output_t run_n(float lon, float lat, float vert,
                                     float jlon, float jlat,
                                     uint32_t *t, int n,
                                     const vehicle_can_state_t *can)
{
    vehicle_motion_output_t out;
    memset(&out, 0, sizeof(out));
    for (int i = 0; i < n; i++) {
        *t += 10u;
        vehicle_motion_input_t in = make_input(lon, lat, vert, jlon, jlat, *t);
        vehicle_motion_update(&in, can, &out);
    }
    return out;
}

/* M1 急刹进入：阈值 + 最短持续 */
static void test_hard_brake_enter_duration(void)
{
    vehicle_motion_init(NULL);
    uint32_t t = 0;

    /* 前 300ms 内（条件自 t=10 起算，到 t=300 仅 290ms）不触发 */
    vehicle_motion_output_t out = run_n(-2.5f, 0, 0, 0, 0, &t, 30, NULL);
    CHECK_TRUE(out.state == VEHICLE_NORMAL, "M1a 未达最短持续不触发");

    out = run_n(-2.5f, 0, 0, 0, 0, &t, 1, NULL);   /* t=310，满 300ms */
    CHECK_TRUE(out.state == VEHICLE_HARD_BRAKE, "M1b 达到 300ms 触发 HARD_BRAKE");
    CHECK_TRUE(out.entered_event == VEHICLE_HARD_BRAKE, "M1c 本拍上报事件边沿");
    CHECK_TRUE((out.active_flags & (1u << VEHICLE_HARD_BRAKE)) != 0,
               "M1d active_flags 置位");
    CHECK_NEAR(out.confidence, 0.6f, 0.001f, "M1e 无 CAN 时置信度=独立判定值");
    CHECK_TRUE(!out.can_corroborated, "M1f 无 CAN 佐证标记");

    /* 边沿只报一次：下一拍无新事件 */
    out = run_n(-2.5f, 0, 0, 0, 0, &t, 1, NULL);
    CHECK_TRUE(out.entered_event == VEHICLE_NORMAL && out.entered_flags == 0,
               "M1g 事件边沿只上报一次");
}

/* M2 最短持续不足 */
static void test_min_duration(void)
{
    vehicle_motion_init(NULL);
    uint32_t t = 0;

    run_n(-2.5f, 0, 0, 0, 0, &t, 20, NULL);        /* 仅 200ms */
    vehicle_motion_output_t out = run_n(0, 0, 0, 0, 0, &t, 10, NULL);
    CHECK_TRUE(out.state == VEHICLE_NORMAL, "M2a 短于 300ms 不触发");
    CHECK_TRUE(out.active_flags == 0, "M2b 无任何激活事件");
}

/* M3 滞回 */
static void test_hysteresis(void)
{
    vehicle_motion_init(NULL);
    uint32_t t = 0;

    run_n(-2.5f, 0, 0, 0, 0, &t, 35, NULL);        /* 触发急刹 */
    vehicle_motion_output_t out = run_n(-1.5f, 0, 0, 0, 0, &t, 10, NULL);
    CHECK_TRUE(out.state == VEHICLE_HARD_BRAKE,
               "M3a 介于进入(-2.0)/退出(-1.2)之间保持激活（滞回）");

    out = run_n(-1.0f, 0, 0, 0, 0, &t, 5, NULL);
    CHECK_TRUE(out.state == VEHICLE_NORMAL, "M3b 越过退出阈值后解除");
    CHECK_TRUE((out.active_flags & (1u << VEHICLE_HARD_BRAKE)) == 0,
               "M3c active_flags 清除");
}

/* M4 冷却防抖 */
static void test_cooldown(void)
{
    vehicle_motion_init(NULL);
    uint32_t t = 0;

    run_n(-2.5f, 0, 0, 0, 0, &t, 35, NULL);   /* t=350 前已激活 */
    run_n(-1.0f, 0, 0, 0, 0, &t, 1, NULL);    /* t=360 解除，冷却至 1360 */

    /* 冷却期内持续超阈 400ms：不得触发 */
    vehicle_motion_output_t out = run_n(-2.5f, 0, 0, 0, 0, &t, 40, NULL);
    CHECK_TRUE(out.state == VEHICLE_NORMAL, "M4a 冷却期内不重复触发");

    /* 冷却结束后（t>1360）条件仍满足：需重新计满 300ms */
    out = run_n(-2.5f, 0, 0, 0, 0, &t, 84, NULL);   /* 到 t=1600，未满 */
    CHECK_TRUE(out.state == VEHICLE_NORMAL,
               "M4b 冷却结束后仍需重新满足最短持续");
    out = run_n(-2.5f, 0, 0, 0, 0, &t, 6, NULL);    /* 到 t=1660，计满 */
    CHECK_TRUE(out.state == VEHICLE_HARD_BRAKE, "M4c 重新计满后正常触发");
    CHECK_TRUE(out.entered_event == VEHICLE_HARD_BRAKE, "M4d 新边沿上报");
}

/* M5 急转弯左右方向 */
static void test_turn_direction(void)
{
    vehicle_motion_init(NULL);
    uint32_t t = 0;

    /* +Y = 车左方：lateral > +1.8 → 急左转 */
    vehicle_motion_output_t out = run_n(0, 2.0f, 0, 0, 0, &t, 35, NULL);
    CHECK_TRUE(out.state == VEHICLE_HARD_TURN_LEFT, "M5a 横向 +2.0 → 急左转");

    vehicle_motion_init(NULL);
    t = 0;
    out = run_n(0, -2.0f, 0, 0, 0, &t, 35, NULL);
    CHECK_TRUE(out.state == VEHICLE_HARD_TURN_RIGHT, "M5b 横向 -2.0 → 急右转");

    /* 低于进入阈值不触发 */
    vehicle_motion_init(NULL);
    t = 0;
    out = run_n(0, 1.5f, 0, 0, 0, &t, 60, NULL);
    CHECK_TRUE(out.state == VEHICLE_NORMAL, "M5c 横向 1.5 低于阈值不触发");
}

/* M6 急加速 + 滞回 */
static void test_hard_accel(void)
{
    vehicle_motion_init(NULL);
    uint32_t t = 0;

    vehicle_motion_output_t out = run_n(2.0f, 0, 0, 0, 0, &t, 35, NULL);
    CHECK_TRUE(out.state == VEHICLE_HARD_ACCEL, "M6a 纵向 +2.0 持续 300ms → 急加速");

    out = run_n(1.2f, 0, 0, 0, 0, &t, 10, NULL);
    CHECK_TRUE(out.state == VEHICLE_HARD_ACCEL, "M6b 滞回区(1.0~1.5)保持激活");

    out = run_n(0.5f, 0, 0, 0, 0, &t, 5, NULL);
    CHECK_TRUE(out.state == VEHICLE_NORMAL, "M6c 低于退出阈值解除");
}

/* M7 jerk 事件 */
static void test_high_jerk(void)
{
    vehicle_motion_init(NULL);
    uint32_t t = 0;

    /* 纵向 jerk 3.0，最短持续 50ms */
    vehicle_motion_output_t out = run_n(0, 0, 0, 3.0f, 0, &t, 4, NULL);
    CHECK_TRUE(out.state == VEHICLE_NORMAL, "M7a jerk 未达最短持续不触发");
    out = run_n(0, 0, 0, 3.0f, 0, &t, 2, NULL);   /* 累计 60ms ≥ 50ms */
    CHECK_TRUE(out.state == VEHICLE_HIGH_LONG_JERK, "M7b 纵向 jerk 超限触发");
    CHECK_TRUE(out.entered_event == VEHICLE_HIGH_LONG_JERK, "M7c 纵向上报边沿");
    CHECK_NEAR(out.confidence, 0.7f, 0.001f, "M7d jerk 事件用基础置信度");

    vehicle_motion_init(NULL);
    t = 0;
    out = run_n(0, 0, 0, 0, -3.0f, &t, 8, NULL);
    CHECK_TRUE(out.state == VEHICLE_HIGH_LAT_JERK, "M7e 横向 jerk 超限触发");
}

/* M8 颠簸事件 */
static void test_bump(void)
{
    vehicle_motion_init(NULL);
    uint32_t t = 0;

    /* 颠簸最短持续 20ms */
    vehicle_motion_output_t out = run_n(0, 0, 3.0f, 0, 0, &t, 1, NULL);
    CHECK_TRUE(out.state == VEHICLE_NORMAL, "M8a 单拍垂向超阈不触发");
    out = run_n(0, 0, 3.0f, 0, 0, &t, 2, NULL);   /* 累计 30ms ≥ 20ms */
    CHECK_TRUE(out.state == VEHICLE_BUMP, "M8b 垂向 +3.0 持续 20ms → 颠簸");

    /* 向下颠簸同样检出 */
    vehicle_motion_init(NULL);
    t = 0;
    out = run_n(0, 0, -3.0f, 0, 0, &t, 5, NULL);
    CHECK_TRUE(out.state == VEHICLE_BUMP, "M8c 垂向 -3.0 同样检出颠簸");
}

/* M9 置信度融合：CAN 有无 / 一致 / 矛盾 */
static void test_confidence_fusion(void)
{
    /* Case A：无 CAN，纯 IMU 独立判定 */
    vehicle_motion_init(NULL);
    uint32_t t = 0;
    vehicle_motion_output_t out = run_n(-2.5f, 0, 0, 0, 0, &t, 35, NULL);
    float conf_imu_only = out.confidence;
    CHECK_TRUE(out.state == VEHICLE_HARD_BRAKE, "M9a 无 CAN 时急刹仍正常触发");
    CHECK_NEAR(conf_imu_only, 0.6f, 0.001f, "M9b 无 CAN 置信度=独立判定值");

    /* Case B：CAN 佐证一致（刹车踏板 ON + 车速快速下降） */
    vehicle_motion_init(NULL);
    vehicle_can_init(NULL);
    t = 0;
    memset(&out, 0, sizeof(out));
    for (int i = 0; i < 35; i++) {
        t += 10u;
        /* 每拍降 0.09km/h ≈ 9km/h/s，高于减速佐证阈值 2.0 */
        vehicle_can_update_speed(50.0f - 0.09f * (float)i, t);
        vehicle_can_update_brake_pedal(true, t);
        vehicle_can_state_t can;
        vehicle_can_get_state(&can, t);
        vehicle_motion_input_t in = make_input(-2.5f, 0, 0, 0, 0, t);
        vehicle_motion_update(&in, &can, &out);
    }
    float conf_agree = out.confidence;
    CHECK_TRUE(out.state == VEHICLE_HARD_BRAKE, "M9c CAN 在场时急刹正常触发");
    CHECK_NEAR(conf_agree, 0.9f, 0.001f, "M9d CAN 佐证一致置信度提高");
    CHECK_TRUE(out.can_corroborated, "M9e CAN 佐证标记置位");

    /* Case C：CAN 信号矛盾（踏板未踩 + 车速不降） */
    vehicle_motion_init(NULL);
    vehicle_can_init(NULL);
    vehicle_can_update_speed(50.0f, 0);
    vehicle_can_update_brake_pedal(false, 0);
    vehicle_can_state_t can;
    vehicle_can_get_state(&can, 0);
    t = 0;
    out = run_n(-2.5f, 0, 0, 0, 0, &t, 35, &can);
    float conf_conflict = out.confidence;
    CHECK_TRUE(out.state == VEHICLE_HARD_BRAKE, "M9f CAN 矛盾时急刹仍触发（不阻塞）");
    CHECK_NEAR(conf_conflict, 0.4f, 0.001f, "M9g CAN 矛盾置信度降低");

    /* 三者排序：佐证一致 > 纯 IMU > 信号矛盾 */
    CHECK_TRUE(conf_agree > conf_imu_only, "M9h 佐证置信度高于纯 IMU");
    CHECK_TRUE(conf_imu_only > conf_conflict, "M9i 纯 IMU 高于矛盾");

    /* 急加速 + 油门开度佐证 */
    vehicle_motion_init(NULL);
    vehicle_can_init(NULL);
    vehicle_can_update_accelerator_pedal(40.0f, 0);
    vehicle_can_get_state(&can, 0);
    t = 0;
    out = run_n(2.0f, 0, 0, 0, 0, &t, 35, &can);
    CHECK_NEAR(out.confidence, 0.9f, 0.001f, "M9j 急加速+油门佐证置信度提高");
    CHECK_TRUE(out.can_corroborated, "M9k 急加速 CAN 佐证标记");
}

/* M10 优先级：急刹优先于急转弯 */
static void test_priority(void)
{
    vehicle_motion_init(NULL);
    uint32_t t = 0;

    /* 同时满足急刹与急左转；边沿只在上报发生的那一拍有效，
     * 因此拆成 30+1 拍，精确捕捉 t=310 的进入时刻 */
    run_n(-2.5f, 2.0f, 0, 0, 0, &t, 30, NULL);   /* 未满最短持续 */
    vehicle_motion_output_t out = run_n(-2.5f, 2.0f, 0, 0, 0, &t, 1, NULL);
    CHECK_TRUE(out.state == VEHICLE_HARD_BRAKE, "M10a 急刹优先级最高");
    CHECK_TRUE((out.active_flags & (1u << VEHICLE_HARD_TURN_LEFT)) != 0,
               "M10b 急转弯仍出现在 active_flags");
    CHECK_TRUE(out.entered_event == VEHICLE_HARD_BRAKE,
               "M10c 边沿代表取优先级最高者");
    CHECK_TRUE((out.entered_flags & (1u << VEHICLE_HARD_TURN_LEFT)) != 0,
               "M10d 急转弯边沿仍在 entered_flags（供计数用）");
}

int main(void)
{
    printf("==== test_vehicle_motion ====\n");
    test_hard_brake_enter_duration();
    test_min_duration();
    test_hysteresis();
    test_cooldown();
    test_turn_direction();
    test_hard_accel();
    test_high_jerk();
    test_bump();
    test_confidence_fusion();
    test_priority();
    TEST_SUMMARY("vehicle_motion");
}
