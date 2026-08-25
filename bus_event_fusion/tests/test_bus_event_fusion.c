/*
 * test_bus_event_fusion.c - 公交事件融合引擎单元测试
 *
 * 必测场景（任务要求）：
 *   1. 时序方向敏感：head_down 后 800ms 急刹 → 触发 SUSPECTED；
 *      急刹后 8 秒才 head_down → 不得触发
 *   2. 单次 HARD_BRAKE 永不产生驾驶员归责
 *   3. CASE 1 / CASE 2 / CASE 3 各自触发正确
 *   4. 30 秒窗口外的事件不参与关联
 *   5. 无 CAN / 车速信息时系统照常工作
 *
 * 附加场景：
 *   6. CASE 2 关联置信度随时间间隔衰减
 *   7. CASE 2 边界：间隔 0 / 2000ms 触发，2001ms 不触发
 *   8. CASE 3：有车速>0 触发；有时间线运动事件触发；两者都无 → 不触发
 *   9. reset 后时间线清空，旧事件不再参与关联
 *  10. 普通 DMS 事件（眨眼/哈欠/人脸丢失）不产生融合输出
 */

#include "bus_event_fusion.h"

#include <stdio.h>
#include <string.h>

static int s_test_pass = 0;
static int s_test_fail = 0;

#define TEST_ASSERT(condition, test_name) do { \
    if (condition) { \
        printf("  [PASS] %s\n", test_name); \
        s_test_pass++; \
    } else { \
        printf("  [FAIL] %s\n", test_name); \
        s_test_fail++; \
    } \
} while (0)

/* ==================== 测试辅助 ==================== */

/* 构造 DMS 事件 */
static fusion_dms_event_t make_dms(fusion_dms_event_type_t type, uint64_t ts_ms,
                                   uint64_t duration_ms)
{
    fusion_dms_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type         = type;
    ev.risk_level   = FUSION_RISK_NORMAL;
    ev.timestamp_ms = ts_ms;
    ev.duration_ms  = duration_ms;
    return ev;
}

/* 构造运动事件 */
static fusion_motion_event_t make_motion(fusion_motion_event_type_t type, uint64_t ts_ms)
{
    fusion_motion_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type              = type;
    ev.timestamp_ms      = ts_ms;
    ev.confidence        = 90;
    ev.longitudinal_accel = -6.0f;
    ev.lateral_accel     = 0.5f;
    return ev;
}

static void fresh_init(void)
{
    bus_event_fusion_init();
}

/* ==================== 测试用例 ==================== */

/* 测试 1：CASE 1 —— 单次 HARD_BRAKE → EMERGENCY_BRAKE，归因 UNKNOWN，永不归责驾驶员 */
static void test_case1_single_hard_brake_unknown(void)
{
    printf("\n--- Test: CASE1 single HARD_BRAKE -> EMERGENCY_BRAKE / UNKNOWN ---\n");

    fresh_init();

    fusion_motion_event_t brake = make_motion(FUSION_MOTION_HARD_BRAKE, 5000);
    bus_safety_event_t out;
    bool triggered = bus_event_fusion_feed_motion(&brake, &out);

    TEST_ASSERT(triggered, "hard brake triggers output");
    TEST_ASSERT(out.event_type == BUS_EVENT_EMERGENCY_BRAKE,
                "event_type == EMERGENCY_BRAKE");
    TEST_ASSERT(out.attribution == ATTRIBUTION_UNKNOWN,
                "attribution == UNKNOWN (single brake never blames driver)");
    TEST_ASSERT(out.attribution != ATTRIBUTION_DRIVER_ATTENTION,
                "attribution != DRIVER_ATTENTION");
    TEST_ASSERT(strstr(out.message, "SUSPECTED") == NULL,
                "message has no SUSPECTED wording");
    TEST_ASSERT(out.human_review_required == true,
                "human_review_required == true");
    TEST_ASSERT(out.timestamp_ms == 5000, "timestamp propagated");
}

