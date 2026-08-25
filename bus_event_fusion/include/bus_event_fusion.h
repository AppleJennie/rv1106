#ifndef BUS_EVENT_FUSION_H
#define BUS_EVENT_FUSION_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bus Event Fusion - 公交事件融合引擎
 *
 * 职责：
 *   融合两路输入事件（DMS 驾驶员监测事件 + 车辆运动事件），
 *   在最近 30 秒事件时间线上做时间关联，输出"公交安全事件"。
 *
 * 产品红线（本模块严格遵守）：
 *   - 本系统是安全提示系统，不是处罚系统；
 *     不提供任何 penalty / fine / punishment / deduct / 罚款 / 扣分 / 自动处罚 语义。
 *   - 无足够信息时，责任归因一律 UNKNOWN。
 *   - 驾驶员相关的推断一律使用 SUSPECTED（疑似）措辞，
 *     且所有输出 human_review_required 恒为 true，必须人工复核。
 *   - AI 风险提示不代表最终责任认定。
 *
 * 设计原则：
 *   - 纯 C11，只依赖标准库，不依赖 RV1106 工程任何头文件，可独立编译测试。
 *   - 所有阈值集中在 fusion_config_t，注释标注"仅工程初始值，需实车标定"。
 *   - 无 CAN / 车速信息时系统照常工作（CASE 1 / CASE 2 不依赖车速）。
 */

/* ==================== 输入：DMS 事件类型 ==================== */
typedef enum {
    FUSION_DMS_EYE_CLOSED = 0,   /* 短暂闭眼 */
    FUSION_DMS_LONG_EYE_CLOSED,  /* 长时间闭眼 */
    FUSION_DMS_YAWN,             /* 哈欠 */
    FUSION_DMS_HEAD_DOWN,        /* 低头 */
    FUSION_DMS_FACE_LOST         /* 人脸丢失 */
} fusion_dms_event_type_t;

/* ==================== 输入：DMS 风险等级 ==================== */
typedef enum {
    FUSION_RISK_NORMAL = 0,   /* 正常 */
    FUSION_RISK_ATTENTION,    /* 留意 */
    FUSION_RISK_WARNING,      /* 警告 */
    FUSION_RISK_HIGH          /* 高风险 */
} fusion_risk_level_t;

/* ==================== 输入：车辆运动事件类型 ==================== */
typedef enum {
    FUSION_MOTION_HARD_ACCEL = 0,   /* 急加速 */
    FUSION_MOTION_HARD_BRAKE,       /* 急刹车 */
    FUSION_MOTION_HARD_TURN_LEFT,   /* 急左转 */
    FUSION_MOTION_HARD_TURN_RIGHT,  /* 急右转 */
    FUSION_MOTION_BUMP,             /* 颠簸 */
    FUSION_MOTION_HIGH_LONG_JERK,   /* 纵向冲击（jerk）过大 */
    FUSION_MOTION_HIGH_LAT_JERK     /* 横向冲击（jerk）过大 */
} fusion_motion_event_type_t;

/* ==================== 输出：责任归因 ====================
 * 注意：除 UNKNOWN / DRIVER_ATTENTION 外的取值当前为保留值，
 * 需要未来接入前向感知（行人/路况）等额外数据源后才会产生；
 * 在缺少这些信息时，归因一律 UNKNOWN。
 */
typedef enum {
    ATTRIBUTION_UNKNOWN = 0,          /* 无足够信息（默认，绝不猜测） */
    ATTRIBUTION_PEDESTRIAN_AVOIDANCE, /* 避让行人（保留，需前向感知数据） */
    ATTRIBUTION_TRAFFIC,              /* 交通状况（保留） */
    ATTRIBUTION_DRIVER_ATTENTION,     /* 驾驶员注意力相关（仅以 SUSPECTED 推断出现） */
    ATTRIBUTION_ROAD_CONDITION,       /* 路况因素（保留） */
    ATTRIBUTION_VEHICLE               /* 车辆因素（保留） */
} bus_attribution_t;

/* ==================== 输出：公交安全事件类型 ==================== */
typedef enum {
    BUS_EVENT_NONE = 0,                          /* 无融合输出 */
    BUS_EVENT_EMERGENCY_BRAKE,                   /* CASE 1：急刹车，原因未知 */
    BUS_EVENT_ATTENTION_RELATED_BRAKE_SUSPECTED, /* CASE 2：疑似注意力相关急刹 */
    BUS_EVENT_FATIGUE_HIGH_RISK                  /* CASE 3：车辆行驶中疲劳高风险 */
} bus_safety_event_type_t;

