#include "dms_heartbeat.h"

#include <string.h>

/* ==================== 内部状态 ==================== */
typedef struct {
    dms_heartbeat_config_t config;
    dms_link_state_e       state;
    uint32_t               last_heartbeat_ms;
    bool                   ever_received;   /* 是否收到过心跳 */
} heartbeat_state_t;

static heartbeat_state_t s_hb;

/* ==================== 公开 API ==================== */

void dms_heartbeat_init(void)
{
    dms_heartbeat_config_t default_config = {
        .degraded_threshold_ms = 3000,
        .lost_threshold_ms     = 5000,
    };
    dms_heartbeat_init_with_config(&default_config);
}

void dms_heartbeat_init_with_config(const dms_heartbeat_config_t *config)
{
    if (!config) return;
    memset(&s_hb, 0, sizeof(s_hb));
    s_hb.config = *config;
    s_hb.state  = DMS_LINK_OK;
    s_hb.ever_received = false;
}

void dms_heartbeat_on_received(uint32_t timestamp_ms)
{
    s_hb.last_heartbeat_ms = timestamp_ms;
    s_hb.ever_received     = true;
    s_hb.state             = DMS_LINK_OK;
}

void dms_heartbeat_tick(uint32_t now_ms)
{
    if (!s_hb.ever_received) {
        /* 从未收到心跳，保持 OK（等待首次心跳） */
        return;
    }

    uint32_t elapsed = now_ms - s_hb.last_heartbeat_ms;

    if (elapsed > s_hb.config.lost_threshold_ms) {
        s_hb.state = DMS_LINK_LOST;
    } else if (elapsed > s_hb.config.degraded_threshold_ms) {
        s_hb.state = DMS_LINK_DEGRADED;
    } else {
        s_hb.state = DMS_LINK_OK;
    }
}

dms_link_state_e dms_heartbeat_get_state(void)
{
    return s_hb.state;
}

uint32_t dms_heartbeat_get_elapsed_ms(uint32_t now_ms)
{
    if (!s_hb.ever_received) return 0;
    return now_ms - s_hb.last_heartbeat_ms;
}

const char* dms_link_state_to_string(dms_link_state_e state)
{
    switch (state) {
    case DMS_LINK_OK:       return "LINK_OK";
    case DMS_LINK_DEGRADED: return "LINK_DEGRADED";
    case DMS_LINK_LOST:     return "LINK_LOST";
    default:                return "UNKNOWN";
    }
}
