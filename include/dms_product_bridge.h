#ifndef DMS_PRODUCT_BRIDGE_H
#define DMS_PRODUCT_BRIDGE_H

#include "common.h"
#include "dms_infer.h"
#include "dms_risk_manager.h"
#include "dms_event_logger.h"
#include "dms_alarm_policy.h"
#include "dms_mcu_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS Product Bridge - Integration V1 胶水层。
 *
 * 职责（纯逻辑，可在 PC 上脱离 RV1106 硬件运行）：
 *
 *   dms_result_t（每帧 AI 结果）
 *      ↓ 解析 status、跟踪各事件持续时间
 *   dms_risk_manager_update()      风险等级/评分
 *      ↓
 *   dms_alarm_policy_update()      本地报警级别（冷却防连响）
 *      ↓
 *   dms_event_logger_write()       事件落盘 CSV（只在事件边沿/升级时写，不按帧写）
 *      ↓
 *   dms_mcu_encode()               MCU 二进制帧入待发队列
 *
 * 本模块不操作：camera / RGA / VENC / RetinaFace / 106 landmark / UART。
 * UART 发送由集成方从 dms_product_bridge_get_mcu_packet() 取包后自行完成。
 *
 * 设计原则：保护司机，不是考核司机。Risk Score 是安全风险，不是处罚分。
 */

/* ==================== 每次 update 的输出 ==================== */
typedef struct {
    dms_risk_result_t risk;            /* 风险评估结果（含等级/评分/事件类型） */
    dms_alarm_level_t alarm_level;     /* 当前报警级别 */
    bool              event_logged;    /* 本次是否写入了事件日志 */
    bool              mcu_event_queued;/* 本次是否入队了 MCU 事件帧 */
} dms_product_bridge_output_t;

/* ==================== API ==================== */

/*
 * 初始化（板端）：事件日志写到 SD 卡默认路径 /mnt/sdcard/dms/events。
 * 内部依次初始化 Risk Manager / Event Logger / Alarm Policy。
 */
bool dms_product_bridge_init(void);

/*
 * 初始化（PC/测试）：事件日志写到指定目录。
 * 供离线 replay、单元测试使用。
 */
bool dms_product_bridge_init_with_dir(const char *event_dir);

/*
 * 每帧 AI 推理完成后调用。
 *
 *   dms          - AI 推理结果（status 字符串为权威状态来源）
 *   monotonic_ms - 单调时间戳（毫秒），用于事件持续时间和滑动窗口
 *
 * 返回本次输出（风险/报警/是否记日志/是否入队 MCU 帧）。
 */
dms_product_bridge_output_t dms_product_bridge_update(const dms_result_t *dms,
                                                      uint64_t monotonic_ms);

/*
 * 心跳帧入队（集成时每秒调用一次）。
 *   timestamp_sec - 秒级时间戳（Unix 或系统启动秒数）
 */
bool dms_product_bridge_queue_heartbeat(bool camera_alive, bool ai_alive,
                                        uint32_t timestamp_sec);

/*
 * 从 MCU 待发队列取出一帧（FIFO）。成功返回 true 且 *size 为帧长度；
 * 队列空返回 false。队列满时丢弃最旧帧（计数见 dropped 统计）。
 */
bool dms_product_bridge_get_mcu_packet(uint8_t *buf, size_t capacity, size_t *size);

/* 当前待发送帧数量 */
size_t dms_product_bridge_pending_packets(void);

/* 因队列满被丢弃的帧总数（调试用） */
uint32_t dms_product_bridge_dropped_packets(void);

/*
 * 重置全部状态（换司机 / 新班次时调用）。
 * 内部重置 Risk Manager、Alarm Policy、事件跟踪与待发队列；不动事件日志文件。
 */
void dms_product_bridge_reset(void);

/* 反初始化（关闭事件日志等） */
void dms_product_bridge_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
