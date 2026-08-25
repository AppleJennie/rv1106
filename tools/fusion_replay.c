/*
 * fusion_replay.c - 公交事件融合引擎离线回放 CLI（PC 主机运行，无硬件依赖）。
 *
 * 从 stdin 或文件读取事件流，驱动 bus_event_fusion，打印触发的 Bus Safety Event。
 *
 * 输入 CSV（# 开头注释，首行表头自动跳过），同一时间基准（毫秒）：
 *   t_ms,DMS,<事件>,<风险等级>,<持续ms>
 *   t_ms,MOTION,<事件>,<置信度0~100>,<纵向加速度>,<横向加速度>
 *   t_ms,SPEED,<车速m/s>
 * DMS 事件：EYE_CLOSED LONG_EYE_CLOSED YAWN HEAD_DOWN FACE_LOST
 * 风险等级：NORMAL ATTENTION WARNING HIGH
 * MOTION 事件：HARD_ACCEL HARD_BRAKE HARD_TURN_LEFT HARD_TURN_RIGHT BUMP
 *              HIGH_LONG_JERK HIGH_LAT_JERK
 *
 * 用法：./fusion_replay [input.csv]
 * 编译：gcc -Wall -Wextra -std=c11 -I bus_event_fusion/include \
 *       -o tools/fusion_replay tools/fusion_replay.c bus_event_fusion/src/bus_event_fusion.c
 */

#include "bus_event_fusion.h"

#include <stdio.h>
#include <string.h>

/* DMS 事件类型字符串 → 枚举（NO_FACE 映射为 FACE_LOST，与 RV1106 侧一致） */
static int parse_dms_type(const char *s)
{
    if (!strcmp(s, "EYE_CLOSED"))      return FUSION_DMS_EYE_CLOSED;
    if (!strcmp(s, "LONG_EYE_CLOSED")) return FUSION_DMS_LONG_EYE_CLOSED;
    if (!strcmp(s, "YAWN"))            return FUSION_DMS_YAWN;
    if (!strcmp(s, "HEAD_DOWN"))       return FUSION_DMS_HEAD_DOWN;
    if (!strcmp(s, "FACE_LOST") || !strcmp(s, "NO_FACE")) return FUSION_DMS_FACE_LOST;
    return -1;
}

static int parse_risk(const char *s)
{
    if (!strcmp(s, "NORMAL"))    return FUSION_RISK_NORMAL;
    if (!strcmp(s, "ATTENTION")) return FUSION_RISK_ATTENTION;
    if (!strcmp(s, "WARNING"))   return FUSION_RISK_WARNING;
    if (!strcmp(s, "HIGH"))      return FUSION_RISK_HIGH;
    return -1;
}

static int parse_motion_type(const char *s)
{
    if (!strcmp(s, "HARD_ACCEL"))      return FUSION_MOTION_HARD_ACCEL;
    if (!strcmp(s, "HARD_BRAKE"))      return FUSION_MOTION_HARD_BRAKE;
    if (!strcmp(s, "HARD_TURN_LEFT"))  return FUSION_MOTION_HARD_TURN_LEFT;
    if (!strcmp(s, "HARD_TURN_RIGHT")) return FUSION_MOTION_HARD_TURN_RIGHT;
    if (!strcmp(s, "BUMP"))            return FUSION_MOTION_BUMP;
    if (!strcmp(s, "HIGH_LONG_JERK"))  return FUSION_MOTION_HIGH_LONG_JERK;
    if (!strcmp(s, "HIGH_LAT_JERK"))   return FUSION_MOTION_HIGH_LAT_JERK;
    return -1;
}

static void print_event(const bus_safety_event_t *ev)
{
    printf("t=%llu BUS_EVENT=%s attribution=%s conf=%d review=%d\n",
           (unsigned long long)ev->timestamp_ms,
           bus_safety_event_type_to_string(ev->event_type),
           bus_attribution_to_string(ev->attribution),
           ev->correlation_confidence,
           ev->human_review_required ? 1 : 0);
    printf("  message: %s\n", ev->message);
    printf("  evidence: %s\n", ev->evidence);
}

int main(int argc, char **argv)
{
    FILE *fp = stdin;
    if (argc > 1 && !(fp = fopen(argv[1], "r"))) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    if (!bus_event_fusion_init()) {
        fprintf(stderr, "fusion init failed\n");
        return 2;
    }

    char line[256];
    long n_in = 0, n_out = 0, n_bad = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        char src[16], a1[32], a2[32];
        uint64_t t_ms;
        /* 先解析公共前缀 t_ms,source */
        if (sscanf(line, "%llu,%15[^,]", (unsigned long long *)&t_ms, src) < 2) {
            continue;  /* 表头或坏行 */
        }

        bus_safety_event_t out;
        memset(&out, 0, sizeof(out));
        int produced = 0;

        if (!strcmp(src, "DMS")) {
            uint64_t dur = 0;
            if (sscanf(line, "%*u,%*[^,],%31[^,],%31[^,],%llu", a1, a2,
                       (unsigned long long *)&dur) < 2) { n_bad++; continue; }
            int type = parse_dms_type(a1), risk = parse_risk(a2);
            if (type < 0 || risk < 0) { n_bad++; continue; }
            fusion_dms_event_t ev = { type, risk, t_ms, dur };
            produced = bus_event_fusion_feed_dms(&ev, &out);
        } else if (!strcmp(src, "MOTION")) {
            int conf = 0;
            float lon = 0.0f, lat = 0.0f;
            if (sscanf(line, "%*u,%*[^,],%31[^,],%d,%f,%f", a1, &conf, &lon, &lat) < 2) {
                n_bad++; continue;
            }
            int type = parse_motion_type(a1);
            if (type < 0) { n_bad++; continue; }
            fusion_motion_event_t ev = { type, t_ms, conf, lon, lat };
            produced = bus_event_fusion_feed_motion(&ev, &out);
        } else if (!strcmp(src, "SPEED")) {
            float mps = 0.0f;
            if (sscanf(line, "%*u,%*[^,],%f", &mps) < 1) { n_bad++; continue; }
            bus_event_fusion_update_speed(mps, t_ms);
        } else {
            n_bad++;
            continue;
        }

        n_in++;
        if (produced) {
            print_event(&out);
            n_out++;
        }
    }

    if (argc > 1) fclose(fp);

    fprintf(stderr, "---\nsummary: inputs=%ld fused_events=%ld bad_lines=%ld\n",
            n_in, n_out, n_bad);
    return 0;
}
