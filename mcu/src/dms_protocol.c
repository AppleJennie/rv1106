#include "dms_protocol.h"

#include <string.h>

/* ==================== CRC16-CCITT ==================== */

uint16_t dms_protocol_crc16(const uint8_t *data, size_t len)
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

/* ==================== Parser 内部辅助 ==================== */

/* 重置 parser 到同步搜索状态 */
static void parser_reset(dms_parser_t *parser)
{
    parser->state       = PARSER_SYNC_0;
    parser->buf_len     = 0;
    parser->payload_len = 0;
    parser->crc_bytes   = 0;
}

/* 从 buffer 解析完整帧并回调 */
static void parser_dispatch(dms_parser_t *parser,
                             dms_frame_callback_t callback,
                             void *user_data)
{
    /* buf 中应有完整帧：fixed(13) + payload(N) + crc(2) */
    uint8_t *buf = parser->buf;
    uint8_t payload_len = parser->payload_len;
    size_t frame_data_len = DMS_PROTO_FIXED_LEN + payload_len;

    /* CRC 校验（覆盖 header 到 payload 末尾） */
    uint16_t received_crc = ((uint16_t)buf[frame_data_len] << 8) |
                            buf[frame_data_len + 1];
    uint16_t computed_crc = dms_protocol_crc16(buf, frame_data_len);

    if (received_crc != computed_crc) {
        parser->stats.crc_errors++;
        parser->stats.resync_count++;
        parser_reset(parser);
        return;
    }

    /* 检查版本 */
    if (buf[2] != DMS_PROTO_VERSION) {
        parser->stats.parser_errors++;
        parser->stats.resync_count++;
        parser_reset(parser);
        return;
    }

    /* 解析帧 */
    dms_parsed_frame_t frame;
    memset(&frame, 0, sizeof(frame));

    frame.event       = buf[3];
    frame.risk_level  = buf[4];
    frame.confidence  = buf[5];
    frame.duration_ms = ((uint16_t)buf[6] << 8) | buf[7];
    frame.timestamp   = ((uint32_t)buf[8] << 24) |
                        ((uint32_t)buf[9] << 16) |
                        ((uint32_t)buf[10] << 8) |
                        (uint32_t)buf[11];
    frame.payload_len = payload_len;

    if (payload_len > 0) {
        memcpy(frame.payload, &buf[DMS_PROTO_FIXED_LEN], payload_len);
    }

    parser->stats.valid_frames++;

    /* 回调 */
    if (callback) {
        callback(&frame, user_data);
    }

    parser_reset(parser);
}

/* ==================== 公开 API ==================== */

void dms_protocol_parser_init(dms_parser_t *parser)
{
    if (!parser) return;
    memset(parser, 0, sizeof(*parser));
    parser_reset(parser);
}

void dms_protocol_feed_byte(dms_parser_t *parser, uint8_t byte,
                             dms_frame_callback_t callback, void *user_data)
{
    if (!parser) return;

    parser->stats.total_bytes++;

    switch (parser->state) {

    case PARSER_SYNC_0:
        if (byte == DMS_PROTO_HEADER_0) {
            parser->buf[0] = byte;
            parser->buf_len = 1;
            parser->state = PARSER_SYNC_1;
        }
        /* 否则继续等待 0xAA */
        break;

    case PARSER_SYNC_1:
        if (byte == DMS_PROTO_HEADER_1) {
            parser->buf[1] = byte;
            parser->buf_len = 2;
            parser->state = PARSER_READ_FIXED;
        } else if (byte == DMS_PROTO_HEADER_0) {
            /* 0xAA 0xAA：重新开始 */
            parser->buf[0] = byte;
            parser->buf_len = 1;
            /* 保持 PARSER_SYNC_1 */
        } else {
            /* 不是 header，重新同步 */
            parser->stats.resync_count++;
            parser_reset(parser);
        }
        break;

    case PARSER_READ_FIXED:
        if (parser->buf_len < DMS_PROTO_MAX_FRAME_LEN) {
            parser->buf[parser->buf_len] = byte;
        }
        parser->buf_len++;

        if (parser->buf_len >= DMS_PROTO_FIXED_LEN) {
            /* 固定部分读取完成，提取 payload_len */
            parser->payload_len = parser->buf[12];  /* payload_len 在偏移 12 */

            if (parser->payload_len > DMS_PROTO_MAX_PAYLOAD) {
                /* payload 长度异常，重新同步 */
                parser->stats.parser_errors++;
                parser->stats.resync_count++;
                parser_reset(parser);
                break;
            }

            if (parser->payload_len > 0) {
                parser->state = PARSER_READ_PAYLOAD;
            } else {
                parser->state = PARSER_READ_CRC;
                parser->crc_bytes = 0;
            }
        }
        break;

    case PARSER_READ_PAYLOAD:
        if (parser->buf_len < DMS_PROTO_MAX_FRAME_LEN) {
            parser->buf[parser->buf_len] = byte;
        }
        parser->buf_len++;

        if (parser->buf_len >= DMS_PROTO_FIXED_LEN + parser->payload_len) {
            parser->state = PARSER_READ_CRC;
            parser->crc_bytes = 0;
        }
        break;

    case PARSER_READ_CRC:
        if (parser->buf_len < DMS_PROTO_MAX_FRAME_LEN) {
            parser->buf[parser->buf_len] = byte;
        }
        parser->buf_len++;
        parser->crc_bytes++;

        if (parser->crc_bytes >= DMS_PROTO_CRC_LEN) {
            /* 完整帧接收完毕，解析并分发 */
            parser_dispatch(parser, callback, user_data);
        }
        break;

    default:
        parser_reset(parser);
        break;
    }
}

void dms_protocol_feed(dms_parser_t *parser, const uint8_t *data, size_t len,
                        dms_frame_callback_t callback, void *user_data)
{
    if (!parser || !data) return;
    for (size_t i = 0; i < len; i++) {
        dms_protocol_feed_byte(parser, data[i], callback, user_data);
    }
}

void dms_protocol_get_stats(const dms_parser_t *parser, dms_parser_stats_t *stats)
{
    if (!parser || !stats) return;
    memcpy(stats, &parser->stats, sizeof(dms_parser_stats_t));
}

void dms_protocol_reset_stats(dms_parser_t *parser)
{
    if (!parser) return;
    memset(&parser->stats, 0, sizeof(dms_parser_stats_t));
}

/* ==================== 字符串转换 ==================== */

const char* dms_event_code_to_string(uint8_t event)
{
    switch (event) {
    case DMS_EVENT_HEARTBEAT:       return "HEARTBEAT";
    case DMS_EVENT_EYE_CLOSED:      return "EYE_CLOSED";
    case DMS_EVENT_LONG_EYE_CLOSED: return "LONG_EYE_CLOSED";
    case DMS_EVENT_YAWN:            return "YAWN";
    case DMS_EVENT_HEAD_DOWN:       return "HEAD_DOWN";
    case DMS_EVENT_FACE_LOST:       return "FACE_LOST";
    case DMS_EVENT_FATIGUE_WARNING: return "FATIGUE_WARNING";
    case DMS_EVENT_FATIGUE_HIGH:    return "FATIGUE_HIGH";
    default:                        return "UNKNOWN";
    }
}

const char* dms_risk_level_to_string(uint8_t level)
{
    switch (level) {
    case DMS_RISK_NORMAL:    return "NORMAL";
    case DMS_RISK_ATTENTION: return "ATTENTION";
    case DMS_RISK_WARNING:   return "WARNING";
    case DMS_RISK_HIGH:      return "HIGH";
    default:                 return "UNKNOWN";
    }
}