/* 测试 2：CASE 2 —— head_down 后 800ms 急刹 → 触发 SUSPECTED */
static void test_case2_head_down_then_brake_800ms(void)
{
    printf("\n--- Test: CASE2 head_down -> 800ms -> hard_brake = SUSPECTED ---\n");

    fresh_init();

    fusion_dms_event_t hd = make_dms(FUSION_DMS_HEAD_DOWN, 1000, 1500);
    hd.risk_level = FUSION_RISK_WARNING;
    bus_safety_event_t out;

    bool t1 = bus_event_fusion_feed_dms(&hd, &out);
    TEST_ASSERT(!t1, "head_down alone produces no fused output");

    fusion_motion_event_t brake = make_motion(FUSION_MOTION_HARD_BRAKE, 1800);
    bool t2 = bus_event_fusion_feed_motion(&brake, &out);

    TEST_ASSERT(t2, "brake after head_down triggers output");
    TEST_ASSERT(out.event_type == BUS_EVENT_ATTENTION_RELATED_BRAKE_SUSPECTED,
                "event_type == ATTENTION_RELATED_BRAKE_SUSPECTED");
    TEST_ASSERT(out.attribution == ATTRIBUTION_DRIVER_ATTENTION,
                "attribution == DRIVER_ATTENTION");
    TEST_ASSERT(strstr(out.message, "SUSPECTED") != NULL,
                "message contains SUSPECTED wording");
    TEST_ASSERT(out.human_review_required == true,
                "human_review_required == true");
    TEST_ASSERT(out.correlation_confidence > 0 && out.correlation_confidence <= 100,
                "confidence in (0,100]");
    TEST_ASSERT(strstr(out.evidence, "gap=800ms") != NULL,
                "evidence records 800ms gap");
}

/* 测试 3：时序方向敏感 —— 急刹后 8 秒才 head_down → 不得触发 SUSPECTED */
static void test_case2_reversed_order_no_trigger(void)
{
    printf("\n--- Test: hard_brake -> 8s -> head_down must NOT trigger SUSPECTED ---\n");

    fresh_init();

    /* 先急刹 */
    fusion_motion_event_t brake = make_motion(FUSION_MOTION_HARD_BRAKE, 1000);
    bus_safety_event_t out;
    bool t1 = bus_event_fusion_feed_motion(&brake, &out);

    TEST_ASSERT(t1, "brake triggers output");
    TEST_ASSERT(out.event_type == BUS_EVENT_EMERGENCY_BRAKE,
                "brake alone -> EMERGENCY_BRAKE");
    TEST_ASSERT(out.attribution == ATTRIBUTION_UNKNOWN,
                "brake alone attribution UNKNOWN");

    /* 8 秒后才低头（反向时序，不允许关联） */
    fusion_dms_event_t hd = make_dms(FUSION_DMS_HEAD_DOWN, 9000, 1500);
    bool t2 = bus_event_fusion_feed_dms(&hd, &out);

    TEST_ASSERT(!t2, "later head_down produces no fused output");
    TEST_ASSERT(out.event_type == BUS_EVENT_NONE || strstr(out.message, "SUSPECTED") == NULL,
                "no SUSPECTED event generated for reversed order");
}

/* 测试 4：CASE 2 关联置信度随时间间隔衰减 */
static void test_case2_confidence_decays_with_gap(void)
{
    printf("\n--- Test: CASE2 confidence decays with gap ---\n");

    /* 第一次：间隔 200ms */
    fresh_init();
    fusion_dms_event_t hd1 = make_dms(FUSION_DMS_HEAD_DOWN, 1000, 500);
    bus_safety_event_t out1;
    bus_event_fusion_feed_dms(&hd1, &out1);
    fusion_motion_event_t brake1 = make_motion(FUSION_MOTION_HARD_BRAKE, 1200);
    bus_event_fusion_feed_motion(&brake1, &out1);

    /* 第二次：间隔 1800ms */
    fresh_init();
    fusion_dms_event_t hd2 = make_dms(FUSION_DMS_HEAD_DOWN, 1000, 500);
    bus_safety_event_t out2;
    bus_event_fusion_feed_dms(&hd2, &out2);
    fusion_motion_event_t brake2 = make_motion(FUSION_MOTION_HARD_BRAKE, 2800);
    bus_event_fusion_feed_motion(&brake2, &out2);

    TEST_ASSERT(out1.event_type == BUS_EVENT_ATTENTION_RELATED_BRAKE_SUSPECTED,
                "gap 200ms triggers SUSPECTED");
    TEST_ASSERT(out2.event_type == BUS_EVENT_ATTENTION_RELATED_BRAKE_SUSPECTED,
                "gap 1800ms triggers SUSPECTED");
    TEST_ASSERT(out1.correlation_confidence > out2.correlation_confidence,
                "confidence decays as gap grows");
}

/* 测试 5：CASE 2 边界 —— 间隔 0 / 2000ms 触发，2001ms 不触发 */
static void test_case2_boundary(void)
{
    printf("\n--- Test: CASE2 boundary 0 / 2000 / 2001 ms ---\n");

    bus_safety_event_t out;

    /* 间隔 0（同一时间戳，0~2 秒含端点） */
    fresh_init();
    fusion_dms_event_t hd0 = make_dms(FUSION_DMS_HEAD_DOWN, 3000, 100);
    bus_event_fusion_feed_dms(&hd0, &out);
    fusion_motion_event_t b0 = make_motion(FUSION_MOTION_HARD_BRAKE, 3000);
    bus_event_fusion_feed_motion(&b0, &out);
    TEST_ASSERT(out.event_type == BUS_EVENT_ATTENTION_RELATED_BRAKE_SUSPECTED,
                "gap 0ms triggers SUSPECTED");

    /* 间隔 2000ms（上限，含端点） */
    fresh_init();
    fusion_dms_event_t hd1 = make_dms(FUSION_DMS_HEAD_DOWN, 3000, 100);
    bus_event_fusion_feed_dms(&hd1, &out);
    fusion_motion_event_t b1 = make_motion(FUSION_MOTION_HARD_BRAKE, 5000);
    bus_event_fusion_feed_motion(&b1, &out);
    TEST_ASSERT(out.event_type == BUS_EVENT_ATTENTION_RELATED_BRAKE_SUSPECTED,
                "gap 2000ms triggers SUSPECTED");

    /* 间隔 2001ms（超出上限）→ CASE 1 */
    fresh_init();
    fusion_dms_event_t hd2 = make_dms(FUSION_DMS_HEAD_DOWN, 3000, 100);
    bus_event_fusion_feed_dms(&hd2, &out);
    fusion_motion_event_t b2 = make_motion(FUSION_MOTION_HARD_BRAKE, 5001);
    bus_event_fusion_feed_motion(&b2, &out);
    TEST_ASSERT(out.event_type == BUS_EVENT_EMERGENCY_BRAKE,
                "gap 2001ms falls back to EMERGENCY_BRAKE");
    TEST_ASSERT(out.attribution == ATTRIBUTION_UNKNOWN,
                "gap 2001ms attribution UNKNOWN");
}

/* 测试 6：CASE 3 —— LONG_EYE_CLOSED + 车速 > 0 → FATIGUE_HIGH_RISK */
static void test_case3_fatigue_with_speed(void)
{
    printf("\n--- Test: CASE3 long_eye_closed + speed>0 -> FATIGUE_HIGH_RISK ---\n");

    fresh_init();
    bus_event_fusion_update_speed(10.0f, 9000); /* 车速 10 m/s */

    fusion_dms_event_t lec = make_dms(FUSION_DMS_LONG_EYE_CLOSED, 10000, 2000);
    lec.risk_level = FUSION_RISK_HIGH;
    bus_safety_event_t out;
    bool triggered = bus_event_fusion_feed_dms(&lec, &out);

    TEST_ASSERT(triggered, "long_eye_closed while moving triggers output");
    TEST_ASSERT(out.event_type == BUS_EVENT_FATIGUE_HIGH_RISK,
                "event_type == FATIGUE_HIGH_RISK");
    TEST_ASSERT(out.attribution == ATTRIBUTION_DRIVER_ATTENTION,
                "attribution == DRIVER_ATTENTION");
    TEST_ASSERT(strstr(out.message, "SUSPECTED") != NULL,
                "message contains SUSPECTED wording");
    TEST_ASSERT(out.human_review_required == true,
                "human_review_required == true");
}

/* 测试 7：CASE 3 —— 无车速，但时间线内存在运动事件 → 触发 */
static void test_case3_fatigue_with_motion_in_window(void)
{
    printf("\n--- Test: CASE3 motion event in timeline (no speed) triggers ---\n");

    fresh_init(); /* 从不调用 update_speed */

    /* 先有一个普通运动事件（如颠簸），证明车在动 */
    fusion_motion_event_t bump = make_motion(FUSION_MOTION_BUMP, 5000);
    bus_safety_event_t out;
    bool t1 = bus_event_fusion_feed_motion(&bump, &out);
    TEST_ASSERT(!t1, "bump itself produces no fused output");

    fusion_dms_event_t lec = make_dms(FUSION_DMS_LONG_EYE_CLOSED, 6000, 2000);
    bool t2 = bus_event_fusion_feed_dms(&lec, &out);

    TEST_ASSERT(t2, "long_eye_closed with motion in window triggers");
    TEST_ASSERT(out.event_type == BUS_EVENT_FATIGUE_HIGH_RISK,
                "event_type == FATIGUE_HIGH_RISK");
}

