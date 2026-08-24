/*
 * test_dms_mcu_protocol.c - MCU Protocol 单元测试
 *
 * 测试场景：
 *   1. 编码/解码一致性
 *   2. 心跳帧编码/解码
 *   3. CRC 错误检测
 *   4. Header 错误检测
 *   5. 边界条件
 */

#include "dms_mcu_protocol.h"
#include <stdio.h>
#include <string.h>

/* 简单的日志 stub */
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

/* ==================== 测试用例 ==================== */

/* 测试 1：基本编码/解码一致性 */
static void test_encode_decode_basic(void)
{
    printf("\n--- Test: Basic Encode/Decode ---\n");

    dms_mcu_frame_t tx_frame;
    dms_mcu_build_event_frame(&tx_frame,
                               DMS_MCU_EVENT_LONG_EYE_CLOSED,
                               DMS_RISK_WARNING,
                               90,
                               1500,
                               1234567890);

    uint8_t buf[DMS_MCU_ENCODE_BUF_SIZE];
    size_t encoded_len = dms_mcu_encode(&tx_frame, buf, sizeof(buf));

    TEST_ASSERT(encoded_len > 0, "encode returns non-zero length");
    TEST_ASSERT(encoded_len == 15, "no-payload frame is 15 bytes");

    /* 检查 header */
    TEST_ASSERT(buf[0] == 0xAA, "header byte 0 is 0xAA");
    TEST_ASSERT(buf[1] == 0x55, "header byte 1 is 0x55");
    TEST_ASSERT(buf[2] == 0x01, "version is 0x01");
    TEST_ASSERT(buf[3] == 0x11, "event is LONG_EYE_CLOSED (0x11)");

    /* 解码 */
    dms_mcu_frame_t rx_frame;
    bool ok = dms_mcu_decode(buf, encoded_len, &rx_frame);

    TEST_ASSERT(ok, "decode succeeds");
    TEST_ASSERT(rx_frame.event == (uint8_t)DMS_MCU_EVENT_LONG_EYE_CLOSED,
                "decoded event matches");
    TEST_ASSERT(rx_frame.risk_level == (uint8_t)DMS_RISK_WARNING,
                "decoded risk level matches");
    TEST_ASSERT(rx_frame.confidence == 90, "decoded confidence matches");
    TEST_ASSERT(rx_frame.duration_ms == 1500, "decoded duration matches");
    TEST_ASSERT(rx_frame.timestamp == 1234567890, "decoded timestamp matches");
    TEST_ASSERT(rx_frame.payload_len == 0, "decoded payload_len is 0");
}

/* 测试 2：心跳帧编码/解码 */
static void test_heartbeat_frame(void)
{
    printf("\n--- Test: Heartbeat Frame ---\n");

    dms_mcu_heartbeat_t hb;
    hb.dms_alive    = 1;
    hb.camera_alive = 1;
    hb.ai_alive     = 1;
    hb.risk_level   = (uint8_t)DMS_RISK_NORMAL;

    dms_mcu_frame_t tx_frame;
    dms_mcu_build_heartbeat_frame(&tx_frame, &hb, 9999);

    uint8_t buf[DMS_MCU_ENCODE_BUF_SIZE];
    size_t encoded_len = dms_mcu_encode(&tx_frame, buf, sizeof(buf));

    TEST_ASSERT(encoded_len > 0, "heartbeat encode returns non-zero");
    TEST_ASSERT(encoded_len == 15 + 4, "heartbeat frame is 19 bytes (15 + 4 payload)");

    /* 解码 */
    dms_mcu_frame_t rx_frame;
    bool ok = dms_mcu_decode(buf, encoded_len, &rx_frame);

    TEST_ASSERT(ok, "heartbeat decode succeeds");
    TEST_ASSERT(rx_frame.event == (uint8_t)DMS_MCU_EVENT_HEARTBEAT,
                "heartbeat event code matches");
    TEST_ASSERT(rx_frame.payload_len == 4, "heartbeat payload_len is 4");
    TEST_ASSERT(rx_frame.payload[0] == 1, "dms_alive is 1");
    TEST_ASSERT(rx_frame.payload[1] == 1, "camera_alive is 1");
    TEST_ASSERT(rx_frame.payload[2] == 1, "ai_alive is 1");
    TEST_ASSERT(rx_frame.payload[3] == (uint8_t)DMS_RISK_NORMAL, "risk_level matches");
}

/* 测试 3：CRC 错误检测 */
static void test_crc_error_detection(void)
{
    printf("\n--- Test: CRC Error Detection ---\n");

    dms_mcu_frame_t tx_frame;
    dms_mcu_build_event_frame(&tx_frame,
                               DMS_MCU_EVENT_YAWN,
                               DMS_RISK_ATTENTION,
                               80,
                               1200,
                               5000);

    uint8_t buf[DMS_MCU_ENCODE_BUF_SIZE];
    size_t encoded_len = dms_mcu_encode(&tx_frame, buf, sizeof(buf));
    TEST_ASSERT(encoded_len > 0, "encode succeeds");

    /* 正常解码应该成功 */
    dms_mcu_frame_t rx_frame;
    bool ok = dms_mcu_decode(buf, encoded_len, &rx_frame);
    TEST_ASSERT(ok, "original frame decodes OK");

    /* 篡改一个数据字节 */
    buf[5] ^= 0xFF;  /* 翻转 confidence 字节 */

    ok = dms_mcu_decode(buf, encoded_len, &rx_frame);
    TEST_ASSERT(!ok, "corrupted frame fails CRC check");
}

