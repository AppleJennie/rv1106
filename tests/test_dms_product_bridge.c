/*
 * test_dms_product_bridge.c - Product Bridge 胶水层端到端测试。
 *
 * 全程使用模拟 dms_result_t，不依赖任何 RV1106 硬件。
 *
 * 编译（PC 上）：
 *   gcc -Wall -Wextra -std=c11 -DDMS_HW_PREPROCESS=0 -I include \
 *       -o tests/test_dms_product_bridge \
 *       tests/test_dms_product_bridge.c \
 *       src/dms/dms_product_bridge.c src/dms/dms_risk_manager.c \
 *       src/dms/dms_event_logger.c src/dms/dms_alarm_policy.c \
 *       src/dms/dms_mcu_protocol.c
 */

#include "dms_product_bridge.h"
#include "dms_mcu_protocol.h"

#include <stdio.h>
#include <string.h>

/* log_* 桩（同其他单测的做法） */
void log_info(const char *fmt, ...)  { (void)fmt; }
void log_warn(const char *fmt, ...)  { (void)fmt; }
void log_error(const char *fmt, ...) { (void)fmt; }
void log_debug(const char *fmt, ...) { (void)fmt; }

static int s_pass = 0, s_fail = 0;

#define TEST(cond, name) do {                                   \
    if (cond) { s_pass++; printf("  PASS %s\n", name); }        \
    else      { s_fail++; printf("  FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); } \
} while (0)

#define TEST_EVENT_DIR "/tmp/dms_pb_test_events"
#define FRAME_STEP_MS  66   /* ~15 FPS */

static dms_result_t make_result(const char *status)
{
    dms_result_t r;
    memset(&r, 0, sizeof(r));
    r.face_found = (strcmp(status, "NO_FACE") != 0) ? 1 : 0;
    r.face_score = r.face_found ? 0.95f : 0.0f;
    snprintf(r.status, sizeof(r.status), "%s", status);
    r.ear = 0.28f; r.ear_baseline = 0.30f;
    r.mar = 0.20f; r.mar_baseline = 0.22f;
    r.head_down_score = 0.10f;
    return r;
}

/* 连续喂 frames 帧相同状态，返回最后一次输出 */
static dms_product_bridge_output_t feed(const char *status, int frames, uint64_t *now)
{
    dms_product_bridge_output_t out;
    memset(&out, 0, sizeof(out));
    dms_result_t r = make_result(status);
    for (int i = 0; i < frames; i++) {
        *now += FRAME_STEP_MS;
        out = dms_product_bridge_update(&r, *now);
    }
    return out;
}

static uint8_t s_pkt[128];

/* 从队列取一帧并解码 */
static bool pop_decoded(dms_mcu_frame_t *frame)
{
    size_t size = 0;
    if (!dms_product_bridge_get_mcu_packet(s_pkt, sizeof(s_pkt), &size)) return false;
    return dms_mcu_decode(s_pkt, size, frame);
}

static void drain_queue(void)
{
    size_t size;
    while (dms_product_bridge_get_mcu_packet(s_pkt, sizeof(s_pkt), &size)) {}
}

int main(void)
{
    printf("=== dms_product_bridge tests ===\n");

    TEST(dms_product_bridge_init_with_dir(TEST_EVENT_DIR), "init_with_dir");

    uint64_t now = 100000;   /* 任意起点 */

    /* ---------- 1. 正常驾驶 ---------- */
    dms_product_bridge_output_t out = feed("NORMAL", 30, &now);
    TEST(out.risk.level == DMS_RISK_NORMAL, "normal stays NORMAL");
    TEST(!out.event_logged, "normal not logged");
    TEST(dms_product_bridge_pending_packets() == 0, "no packet on normal");

    /* ---------- 2. 长闭眼 1.6s → WARNING + 记日志 + MCU 帧 ---------- */
    out = feed("LONG_EYE_CLOSED", 25, &now);   /* 25*66 ≈ 1.65s */
    TEST(out.risk.level == DMS_RISK_WARNING, "long eye closed 1.6s -> WARNING");
    TEST(strcmp(out.risk.event_type, "LONG_EYE_CLOSED") == 0, "event type string");
    TEST(out.risk.event_duration_ms >= 1500, "duration tracked >= 1500ms");
    TEST(out.alarm_level == ALARM_WARNING, "alarm WARNING");
    TEST(dms_product_bridge_pending_packets() >= 2, "packets: edge + level-up");

    dms_mcu_frame_t f;
    TEST(pop_decoded(&f), "packet decodes (crc ok)");
    TEST(f.event == DMS_MCU_EVENT_LONG_EYE_CLOSED, "mcu event code 0x11");
    TEST(f.risk_level == DMS_RISK_ATTENTION || f.risk_level == DMS_RISK_WARNING,
         "mcu risk level sane");
    drain_queue();

    /* ---------- 3. 恢复后再长闭眼（60s 内第 2 次）→ HIGH ---------- */
    feed("NORMAL", 50, &now);          /* 正常 ~3.3s（仍在恢复滞后内，保持 WARNING） */
    out = feed("LONG_EYE_CLOSED", 25, &now);
    TEST(out.risk.level == DMS_RISK_HIGH, "2nd long eye closed in 60s -> HIGH");
    TEST(out.alarm_level == ALARM_HIGH || out.alarm_level == ALARM_NONE,
         "alarm HIGH (or cooling down)");
    drain_queue();

    /* ---------- 4. 恢复滞后 + 滑窗老化：HIGH 后立刻回 NORMAL 不立刻降级 ---------- */
    out = feed("NORMAL", 10, &now);
    TEST(out.risk.level == DMS_RISK_HIGH, "hysteresis: still HIGH right after");
    out = feed("NORMAL", 1000, &now);  /* ~66s：60s 滑窗内事件老化 + 5s 恢复滞后 */
    TEST(out.risk.level == DMS_RISK_NORMAL, "drops to NORMAL after window ages out");
    drain_queue();

    /* ---------- 5. 60s 内 3 次哈欠 → WARNING ---------- */
    for (int k = 0; k < 3; k++) {
        feed("YAWN", 15, &now);        /* 每次哈欠 ~1s */
        feed("NORMAL", 100, &now);     /* 间隔 ~6.6s */
    }
    out = feed("NORMAL", 1, &now);
    TEST(out.risk.level >= DMS_RISK_ATTENTION, "3 yawns in 60s -> ATTENTION/WARNING");
    drain_queue();

    /* ---------- 6. 心跳帧 ---------- */
    TEST(dms_product_bridge_queue_heartbeat(true, true, (uint32_t)(now / 1000)),
         "queue heartbeat");
    TEST(pop_decoded(&f), "heartbeat decodes");
    TEST(f.event == DMS_MCU_EVENT_HEARTBEAT, "heartbeat event code 0x01");

    /* ---------- 7. 缓冲区太小不出队 ---------- */
    dms_product_bridge_queue_heartbeat(true, true, (uint32_t)(now / 1000));
    size_t pending_before = dms_product_bridge_pending_packets();
    size_t size = 0;
    uint8_t tiny[4];
    TEST(!dms_product_bridge_get_mcu_packet(tiny, sizeof(tiny), &size),
         "tiny buffer rejected");
    TEST(dms_product_bridge_pending_packets() == pending_before,
         "queue unchanged after reject");
    drain_queue();

    /* ---------- 8. 事件 CSV 落盘内容 ---------- */
    dms_event_logger_flush();
    char csv_path[600];
    snprintf(csv_path, sizeof(csv_path), "%s/events.csv", TEST_EVENT_DIR);
    FILE *fp = fopen(csv_path, "r");
    TEST(fp != NULL, "events.csv exists");
    if (fp) {
        char content[8192];
        size_t n = fread(content, 1, sizeof(content) - 1, fp);
        content[n] = '\0';
        fclose(fp);
        TEST(strstr(content, "LONG_EYE_CLOSED") != NULL, "csv has LONG_EYE_CLOSED");
        TEST(strstr(content, "YAWN") != NULL, "csv has YAWN");
        TEST(strstr(content, ",NORMAL,") == NULL, "csv has no NORMAL rows");
    }

    /* ---------- 9. reset ---------- */
    dms_product_bridge_queue_heartbeat(true, true, (uint32_t)(now / 1000));
    dms_product_bridge_reset();
    TEST(dms_product_bridge_pending_packets() == 0, "queue empty after reset");
    TEST(dms_risk_manager_get_level() == DMS_RISK_NORMAL, "level NORMAL after reset");

    dms_product_bridge_deinit();

    printf("=== Results: %d passed, %d failed ===\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
