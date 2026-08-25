/*
 * vehicle_motion.c - 车辆运动事件状态机实现
 *
 * 每个事件独立跟踪：进入阈值 / 退出阈值（滞回）/ 最短持续 / 冷却防抖。
 * 主状态取当前激活事件中优先级最高者：
 *   HARD_BRAKE > HARD_ACCEL > HARD_TURN_LEFT > HARD_TURN_RIGHT
 *   > BUMP > HIGH_LONG_JERK > HIGH_LAT_JERK
 * （减速类对站立乘客安全影响最大，故急刹优先级最高；该顺序为工程
 *   设计选择，可按运营需求调整。）
 *
 * 置信度融合（仅描述检测可信度，与责任无关）：
 *   急刹 + CAN(刹车踏板 ON 且车速快速下降) → conf_can_agree
 *   急刹 + CAN 信号矛盾                     → conf_can_conflict
 *   急刹 + 无 CAN                           → conf_imu_only（独立判定）
 *   急加速同理（油门开度佐证）；其余事件无 CAN 融合逻辑。
 *
 * 详见 vehicle_motion.h 头部注释。仅依赖 C 标准库 + libm。
 */
#include "vehicle_motion.h"

#include <math.h>
#include <string.h>

/* ==================== 内部数据结构 ==================== */

/* 条件数量：事件枚举 1..7 各对应一个跟踪条件 */
#define COND_COUNT 7

/* 单个事件的跟踪状态 */
typedef struct {
    bool     active;             /* 事件已确认激活 */
    bool     cond_tracking;      /* 进入条件持续满足计时中 */
    uint32_t cond_since_ms;      /* 进入条件满足的起始时刻 */
    uint32_t cooldown_until_ms;  /* 冷却截止时刻（之前禁止再次激活） */
} cond_track_t;

typedef struct {
    vehicle_motion_config_t cfg;
    cond_track_t tr[COND_COUNT];

    /* 车速历史（减速佐证用） */
    float    prev_speed_kph;
    uint32_t prev_speed_ts;
    bool     prev_speed_valid;

    bool initialized;
} motion_state_t;

static motion_state_t s_mo;

/* 事件优先级表（越靠前优先级越高） */
static const vehicle_motion_event_t s_priority[COND_COUNT] = {
    VEHICLE_HARD_BRAKE,
    VEHICLE_HARD_ACCEL,
    VEHICLE_HARD_TURN_LEFT,
    VEHICLE_HARD_TURN_RIGHT,
    VEHICLE_BUMP,
    VEHICLE_HIGH_LONG_JERK,
    VEHICLE_HIGH_LAT_JERK
};

/* ==================== 默认配置 ==================== */

void vehicle_motion_get_default_config(vehicle_motion_config_t *config)
{
    if (!config) return;

    memset(config, 0, sizeof(*config));

    /* 仅工程初始值，需实车标定；不是公交处罚标准 */
    config->hard_accel_enter_ms2   = 1.5f;
    config->hard_accel_exit_ms2    = 1.0f;
    config->hard_accel_min_ms      = 300;
    config->hard_accel_cooldown_ms = 1000;

    config->hard_brake_enter_ms2   = -2.0f;
    config->hard_brake_exit_ms2    = -1.2f;
    config->hard_brake_min_ms      = 300;
    config->hard_brake_cooldown_ms = 1000;

    config->hard_turn_enter_ms2    = 1.8f;
    config->hard_turn_exit_ms2     = 1.2f;
    config->hard_turn_min_ms       = 300;
    config->hard_turn_cooldown_ms  = 1000;

    config->bump_enter_ms2         = 2.5f;
    config->bump_exit_ms2          = 1.5f;
    config->bump_min_ms            = 20;
    config->bump_cooldown_ms       = 500;

    config->high_jerk_enter_ms3    = 2.5f;
    config->high_jerk_exit_ms3     = 1.5f;
    config->high_jerk_min_ms       = 50;
    config->high_jerk_cooldown_ms  = 500;

    config->conf_imu_only           = 0.6f;
    config->conf_can_agree          = 0.9f;
    config->conf_can_conflict       = 0.4f;
    config->conf_other_event        = 0.7f;
    config->brake_speed_drop_kph_s  = 2.0f;
    config->accel_pedal_confirm_pct = 5.0f;
}

/* ==================== 内部辅助函数 ==================== */

/* now 是否早于 deadline（有符号差比较，容忍时间戳回绕） */
static bool time_before(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) < 0;
}

