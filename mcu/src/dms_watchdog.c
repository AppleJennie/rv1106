#include "dms_watchdog.h"
#include "dms_heartbeat.h"

#include <string.h>

/* ==================== 内部状态 ==================== */
typedef struct {
    dms_watchdog_state_e state;
    uint32_t             reset_request_count;
    uint32_t             timeout_start_ms;   /* 进入 timeout 的时间 */
    bool                 reset_requested;    /* 是否已请求复位 */
} watchdog_state_t;

static watchdog_state_t s_wd;

/* 超时后多久触发复位请求（默认 10 秒） */
#define WATCHDOG_RESET_DELAY_MS  10000

/* ==================== 公开 API ==================== */

void dms_watchdog_init(void)
{
    memset(&s_wd, 0, sizeof(s_wd));
    s_wd.state = WATCHDOG_NORMAL;
}

void dms_watchdog_tick(uint32_t now_ms)
{
    dms_link_state_e link = dms_heartbeat_get_state();

    switch (link) {
    case DMS_LINK_OK:
        s_wd.state = WATCHDOG_NORMAL;
        s_wd.reset_requested = false;
        break;

    case DMS_LINK_DEGRADED:
        if (s_wd.state == WATCHDOG_NORMAL) {
            s_wd.state = WATCHDOG_WARNING;
        }
        break;

    case DMS_LINK_LOST:
        if (s_wd.state != WATCHDOG_TIMEOUT) {
            s_wd.state = WATCHDOG_TIMEOUT;
            s_wd.timeout_start_ms = now_ms;
        }

        /* 超时足够久后请求复位（当前仅记录，不实际控制） */
        if (!s_wd.reset_requested &&
            (now_ms - s_wd.timeout_start_ms) >= WATCHDOG_RESET_DELAY_MS) {
            dms_watchdog_request_rv1106_reset();
            s_wd.reset_requested = true;
        }
        break;
    }
}

dms_watchdog_state_e dms_watchdog_get_state(void)
{
    return s_wd.state;
}

bool dms_watchdog_request_rv1106_reset(void)
{
    /*
     * STUB: 当前只记录请求，不实际控制 RV1106 复位引脚。
     *
     * 未来实现：
     *   - GPIO 拉低 RV1106 RESET 引脚
     *   - 或控制 RV1106 电源 MOSFET
     *   - 复位后等待 RV1106 重新启动并恢复心跳
     */
    s_wd.reset_request_count++;
    return true;
}

uint32_t dms_watchdog_get_reset_request_count(void)
{
    return s_wd.reset_request_count;
}

const char* dms_watchdog_state_to_string(dms_watchdog_state_e state)
{
    switch (state) {
    case WATCHDOG_NORMAL:  return "NORMAL";
    case WATCHDOG_WARNING: return "WARNING";
    case WATCHDOG_TIMEOUT: return "TIMEOUT";
    default:               return "UNKNOWN";
    }
}
