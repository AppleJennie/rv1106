#ifndef DMS_HEARTBEAT_H
#define DMS_HEARTBEAT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS Heartbeat Monitor - MCU 侧心跳监控模块。
 *
 * 监控 RV1106 的心跳，判断通信链路状态。
 *
 * 状态：
 *   DMS_LINK_OK       - 正常（< 3 秒未收到心跳）
 *   DMS_LINK_DEGRADED - 降级（3~5 秒未收到心跳）
 *   DMS_LINK_LOST     - 丢失（> 5 秒未收到心跳）
 */

/* ==================== 链路状态 ==================== */
typedef enum {
    DMS_LINK_OK = 0,
    DMS_LINK_DEGRADED,
    DMS_LINK_LOST,
} dms_link_state_e;

/* ==================== 心跳配置 ==================== */
typedef struct {
    uint32_t degraded_threshold_ms;   /* 降级阈值（默认 3000ms） */
    uint32_t lost_threshold_ms;       /* 丢失阈值（默认 5000ms） */
} dms_heartbeat_config_t;

/* ==================== API ==================== */

/* 初始化心跳监控 */
void dms_heartbeat_init(void);

/* 使用自定义配置初始化 */
void dms_heartbeat_init_with_config(const dms_heartbeat_config_t *config);

/* 收到心跳时调用（更新 last_heartbeat 时间戳） */
void dms_heartbeat_on_received(uint32_t timestamp_ms);

/*
 * 周期性调用（建议每 100ms 或更快）。
 * 检查心跳超时，更新链路状态。
 */
void dms_heartbeat_tick(uint32_t now_ms);

/* 获取当前链路状态 */
dms_link_state_e dms_heartbeat_get_state(void);

/* 获取距上次心跳的毫秒数 */
uint32_t dms_heartbeat_get_elapsed_ms(uint32_t now_ms);

/* 链路状态转字符串 */
const char* dms_link_state_to_string(dms_link_state_e state);

#ifdef __cplusplus
}
#endif

#endif
