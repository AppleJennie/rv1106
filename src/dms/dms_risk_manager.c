#include "dms_risk_manager.h"
#include "sys_logger.h"

#include <string.h>

/* ==================== 内部常量 ==================== */

/* 滑动窗口最大事件数（防止内存无限增长） */
#define MAX_EVENTS_PER_WINDOW   64

/* 事件类型字符串 */
#define EVENT_STR_NONE              "NONE"
#define EVENT_STR_EYE_CLOSED        "EYE_CLOSED"
#define EVENT_STR_LONG_EYE_CLOSED   "LONG_EYE_CLOSED"
#define EVENT_STR_YAWN              "YAWN"
#define EVENT_STR_HEAD_DOWN         "HEAD_DOWN"
#define EVENT_STR_FACE_LOST         "FACE_LOST"
#define EVENT_STR_COMBO             "COMBO_FATIGUE"

/* ==================== 内部数据结构 ==================== */

/* 单个事件记录 */
typedef struct {
    uint64_t timestamp_ms;
    int      event_type;    /* 0=eye_closed, 1=long_eye_closed, 2=yawn, 3=head_down */
} window_event_t;

/* 内部状态 */
typedef struct {
    dms_risk_config_t config;

    /* 当前风险状态 */
    dms_risk_level_t current_level;
    int              current_score;
    char             current_event[32];
    uint64_t         current_duration_ms;

    /* 恢复滞后 */
    uint64_t         recovery_start_ms;  /* 开始恢复正常的时间 */
    bool             in_recovery;        /* 是否处于恢复阶段 */
    dms_risk_level_t pre_recovery_level; /* 恢复前的等级 */

    /* 滑动窗口事件缓冲 */
    window_event_t   events[MAX_EVENTS_PER_WINDOW];
    int              event_head;    /* 环形缓冲写指针 */
    int              event_count;   /* 当前缓冲中的事件数 */

    /* 上一帧的事件状态（用于检测事件边沿） */
    int prev_eye_closed;
    int prev_long_eye_closed;
    int prev_yawn;
    int prev_head_down;

    /* 评分衰减 */
    uint64_t last_decay_ms;

    bool initialized;
} risk_manager_state_t;

static risk_manager_state_t s_rm;

/* ==================== 默认配置 ==================== */

void dms_risk_manager_get_default_config(dms_risk_config_t *config)
{
    if (!config) return;

    memset(config, 0, sizeof(*config));

    /* 滑动窗口 */
    config->window_short_ms = 60000;        /* 60 秒 */
    config->window_long_ms  = 300000;       /* 5 分钟 */

    /* 单次事件直接升级 */
    config->long_eye_closed_warn_ms = 1500;  /* 长闭眼 1.5s → WARNING */
    config->head_down_warn_ms       = 3000;  /* 持续低头 3s → WARNING */

    /* 短窗口（60s）次数阈值 */
    config->yawn_count_attention       = 1;  /* 60s 内 1 次哈欠 → ATTENTION */
    config->yawn_count_warning         = 3;  /* 60s 内 3 次哈欠 → WARNING */
    config->long_eye_closed_count_high = 2;  /* 60s 内 2 次长闭眼 → HIGH */

    /* 长窗口（5min）次数阈值 */
    config->yawn_count_5min_warning       = 5;  /* 5min 内 5 次哈欠 → WARNING */
    config->fatigue_event_count_5min_high = 8;  /* 5min 内 8 次疲劳事件 → HIGH */

    /* 组合风险 */
    config->combo_event_types_warning = 2;   /* 短窗口内 2 种以上事件 → WARNING */

    /* 恢复滞后 */
    config->recovery_hold_ms = 5000;         /* 恢复正常后保持 5 秒 */

    /* 评分权重 */
    config->score_eye_closed      = 5;
    config->score_long_eye_closed = 25;
    config->score_yawn            = 10;
    config->score_head_down       = 15;
    config->score_combo_bonus     = 10;
    config->score_decay_per_sec   = 2;
}

/* ==================== 内部辅助函数 ==================== */

/* 添加事件到环形缓冲 */
static void add_event(int event_type, uint64_t timestamp_ms)
{
    s_rm.events[s_rm.event_head].timestamp_ms = timestamp_ms;
    s_rm.events[s_rm.event_head].event_type   = event_type;
    s_rm.event_head = (s_rm.event_head + 1) % MAX_EVENTS_PER_WINDOW;
    if (s_rm.event_count < MAX_EVENTS_PER_WINDOW) {
        s_rm.event_count++;
    }
}

