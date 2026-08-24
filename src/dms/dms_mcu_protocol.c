#include "dms_mcu_protocol.h"
#include "sys_logger.h"

#include <string.h>

/* ==================== CRC16-CCITT 实现 ==================== */

/* CRC16-CCITT ( polynomial 0x1021, initial value 0xFFFF ) */
uint16_t dms_mcu_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* ==================== 编码 ==================== */

size_t dms_mcu_encode(const dms_mcu_frame_t *frame, uint8_t *buf, size_t buf_size)
{
    if (!frame || !buf) return 0;

    /* 计算需要的缓冲区大小：
     * header(2) + version(1) + event(1) + risk_level(1) + confidence(1)
     * + duration_ms(2) + timestamp(4) + payload_len(1) + payload(N) + crc(2)
     */
    size_t payload_len = frame->payload_len;
    if (payload_len > DMS_MCU_MAX_PAYLOAD) {
        payload_len = DMS_MCU_MAX_PAYLOAD;
    }

    size_t total = 2 + 1 + 1 + 1 + 1 + 2 + 4 + 1 + payload_len + 2;
    if (buf_size < total) return 0;

    size_t offset = 0;

    /* Header */
    buf[offset++] = DMS_MCU_HEADER_0;
    buf[offset++] = DMS_MCU_HEADER_1;

    /* Version */
    buf[offset++] = DMS_MCU_PROTOCOL_VER;

    /* Event */
    buf[offset++] = frame->event;

    /* Risk level */
    buf[offset++] = frame->risk_level;

    /* Confidence */
    buf[offset++] = frame->confidence;

    /* Duration (big-endian) */
    buf[offset++] = (uint8_t)(frame->duration_ms >> 8);
    buf[offset++] = (uint8_t)(frame->duration_ms & 0xFF);

    /* Timestamp (big-endian) */
    buf[offset++] = (uint8_t)(frame->timestamp >> 24);
    buf[offset++] = (uint8_t)(frame->timestamp >> 16);
    buf[offset++] = (uint8_t)(frame->timestamp >> 8);
    buf[offset++] = (uint8_t)(frame->timestamp & 0xFF);

    /* Payload length */
    buf[offset++] = (uint8_t)payload_len;

    /* Payload */
    if (payload_len > 0) {
        memcpy(&buf[offset], frame->payload, payload_len);
        offset += payload_len;
    }

    /* CRC16 over everything except CRC itself */
    uint16_t crc = dms_mcu_crc16(buf, offset);
    buf[offset++] = (uint8_t)(crc >> 8);
    buf[offset++] = (uint8_t)(crc & 0xFF);

    return offset;
}

/* ==================== 解码 ==================== */

bool dms_mcu_decode(const uint8_t *buf, size_t len, dms_mcu_frame_t *frame)
{
    if (!buf || !frame || len < 13) return false;  /* 最小帧：无 payload 时 13 字节 */

    /* 检查 header */
    if (buf[0] != DMS_MCU_HEADER_0 || buf[1] != DMS_MCU_HEADER_1) {
        return false;
    }

    /* 检查版本 */
    if (buf[2] != DMS_MCU_PROTOCOL_VER) {
        return false;
    }

    size_t offset = 0;

    /* 跳过 header */
    offset += 2;

    /* Version */
    frame->version = buf[offset++];

    /* Event */
    frame->event = buf[offset++];

    /* Risk level */
    frame->risk_level = buf[offset++];

    /* Confidence */
    frame->confidence = buf[offset++];

    /* Duration (big-endian) */
    frame->duration_ms = ((uint16_t)buf[offset] << 8) | buf[offset + 1];
    offset += 2;

    /* Timestamp (big-endian) */
    frame->timestamp = ((uint32_t)buf[offset] << 24) |
                       ((uint32_t)buf[offset + 1] << 16) |
                       ((uint32_t)buf[offset + 2] << 8) |
                       (uint32_t)buf[offset + 3];
    offset += 4;

    /* Payload length */
    frame->payload_len = buf[offset++];
    if (frame->payload_len > DMS_MCU_MAX_PAYLOAD) {
        return false;
    }

    /* 检查总长度是否匹配 */
    size_t expected_len = offset + frame->payload_len + 2;
    if (len < expected_len) {
        return false;
    }

    /* Payload */
    if (frame->payload_len > 0) {
        memcpy(frame->payload, &buf[offset], frame->payload_len);
        offset += frame->payload_len;
    }

    /* CRC16 */
    uint16_t received_crc = ((uint16_t)buf[offset] << 8) | buf[offset + 1];
    uint16_t computed_crc = dms_mcu_crc16(buf, offset);

    if (received_crc != computed_crc) {
        return false;  /* CRC 校验失败 */
    }

    return true;
}

/* ==================== 便捷构造函数 ==================== */

void dms_mcu_build_event_frame(dms_mcu_frame_t *frame,
                                dms_mcu_event_t event,
                                dms_risk_level_t risk_level,
                                uint8_t confidence,
                                uint16_t duration_ms,
                                uint32_t timestamp)
{
    if (!frame) return;

    memset(frame, 0, sizeof(*frame));
    frame->header[0]   = DMS_MCU_HEADER_0;
    frame->header[1]   = DMS_MCU_HEADER_1;
    frame->version     = DMS_MCU_PROTOCOL_VER;
    frame->event       = (uint8_t)event;
    frame->risk_level  = (uint8_t)risk_level;
    frame->confidence  = confidence;
    frame->duration_ms = duration_ms;
    frame->timestamp   = timestamp;
    frame->payload_len = 0;
}

void dms_mcu_build_heartbeat_frame(dms_mcu_frame_t *frame,
                                    const dms_mcu_heartbeat_t *hb,
                                    uint32_t timestamp)
{
    if (!frame || !hb) return;

    memset(frame, 0, sizeof(*frame));
    frame->header[0]   = DMS_MCU_HEADER_0;
    frame->header[1]   = DMS_MCU_HEADER_1;
    frame->version     = DMS_MCU_PROTOCOL_VER;
    frame->event       = (uint8_t)DMS_MCU_EVENT_HEARTBEAT;
    frame->risk_level  = hb->risk_level;
    frame->confidence  = 100;
    frame->duration_ms = 0;
    frame->timestamp   = timestamp;

    /* 心跳 payload：4 字节状态 */
    frame->payload_len = 4;
    frame->payload[0]  = hb->dms_alive;
    frame->payload[1]  = hb->camera_alive;
    frame->payload[2]  = hb->ai_alive;
    frame->payload[3]  = hb->risk_level;
}
