#include "dms_alarm_policy.h"
#include "sys_logger.h"

#include <string.h>

/* ==================== 内部状态 ==================== */
typedef struct {
    dms_alarm_policy_config_t config;
    dms_alarm_level_t current_alarm;
    uint64_t          last_alarm_ms;   /* 上次报警时间 */
    bool              initialized;
} alarm_policy_state_t;

static alarm_policy_state_t s_ap;

/* ==================== 默认配置 ==================== */

void dms_alarm_policy_get_default_config(dms_alarm_policy_config_t *config)
{
    if (!config) return;

    memset(config, 0, sizeof(*config));

    /* 默认报警映射 */
    config->alarm_for_attention = ALARM_NONE;        /* ATTENTION 不报警 */
    config->alarm_for_warning   = ALARM_WARNING;     /* WARNING 警告音 */
    config->alarm_for_high      = ALARM_HIGH;        /* HIGH 高危警报 */

    /* 特定事件覆盖 */
    config->alarm_for_long_eye_closed = ALARM_WARNING;  /* 长闭眼直接 WARNING */
    config->alarm_for_head_down       = ALARM_WARNING;  /* 持续低头 WARNING */

    /* 冷却时间 */
    config->cooldown_ms = 3000;  /* 3 秒冷却 */
}

/* ==================== 公开 API ==================== */

bool dms_alarm_policy_init(void)
{
    dms_alarm_policy_config_t config;
    dms_alarm_policy_get_default_config(&config);
    return dms_alarm_policy_init_with_config(&config);
}

bool dms_alarm_policy_init_with_config(const dms_alarm_policy_config_t *config)
{
    if (!config) return false;

    memset(&s_ap, 0, sizeof(s_ap));
    memcpy(&s_ap.config, config, sizeof(dms_alarm_policy_config_t));

    s_ap.current_alarm = ALARM_NONE;
    s_ap.initialized   = true;

    log_info("[AlarmPolicy] initialized");
    return true;
}

dms_alarm_level_t dms_alarm_policy_update(const dms_risk_result_t *risk_result,
                                           uint64_t timestamp_ms)
{
    if (!s_ap.initialized || !risk_result) return ALARM_NONE;

    dms_alarm_level_t desired = ALARM_NONE;

    /* 根据风险等级确定基础报警级别 */
    switch (risk_result->level) {
    case DMS_RISK_NORMAL:
        desired = ALARM_NONE;
        break;
    case DMS_RISK_ATTENTION:
        desired = s_ap.config.alarm_for_attention;
        break;
    case DMS_RISK_WARNING:
        desired = s_ap.config.alarm_for_warning;
        break;
    case DMS_RISK_HIGH:
        desired = s_ap.config.alarm_for_high;
        break;
    }

    /* 特定事件覆盖 */
    if (strcmp(risk_result->event_type, "LONG_EYE_CLOSED") == 0) {
        if (s_ap.config.alarm_for_long_eye_closed > desired) {
            desired = s_ap.config.alarm_for_long_eye_closed;
        }
    }
    if (strcmp(risk_result->event_type, "HEAD_DOWN") == 0) {
        if (s_ap.config.alarm_for_head_down > desired) {
            desired = s_ap.config.alarm_for_head_down;
        }
    }

    /* 冷却时间检查：如果新报警级别低于当前，且还在冷却期内，保持当前 */
    if (desired < s_ap.current_alarm && s_ap.last_alarm_ms > 0) {
        if ((timestamp_ms - s_ap.last_alarm_ms) < s_ap.config.cooldown_ms) {
            return s_ap.current_alarm;
        }
    }

    /* 更新状态 */
    if (desired != ALARM_NONE) {
        s_ap.last_alarm_ms = timestamp_ms;
    }
    s_ap.current_alarm = desired;

    return desired;
}

void dms_alarm_policy_reset(void)
{
    s_ap.current_alarm = ALARM_NONE;
    s_ap.last_alarm_ms = 0;
    log_info("[AlarmPolicy] reset");
}

const char* dms_alarm_level_to_string(dms_alarm_level_t level)
{
    switch (level) {
    case ALARM_NONE:       return "NONE";
    case ALARM_SHORT_BEEP: return "SHORT_BEEP";
    case ALARM_WARNING:    return "WARNING";
    case ALARM_HIGH:       return "HIGH";
    default:               return "UNKNOWN";
    }
}