/* 统计指定时间窗口内某类事件的次数 */
static int count_events_in_window(int event_type, uint64_t now_ms, uint32_t window_ms)
{
    int count = 0;
    uint64_t cutoff = (now_ms > window_ms) ? (now_ms - window_ms) : 0;

    for (int i = 0; i < s_rm.event_count; i++) {
        if (s_rm.events[i].event_type == event_type &&
            s_rm.events[i].timestamp_ms >= cutoff) {
            count++;
        }
    }
    return count;
}

/* 统计短窗口内出现的事件种类数 */
static int count_event_types_in_short_window(uint64_t now_ms)
{
    uint64_t cutoff = (now_ms > s_rm.config.window_short_ms)
                      ? (now_ms - s_rm.config.window_short_ms) : 0;
    bool seen[4] = {false, false, false, false};
    int types = 0;

    for (int i = 0; i < s_rm.event_count; i++) {
        if (s_rm.events[i].timestamp_ms >= cutoff) {
            int t = s_rm.events[i].event_type;
            if (t >= 0 && t < 4 && !seen[t]) {
                seen[t] = true;
                types++;
            }
        }
    }
    return types;
}

/* 统计长窗口内所有疲劳事件总数 */
static int count_all_fatigue_events_in_long_window(uint64_t now_ms)
{
    uint64_t cutoff = (now_ms > s_rm.config.window_long_ms)
                      ? (now_ms - s_rm.config.window_long_ms) : 0;
    int count = 0;

    for (int i = 0; i < s_rm.event_count; i++) {
        if (s_rm.events[i].timestamp_ms >= cutoff) {
            count++;
        }
    }
    return count;
}

/* 评分衰减 */
static void apply_score_decay(uint64_t now_ms)
{
    if (s_rm.last_decay_ms == 0) {
        s_rm.last_decay_ms = now_ms;
        return;
    }

    uint64_t elapsed_ms = now_ms - s_rm.last_decay_ms;
    if (elapsed_ms >= 1000) {
        int decay = (int)(elapsed_ms / 1000) * s_rm.config.score_decay_per_sec;
        s_rm.current_score -= decay;
        if (s_rm.current_score < 0) s_rm.current_score = 0;
        s_rm.last_decay_ms = now_ms;
    }
}

/* 根据评分和事件统计确定风险等级 */
static dms_risk_level_t determine_risk_level(const dms_risk_input_t *input)
{
    uint64_t now = input->timestamp_ms;
    const dms_risk_config_t *cfg = &s_rm.config;

    /* ---- HIGH 条件 ---- */

    /* 短窗口内长闭眼次数达到阈值 */
    int lec_short = count_events_in_window(1, now, cfg->window_short_ms);
    if (lec_short >= cfg->long_eye_closed_count_high) {
        return DMS_RISK_HIGH;
    }

    /* 长窗口内疲劳事件总数达到阈值 */
    int total_long = count_all_fatigue_events_in_long_window(now);
    if (total_long >= cfg->fatigue_event_count_5min_high) {
        return DMS_RISK_HIGH;
    }

    /* ---- WARNING 条件 ---- */

    /* 当前正在长闭眼且持续足够久 */
    if (input->long_eye_closed &&
        input->long_eye_closed_duration_ms >= cfg->long_eye_closed_warn_ms) {
        return DMS_RISK_WARNING;
    }

    /* 当前持续低头 */
    if (input->head_down &&
        input->head_down_duration_ms >= cfg->head_down_warn_ms) {
        return DMS_RISK_WARNING;
    }

    /* 短窗口内哈欠次数 */
    int yawn_short = count_events_in_window(2, now, cfg->window_short_ms);
    if (yawn_short >= cfg->yawn_count_warning) {
        return DMS_RISK_WARNING;
    }

    /* 长窗口内哈欠次数 */
    int yawn_long = count_events_in_window(2, now, cfg->window_long_ms);
    if (yawn_long >= cfg->yawn_count_5min_warning) {
        return DMS_RISK_WARNING;
    }

    /* 组合事件 */
    int combo_types = count_event_types_in_short_window(now);
    if (combo_types >= cfg->combo_event_types_warning) {
        return DMS_RISK_WARNING;
    }

    /* ---- ATTENTION 条件 ---- */

    /* 短窗口内有哈欠 */
    if (yawn_short >= cfg->yawn_count_attention) {
        return DMS_RISK_ATTENTION;
    }

    /* 当前正在长闭眼（但时间还不够 WARNING） */
    if (input->long_eye_closed) {
        return DMS_RISK_ATTENTION;
    }

    /* 当前正在闭眼 */
    if (input->eye_closed) {
        return DMS_RISK_ATTENTION;
    }

    /* 当前正在低头（但时间不够 WARNING） */
    if (input->head_down) {
        return DMS_RISK_ATTENTION;
    }

    return DMS_RISK_NORMAL;
}