/* 测试 4：Header 错误检测 */
static void test_header_error(void)
{
    printf("\n--- Test: Header Error Detection ---\n");

    dms_mcu_frame_t tx_frame;
    dms_mcu_build_event_frame(&tx_frame,
                               DMS_MCU_EVENT_HEAD_DOWN,
                               DMS_RISK_WARNING,
                               85,
                               3000,
                               6000);

    uint8_t buf[DMS_MCU_ENCODE_BUF_SIZE];
    size_t encoded_len = dms_mcu_encode(&tx_frame, buf, sizeof(buf));

    /* 篡改 header */
    buf[0] = 0xBB;

    dms_mcu_frame_t rx_frame;
    bool ok = dms_mcu_decode(buf, encoded_len, &rx_frame);
    TEST_ASSERT(!ok, "bad header byte 0 rejected");

    /* 恢复，篡改第二个 header 字节 */
    buf[0] = 0xAA;
    buf[1] = 0x66;

    ok = dms_mcu_decode(buf, encoded_len, &rx_frame);
    TEST_ASSERT(!ok, "bad header byte 1 rejected");
}

/* 测试 5：缓冲区太小 */
static void test_buffer_too_small(void)
{
    printf("\n--- Test: Buffer Too Small ---\n");

    dms_mcu_frame_t tx_frame;
    dms_mcu_build_event_frame(&tx_frame,
                               DMS_MCU_EVENT_EYE_CLOSED,
                               DMS_RISK_ATTENTION,
                               70,
                               500,
                               3000);

    uint8_t small_buf[5];
    size_t encoded_len = dms_mcu_encode(&tx_frame, small_buf, sizeof(small_buf));
    TEST_ASSERT(encoded_len == 0, "encode with tiny buffer returns 0");
}

/* 测试 6：NULL 参数 */
static void test_null_params(void)
{
    printf("\n--- Test: NULL Parameters ---\n");

    uint8_t buf[DMS_MCU_ENCODE_BUF_SIZE];
    dms_mcu_frame_t frame;

    size_t len = dms_mcu_encode(NULL, buf, sizeof(buf));
    TEST_ASSERT(len == 0, "encode with NULL frame returns 0");

    len = dms_mcu_encode(&frame, NULL, sizeof(buf));
    TEST_ASSERT(len == 0, "encode with NULL buf returns 0");

    bool ok = dms_mcu_decode(NULL, 10, &frame);
    TEST_ASSERT(!ok, "decode with NULL buf fails");

    ok = dms_mcu_decode(buf, 10, NULL);
    TEST_ASSERT(!ok, "decode with NULL frame fails");
}

/* 测试 7：所有事件类型编码/解码 */
static void test_all_event_types(void)
{
    printf("\n--- Test: All Event Types ---\n");

    dms_mcu_event_t events[] = {
        DMS_MCU_EVENT_HEARTBEAT,
        DMS_MCU_EVENT_EYE_CLOSED,
        DMS_MCU_EVENT_LONG_EYE_CLOSED,
        DMS_MCU_EVENT_YAWN,
        DMS_MCU_EVENT_HEAD_DOWN,
        DMS_MCU_EVENT_FACE_LOST,
        DMS_MCU_EVENT_FATIGUE_WARNING,
        DMS_MCU_EVENT_FATIGUE_HIGH,
    };

    const char *names[] = {
        "HEARTBEAT", "EYE_CLOSED", "LONG_EYE_CLOSED", "YAWN",
        "HEAD_DOWN", "FACE_LOST", "FATIGUE_WARNING", "FATIGUE_HIGH"
    };

    for (int i = 0; i < 8; i++) {
        dms_mcu_frame_t tx;
        dms_mcu_build_event_frame(&tx, events[i], DMS_RISK_WARNING, 95, 2000, 12345);

        uint8_t buf[DMS_MCU_ENCODE_BUF_SIZE];
        size_t len = dms_mcu_encode(&tx, buf, sizeof(buf));

        dms_mcu_frame_t rx;
        bool ok = dms_mcu_decode(buf, len, &rx);

        char test_name[64];
        snprintf(test_name, sizeof(test_name), "event %s encode/decode", names[i]);
        TEST_ASSERT(ok && rx.event == (uint8_t)events[i], test_name);
    }
}

/* 测试 8：CRC16 已知值验证 */
static void test_crc16_known_value(void)
{
    printf("\n--- Test: CRC16 Known Value ---\n");

    /* CRC16-CCITT of "123456789" should be 0x29B1 */
    const char *data = "123456789";
    uint16_t crc = dms_mcu_crc16((const uint8_t *)data, 9);

    TEST_ASSERT(crc == 0x29B1, "CRC16 of '123456789' is 0x29B1");
}

/* 测试 9：长度不足的解码 */
static void test_decode_short_buffer(void)
{
    printf("\n--- Test: Decode Short Buffer ---\n");

    uint8_t buf[5] = {0xAA, 0x55, 0x01, 0x10, 0x00};
    dms_mcu_frame_t frame;

    bool ok = dms_mcu_decode(buf, sizeof(buf), &frame);
    TEST_ASSERT(!ok, "decode with too-short buffer fails");
}

/* ==================== 主函数 ==================== */

int main(void)
{
    printf("=== DMS MCU Protocol Unit Tests ===\n");

    test_encode_decode_basic();
    test_heartbeat_frame();
    test_crc_error_detection();
    test_header_error();
    test_buffer_too_small();
    test_null_params();
    test_all_event_types();
    test_crc16_known_value();
    test_decode_short_buffer();

    printf("\n=== Results: %d passed, %d failed ===\n", s_test_pass, s_test_fail);
    return (s_test_fail > 0) ? 1 : 0;
}