/* 计算某事件的进入/退出条件及时间参数 */
static void eval_cond(vehicle_motion_event_t ev,
                      const vehicle_motion_input_t *in,
                      const vehicle_motion_config_t *c,
                      bool *enter_met, bool *exit_met,
                      uint32_t *min_ms, uint32_t *cooldown_ms)
{
    switch (ev) {
    case VEHICLE_HARD_ACCEL:
        *enter_met = in->longitudinal > c->hard_accel_enter_ms2;
        *exit_met  = in->longitudinal < c->hard_accel_exit_ms2;
        *min_ms = c->hard_accel_min_ms;
        *cooldown_ms = c->hard_accel_cooldown_ms;
        break;
    case VEHICLE_HARD_BRAKE:
        *enter_met = in->longitudinal < c->hard_brake_enter_ms2;
        *exit_met  = in->longitudinal > c->hard_brake_exit_ms2;
        *min_ms = c->hard_brake_min_ms;
        *cooldown_ms = c->hard_brake_cooldown_ms;
        break;
    case VEHICLE_HARD_TURN_LEFT:
        *enter_met = in->lateral > c->hard_turn_enter_ms2;
        *exit_met  = in->lateral < c->hard_turn_exit_ms2;
        *min_ms = c->hard_turn_min_ms;
        *cooldown_ms = c->hard_turn_cooldown_ms;
        break;
    case VEHICLE_HARD_TURN_RIGHT:
        *enter_met = in->lateral < -c->hard_turn_enter_ms2;
        *exit_met  = in->lateral > -c->hard_turn_exit_ms2;
        *min_ms = c->hard_turn_min_ms;
        *cooldown_ms = c->hard_turn_cooldown_ms;
        break;
    case VEHICLE_BUMP:
        *enter_met = fabsf(in->vertical_accel) > c->bump_enter_ms2;
        *exit_met  = fabsf(in->vertical_accel) < c->bump_exit_ms2;
        *min_ms = c->bump_min_ms;
        *cooldown_ms = c->bump_cooldown_ms;
        break;
    case VEHICLE_HIGH_LONG_JERK:
        *enter_met = fabsf(in->longitudinal_jerk) > c->high_jerk_enter_ms3;
        *exit_met  = fabsf(in->longitudinal_jerk) < c->high_jerk_exit_ms3;
        *min_ms = c->high_jerk_min_ms;
        *cooldown_ms = c->high_jerk_cooldown_ms;
        break;
    case VEHICLE_HIGH_LAT_JERK:
        *enter_met = fabsf(in->lateral_jerk) > c->high_jerk_enter_ms3;
        *exit_met  = fabsf(in->lateral_jerk) < c->high_jerk_exit_ms3;
        *min_ms = c->high_jerk_min_ms;
        *cooldown_ms = c->high_jerk_cooldown_ms;
        break;
    default:
        *enter_met = false;
        *exit_met = true;
        *min_ms = 0;
        *cooldown_ms = 0;
        break;
    }
}

/* 更新单个事件的跟踪状态机 */
static void update_cond(cond_track_t *t, bool enter_met, bool exit_met,
                        uint32_t min_ms, uint32_t cooldown_ms,
                        uint32_t now_ms, bool *entered)
{
    *entered = false;

    if (t->active) {
        if (exit_met) {
            t->active = false;
            t->cooldown_until_ms = now_ms + cooldown_ms;
        }
        return;
    }

    /* 冷却期内：暂停条件计时，防止抖动重复触发 */
    if (time_before(now_ms, t->cooldown_until_ms)) {
        t->cond_tracking = false;
        return;
    }

    if (enter_met) {
        if (!t->cond_tracking) {
            t->cond_tracking = true;
            t->cond_since_ms = now_ms;
        }
        if ((uint32_t)(now_ms - t->cond_since_ms) >= min_ms) {
            t->active = true;
            t->cond_tracking = false;
            *entered = true;
        }
    } else {
        t->cond_tracking = false;
    }
}

/* ==================== 对外接口 ==================== */

int vehicle_motion_init(const vehicle_motion_config_t *config)
{
    vehicle_motion_config_t cfg;
    if (config) {
        cfg = *config;
    } else {
        vehicle_motion_get_default_config(&cfg);
    }

    /* 参数合法性检查 */
    if (cfg.conf_imu_only < 0.0f || cfg.conf_imu_only > 1.0f) return -1;
    if (cfg.conf_can_agree < 0.0f || cfg.conf_can_agree > 1.0f) return -1;
    if (cfg.conf_can_conflict < 0.0f || cfg.conf_can_conflict > 1.0f) return -1;
    if (cfg.conf_other_event < 0.0f || cfg.conf_other_event > 1.0f) return -1;

    memset(&s_mo, 0, sizeof(s_mo));
    s_mo.cfg = cfg;
    s_mo.initialized = true;
    return 0;
}

