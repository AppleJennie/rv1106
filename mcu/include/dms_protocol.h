#ifndef DMS_PROTOCOL_H
#define DMS_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS Protocol - MCU 侧 UART 字节流协议解析器。
 *
 * 职责：
 *   从 UART 字节流中解析 DMS 协议帧。
 *   支持分包、粘包、丢字节、CRC 错误、错误 header。
 *
 * 设计原则：
 *   - 逐字节状态机，不假设一次收到完整帧
 *   - CRC 错误后自动重新同步
 *   - 无动态内存分配
 *   - 可移植到任何 MCU 平台
 *
 * 协议格式见 docs/DMS_MCU_Protocol.md
 */

/* ==================== 协议常量（与 RV1106 侧对齐） ==================== */
#define DMS_PROTO_HEADER_0       0xAA
#define DMS_PROTO_HEADER_1       0x55
#define DMS_PROTO_VERSION        0x01
#define DMS_PROTO_MAX_PAYLOAD    32

/* 帧固定部分长度：header(2) + ver(1) + event(1) + risk(1) + conf(1) + dur(2) + ts(4) + len(1) = 13 */
#define DMS_PROTO_FIXED_LEN      13
/* CRC 长度 */
#define DMS_PROTO_CRC_LEN        2
/* 最小帧长（无 payload） */
#define DMS_PROTO_MIN_FRAME_LEN  (DMS_PROTO_FIXED_LEN + DMS_PROTO_CRC_LEN)  /* 15 */
/* 最大帧长 */
#define DMS_PROTO_MAX_FRAME_LEN  (DMS_PROTO_MIN_FRAME_LEN + DMS_PROTO_MAX_PAYLOAD)  /* 47 */

/* ==================== 事件码 ==================== */
typedef enum {
    DMS_EVENT_HEARTBEAT       = 0x01,
    DMS_EVENT_EYE_CLOSED      = 0x10,
    DMS_EVENT_LONG_EYE_CLOSED = 0x11,
    DMS_EVENT_YAWN            = 0x12,
    DMS_EVENT_HEAD_DOWN       = 0x13,
    DMS_EVENT_FACE_LOST       = 0x14,
    DMS_EVENT_FATIGUE_WARNING = 0x20,
    DMS_EVENT_FATIGUE_HIGH    = 0x21,
} dms_event_code_t;

/* ==================== 风险等级 ==================== */
typedef enum {
    DMS_RISK_NORMAL = 0,
    DMS_RISK_ATTENTION,
    DMS_RISK_WARNING,
    DMS_RISK_HIGH,
} dms_risk_level_e;

/* ==================== 解析后的帧 ==================== */
typedef struct {
    uint8_t  event;
    uint8_t  risk_level;
    uint8_t  confidence;
    uint16_t duration_ms;
    uint32_t timestamp;
    uint8_t  payload_len;
    uint8_t  payload[DMS_PROTO_MAX_PAYLOAD];
} dms_parsed_frame_t;

/* ==================== Parser 状态 ==================== */
typedef enum {
    PARSER_SYNC_0 = 0,    /* 等待 0xAA */
    PARSER_SYNC_1,        /* 等待 0x55 */
    PARSER_READ_FIXED,    /* 读取固定部分 */
    PARSER_READ_PAYLOAD,  /* 读取 payload */
    PARSER_READ_CRC,      /* 读取 CRC */
} parser_state_e;

/* ==================== Parser 统计 ==================== */
typedef struct {
    uint32_t total_bytes;       /* 总接收字节数 */
    uint32_t valid_frames;      /* 有效帧数 */
    uint32_t crc_errors;        /* CRC 错误数 */
    uint32_t resync_count;      /* 重新同步次数 */
    uint32_t parser_errors;     /* 解析错误数（版本错误、长度超限等） */
    uint32_t unknown_events;    /* 未知事件码数 */
} dms_parser_stats_t;

/* ==================== Parser 上下文 ==================== */
typedef struct {
    parser_state_e state;
    uint8_t        buf[DMS_PROTO_MAX_FRAME_LEN];
    uint8_t        buf_len;         /* 当前缓冲区中的字节数 */
    uint8_t        payload_len;     /* 当前帧的 payload 长度 */
    uint8_t        crc_bytes;       /* 已读取的 CRC 字节数 */
    dms_parser_stats_t stats;
} dms_parser_t;

/* ==================== 回调函数类型 ==================== */
typedef void (*dms_frame_callback_t)(const dms_parsed_frame_t *frame, void *user_data);

/* ==================== API ==================== */

/* 初始化 parser */
void dms_protocol_parser_init(dms_parser_t *parser);

/*
 * 逐字节喂给 parser。
 * 当解析出完整有效帧时，通过 callback 返回。
 * 可以在 UART 中断或 DMA 回调中调用。
 */
void dms_protocol_feed_byte(dms_parser_t *parser, uint8_t byte,
                             dms_frame_callback_t callback, void *user_data);

/*
 * 批量喂数据（便捷函数）。
 */
void dms_protocol_feed(dms_parser_t *parser, const uint8_t *data, size_t len,
                        dms_frame_callback_t callback, void *user_data);

/* 获取统计信息 */
void dms_protocol_get_stats(const dms_parser_t *parser, dms_parser_stats_t *stats);

/* 重置统计信息 */
void dms_protocol_reset_stats(dms_parser_t *parser);

/* CRC16-CCITT 计算 */
uint16_t dms_protocol_crc16(const uint8_t *data, size_t len);

/* 事件码转字符串 */
const char* dms_event_code_to_string(uint8_t event);

/* 风险等级转字符串 */
const char* dms_risk_level_to_string(uint8_t level);

#ifdef __cplusplus
}
#endif

#endif
