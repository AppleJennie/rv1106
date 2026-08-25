/*
 * bus_event_fusion.c - 公交事件融合引擎实现
 *
 * 重要产品声明：
 *   本模块输出的一切事件均为"AI 安全风险提示"，不代表最终责任认定。
 *   - 无足够信息时归因一律 UNKNOWN，绝不猜测；
 *   - 涉及驾驶员的推断一律使用 SUSPECTED（疑似）措辞；
 *   - 所有输出 human_review_required 恒为 true，必须人工复核；
 *   - 本系统不提供任何处罚类语义（无 penalty / fine / 罚款 / 扣分 概念）。
 */

#include "bus_event_fusion.h"

#include <string.h>
#include <stdio.h>

/* ==================== 内部常量 ==================== */

/* 时间线环形缓冲容量（仅限制内存上限；30 秒窗口由时间戳 cutoff 保证） */
#define MAX_TIMELINE_EVENTS 128

/* 时间线事件来源 */
#define SRC_DMS    0
#define SRC_MOTION 1

/* ==================== 内部数据结构 ==================== */

/* 时间线中的单条事件记录（DMS 与运动事件共用） */
typedef struct {
    int      source;            /* SRC_DMS / SRC_MOTION */
    int      type;              /* fusion_dms_event_type_t / fusion_motion_event_type_t */
    uint64_t timestamp_ms;
    uint64_t duration_ms;       /* 仅 DMS 事件有效 */
    int      risk_level;        /* 仅 DMS 事件有效 */
    int      confidence;        /* 仅运动事件有效 */
    float    longitudinal_accel;
    float    lateral_accel;
} timeline_event_t;

/* 模块内部状态 */
typedef struct {
    fusion_config_t  config;

    /* 最近 30 秒事件时间线（环形缓冲） */
    timeline_event_t events[MAX_TIMELINE_EVENTS];
    int              head;   /* 写指针 */
    int              count;  /* 当前缓冲中的事件数 */

    /* 车速（可选信息源，无 CAN 时保持 invalid） */
    bool             speed_valid;
    float            speed_mps;
    uint64_t         speed_timestamp_ms;

    bool             initialized;
} fusion_state_t;

static fusion_state_t s_fusion;

/* ==================== 默认配置 ==================== */

void bus_event_fusion_get_default_config(fusion_config_t *config)
{
    if (!config) return;

    memset(config, 0, sizeof(*config));

    /* 以下均为仅工程初始值，需实车标定 */
    config->timeline_window_ms               = 30000; /* 30 秒事件时间线 */
    config->head_down_brake_correlate_max_ms = 2000;  /* 低头后 0~2 秒内急刹才疑似相关 */
    config->brake_correlation_conf_near      = 90;    /* 间隔为 0 时置信度 */
    config->brake_correlation_conf_far       = 40;    /* 间隔达到上限时置信度 */
    config->fatigue_high_risk_confidence     = 80;    /* 疲劳高风险置信度 */
}

/* ==================== 内部辅助函数 ==================== */

/* 添加事件到时间线环形缓冲 */
static void timeline_add(const timeline_event_t *ev)
{
    s_fusion.events[s_fusion.head] = *ev;
    s_fusion.head = (s_fusion.head + 1) % MAX_TIMELINE_EVENTS;
    if (s_fusion.count < MAX_TIMELINE_EVENTS) {
        s_fusion.count++;
    }
}

/* 计算 30 秒窗口的截止时间戳（窗口外的事件不参与关联） */
static uint64_t timeline_cutoff(uint64_t now_ms)
{
    uint32_t window = s_fusion.config.timeline_window_ms;
    return (now_ms >= window) ? (now_ms - window) : 0;
}

/*
 * 在时间线窗口内查找 now_ms 之前最近一次 HEAD_DOWN，
 * 且满足 0 <= (now_ms - head_down_ts) <= max_gap_ms。
 * 找到返回 true 并通过 out_gap_ms / out_event 输出间隔与事件。
 * 注意时序方向：只向前回溯（head_down 必须先于当前事件），反向不关联。
 */
