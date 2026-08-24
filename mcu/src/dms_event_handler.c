#include "dms_event_handler.h"
#include "dms_heartbeat.h"
#include "dms_alarm.h"

#include <string.h>

/* ==================== 内部状态 ==================== */
static dms_mcu_state_t s_state;
static uint32_t        s_local_tick_ms;

/* ==================== 公开 API ==================== */

void dms_event_handler_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.link_ok    = 1;
    s_state.risk_level = DMS_RISK_NORMAL;
    s_state.alarm_level = DMS_ALARM_NONE;
    s_local_tick_ms    = 0;
}

void dms_event_handler_on_frame(const dms_parsed_frame_t *frame, void *user_data)
{
    (void)user_data;
    if (!frame) return;

    /* 更新最后事件 */
    s_state.last_event        = frame->event;
    s_state.event_duration_ms = frame->duration_ms;
    s_state.risk_level        = frame->risk_level;

    /* 根据事件类型处理 */
    switch (frame->event) {

    case DMS_EVENT_HEARTBEAT:
        /* 更新心跳时间 */
        s_state.last_heartbeat_ms = s_local_tick_ms;
        dms_heartbeat_on_received(s_local_tick_ms);

        /* 心跳 payload 中包含子状态 */
        if (frame->payload_len >= 4) {
            /* payload[3] = risk_level from heartbeat */
            s_state.risk_level = frame->payload[3];
        }
        break;

    case DMS_EVENT_LONG_EYE_CLOSED:
        /* 长闭眼：WARNING 级别报警 */
        dms_alarm_set_level(DMS_ALARM_WARNING);
        s_state.alarm_level = DMS_ALARM_WARNING;
        break;

    case DMS_EVENT_FATIGUE_WARNING:
        dms_alarm_set_level(DMS_ALARM_WARNING);
        s_state.alarm_level = DMS_ALARM_WARNING;
        break;

    case DMS_EVENT_FATIGUE_HIGH:
        dms_alarm_set_level(DMS_ALARM_HIGH);
        s_state.alarm_level = DMS_ALARM_HIGH;
        break;

    case DMS_EVENT_EYE_CLOSED:
    case DMS_EVENT_YAWN:
    case DMS_EVENT_HEAD_DOWN:
    case DMS_EVENT_FACE_LOST:
        /* 普通事件：根据 risk_level 决定报警 */
        if (frame->risk_level >= DMS_RISK_HIGH) {
            dms_alarm_set_level(DMS_ALARM_HIGH);
            s_state.alarm_level = DMS_ALARM_HIGH;
        } else if (frame->risk_level >= DMS_RISK_WARNING) {
            dms_alarm_set_level(DMS_ALARM_WARNING);
            s_state.alarm_level = DMS_ALARM_WARNING;
        } else {
            /* ATTENTION / NORMAL：不报警 */
            dms_alarm_set_level(DMS_ALARM_NONE);
            s_state.alarm_level = DMS_ALARM_NONE;
        }
        break;

    default:
        /* 未知事件：忽略 */
        break;
    }
}

const dms_mcu_state_t* dms_event_handler_get_state(void)
{
    return &s_state;
}

void dms_event_handler_tick(uint32_t ms)
{
    s_local_tick_ms = ms;

    /* 更新心跳监控 */
    dms_heartbeat_tick(ms);

    /* 更新链路状态 */
    dms_link_state_e link = dms_heartbeat_get_state();
    s_state.link_ok = (link == DMS_LINK_OK) ? 1 : 0;

    /* 蜂鸣器 tick */
    dms_alarm_tick(ms);
}