/* 测试 8：CASE 3 —— 无车速且时间线无运动事件 → 不触发 */
static void test_case3_no_motion_info_no_trigger(void)
{
    printf("\n--- Test: CASE3 no speed & no motion -> no trigger ---\n");

    fresh_init();

    fusion_dms_event_t lec = make_dms(FUSION_DMS_LONG_EYE_CLOSED, 10000, 2000);
    bus_safety_event_t out;
    bool triggered = bus_event_fusion_feed_dms(&lec, &out);

    TEST_ASSERT(!triggered, "no motion info -> no FATIGUE_HIGH_RISK (conservative)");
}

/* 测试 9：30 秒窗口外的事件不参与关联 */
static void test_window_30s_exclusion(void)
{
    printf("\n--- Test: events outside 30s window are excluded ---\n");

    /* 场景 A：运动事件在 31 秒前 → CASE 3 不得触发 */
    fresh_init();
    fusion_motion_event_t bump = make_motion(FUSION_MOTION_BUMP, 1000);
    bus_safety_event_t out;
    bus_event_fusion_feed_motion(&bump, &out);

    fusion_dms_event_t lec = make_dms(FUSION_DMS_LONG_EYE_CLOSED, 32000, 2000);
    bool t1 = bus_event_fusion_feed_dms(&lec, &out);
    TEST_ASSERT(!t1, "motion event 31s ago excluded from CASE3 correlation");

    /* 场景 B：窗口内的运动事件（29 秒前）仍可关联 → 对照 */
    fresh_init();
    fusion_motion_event_t bump2 = make_motion(FUSION_MOTION_BUMP, 1000);
    bus_event_fusion_feed_motion(&bump2, &out);

    fusion_dms_event_t lec2 = make_dms(FUSION_DMS_LONG_EYE_CLOSED, 30000, 2000);
    bool t2 = bus_event_fusion_feed_dms(&lec2, &out);
    TEST_ASSERT(t2 && out.event_type == BUS_EVENT_FATIGUE_HIGH_RISK,
                "motion event 29s ago still correlates (inside window)");

    /* 场景 C：31 秒前的低头 + 当前急刹 → 只能 CASE 1 */
    fresh_init();
    fusion_dms_event_t hd = make_dms(FUSION_DMS_HEAD_DOWN, 1000, 500);
    bus_event_fusion_feed_dms(&hd, &out);
    fusion_motion_event_t brake = make_motion(FUSION_MOTION_HARD_BRAKE, 32000);
    bus_event_fusion_feed_motion(&brake, &out);
    TEST_ASSERT(out.event_type == BUS_EVENT_EMERGENCY_BRAKE,
                "head_down 31s ago excluded from CASE2 correlation");
    TEST_ASSERT(out.attribution == ATTRIBUTION_UNKNOWN,
                "attribution UNKNOWN for out-of-window case");
}

/* 测试 10：无 CAN / 车速信息时系统照常工作 */
static void test_no_can_system_works(void)
{
    printf("\n--- Test: system works without CAN/speed info ---\n");

    fresh_init(); /* 从不调用 update_speed */

    bus_safety_event_t out;

    /* CASE 1 照常 */
    fusion_motion_event_t brake1 = make_motion(FUSION_MOTION_HARD_BRAKE, 1000);
    bool t1 = bus_event_fusion_feed_motion(&brake1, &out);
    TEST_ASSERT(t1 && out.event_type == BUS_EVENT_EMERGENCY_BRAKE &&
                out.attribution == ATTRIBUTION_UNKNOWN,
                "CASE1 works without CAN");

    /* CASE 2 照常 */
    fresh_init();
    fusion_dms_event_t hd = make_dms(FUSION_DMS_HEAD_DOWN, 1000, 500);
    bus_event_fusion_feed_dms(&hd, &out);
    fusion_motion_event_t brake2 = make_motion(FUSION_MOTION_HARD_BRAKE, 1800);
    bool t2 = bus_event_fusion_feed_motion(&brake2, &out);
    TEST_ASSERT(t2 && out.event_type == BUS_EVENT_ATTENTION_RELATED_BRAKE_SUSPECTED,
                "CASE2 works without CAN");

    /* CASE 3 照常（依赖时间线运动事件而非车速） */
    fresh_init();
    fusion_motion_event_t accel = make_motion(FUSION_MOTION_HARD_ACCEL, 2000);
    bus_event_fusion_feed_motion(&accel, &out);
    fusion_dms_event_t lec = make_dms(FUSION_DMS_LONG_EYE_CLOSED, 3000, 2000);
    bool t3 = bus_event_fusion_feed_dms(&lec, &out);
    TEST_ASSERT(t3 && out.event_type == BUS_EVENT_FATIGUE_HIGH_RISK,
                "CASE3 works without CAN (motion in timeline)");
}

