/*
 * test_dms_risk_manager.c - Risk Manager 单元测试
 *
 * 测试场景：
 *   1. 正常状态 → NORMAL
 *   2. 一次哈欠 → 不得 HIGH
 *   3. 普通眨眼 → NORMAL
 *   4. 长闭眼 → WARNING
 *   5. 短时间重复长闭眼 → HIGH
 *   6. 持续低头 → WARNING
 *   7. 恢复正常 → 有滞后后回 NORMAL
 */

#include "dms_risk_manager.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* 简单的日志 stub（单元测试不需要完整日志系统） */
void log_info(const char *fmt, ...) { (void)fmt; }
void log_warn(const char *fmt, ...) { (void)fmt; }
void log_error(const char *fmt, ...) { (void)fmt; }
void log_debug(const char *fmt, ...) { (void)fmt; }

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

/* 构造基础输入 */
static dms_risk_input_t make_input(uint64_t ts_ms)
{
    dms_risk_input_t input;
    memset(&input, 0, sizeof(input));
    input.face_found = 1;
    input.ear = 0.30f;
    input.mar = 0.25f;
    input.head_down_score = 0.05f;
    input.timestamp_ms = ts_ms;
    return input;
}

/* ==================== 测试用例 ==================== */

/* 测试 1：正常状态 → NORMAL */
static void test_normal_state(void)
{
    printf("\n--- Test: Normal State ---\n");

    dms_risk_manager_reset();

    dms_risk_input_t input = make_input(1000);
    dms_risk_result_t result = dms_risk_manager_update(&input);

    TEST_ASSERT(result.level == DMS_RISK_NORMAL, "normal state is NORMAL");
    TEST_ASSERT(result.risk_score == 0, "normal state score is 0");
    TEST_ASSERT(result.alarm_requested == false, "normal state no alarm");
    TEST_ASSERT(result.save_event_requested == false, "normal state no save");
}

/* 测试 2：一次哈欠 → 不得 HIGH */
static void test_single_yawn_not_high(void)
{
    printf("\n--- Test: Single Yawn Not HIGH ---\n");

    dms_risk_manager_reset();

    /* 模拟哈欠开始 */
    dms_risk_input_t input = make_input(1000);
    input.yawn = 1;
    input.yawn_duration_ms = 1200;
    input.mar = 0.55f;

    dms_risk_result_t result = dms_risk_manager_update(&input);

    TEST_ASSERT(result.level != DMS_RISK_HIGH, "single yawn is not HIGH");
    TEST_ASSERT(result.level == DMS_RISK_ATTENTION || result.level == DMS_RISK_NORMAL,
                "single yawn is ATTENTION or NORMAL");

    /* 哈欠结束 */
    input = make_input(2500);
    result = dms_risk_manager_update(&input);

    /* 继续正常状态，哈欠事件在窗口内 */
    input = make_input(3000);
    result = dms_risk_manager_update(&input);

    /* 一次哈欠可能是 ATTENTION 或 NORMAL，但绝不能是 HIGH */
    TEST_ASSERT(result.level != DMS_RISK_HIGH, "after single yawn, still not HIGH");
}

/* 测试 3：普通眨眼 → NORMAL */
static void test_normal_blink(void)
{
    printf("\n--- Test: Normal Blink ---\n");

    dms_risk_manager_reset();

    /* 模拟短暂闭眼（正常眨眼，< 800ms） */
    dms_risk_input_t input = make_input(1000);
    input.eye_closed = 1;
    input.eye_closed_duration_ms = 200;  /* 200ms 眨眼 */
    input.ear = 0.15f;

    dms_risk_result_t result = dms_risk_manager_update(&input);

    /* 普通眨眼可能是 ATTENTION 或 NORMAL，但不应该是 WARNING/HIGH */
    TEST_ASSERT(result.level < DMS_RISK_WARNING, "normal blink is not WARNING+");

    /* 眨眼结束 */
    input = make_input(1300);
    result = dms_risk_manager_update(&input);

    /* 眨眼等级不高于 ATTENTION，经过一段时间 + 恢复滞后后应回 NORMAL */
    /* 从事件结束(1300ms)起算，需要过 recovery_hold_ms(5000ms)，即 6300ms 后 */
    input = make_input(7000);
    result = dms_risk_manager_update(&input);
    TEST_ASSERT(result.level == DMS_RISK_NORMAL, "after blink ends + hold time, back to NORMAL");
}

