#ifndef DMS_ALARM_H
#define DMS_ALARM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS Alarm - MCU 侧蜂鸣器非阻塞控制模块。
 *
 * 使用系统 tick + 状态机控制蜂鸣节奏。
 * 禁止使用 delay_ms() 阻塞 MCU。
 *
 * 报警级别：
 *   DMS_ALARM_NONE    - 不报警
 *   DMS_ALARM_SHORT   - 短促提示音（单次）
 *   DMS_ALARM_WARNING - 警告音（间歇短促）
 *   DMS_ALARM_HIGH    - 高危警报（间歇急促）
 */

/* ==================== 报警级别 ==================== */
typedef enum {
    DMS_ALARM_NONE = 0,
    DMS_ALARM_SHORT,
    DMS_ALARM_WARNING,
    DMS_ALARM_HIGH,
} dms_alarm_level_e;

/* ==================== 蜂鸣器模式 ==================== */
typedef struct {
    uint16_t on_ms;         /* 蜂鸣 ON 时间 */
    uint16_t off_ms;        /* 蜂鸣 OFF 时间 */
    uint8_t  repeat_count;  /* 重复次数（0 = 无限循环） */
    uint16_t pause_ms;      /* 每组之间的暂停时间 */
} dms_buzzer_pattern_t;

/* ==================== 蜂鸣器状态 ==================== */
typedef enum {
    BUZZER_IDLE = 0,
    BUZZER_ON,
    BUZZER_OFF,
    BUZZER_PAUSE,
} buzzer_state_e;

/* ==================== API ==================== */

/* 初始化蜂鸣器模块 */
void dms_alarm_init(void);

/*
 * 设置报警级别。
 * 根据级别选择对应的蜂鸣模式。
 */
void dms_alarm_set_level(dms_alarm_level_e level);

/* 获取当前报警级别 */
dms_alarm_level_e dms_alarm_get_level(void);

/*
 * 周期性调用（建议每 10~50ms）。
 * 驱动蜂鸣器状态机。
 *
 * @param now_ms  当前系统毫秒 tick
 *
 * 该函数内部会调用 dms_alarm_buzzer_hw_set() 控制实际 GPIO。
 */
void dms_alarm_tick(uint32_t now_ms);

/*
 * 蜂鸣器硬件控制接口（平台相关，需要外部实现）。
 * 在 MCU 工程中由 BSP 层实现。
 *
 * @param on  true = 蜂鸣器 ON, false = 蜂鸣器 OFF
 */
extern void dms_alarm_buzzer_hw_set(bool on);

/* 报警级别转字符串 */
const char* dms_alarm_level_to_string(dms_alarm_level_e level);

#ifdef __cplusplus
}
#endif

#endif