/* 应用恢复滞后 */
static dms_risk_level_t apply_recovery_hysteresis(dms_risk_level_t new_level,
                                                    uint64_t now_ms)
{
    /* 如果新等级 >= 当前等级，直接采用（升级不滞后） */
    if (new_level >= s_rm.current_level) {
        s_rm.in_recovery = false;
        return new_level;
    }

    /* 降级需要滞后 */
    if (!s_rm.in_recovery) {
        s_rm.in_recovery = true;
        s_rm.recovery_start_ms = now_ms;
        s_rm.pre_recovery_level = s_rm.current_level;
        return s_rm.current_level;  /* 保持当前等级 */
    }

    /* 检查是否已经保持了足够久 */
    if ((now_ms - s_rm.recovery_start_ms) >= s_rm.config.recovery_hold_ms) {
        s_rm.in_recovery = false;
        return new_level;  /* 允许降级 */
    }

    /* 还在保持期，维持当前等级 */
    return s_rm.current_level;
}

/* 选择主要事件类型字符串 */
static void select_event_string(const dms_risk_input_t *input, dms_risk_level_t level,
                                 char *buf, size_t buf_size)
{
    if (level == DMS_RISK_NORMAL) {
        snprintf(buf, buf_size, "%s", EVENT_STR_NONE);
        return;
    }

    /* 优先级：长闭眼 > 低头 > 哈欠 > 闭眼 */
    if (input->long_eye_closed) {
        snprintf(buf, buf_size, "%s", EVENT_STR_LONG_EYE_CLOSED);
    } else if (input->head_down) {
        snprintf(buf, buf_size, "%s", EVENT_STR_HEAD_DOWN);
    } else if (input->yawn) {
        snprintf(buf, buf_size, "%s", EVENT_STR_YAWN);
    } else if (input->eye_closed) {
        snprintf(buf, buf_size, "%s", EVENT_STR_EYE_CLOSED);
    } else if (!input->face_found) {
        snprintf(buf, buf_size, "%s", EVENT_STR_FACE_LOST);
    } else {
        /* 窗口统计触发的风险 */
        int lec = count_events_in_window(1, input->timestamp_ms, s_rm.config.window_short_ms);
        int yawn = count_events_in_window(2, input->timestamp_ms, s_rm.config.window_short_ms);
        if (lec > 0 && yawn > 0) {
            snprintf(buf, buf_size, "%s", EVENT_STR_COMBO);
        } else if (lec > 0) {
            snprintf(buf, buf_size, "%s", EVENT_STR_LONG_EYE_CLOSED);
        } else if (yawn > 0) {
            snprintf(buf, buf_size, "%s", EVENT_STR_YAWN);
        } else {
            snprintf(buf, buf_size, "%s", EVENT_STR_COMBO);
        }
    }
}

/* ==================== 公开 API ==================== */

bool dms_risk_manager_init(void)
{
    dms_risk_config_t config;
    dms_risk_manager_get_default_config(&config);
    return dms_risk_manager_init_with_config(&config);
}

bool dms_risk_manager_init_with_config(const dms_risk_config_t *config)
{
    if (!config) return false;

    memset(&s_rm, 0, sizeof(s_rm));
    memcpy(&s_rm.config, config, sizeof(dms_risk_config_t));

    s_rm.current_level = DMS_RISK_NORMAL;
    s_rm.current_score = 0;
    s_rm.initialized   = true;

    log_info("[RiskManager] initialized");
    return true;
}