static bool find_recent_head_down(uint64_t now_ms, uint32_t max_gap_ms,
                                  uint64_t *out_gap_ms, timeline_event_t *out_event)
{
    uint64_t cutoff = timeline_cutoff(now_ms);
    bool     found  = false;
    uint64_t best_gap = 0;

    for (int i = 0; i < s_fusion.count; i++) {
        const timeline_event_t *ev = &s_fusion.events[i];
        if (ev->source != SRC_DMS || ev->type != FUSION_DMS_HEAD_DOWN) {
            continue;
        }
        if (ev->timestamp_ms < cutoff || ev->timestamp_ms > now_ms) {
            continue; /* 窗口外或未来事件（反向）不参与关联 */
        }
        uint64_t gap = now_ms - ev->timestamp_ms;
        if (gap > max_gap_ms) {
            continue;
        }
        /* 取时间间隔最小（最接近）的一次低头 */
        if (!found || gap < best_gap) {
            found    = true;
            best_gap = gap;
            if (out_event) *out_event = *ev;
        }
    }

    if (found && out_gap_ms) *out_gap_ms = best_gap;
    return found;
}

/* 判断时间线窗口内是否存在车辆运动事件（用于推断"车辆仍在运动"） */
static bool has_motion_event_in_window(uint64_t now_ms)
{
    uint64_t cutoff = timeline_cutoff(now_ms);

    for (int i = 0; i < s_fusion.count; i++) {
        const timeline_event_t *ev = &s_fusion.events[i];
        if (ev->source == SRC_MOTION &&
            ev->timestamp_ms >= cutoff && ev->timestamp_ms <= now_ms) {
            return true;
        }
    }
    return false;
}

/* 判断车辆是否在运动：优先车速，其次时间线内的运动事件；两者都没有时返回 false */
static bool vehicle_is_moving(uint64_t now_ms)
{
    if (s_fusion.speed_valid && s_fusion.speed_mps > 0.0f) {
        return true;
    }
    return has_motion_event_in_window(now_ms);
}

/* CASE 2 关联置信度：随低头→急刹时间间隔线性衰减 */
static int brake_correlation_confidence(uint64_t gap_ms)
{
    const fusion_config_t *cfg = &s_fusion.config;
    if (cfg->head_down_brake_correlate_max_ms == 0) {
        return cfg->brake_correlation_conf_near;
    }
    int span = cfg->brake_correlation_conf_near - cfg->brake_correlation_conf_far;
    int conf = cfg->brake_correlation_conf_near
             - span * (int)gap_ms / (int)cfg->head_down_brake_correlate_max_ms;
    if (conf < 0)   conf = 0;
    if (conf > 100) conf = 100;
    return conf;
}

/* 填充输出事件的公共字段（human_review_required 恒为 true） */
static void fill_common(bus_safety_event_t *out, bus_safety_event_type_t type,
                        bus_attribution_t attribution, int confidence,
                        uint64_t timestamp_ms)
{
    memset(out, 0, sizeof(*out));
    out->event_type            = type;
    out->attribution           = attribution;
    out->correlation_confidence = confidence;
    out->human_review_required = true; /* 所有输出必须人工复核 */
    out->timestamp_ms          = timestamp_ms;
}

/* ==================== 公开 API ==================== */

bool bus_event_fusion_init(void)
{
    fusion_config_t config;
    bus_event_fusion_get_default_config(&config);
    return bus_event_fusion_init_with_config(&config);
}

bool bus_event_fusion_init_with_config(const fusion_config_t *config)
{
    if (!config) return false;

    memset(&s_fusion, 0, sizeof(s_fusion));
    memcpy(&s_fusion.config, config, sizeof(fusion_config_t));
    s_fusion.initialized = true;
    return true;
}

