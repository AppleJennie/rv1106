#ifndef DMS_EVENT_HANDLER_H
#define DMS_EVENT_HANDLER_H

#include "dms_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS Event Handler - MCU 侧事件分发模块。
 *
 * 接收解析后的协议帧，根据事件类型分发到对应处理模块。
 * 更新 DMS MCU 全局状态。
 */

/* ==================== DMS MCU 全局状态（Driver Safety Output） ==================== */
typedef struct {
    uint8_t  link_ok;             /* 通信链路正常 */
    uint8_t  risk_level;          /* 当前风险等级 */
    uint8_t  alarm_level;         /* 当前报警级别 */
    uint8_t  last_event;          /* 最后收到的事件码 */
    uint32_t event_duration_ms;   /* 最后事件的持续时间 */
    uint32_t last_heartbeat_ms;   /* 最后心跳时间（MCU 本地 ms tick） */
} dms_mcu_state_t;

/* ==================== API ==================== */

/* 初始化事件处理器 */
void dms_event_handler_init(void);

/*
 * 处理解析后的帧（由 protocol parser 回调调用）。
 */
void dms_event_handler_on_frame(const dms_parsed_frame_t *frame, void *user_data);

/* 获取当前 MCU 全局状态 */
const dms_mcu_state_t* dms_event_handler_get_state(void);

/* 更新本地 tick（由系统定时器调用，用于心跳超时检测） */
void dms_event_handler_tick(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif
