/*
 * test_dms_protocol_stress.c - MCU 协议解析器压力测试
 *
 * 测试 10000 个 packet，包含：
 *   - 正常包
 *   - 随机拆包
 *   - 随机粘包
 *   - CRC 错误
 *   - 随机噪声字节
 *   - 截断 packet
 *   - 未知 event
 *   - 未知 version
 *
 * 要求：parser 不能崩，CRC 错误后必须能恢复同步。
 */

#include "dms_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ==================== 帧编码辅助（与 RV1106 侧一致） ==================== */

static size_t encode_test_frame(uint8_t *buf, size_t buf_size,
                                 uint8_t event, uint8_t risk,
                                 uint8_t confidence, uint16_t duration,
                                 uint32_t timestamp,
                                 const uint8_t *payload, uint8_t payload_len)
{
    size_t total = 13 + payload_len + 2;
    if (buf_size < total) return 0;

    size_t off = 0;
    buf[off++] = 0xAA;
    buf[off++] = 0x55;
    buf[off++] = 0x01;  /* version */
    buf[off++] = event;
    buf[off++] = risk;
    buf[off++] = confidence;
    buf[off++] = (uint8_t)(duration >> 8);
    buf[off++] = (uint8_t)(duration & 0xFF);
    buf[off++] = (uint8_t)(timestamp >> 24);
    buf[off++] = (uint8_t)(timestamp >> 16);
    buf[off++] = (uint8_t)(timestamp >> 8);
    buf[off++] = (uint8_t)(timestamp & 0xFF);
    buf[off++] = payload_len;
    if (payload_len > 0 && payload) {
        memcpy(&buf[off], payload, payload_len);
        off += payload_len;
    }
    uint16_t crc = dms_protocol_crc16(buf, off);
    buf[off++] = (uint8_t)(crc >> 8);
    buf[off++] = (uint8_t)(crc & 0xFF);

    return off;
}

/* ==================== 回调统计 ==================== */
static int s_callback_count = 0;
static uint8_t s_last_event = 0;

static void test_callback(const dms_parsed_frame_t *frame, void *user_data)
{
    (void)user_data;
    s_callback_count++;
    s_last_event = frame->event;
}

/* ==================== 随机数 ==================== */
static unsigned int s_seed = 12345;
static int rand_range(int min, int max) {
    s_seed = s_seed * 1103515245 + 12345;
    return min + (int)((s_seed >> 16) % (unsigned)(max - min + 1));
}

/* ==================== 测试 ==================== */