void bus_event_fusion_reset(void)
{
    fusion_config_t saved;
    memcpy(&saved, &s_fusion.config, sizeof(fusion_config_t));

    memset(&s_fusion, 0, sizeof(s_fusion));
    memcpy(&s_fusion.config, &saved, sizeof(fusion_config_t));
    s_fusion.initialized = true;
}

void bus_event_fusion_update_speed(float speed_mps, uint64_t timestamp_ms)
{
    if (!s_fusion.initialized) return;
    s_fusion.speed_valid        = true;
    s_fusion.speed_mps          = speed_mps;
    s_fusion.speed_timestamp_ms = timestamp_ms;
}

bool bus_event_fusion_feed_dms(const fusion_dms_event_t *event, bus_safety_event_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!s_fusion.initialized || !event) return false;

    /* 1. 进入时间线 */
    timeline_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.source       = SRC_DMS;
    ev.type         = (int)event->type;
    ev.timestamp_ms = event->timestamp_ms;
    ev.duration_ms  = event->duration_ms;
    ev.risk_level   = (int)event->risk_level;
    timeline_add(&ev);

    /* 2. CASE 3：长时间闭眼 + 车辆仍在运动 → 疲劳高风险 */
    if (event->type == FUSION_DMS_LONG_EYE_CLOSED &&
        vehicle_is_moving(event->timestamp_ms)) {
        if (!out) return true;

        fill_common(out, BUS_EVENT_FATIGUE_HIGH_RISK,
                    ATTRIBUTION_DRIVER_ATTENTION,
                    s_fusion.config.fatigue_high_risk_confidence,
                    event->timestamp_ms);

        if (s_fusion.speed_valid) {
            snprintf(out->evidence, sizeof(out->evidence),
                     "long_eye_closed@%llums(duration=%llums); vehicle_moving=true"
                     "(speed=%.1f m/s, motion_in_window=%d)",
                     (unsigned long long)event->timestamp_ms,
                     (unsigned long long)event->duration_ms,
                     (double)s_fusion.speed_mps,
                     has_motion_event_in_window(event->timestamp_ms) ? 1 : 0);
        } else {
            snprintf(out->evidence, sizeof(out->evidence),
                     "long_eye_closed@%llums(duration=%llums); vehicle_moving=true"
                     "(speed=unavailable, motion_in_window=%d)",
                     (unsigned long long)event->timestamp_ms,
                     (unsigned long long)event->duration_ms,
                     has_motion_event_in_window(event->timestamp_ms) ? 1 : 0);
        }

        /* SUSPECTED 措辞：疑似推断，不代表最终责任认定，必须人工复核 */
        snprintf(out->message, sizeof(out->message),
                 "SUSPECTED：检测到长时间闭眼且车辆仍在运动，疑似疲劳驾驶高风险"
                 "（SUSPECTED，疑似推断，非最终认定），请立即提醒驾驶员注意安全，"
                 "并由人工复核；AI 安全提示不代表最终责任认定。");
        return true;
    }

    /* 其他 DMS 事件（眨眼/哈欠/低头/人脸丢失）本身不产生融合输出 */
    return false;
}

