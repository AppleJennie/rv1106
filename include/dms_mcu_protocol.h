#ifndef DMS_MCU_PROTOCOL_H
#define DMS_MCU_PROTOCOL_H

#include "common.h"
#include "dms_risk_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS MCU Protocol - RV1106 与 MCU 之间的通讯协议。
 *
 * 职责划分：
 *   RV1106：看懂司机状态（AI 推理、风险评估）
 *   MCU：蜂鸣器、IMU、CAN、车辆 IO、硬件看门狗、电源管理
 *
 * 协议设计原则：
 *   - 简单、稳定、二进制
 *   - 不依赖 JSON
 *   - 包含 CRC 校验
 *   - 可扩展
 *
 * 本模块只实现协议编码/解码，不绑定具体 UART 设备。
 */

/* ==================== 协议常量 ==================== */
#define DMS_MCU_HEADER_0        0xAA
#define DMS_MCU_HEADER_1        0x55
#define DMS_MCU_PROTOCOL_VER    0x01
#define DMS_MCU_MAX_PAYLOAD     32
#define DMS_MCU_FRAME_OVERHEAD  8   /* header(2) + ver(1) + event(1) + len(1) + crc(2) + reserved(1) */

/* ==================== 事件码 ==================== */
typedef enum {
    DMS_MCU_EVENT_HEARTBEAT       = 0x01,
    DMS_MCU_EVENT_EYE_CLOSED      = 0x10,
    DMS_MCU_EVENT_LONG_EYE_CLOSED = 0x11,
    DMS_MCU_EVENT_YAWN            = 0x12,
    DMS_MCU_EVENT_HEAD_DOWN       = 0x13,
    DMS_MCU_EVENT_FACE_LOST       = 0x14,
    DMS_MCU_EVENT_FATIGUE_WARNING = 0x20,
    DMS_MCU_EVENT_FATIGUE_HIGH    = 0x21,
} dms_mcu_event_t;

/* ==================== 心跳子状态 ==================== */
typedef struct {
    uint8_t  dms_alive;       /* DMS 应用存活 */
    uint8_t  camera_alive;    /* 摄像头存活 */
    uint8_t  ai_alive;        /* AI 推理存活 */
    uint8_t  risk_level;      /* 当前风险等级（dms_risk_level_t） */
} dms_mcu_heartbeat_t;

/* ==================== 报文结构 ==================== */
typedef struct {
    uint8_t  header[2];       /* 固定 0xAA 0x55 */
    uint8_t  version;         /* 协议版本 */
    uint8_t  event;           /* 事件码 */
    uint8_t  risk_level;      /* 风险等级 */
    uint8_t  confidence;      /* 置信度 0~100 */
    uint16_t duration_ms;     /* 事件持续时间（毫秒） */
    uint32_t timestamp;       /* 时间戳（秒级 Unix timestamp 或系统启动秒数） */
    uint8_t  payload_len;     /* 附加数据长度 */
    uint8_t  payload[DMS_MCU_MAX_PAYLOAD]; /* 附加数据（心跳状态等） */
    uint16_t crc;             /* CRC16-CCITT */
} dms_mcu_frame_t;

/* ==================== 编码后缓冲区 ==================== */
#define DMS_MCU_ENCODE_BUF_SIZE  (sizeof(dms_mcu_frame_t) + 8)

/* ==================== API ==================== */

/*
 * 编码一帧数据到缓冲区。
 * 返回编码后的字节数，失败返回 0。
 */
size_t dms_mcu_encode(const dms_mcu_frame_t *frame, uint8_t *buf, size_t buf_size);

/*
 * 从缓冲区解码一帧数据。
 * 成功返回 true，失败（CRC 错误、header 错误等）返回 false。
 */
bool dms_mcu_decode(const uint8_t *buf, size_t len, dms_mcu_frame_t *frame);

/*
 * 构造一帧事件报文（便捷函数）。
 */
void dms_mcu_build_event_frame(dms_mcu_frame_t *frame,
                                dms_mcu_event_t event,
                                dms_risk_level_t risk_level,
                                uint8_t confidence,
                                uint16_t duration_ms,
                                uint32_t timestamp);

/*
 * 构造一帧心跳报文（便捷函数）。
 */
void dms_mcu_build_heartbeat_frame(dms_mcu_frame_t *frame,
                                    const dms_mcu_heartbeat_t *hb,
                                    uint32_t timestamp);

/*
 * CRC16-CCITT 计算（暴露给外部验证用）。
 */
uint16_t dms_mcu_crc16(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