dms_risk_result_t dms_risk_manager_update(const dms_risk_input_t *input)
{
    dms_risk_result_t result;
    memset(&result, 0, sizeof(result));

    if (!s_rm.initialized || !input) {
        result.level = DMS_RISK_NORMAL;
        return result;
    }

    uint64_t now = input->timestamp_ms;

    /* 1. 检测事件边沿，添加到滑动窗口 */
    /* 注意：长闭眼时不重复记录 eye_closed，避免 combo 误判 */
    if (input->eye_closed && !s_rm.prev_eye_closed && !input->long_eye_closed) {
        add_event(0, now);
        s_rm.current_score += s_rm.config.score_eye_closed;
    }
    if (input->long_eye_closed && !s_rm.prev_long_eye_closed) {
        add_event(1, now);
        s_rm.current_score += s_rm.config.score_long_eye_closed;
    }
    if (input->yawn && !s_rm.prev_yawn) {
        add_event(2, now);
        s_rm.current_score += s_rm.config.score_yawn;
    }
    if (input->head_down && !s_rm.prev_head_down) {
        add_event(3, now);
        s_rm.current_score += s_rm.config.score_head_down;
    }

    /* 组合事件加分 */
    int combo = count_event_types_in_short_window(now);
    if (combo >= s_rm.config.combo_event_types_warning) {
        /* 只在新事件进入时加一次组合分，避免每帧都加 */
        if ((input->eye_closed && !s_rm.prev_eye_closed) ||
            (input->long_eye_closed && !s_rm.prev_long_eye_closed) ||
            (input->yawn && !s_rm.prev_yawn) ||
            (input->head_down && !s_rm.prev_head_down)) {
            s_rm.current_score += s_rm.config.score_combo_bonus;
        }
    }

    /* 2. 更新边沿检测 */
    s_rm.prev_eye_closed      = input->eye_closed;
    s_rm.prev_long_eye_closed = input->long_eye_closed;
    s_rm.prev_yawn            = input->yawn;
    s_rm.prev_head_down       = input->head_down;

    /* 3. 评分衰减 */
    apply_score_decay(now);

    /* 限制评分范围 */
    if (s_rm.current_score > 100) s_rm.current_score = 100;
    if (s_rm.current_score < 0)   s_rm.current_score = 0;

    /* 4. 确定风险等级 */
    dms_risk_level_t raw_level = determine_risk_level(input);

    /* 5. 应用恢复滞后 */
    dms_risk_level_t final_level = apply_recovery_hysteresis(raw_level, now);

    /* 6. 更新状态 */
    s_rm.current_level = final_level;

    /* 7. 选择事件字符串 */
    select_event_string(input, final_level, s_rm.current_event, sizeof(s_rm.current_event));

    /* 8. 确定持续时间 */
    if (input->long_eye_closed) {
        s_rm.current_duration_ms = input->long_eye_closed_duration_ms;
    } else if (input->head_down) {
        s_rm.current_duration_ms = input->head_down_duration_ms;
    } else if (input->yawn) {
        s_rm.current_duration_ms = input->yawn_duration_ms;
    } else if (input->eye_closed) {
        s_rm.current_duration_ms = input->eye_closed_duration_ms;
    } else {
        s_rm.current_duration_ms = 0;
    }

    /* 9. 填充结果 */
    result.level               = final_level;
    result.risk_score          = s_rm.current_score;
    result.event_duration_ms   = s_rm.current_duration_ms;
    result.alarm_requested     = (final_level >= DMS_RISK_WARNING);
    result.save_event_requested = (final_level >= DMS_RISK_ATTENTION);
    snprintf(result.event_type, sizeof(result.event_type), "%s", s_rm.current_event);

    return result;
}

void dms_risk_manager_reset(void)
{
    dms_risk_config_t saved_config;
    memcpy(&saved_config, &s_rm.config, sizeof(dms_risk_config_t));

    memset(&s_rm, 0, sizeof(s_rm));
    memcpy(&s_rm.config, &saved_config, sizeof(dms_risk_config_t));

    s_rm.current_level = DMS_RISK_NORMAL;
    s_rm.initialized   = true;

    log_info("[RiskManager] reset");
}

dms_risk_level_t dms_risk_manager_get_level(void)
{
    return s_rm.current_level;
}

int dms_risk_manager_get_score(void)
{
    return s_rm.current_score;
}

void dms_risk_manager_deinit(void)
{
    s_rm.initialized = false;
    log_info("[RiskManager] deinitialized");
}

const char* dms_risk_level_to_string(dms_risk_level_t level)
{
    switch (level) {
    case DMS_RISK_NORMAL:    return "NORMAL";
    case DMS_RISK_ATTENTION: return "ATTENTION";
    case DMS_RISK_WARNING:   return "WARNING";
    case DMS_RISK_HIGH:      return "HIGH";
    default:                 return "UNKNOWN";
    }
}
