#ifndef DMS_EVENT_LOGGER_H
#define DMS_EVENT_LOGGER_H

#include "common.h"
#include "dms_risk_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS Event Logger - 事件日志模块。
 *
 * 将风险事件写入 CSV 文件，支持事件快照保存接口。
 * 文件路径：/mnt/sdcard/dms/events/events.csv
 *
 * 注意：NORMAL 状态的事件不记录。
 */

/* ==================== 事件记录结构 ==================== */
typedef struct {
    uint64_t timestamp_ms;          /* 事件时间戳 */
    char     event_type[32];        /* 事件类型字符串 */
    dms_risk_level_t risk_level;    /* 风险等级 */
    int      risk_score;            /* 风险评分 */
    uint64_t duration_ms;           /* 事件持续时间 */

    float    ear;                   /* 当前 EAR 值 */
    float    ear_baseline;          /* EAR 个人基线 */
    float    mar;                   /* 当前 MAR 值 */
    float    mar_baseline;          /* MAR 个人基线 */
    float    head_down_score;       /* 低头分数 */
    float    face_score;            /* 人脸检测置信度 */

    /* 预留字段，当前无数据时填 0 或空 */
    float    vehicle_speed;         /* 车速（预留） */
    char     route_id[32];          /* 线路 ID（预留） */
    char     driver_id[32];         /* 司机 ID（预留） */
    char     shift_id[32];          /* 班次 ID（预留） */
} dms_event_record_t;

/* ==================== API ==================== */

/*
 * 初始化 Event Logger。
 * 创建 /mnt/sdcard/dms/events/ 目录，打开/创建 events.csv。
 * 如果文件为空则写入 CSV header。
 */
bool dms_event_logger_init(void);

/*
 * 写入一条事件记录到 CSV。
 * risk_level 为 DMS_RISK_NORMAL 时直接忽略（不记录）。
 */
bool dms_event_logger_write(const dms_event_record_t *record);

/*
 * 保存事件快照图片（接口定义，当前不实现具体保存逻辑）。
 * 文件名格式：YYYYMMDD_HHMMSS_EVENT_TYPE.jpg
 * 由调用方提供 JPEG 数据。
 *
 * 注意：NORMAL 帧绝对不保存。
 */
bool dms_event_logger_save_snapshot(const char *event_type,
                                    const uint8_t *jpeg,
                                    size_t jpeg_size);

/* 刷新缓冲区到磁盘 */
void dms_event_logger_flush(void);

/* 关闭并释放资源 */
void dms_event_logger_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