void vehicle_motion_update(const vehicle_motion_input_t *in,
                           const vehicle_can_state_t *can,
                           vehicle_motion_output_t *out)
{
    if (!out) return;

    memset(out, 0, sizeof(*out));
    out->state = VEHICLE_NORMAL;
    out->entered_event = VEHICLE_NORMAL;

    if (!s_mo.initialized || !in) return;

    const vehicle_motion_config_t *c = &s_mo.cfg;
    uint32_t now = in->timestamp_ms;

    /* 车速下降速率（急刹佐证）：仅在本帧携带有效车速时更新 */
    bool speed_dropping = false;
    if (can && (can->valid_flags & VEHICLE_CAN_VALID_SPEED)) {
        if (s_mo.prev_speed_valid) {
            uint32_t dms = now - s_mo.prev_speed_ts;
            if (dms > 0) {
                float rate = (s_mo.prev_speed_kph - can->speed_kph) * 1000.0f / (float)dms;
                speed_dropping = (rate >= c->brake_speed_drop_kph_s);
            }
        }
        s_mo.prev_speed_kph = can->speed_kph;
        s_mo.prev_speed_ts = now;
        s_mo.prev_speed_valid = true;
    }

    /* 逐事件更新跟踪状态机 */
    bool entered[COND_COUNT];
    for (int i = 0; i < COND_COUNT; i++) {
        vehicle_motion_event_t ev = (vehicle_motion_event_t)(i + 1);
        bool enter_met, exit_met;
        uint32_t min_ms, cooldown_ms;
        eval_cond(ev, in, c, &enter_met, &exit_met, &min_ms, &cooldown_ms);
        update_cond(&s_mo.tr[i], enter_met, exit_met, min_ms, cooldown_ms,
                    now, &entered[i]);
    }

    /* 汇总激活/边沿 flags */
    for (int i = 0; i < COND_COUNT; i++) {
        if (s_mo.tr[i].active) {
            out->active_flags |= (1u << (unsigned int)(i + 1));
        }
        if (entered[i]) {
            out->entered_flags |= (1u << (unsigned int)(i + 1));
        }
    }

    /* 主状态 / 本拍新确认事件代表：按优先级取 */
    for (int p = 0; p < COND_COUNT; p++) {
        int idx = (int)s_priority[p] - 1;
        if (s_mo.tr[idx].active) {
            out->state = s_priority[p];
            break;
        }
    }
    for (int p = 0; p < COND_COUNT; p++) {
        int idx = (int)s_priority[p] - 1;
        if (entered[idx]) {
            out->entered_event = s_priority[p];
            break;
        }
    }

    /* 置信度融合（NORMAL 时恒为 0，仅对事件状态有意义） */
    switch (out->state) {
    case VEHICLE_HARD_BRAKE: {
        bool has = can
                && (can->valid_flags & VEHICLE_CAN_VALID_BRAKE)
                && (can->valid_flags & VEHICLE_CAN_VALID_SPEED);
        if (!has) {
            out->confidence = c->conf_imu_only;      /* 纯 IMU 独立判定 */
        } else if (can->brake_pedal && speed_dropping) {
            out->confidence = c->conf_can_agree;     /* CAN 佐证一致 */
            out->can_corroborated = true;
        } else {
            out->confidence = c->conf_can_conflict;  /* CAN 信号矛盾 */
        }
        break;
    }
    case VEHICLE_HARD_ACCEL: {
        bool has = can && (can->valid_flags & VEHICLE_CAN_VALID_ACCEL);
        if (!has) {
            out->confidence = c->conf_imu_only;
        } else if (can->accelerator_pedal >= c->accel_pedal_confirm_pct) {
            out->confidence = c->conf_can_agree;
            out->can_corroborated = true;
        } else {
            out->confidence = c->conf_can_conflict;
        }
        break;
    }
    case VEHICLE_NORMAL:
        break;
    default:
        /* 转弯/颠簸/jerk 类事件暂无 CAN 佐证逻辑 */
        out->confidence = c->conf_other_event;
        break;
    }
}

void vehicle_motion_reset(void)
{
    if (!s_mo.initialized) return;

    vehicle_motion_config_t keep = s_mo.cfg;
    memset(&s_mo, 0, sizeof(s_mo));
    s_mo.cfg = keep;
    s_mo.initialized = true;
}

const char *vehicle_motion_event_name(vehicle_motion_event_t event)
{
    switch (event) {
    case VEHICLE_NORMAL:         return "NORMAL";
    case VEHICLE_HARD_ACCEL:     return "HARD_ACCEL";
    case VEHICLE_HARD_BRAKE:     return "HARD_BRAKE";
    case VEHICLE_HARD_TURN_LEFT: return "HARD_TURN_LEFT";
    case VEHICLE_HARD_TURN_RIGHT:return "HARD_TURN_RIGHT";
    case VEHICLE_BUMP:           return "BUMP";
    case VEHICLE_HIGH_LONG_JERK: return "HIGH_LONG_JERK";
    case VEHICLE_HIGH_LAT_JERK:  return "HIGH_LAT_JERK";
    default:                     return "UNKNOWN";
    }
}