/* 测试 4：长闭眼 → WARNING */
static void test_long_eye_closed_warning(void)
{
    printf("\n--- Test: Long Eye Closed → WARNING ---\n");

    dms_risk_manager_reset();

    /* 模拟长闭眼（>= 1500ms） */
    dms_risk_input_t input = make_input(1000);
    input.eye_closed = 1;
    input.long_eye_closed = 1;
    input.long_eye_closed_duration_ms = 1600;
    input.ear = 0.10f;

    dms_risk_result_t result = dms_risk_manager_update(&input);

    TEST_ASSERT(result.level == DMS_RISK_WARNING, "long eye closed is WARNING");
    TEST_ASSERT(result.alarm_requested == true, "long eye closed requests alarm");
    TEST_ASSERT(result.save_event_requested == true, "long eye closed requests save");
    TEST_ASSERT(strcmp(result.event_type, "LONG_EYE_CLOSED") == 0,
                "event type is LONG_EYE_CLOSED");
}

/* 测试 5：短时间重复长闭眼 → HIGH */
static void test_repeated_long_eye_closed_high(void)
{
    printf("\n--- Test: Repeated Long Eye Closed → HIGH ---\n");

    dms_risk_manager_reset();

    uint64_t ts = 1000;

    /* 第一次长闭眼 */
    dms_risk_input_t input = make_input(ts);
    input.eye_closed = 1;
    input.long_eye_closed = 1;
    input.long_eye_closed_duration_ms = 1600;
    dms_risk_result_t result = dms_risk_manager_update(&input);
    TEST_ASSERT(result.level == DMS_RISK_WARNING, "first long eye closed is WARNING");

    /* 结束 */
    ts += 2000;
    input = make_input(ts);
    result = dms_risk_manager_update(&input);

    /* 恢复正常一段时间（但不足以降级） */
    ts += 1000;
    input = make_input(ts);
    result = dms_risk_manager_update(&input);

    /* 第二次长闭眼（在 60s 窗口内） */
    ts += 2000;
    input = make_input(ts);
    input.eye_closed = 1;
    input.long_eye_closed = 1;
    input.long_eye_closed_duration_ms = 1600;
    result = dms_risk_manager_update(&input);

    TEST_ASSERT(result.level == DMS_RISK_HIGH, "repeated long eye closed is HIGH");
    TEST_ASSERT(result.alarm_requested == true, "HIGH level requests alarm");
}

/* 测试 6：持续低头 → WARNING */
static void test_head_down_warning(void)
{
    printf("\n--- Test: Head Down → WARNING ---\n");

    dms_risk_manager_reset();

    /* 模拟持续低头（>= 3000ms） */
    dms_risk_input_t input = make_input(1000);
    input.head_down = 1;
    input.head_down_duration_ms = 3200;
    input.head_down_score = 0.35f;

    dms_risk_result_t result = dms_risk_manager_update(&input);

    TEST_ASSERT(result.level == DMS_RISK_WARNING, "sustained head down is WARNING");
    TEST_ASSERT(strcmp(result.event_type, "HEAD_DOWN") == 0,
                "event type is HEAD_DOWN");
}

/* 测试 7：恢复正常 → 有滞后后回 NORMAL */
static void test_recovery_hysteresis(void)
{
    printf("\n--- Test: Recovery Hysteresis ---\n");

    dms_risk_manager_reset();

    /* 先触发 WARNING */
    dms_risk_input_t input = make_input(1000);
    input.eye_closed = 1;
    input.long_eye_closed = 1;
    input.long_eye_closed_duration_ms = 1600;
    dms_risk_result_t result = dms_risk_manager_update(&input);
    TEST_ASSERT(result.level == DMS_RISK_WARNING, "setup: WARNING level");

    /* 事件结束 */
    input = make_input(3000);
    result = dms_risk_manager_update(&input);

    /* 立即恢复正常 → 应该还是 WARNING（滞后） */
    input = make_input(3500);
    result = dms_risk_manager_update(&input);
    TEST_ASSERT(result.level == DMS_RISK_WARNING,
                "immediately after event, still WARNING (hysteresis)");

    /* 过了 recovery_hold_ms (5000ms) 后 → 应该降级 */
    /* 从 recovery 开始（3000ms 时事件结束，in_recovery 从 3500ms 开始）算起，到 3500+5000=8500ms */
    /* 但注意：长闭眼事件在窗口中，60s 窗口内 1 次长闭眼不会触发 HIGH，
     * 但 determine_risk_level 中 input->long_eye_closed=0 时不会匹配 WARNING 条件，
     * 窗口内 1 次长闭眼也不满足 HIGH 条件（需要 2 次），所以 raw_level 应该是 NORMAL */
    input = make_input(9000);
    result = dms_risk_manager_update(&input);
    TEST_ASSERT(result.level < DMS_RISK_WARNING,
                "after recovery hold time, level drops below WARNING");
}

