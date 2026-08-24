#ifndef DMS_RISK_MANAGER_H
#define DMS_RISK_MANAGER_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS Risk Manager - 产品级风险评估模块。
 *
 * 职责：
 *   基于疲劳特征事件（闭眼/长闭眼/哈欠/低头），
 *   在 60 秒和 5 分钟滑动窗口内统计事件频次，
 *   输出统一的风险等级和风险评分。
 *
 * 设计原则：
 *   - 保护司机，不是考核司机
 *   - 单次正常生理行为（眨眼、单次哈欠）不会触发高风险
 *   - 所有阈值集中定义，便于产品调参
 *   - 有滞后恢复机制，避免风险等级抖动
 */

/* ==================== 风险等级 ==================== */
typedef enum {
    DMS_RISK_NORMAL = 0,    /* 无明显风险 */
    DMS_RISK_ATTENTION,     /* 轻度、单次行为，需留意 */
    DMS_RISK_WARNING,       /* 明确风险信号，需警告 */
    DMS_RISK_HIGH           /* 连续/组合高风险，需强警告 */
} dms_risk_level_t;

/* ==================== 风险结果 ==================== */
typedef struct {
    dms_risk_level_t level;
    int              risk_score;            /* 0~100 安全风险分 */
    char             event_type[32];        /* 触发事件类型字符串 */
    uint64_t         event_duration_ms;     /* 当前事件持续时间 */
    bool             alarm_requested;       /* 是否请求报警 */
    bool             save_event_requested;  /* 是否请求保存事件记录 */
} dms_risk_result_t;

/* ==================== 输入事件结构 ==================== */
typedef struct {
    /* 事件标志（本帧是否处于该状态） */
    int face_found;
    int eye_closed;
    int long_eye_closed;
    int yawn;
    int head_down;

    /* 连续特征值 */
    float ear;
    float mar;
    float head_down_score;

    /* 各事件已持续的时间（毫秒），由上层状态机提供 */
    uint64_t eye_closed_duration_ms;
    uint64_t long_eye_closed_duration_ms;
    uint64_t yawn_duration_ms;
    uint64_t head_down_duration_ms;

    /* 当前时间戳（毫秒） */
    uint64_t timestamp_ms;
} dms_risk_input_t;

/* ==================== 阈值配置 ====================
 * 所有时间和次数阈值集中在此，不散落 magic number。
 */
typedef struct {
    /* --- 滑动窗口长度 --- */
    uint32_t window_short_ms;       /* 短窗口（默认 60 秒） */
    uint32_t window_long_ms;        /* 长窗口（默认 300 秒 = 5 分钟） */

    /* --- 单次事件直接升级阈值 --- */
    uint32_t long_eye_closed_warn_ms;    /* 长闭眼持续此时间 → WARNING */
    uint32_t head_down_warn_ms;          /* 持续低头此时间 → WARNING */

    /* --- 短窗口（60s）内事件次数阈值 --- */
    int      yawn_count_attention;       /* 60s 内哈欠次数 → ATTENTION */
    int      yawn_count_warning;         /* 60s 内哈欠次数 → WARNING */
    int      long_eye_closed_count_high; /* 60s 内长闭眼次数 → HIGH */

    /* --- 长窗口（5min）内事件次数阈值 --- */
    int      yawn_count_5min_warning;    /* 5min 内哈欠次数 → WARNING */
    int      fatigue_event_count_5min_high; /* 5min 内疲劳事件总数 → HIGH */

    /* --- 组合风险 --- */
    int      combo_event_types_warning;  /* 短窗口内同时出现的事件种类数 → WARNING */

    /* --- 恢复滞后 --- */
    uint32_t recovery_hold_ms;           /* 恢复正常后保持当前等级的时间 */

    /* --- 风险评分权重 --- */
    int      score_eye_closed;           /* 每次闭眼加分 */
    int      score_long_eye_closed;      /* 每次长闭眼加分 */
    int      score_yawn;                 /* 每次哈欠加分 */
    int      score_head_down;            /* 每次低头加分 */
    int      score_combo_bonus;          /* 组合事件额外加分 */
    int      score_decay_per_sec;        /* 每秒衰减分 */
} dms_risk_config_t;

/* ==================== API ==================== */

/* 使用默认配置初始化 */
bool dms_risk_manager_init(void);

/* 使用自定义配置初始化 */
bool dms_risk_manager_init_with_config(const dms_risk_config_t *config);

/* 获取默认配置（供外部查看/修改后传给 init_with_config） */
void dms_risk_manager_get_default_config(dms_risk_config_t *config);

/*
 * 每帧调用，输入当前事件状态，输出风险评估结果。
 * 该函数内部维护滑动窗口和评分衰减。
 */
dms_risk_result_t dms_risk_manager_update(const dms_risk_input_t *input);

/* 重置所有状态（新 session / 新司机时调用） */
void dms_risk_manager_reset(void);

/* 获取当前风险等级（不推进状态） */
dms_risk_level_t dms_risk_manager_get_level(void);

/* 获取当前风险评分 */
int dms_risk_manager_get_score(void);

/* 反初始化 */
void dms_risk_manager_deinit(void);

/* 风险等级转字符串（调试用） */
const char* dms_risk_level_to_string(dms_risk_level_t level);

#ifdef __cplusplus
}
#endif

#endif