/* ==================== 输入结构 ==================== */

/* DMS 事件输入 */
typedef struct {
    fusion_dms_event_type_t type;
    fusion_risk_level_t     risk_level;
    uint64_t                timestamp_ms;
    uint64_t                duration_ms;
} fusion_dms_event_t;

/* 车辆运动事件输入 */
typedef struct {
    fusion_motion_event_type_t type;
    uint64_t                   timestamp_ms;
    int                        confidence;        /* 运动事件检测置信度 0~100 */
    float                      longitudinal_accel; /* 纵向加速度 m/s^2（无该信息填 0） */
    float                      lateral_accel;      /* 横向加速度 m/s^2（无该信息填 0） */
} fusion_motion_event_t;

/* ==================== 输出结构 ==================== */
typedef struct {
    bus_safety_event_type_t event_type;
    bus_attribution_t       attribution;
    int                     correlation_confidence; /* 时间关联置信度 0~100 */
    char                    evidence[192];  /* 关联证据（时间戳、间隔等，供人工复核） */
    char                    message[256];   /* 人类可读提示；涉及驾驶员必含 SUSPECTED 措辞 */
    bool                    human_review_required; /* 恒为 true：所有输出必须人工复核 */
    uint64_t                timestamp_ms;   /* 输出事件时间戳（= 触发输入的时间戳） */
} bus_safety_event_t;

/* ==================== 阈值配置 ====================
 * 所有阈值集中在此，不散落 magic number。
 * 以下默认值均为"仅工程初始值，需实车标定"。
 */
typedef struct {
    /* 事件时间线窗口长度（毫秒），窗口外的事件不参与关联。默认 30000。仅工程初始值，需实车标定 */
    uint32_t timeline_window_ms;

    /* CASE 2：HEAD_DOWN 之后多少毫秒内出现 HARD_BRAKE 才判定为疑似相关。默认 2000。仅工程初始值，需实车标定 */
    uint32_t head_down_brake_correlate_max_ms;

    /* CASE 2 关联置信度衰减：时间间隔为 0 时的置信度。默认 90。仅工程初始值，需实车标定 */
    int      brake_correlation_conf_near;

    /* CASE 2 关联置信度衰减：时间间隔达到上限时的置信度。默认 40。仅工程初始值，需实车标定 */
    int      brake_correlation_conf_far;

    /* CASE 3 输出置信度。默认 80。仅工程初始值，需实车标定 */
    int      fatigue_high_risk_confidence;
} fusion_config_t;

/* ==================== API ==================== */

/* 使用默认配置初始化 */
bool bus_event_fusion_init(void);

/* 使用自定义配置初始化 */
bool bus_event_fusion_init_with_config(const fusion_config_t *config);

/* 获取默认配置（供外部查看/修改后传给 init_with_config） */
void bus_event_fusion_get_default_config(fusion_config_t *config);

/*
 * 喂入一个 DMS 事件。
 * 若触发融合输出（CASE 3）返回 true 并填充 out；否则返回 false（out 内容为零）。
 */
bool bus_event_fusion_feed_dms(const fusion_dms_event_t *event, bus_safety_event_t *out);

/*
 * 喂入一个车辆运动事件。
 * 若触发融合输出（CASE 1 / CASE 2）返回 true 并填充 out；否则返回 false。
 */
bool bus_event_fusion_feed_motion(const fusion_motion_event_t *event, bus_safety_event_t *out);

/*
 * 更新车速（可选信息源）。
 * 无 CAN / 车速信息时可以不调用，系统照常工作（CASE 1 / CASE 2 不依赖车速；
 * CASE 3 退化为依据时间线内是否存在运动事件判断车辆是否在运动）。
 */
void bus_event_fusion_update_speed(float speed_mps, uint64_t timestamp_ms);

/* 重置所有状态（清空时间线与车速信息；保留配置） */
void bus_event_fusion_reset(void);

/* 字符串转换（调试/日志用） */
const char* bus_safety_event_type_to_string(bus_safety_event_type_t type);
const char* bus_attribution_to_string(bus_attribution_t attribution);

#ifdef __cplusplus
}
#endif

#endif /* BUS_EVENT_FUSION_H */
