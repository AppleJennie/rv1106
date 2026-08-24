#ifndef DMS_WATCHDOG_H
#define DMS_WATCHDOG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS Watchdog - MCU 侧看门狗模块。
 *
 * 监控 RV1106 心跳超时，提供 RV1106 复位接口。
 * 当前版本只提供接口定义和 stub 实现。
 * 不实际控制 RV1106 电源。
 */

/* ==================== 看门狗状态 ==================== */
typedef enum {
    WATCHDOG_NORMAL = 0,       /* 正常 */
    WATCHDOG_WARNING,          /* 心跳降级，注意观察 */
    WATCHDOG_TIMEOUT,          /* 心跳超时，需要干预 */
} dms_watchdog_state_e;

/* ==================== API ==================== */

/* 初始化看门狗 */
void dms_watchdog_init(void);

/*
 * 周期性调用（建议每 100ms）。
 * 根据心跳状态更新看门狗状态。
 */
void dms_watchdog_tick(uint32_t now_ms);

/* 获取看门狗状态 */
dms_watchdog_state_e dms_watchdog_get_state(void);

/*
 * 请求复位 RV1106（stub）。
 * 当前版本只记录请求，不实际控制硬件。
 * 未来由 MCU GPIO 控制 RV1106 复位引脚或电源。
 *
 * @return true = 请求已记录
 */
bool dms_watchdog_request_rv1106_reset(void);

/* 获取复位请求计数（调试用） */
uint32_t dms_watchdog_get_reset_request_count(void);

/* 看门狗状态转字符串 */
const char* dms_watchdog_state_to_string(dms_watchdog_state_e state);

#ifdef __cplusplus
}
#endif

#endif