/* 测试 8：多次哈欠 → WARNING */
static void test_multiple_yawns_warning(void)
{
    printf("\n--- Test: Multiple Yawns → WARNING ---\n");

    dms_risk_manager_reset();

    uint64_t ts = 1000;

    /* 3 次哈欠，每次间隔 5 秒 */
    for (int i = 0; i < 3; i++) {
        /* 哈欠开始 */
        dms_risk_input_t input = make_input(ts);
        input.yawn = 1;
        input.yawn_duration_ms = 1200;
        input.mar = 0.55f;
        dms_risk_manager_update(&input);

        /* 哈欠结束 */
        ts += 1500;
        input = make_input(ts);
        dms_risk_manager_update(&input);

        ts += 3500;  /* 间隔 */
    }

    /* 检查当前状态 */
    dms_risk_input_t input = make_input(ts);
    dms_risk_result_t result = dms_risk_manager_update(&input);

    TEST_ASSERT(result.level >= DMS_RISK_WARNING,
                "3 yawns in 60s window is WARNING+");
}

/* 测试 9：无脸状态 */
static void test_face_lost(void)
{
    printf("\n--- Test: Face Lost ---\n");

    dms_risk_manager_reset();

    dms_risk_input_t input = make_input(1000);
    input.face_found = 0;

    dms_risk_result_t result = dms_risk_manager_update(&input);

    /* 无脸不应该是 HIGH */
    TEST_ASSERT(result.level != DMS_RISK_HIGH, "face lost is not HIGH");
}

/* 测试 10：组合事件 */
static void test_combo_events(void)
{
    printf("\n--- Test: Combo Events ---\n");

    dms_risk_manager_reset();

    uint64_t ts = 1000;

    /* 先哈欠 */
    dms_risk_input_t input = make_input(ts);
    input.yawn = 1;
    input.yawn_duration_ms = 1200;
    dms_risk_manager_update(&input);

    ts += 1500;
    input = make_input(ts);
    dms_risk_manager_update(&input);

    /* 再低头 */
    ts += 2000;
    input = make_input(ts);
    input.head_down = 1;
    input.head_down_duration_ms = 500;
    dms_risk_manager_update(&input);

    ts += 1000;
    input = make_input(ts);
    dms_risk_manager_update(&input);

    /* 组合事件应该提升风险等级 */
    dms_risk_result_t result = dms_risk_manager_update(&input);
    TEST_ASSERT(result.level >= DMS_RISK_ATTENTION,
                "combo events at least ATTENTION");
}

/* ==================== 主函数 ==================== */

int main(void)
{
    printf("=== DMS Risk Manager Unit Tests ===\n");

    /* 初始化 */
    bool ok = dms_risk_manager_init();
    if (!ok) {
        printf("Failed to init risk manager\n");
        return 1;
    }

    /* 运行测试 */
    test_normal_state();
    test_single_yawn_not_high();
    test_normal_blink();
    test_long_eye_closed_warning();
    test_repeated_long_eye_closed_high();
    test_head_down_warning();
    test_recovery_hysteresis();
    test_multiple_yawns_warning();
    test_face_lost();
    test_combo_events();

    /* 清理 */
    dms_risk_manager_deinit();

    /* 汇总 */
    printf("\n=== Results: %d passed, %d failed ===\n", s_test_pass, s_test_fail);
    return (s_test_fail > 0) ? 1 : 0;
}