/* 测试 11：reset 后时间线清空 */
static void test_reset_clears_timeline(void)
{
    printf("\n--- Test: reset clears timeline ---\n");

    fresh_init();
    fusion_dms_event_t hd = make_dms(FUSION_DMS_HEAD_DOWN, 1000, 500);
    bus_safety_event_t out;
    bus_event_fusion_feed_dms(&hd, &out);

    bus_event_fusion_reset();

    /* reset 后 800ms 间隔内的急刹不应再关联到 reset 前的低头 */
    fusion_motion_event_t brake = make_motion(FUSION_MOTION_HARD_BRAKE, 1800);
    bus_event_fusion_feed_motion(&brake, &out);
    TEST_ASSERT(out.event_type == BUS_EVENT_EMERGENCY_BRAKE,
                "after reset, old head_down no longer correlates");
    TEST_ASSERT(out.attribution == ATTRIBUTION_UNKNOWN,
                "after reset, attribution UNKNOWN");

    /* reset 后旧车速也被清空 */
    fresh_init();
    bus_event_fusion_update_speed(8.0f, 1000);
    bus_event_fusion_reset();
    fusion_dms_event_t lec = make_dms(FUSION_DMS_LONG_EYE_CLOSED, 2000, 2000);
    bool t = bus_event_fusion_feed_dms(&lec, &out);
    TEST_ASSERT(!t, "after reset, stale speed no longer counts");
}

/* 测试 12：普通 DMS 事件不产生融合输出 */
static void test_normal_dms_events_no_output(void)
{
    printf("\n--- Test: normal DMS events produce no fused output ---\n");

    fresh_init();
    bus_event_fusion_update_speed(10.0f, 1000); /* 即便车在动 */

    bus_safety_event_t out;

    fusion_dms_event_t eye = make_dms(FUSION_DMS_EYE_CLOSED, 2000, 300);
    TEST_ASSERT(!bus_event_fusion_feed_dms(&eye, &out), "EYE_CLOSED no output");

    fusion_dms_event_t yawn = make_dms(FUSION_DMS_YAWN, 3000, 1200);
    TEST_ASSERT(!bus_event_fusion_feed_dms(&yawn, &out), "YAWN no output");

    fusion_dms_event_t lost = make_dms(FUSION_DMS_FACE_LOST, 4000, 800);
    TEST_ASSERT(!bus_event_fusion_feed_dms(&lost, &out), "FACE_LOST no output");

    fusion_dms_event_t hd = make_dms(FUSION_DMS_HEAD_DOWN, 5000, 1500);
    TEST_ASSERT(!bus_event_fusion_feed_dms(&hd, &out),
                "HEAD_DOWN itself no output (only correlates later brake)");
}

/* ==================== main ==================== */

int main(void)
{
    printf("==============================================\n");
    printf(" bus_event_fusion unit tests\n");
    printf("==============================================\n");

    test_case1_single_hard_brake_unknown();
    test_case2_head_down_then_brake_800ms();
    test_case2_reversed_order_no_trigger();
    test_case2_confidence_decays_with_gap();
    test_case2_boundary();
    test_case3_fatigue_with_speed();
    test_case3_fatigue_with_motion_in_window();
    test_case3_no_motion_info_no_trigger();
    test_window_30s_exclusion();
    test_no_can_system_works();
    test_reset_clears_timeline();
    test_normal_dms_events_no_output();

    printf("\n==============================================\n");
    printf(" TOTAL: PASS=%d FAIL=%d\n", s_test_pass, s_test_fail);
    printf("==============================================\n");

    return (s_test_fail == 0) ? 0 : 1;
}
