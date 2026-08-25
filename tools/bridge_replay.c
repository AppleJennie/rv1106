/*
 * bridge_replay.c - Product Bridge 离线回放 CLI（PC 主机运行，无硬件依赖）。
 *
 * 从 stdin 或文件读取模拟 AI 结果流，逐条驱动 dms_product_bridge，
 * 打印每条输入对应的风险等级/报警/日志/ MCU 帧（hex）。
 *
 * 输入 CSV 格式（首行可为表头，自动跳过；# 开头为注释）：
 *   t_ms,status,face_score,ear,mar,head_down_score
 *   100000,NORMAL,0.95,0.28,0.20,0.10
 *   100066,LONG_EYE_CLOSED,0.95,0.10,0.20,0.10
 * status 取值：NORMAL / EYE_CLOSED / LONG_EYE_CLOSED / YAWN / HEAD_DOWN / NO_FACE
 *
 * 用法：
 *   ./bridge_replay [input.csv] [--event-dir DIR] [--no-heartbeat]
 *
 * 编译：
 *   gcc -Wall -Wextra -std=c11 -DDMS_HW_PREPROCESS=0 -I include \
 *       -o tools/bridge_replay tools/bridge_replay.c \
 *       src/dms/dms_product_bridge.c src/dms/dms_risk_manager.c \
 *       src/dms/dms_event_logger.c src/dms/dms_alarm_policy.c \
 *       src/dms/dms_mcu_protocol.c
 */

#include "dms_product_bridge.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* log_* 桩 */
void log_info(const char *fmt, ...)  { (void)fmt; }
void log_warn(const char *fmt, ...)  { (void)fmt; }
void log_error(const char *fmt, ...) { (void)fmt; }
void log_debug(const char *fmt, ...) { (void)fmt; }

static void drain_and_print(void)
{
    uint8_t buf[128];
    size_t size;
    while (dms_product_bridge_get_mcu_packet(buf, sizeof(buf), &size)) {
        printf("  MCU> ");
        for (size_t i = 0; i < size; i++) printf("%02X", buf[i]);
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *event_dir  = "/tmp/dms_replay_events";
    int heartbeat = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--event-dir") == 0 && i + 1 < argc) {
            event_dir = argv[++i];
        } else if (strcmp(argv[i], "--no-heartbeat") == 0) {
            heartbeat = 0;
        } else if (argv[i][0] != '-') {
            input_path = argv[i];
        } else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return 2;
        }
    }

    FILE *fp = stdin;
    if (input_path && !(fp = fopen(input_path, "r"))) {
        fprintf(stderr, "cannot open %s\n", input_path);
        return 2;
    }

    if (!dms_product_bridge_init_with_dir(event_dir)) {
        fprintf(stderr, "bridge init failed\n");
        return 2;
    }

    char line[256];
    uint64_t last_hb_ms = 0;
    long n_lines = 0, n_logged = 0, n_queued = 0;
    int max_level = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        uint64_t t_ms;
        char status[32];
        float face_score, ear, mar, head_down_score;
        int n = sscanf(line, "%llu,%31[^,],%f,%f,%f,%f",
                       (unsigned long long *)&t_ms, status,
                       &face_score, &ear, &mar, &head_down_score);
        if (n < 2) continue;   /* 表头或坏行 */

        dms_result_t r;
        memset(&r, 0, sizeof(r));
        r.face_found = (strcmp(status, "NO_FACE") != 0) ? 1 : 0;
        r.face_score = (n >= 3) ? face_score : (r.face_found ? 0.95f : 0.0f);
        r.ear  = (n >= 4) ? ear  : 0.28f;
        r.mar  = (n >= 5) ? mar  : 0.20f;
        r.head_down_score = (n >= 6) ? head_down_score : 0.10f;
        snprintf(r.status, sizeof(r.status), "%s", status);

        /* 1Hz 心跳 */
        if (heartbeat && (last_hb_ms == 0 || t_ms - last_hb_ms >= 1000)) {
            dms_product_bridge_queue_heartbeat(1, 1, (uint32_t)(t_ms / 1000));
            last_hb_ms = t_ms;
        }

        dms_product_bridge_output_t out = dms_product_bridge_update(&r, t_ms);

        printf("t=%llu status=%-16s risk=%-9s score=%3d alarm=%-12s logged=%d queued=%d\n",
               (unsigned long long)t_ms, status,
               dms_risk_level_to_string(out.risk.level),
               out.risk.risk_score,
               dms_alarm_level_to_string(out.alarm_level),
               out.event_logged ? 1 : 0,
               out.mcu_event_queued ? 1 : 0);
        drain_and_print();

        n_lines++;
        if (out.event_logged) n_logged++;
        if (out.mcu_event_queued) n_queued++;
        if ((int)out.risk.level > max_level) max_level = (int)out.risk.level;
    }

    if (input_path) fclose(fp);

    fprintf(stderr, "---\nsummary: frames=%ld events_logged=%ld mcu_events=%ld "
            "max_risk=%s dropped_pkts=%u event_csv=%s/events.csv\n",
            n_lines, n_logged, n_queued,
            dms_risk_level_to_string((dms_risk_level_t)max_level),
            dms_product_bridge_dropped_packets(), event_dir);

    dms_product_bridge_deinit();
    return 0;
}
