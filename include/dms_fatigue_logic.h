#ifndef DMS_FATIGUE_LOGIC_H
#define DMS_FATIGUE_LOGIC_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 疲劳规则状态机。
 * 根据 EAR / MAR / 低头分数的连续帧数判断状态。
 */

typedef struct {
    /* 阈值配置 */
    float ear_threshold;
    float mar_threshold;
    float head_down_score_threshold;

    int   eye_closed_frames_thresh;   /* EAR 低于阈值持续帧数 */
    int   yawn_frames_thresh;         /* MAR 高于阈值持续帧数 */
    int   head_down_frames_thresh;    /* 低头持续帧数 */

    /* 当前连续计数 */
    int   eye_closed_counter;
    int   yawn_counter;
    int   head_down_counter;
} dms_fatigue_logic_t;

/* 初始化，fps 用于把秒数转换为帧数 */
bool dms_fatigue_logic_init(dms_fatigue_logic_t *logic, float fps);

/*
 * 输入当前帧的 EAR / MAR / head_down_score / face_found，
 * 返回状态字符串：NORMAL / NO_FACE / EYE_CLOSED / YAWN / HEAD_DOWN / FATIGUE
 */
const char* dms_fatigue_logic_update(dms_fatigue_logic_t *logic,
                                      float ear,
                                      float mar,
                                      float head_down_score,
                                      int face_found);

#ifdef __cplusplus
}
#endif

#endif