bool bus_event_fusion_feed_motion(const fusion_motion_event_t *event, bus_safety_event_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!s_fusion.initialized || !event) return false;

    /* 1. 进入时间线 */
    timeline_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.source            = SRC_MOTION;
    ev.type              = (int)event->type;
    ev.timestamp_ms      = event->timestamp_ms;
    ev.confidence        = event->confidence;
    ev.longitudinal_accel = event->longitudinal_accel;
    ev.lateral_accel     = event->lateral_accel;
    timeline_add(&ev);

    /* 2. 仅 HARD_BRAKE 触发 CASE 1 / CASE 2，其余运动事件只作为时间线证据 */
    if (event->type != FUSION_MOTION_HARD_BRAKE) {
        return false;
    }

    /* CASE 2：低头后 0~2 秒内的急刹 → 疑似注意力相关（SUSPECTED，非认定） */
    uint64_t         gap_ms = 0;
    timeline_event_t hd;
    if (find_recent_head_down(event->timestamp_ms,
                              s_fusion.config.head_down_brake_correlate_max_ms,
                              &gap_ms, &hd)) {
        if (!out) return true;

        fill_common(out, BUS_EVENT_ATTENTION_RELATED_BRAKE_SUSPECTED,
                    ATTRIBUTION_DRIVER_ATTENTION,
                    brake_correlation_confidence(gap_ms),
                    event->timestamp_ms);

        snprintf(out->evidence, sizeof(out->evidence),
                 "head_down@%llums(duration=%llums); hard_brake@%llums"
                 "(conf=%d,long_accel=%.2f); gap=%llums<=%ums",
                 (unsigned long long)hd.timestamp_ms,
                 (unsigned long long)hd.duration_ms,
                 (unsigned long long)event->timestamp_ms,
                 event->confidence,
                 (double)event->longitudinal_accel,
                 (unsigned long long)gap_ms,
                 s_fusion.config.head_down_brake_correlate_max_ms);

        /* SUSPECTED 措辞：单次急刹 + 低头的时序接近只是疑似线索，绝不直接归责 */
        snprintf(out->message, sizeof(out->message),
                 "SUSPECTED：急刹前 %llu ms 检测到驾驶员低头，疑似与驾驶员注意力相关"
                 "（SUSPECTED，疑似推断，非最终认定），需人工复核；"
                 "AI 安全提示不代表最终责任认定。",
                 (unsigned long long)gap_ms);
        return true;
    }

    /* CASE 1：无低头关联的急刹 → EMERGENCY_BRAKE，归因 UNKNOWN，绝不归责驾驶员 */
    if (!out) return true;

    fill_common(out, BUS_EVENT_EMERGENCY_BRAKE,
                ATTRIBUTION_UNKNOWN, 0, event->timestamp_ms);

    snprintf(out->evidence, sizeof(out->evidence),
             "hard_brake@%llums(conf=%d,long_accel=%.2f,lat_accel=%.2f); "
             "no_head_down_within_%ums",
             (unsigned long long)event->timestamp_ms,
             event->confidence,
             (double)event->longitudinal_accel,
             (double)event->lateral_accel,
             s_fusion.config.head_down_brake_correlate_max_ms);

    snprintf(out->message, sizeof(out->message),
             "检测到急刹车事件，当前无足够信息判断原因（UNKNOWN）；"
             "可能由避让、路况、交通状况等多种因素引起，需人工复核；"
             "AI 安全提示不代表最终责任认定。");
    return true;
}

/* ==================== 字符串转换 ==================== */

const char* bus_safety_event_type_to_string(bus_safety_event_type_t type)
{
    switch (type) {
    case BUS_EVENT_NONE:                            return "NONE";
    case BUS_EVENT_EMERGENCY_BRAKE:                 return "EMERGENCY_BRAKE";
    case BUS_EVENT_ATTENTION_RELATED_BRAKE_SUSPECTED: return "ATTENTION_RELATED_BRAKE_SUSPECTED";
    case BUS_EVENT_FATIGUE_HIGH_RISK:               return "FATIGUE_HIGH_RISK";
    default:                                        return "UNKNOWN";
    }
}

const char* bus_attribution_to_string(bus_attribution_t attribution)
{
    switch (attribution) {
    case ATTRIBUTION_UNKNOWN:             return "UNKNOWN";
    case ATTRIBUTION_PEDESTRIAN_AVOIDANCE: return "PEDESTRIAN_AVOIDANCE";
    case ATTRIBUTION_TRAFFIC:             return "TRAFFIC";
    case ATTRIBUTION_DRIVER_ATTENTION:    return "DRIVER_ATTENTION";
    case ATTRIBUTION_ROAD_CONDITION:      return "ROAD_CONDITION";
    case ATTRIBUTION_VEHICLE:             return "VEHICLE";
    default:                              return "UNKNOWN";
    }
}
