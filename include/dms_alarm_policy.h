#ifndef DMS_ALARM_POLICY_H
#define DMS_ALARM_POLICY_H

#include "common.h"
#include "dms_risk_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS Alarm Policy - 本地报警策略模块。
 *
 * 根据 Risk Manager 输出的风险等级和事件类型，
 * 决定是否需要蜂鸣器报警以及报警级别。
 *
 * 当前版本只输出报警级别，不直接控制 GPIO。
 * 后续由 MCU 执行实际蜂鸣。
 */

/* ==================== 报警级别 ==================== */
typedef enum {
    ALARM_NONE = 0,         /* 不报警 */
    ALARM_SHORT_BEEP,       /* 短促提示音（单次） */
    ALARM_WARNING,          /* 警告音（连续短促） */
    ALARM_HIGH              /* 高危警报（连续急促） */
} dms_alarm_level_t;

/* ==================== 报警策略配置 ==================== */
typedef struct {
    /* 各风险等级对应的报警级别 */
    dms_alarm_level_t alarm_for_attention;   /* ATTENTION 时的报警级别 */
    dms_alarm_level_t alarm_for_warning;     /* WARNING 时的报警级别 */
    dms_alarm_level_t alarm_for_high;        /* HIGH 时的报警级别 */

    /* 特定事件的报警覆盖 */
    dms_alarm_level_t alarm_for_long_eye_closed;  /* 长闭眼直接报警级别 */
    dms_alarm_level_t alarm_for_head_down;        /* 持续低头报警级别 */

    /* 报警冷却时间（防止连续蜂鸣） */
    uint32_t cooldown_ms;                     /* 两次报警之间的最小间隔 */
} dms_alarm_policy_config_t;

/* ==================== API ==================== */

/* 使用默认配置初始化 */
bool dms_alarm_policy_init(void);

/* 使用自定义配置初始化 */
bool dms_alarm_policy_init_with_config(const dms_alarm_policy_config_t *config);

/* 获取默认配置 */
void dms_alarm_policy_get_default_config(dms_alarm_policy_config_t *config);

/*
 * 根据风险评估结果更新报警策略。
 * 返回当前应执行的报警级别。
 *
 * 该函数内部处理冷却时间，防止连续蜂鸣。
 */
dms_alarm_level_t dms_alarm_policy_update(const dms_risk_result_t *risk_result,
                                           uint64_t timestamp_ms);

/* 重置报警状态 */
void dms_alarm_policy_reset(void);

/* 报警级别转字符串（调试用） */
const char* dms_alarm_level_to_string(dms_alarm_level_t level);

#ifdef __cplusplus
}
#endif

#endif