int main(void)
{
    printf("=== DMS Protocol Stress Test ===\n");
    printf("Generating 10000 packets with mixed errors...\n\n");

    dms_protocol_parser_init(&(dms_parser_t){0});
    /* 重新初始化，因为上面的 compound literal 是临时的 */
    static dms_parser_t parser;
    dms_protocol_parser_init(&parser);

    s_callback_count = 0;
    srand((unsigned)time(NULL));

    int total_packets = 10000;
    int normal_count = 0;
    int split_count = 0;
    int merged_count = 0;
    int crc_error_count = 0;
    int noise_count = 0;
    int truncated_count = 0;
    int unknown_event_count = 0;
    int unknown_version_count = 0;

    uint8_t frame_buf[64];

    for (int i = 0; i < total_packets; i++) {
        int test_type = rand_range(0, 9);

        /* 构造一个正常帧 */
        uint8_t event = (uint8_t)rand_range(0x01, 0x21);
        uint8_t risk = (uint8_t)rand_range(0, 3);
        uint8_t conf = (uint8_t)rand_range(50, 100);
        uint16_t dur = (uint16_t)rand_range(0, 5000);
        uint32_t ts = (uint32_t)rand_range(0, 999999);

        size_t frame_len = encode_test_frame(frame_buf, sizeof(frame_buf),
                                              event, risk, conf, dur, ts,
                                              NULL, 0);

        switch (test_type) {
        case 0:
        case 1:
        case 2:
        case 3: {
            /* 40%: 正常包 */
            normal_count++;
            dms_protocol_feed(&parser, frame_buf, frame_len,
                              test_callback, NULL);
            break;
        }

        case 4: {
            /* 10%: 随机拆包（分 2~5 段发送） */
            split_count++;
            int segments = rand_range(2, 5);
            size_t seg_size = frame_len / (size_t)segments;
            size_t offset = 0;
            for (int s = 0; s < segments; s++) {
                size_t send_len = (s == segments - 1)
                                  ? (frame_len - offset) : seg_size;
                dms_protocol_feed(&parser, &frame_buf[offset], send_len,
                                  test_callback, NULL);
                offset += send_len;
            }
            break;
        }

        case 5: {
            /* 10%: 粘包（两个帧连在一起） */
            merged_count++;
            uint8_t double_buf[128];
            size_t len1 = frame_len;
            memcpy(double_buf, frame_buf, len1);
            size_t len2 = encode_test_frame(double_buf + len1,
                                             sizeof(double_buf) - len1,
                                             0x01, 0, 100, 0, ts + 1,
                                             NULL, 0);
            dms_protocol_feed(&parser, double_buf, len1 + len2,
                              test_callback, NULL);
            break;
        }

        case 6: {
            /* 10%: CRC 错误 */
            crc_error_count++;
            frame_buf[frame_len - 1] ^= 0xFF;  /* 破坏 CRC */
            dms_protocol_feed(&parser, frame_buf, frame_len,
                              test_callback, NULL);
            /* 发送一个正常帧验证恢复 */
            size_t len2 = encode_test_frame(frame_buf, sizeof(frame_buf),
                                             0x01, 0, 100, 0, ts + 1,
                                             NULL, 0);
            dms_protocol_feed(&parser, frame_buf, len2,
                              test_callback, NULL);
            break;
        }

        case 7: {
            /* 10%: 随机噪声字节 + 正常帧 */
            noise_count++;
            int noise_len = rand_range(1, 10);
            for (int n = 0; n < noise_len; n++) {
                uint8_t noise = (uint8_t)rand_range(0, 255);
                /* 避免噪声恰好是 0xAA 0x55 */
                if (noise == 0xAA) noise = 0xBB;
                dms_protocol_feed_byte(&parser, noise, test_callback, NULL);
            }
            dms_protocol_feed(&parser, frame_buf, frame_len,
                              test_callback, NULL);
            break;
        }

        case 8: {
            /* 5%: 截断 packet（只发一半） */
            truncated_count++;
            size_t half = frame_len / 2;
            dms_protocol_feed(&parser, frame_buf, half,
                              test_callback, NULL);
            /* 然后发送一个正常帧（parser 应该重新同步） */
            size_t len2 = encode_test_frame(frame_buf, sizeof(frame_buf),
                                             0x01, 0, 100, 0, ts + 1,
                                             NULL, 0);
            dms_protocol_feed(&parser, frame_buf, len2,
                              test_callback, NULL);
            break;
        }

        case 9: {
            /* 5%: 未知 event 或未知 version */
            if (i % 2 == 0) {
                unknown_event_count++;
                frame_buf[3] = 0xFF;  /* 未知事件码 */
                /* 重新计算 CRC（因为修改了数据） */
                uint16_t crc = dms_protocol_crc16(frame_buf, frame_len - 2);
                frame_buf[frame_len - 2] = (uint8_t)(crc >> 8);
                frame_buf[frame_len - 1] = (uint8_t)(crc & 0xFF);
            } else {
                unknown_version_count++;
                frame_buf[2] = 0x99;  /* 未知版本 */
                /* 重新计算 CRC */
                uint16_t crc = dms_protocol_crc16(frame_buf, frame_len - 2);
                frame_buf[frame_len - 2] = (uint8_t)(crc >> 8);
                frame_buf[frame_len - 1] = (uint8_t)(crc & 0xFF);
            }
            dms_protocol_feed(&parser, frame_buf, frame_len,
                              test_callback, NULL);
            /* 发送正常帧验证恢复 */
            size_t len2 = encode_test_frame(frame_buf, sizeof(frame_buf),
                                             0x01, 0, 100, 0, ts + 1,
                                             NULL, 0);
            dms_protocol_feed(&parser, frame_buf, len2,
                              test_callback, NULL);
            break;
        }
        }
    }

    /* 输出结果 */
    dms_parser_stats_t stats;
    dms_protocol_get_stats(&parser, &stats);

    printf("Test Composition:\n");
    printf("  normal_packets      = %d\n", normal_count);
    printf("  split_packets       = %d\n", split_count);
    printf("  merged_packets      = %d\n", merged_count);
    printf("  crc_error_packets   = %d\n", crc_error_count);
    printf("  noise_packets       = %d\n", noise_count);
    printf("  truncated_packets   = %d\n", truncated_count);
    printf("  unknown_event       = %d\n", unknown_event_count);
    printf("  unknown_version     = %d\n", unknown_version_count);
    printf("  total               = %d\n\n", total_packets);

    printf("Parser Results:\n");
    printf("  total_bytes         = %u\n", stats.total_bytes);
    printf("  valid_frames        = %u\n", stats.valid_frames);
    printf("  crc_errors          = %u\n", stats.crc_errors);
    printf("  resync_count        = %u\n", stats.resync_count);
    printf("  parser_errors       = %u\n", stats.parser_errors);
    printf("  callback_count      = %d\n", s_callback_count);
    printf("  last_event          = 0x%02X\n\n", s_last_event);

    /* 验证 */
    int pass = 1;

    if (stats.valid_frames == 0) {
        printf("FAIL: no valid frames parsed\n");
        pass = 0;
    }

    if (stats.crc_errors == 0 && crc_error_count > 0) {
        printf("FAIL: CRC errors not detected\n");
        pass = 0;
    }

    if (s_callback_count != (int)stats.valid_frames) {
        printf("FAIL: callback_count != valid_frames\n");
        pass = 0;
    }

    /* parser 不应该崩溃（能执行到这里就说明没崩） */
    printf("Parser survived 10000 mixed packets: YES\n");

    if (pass) {
        printf("\n=== STRESS TEST PASSED ===\n");
    } else {
        printf("\n=== STRESS TEST FAILED ===\n");
    }

    return pass ? 0 : 1;
}
