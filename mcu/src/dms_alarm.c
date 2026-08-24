#include "dms_alarm.h"

#include <string.h>

/* ==================== 预定义蜂鸣模式 ==================== */

/* NONE：不响 */
static const dms_buzzer_pattern_t PATTERN_NONE = {
    .on_ms = 0, .off_ms = 0, .repeat_count = 0, .pause_ms = 0
};

/* SHORT：短促单次提示（100ms ON） */
static const dms_buzzer_pattern_t PATTERN_SHORT = {
    .on_ms = 100, .off_ms = 0, .repeat_count = 1, .pause_ms = 0
};

/* WARNING：间歇短促（200ms ON, 300ms OFF, 3次, 1000ms 暂停） */
static const dms_buzzer_pattern_t PATTERN_WARNING = {
    .on_ms = 200, .off_ms = 300, .repeat_count = 3, .pause_ms = 1000
};

/* HIGH：间歇急促（100ms ON, 100ms OFF, 5次, 500ms 暂停） */
static const dms_buzzer_pattern_t PATTERN_HIGH = {
    .on_ms = 100, .off_ms = 100, .repeat_count = 5, .pause_ms = 500
};

/* ==================== 内部状态 ==================== */
typedef struct {
    dms_alarm_level_e  level;
    const dms_buzzer_pattern_t *pattern;

    buzzer_state_e     buzzer_state;
    uint32_t           state_start_ms;   /* 当前状态开始时间 */
    uint8_t            current_repeat;   /* 当前重复次数 */

    bool               hw_state;         /* 当前硬件输出状态 */
} alarm_state_t;

static alarm_state_t s_alarm;

/* ==================== 内部辅助 ==================== */

static const dms_buzzer_pattern_t* get_pattern(dms_alarm_level_e level)
{
    switch (level) {
    case DMS_ALARM_SHORT:   return &PATTERN_SHORT;
    case DMS_ALARM_WARNING: return &PATTERN_WARNING;
    case DMS_ALARM_HIGH:    return &PATTERN_HIGH;
    case DMS_ALARM_NONE:
    default:                return &PATTERN_NONE;
    }
}

static void set_buzzer(bool on)
{
    if (s_alarm.hw_state != on) {
        s_alarm.hw_state = on;
        dms_alarm_buzzer_hw_set(on);
    }
}

static void enter_state(buzzer_state_e state, uint32_t now_ms)
{
    s_alarm.buzzer_state   = state;
    s_alarm.state_start_ms = now_ms;

    switch (state) {
    case BUZZER_ON:
        set_buzzer(true);
        break;
    case BUZZER_OFF:
    case BUZZER_PAUSE:
    case BUZZER_IDLE:
    default:
        set_buzzer(false);
        break;
    }
}

/* ==================== 公开 API ==================== */

void dms_alarm_init(void)
{
    memset(&s_alarm, 0, sizeof(s_alarm));
    s_alarm.level        = DMS_ALARM_NONE;
    s_alarm.pattern      = &PATTERN_NONE;
    s_alarm.buzzer_state = BUZZER_IDLE;
    s_alarm.hw_state     = false;
}

void dms_alarm_set_level(dms_alarm_level_e level)
{
    if (s_alarm.level == level) return;

    s_alarm.level   = level;
    s_alarm.pattern = get_pattern(level);

    /* 切换到新模式时重置状态机 */
    s_alarm.current_repeat = 0;

    if (level == DMS_ALARM_NONE) {
        enter_state(BUZZER_IDLE, 0);
    }
    /* 非 NONE 模式在下一次 tick 时启动 */
}

dms_alarm_level_e dms_alarm_get_level(void)
{
    return s_alarm.level;
}

void dms_alarm_tick(uint32_t now_ms)
{
    if (s_alarm.level == DMS_ALARM_NONE || !s_alarm.pattern) {
        if (s_alarm.buzzer_state != BUZZER_IDLE) {
            enter_state(BUZZER_IDLE, now_ms);
        }
        return;
    }

    const dms_buzzer_pattern_t *p = s_alarm.pattern;
    uint32_t elapsed = now_ms - s_alarm.state_start_ms;

    switch (s_alarm.buzzer_state) {

    case BUZZER_IDLE:
        /* 启动报警 */
        s_alarm.current_repeat = 0;
        enter_state(BUZZER_ON, now_ms);
        break;

    case BUZZER_ON:
        if (elapsed >= p->on_ms) {
            s_alarm.current_repeat++;
            if (p->repeat_count > 0 && s_alarm.current_repeat >= p->repeat_count) {
                /* 完成一组，进入暂停 */
                if (p->pause_ms > 0) {
                    enter_state(BUZZER_PAUSE, now_ms);
                } else {
                    /* 无暂停，重新开始 */
                    s_alarm.current_repeat = 0;
                    enter_state(BUZZER_ON, now_ms);
                }
            } else {
                /* 继续当前组 */
                enter_state(BUZZER_OFF, now_ms);
            }
        }
        break;

    case BUZZER_OFF:
        if (elapsed >= p->off_ms) {
            enter_state(BUZZER_ON, now_ms);
        }
        break;

    case BUZZER_PAUSE:
        if (elapsed >= p->pause_ms) {
            /* 暂停结束，重新开始 */
            s_alarm.current_repeat = 0;
            enter_state(BUZZER_ON, now_ms);
        }
        break;

    default:
        enter_state(BUZZER_IDLE, now_ms);
        break;
    }
}

const char* dms_alarm_level_to_string(dms_alarm_level_e level)
{
    switch (level) {
    case DMS_ALARM_NONE:    return "NONE";
    case DMS_ALARM_SHORT:   return "SHORT";
    case DMS_ALARM_WARNING: return "WARNING";
    case DMS_ALARM_HIGH:    return "HIGH";
    default:                return "UNKNOWN";
    }
}
