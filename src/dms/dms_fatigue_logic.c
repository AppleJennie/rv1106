#include "dms_fatigue_logic.h"
#include "sys_logger.h"

#include <string.h>

/*
 * 阈值说明：
 *  - EAR 需要 68/106 点眼睑关键点才能准确计算；当前 BlazeFace 仅输出 5 点，
 *    因此 EAR 固定为正常值，eye-closed 报警不会触发。
 *  - MAR 使用 mouth_width / face_width 近似，阈值相应调低。
 */
#define EAR_THRESHOLD               0.20f
#define MAR_THRESHOLD               0.28f
#define HEAD_DOWN_SCORE_THRESHOLD   0.65f

#define EAR_DURATION_S              0.8f
#define MAR_DURATION_S              1.0f
#define HEAD_DOWN_DURATION_S        1.5f

bool dms_fatigue_logic_init(dms_fatigue_logic_t *logic, float fps)
{
    if (!logic || fps <= 0.0f) {
        return false;
    }

    memset(logic, 0, sizeof(*logic));

    logic->ear_threshold = EAR_THRESHOLD;
    logic->mar_threshold = MAR_THRESHOLD;
    logic->head_down_score_threshold = HEAD_DOWN_SCORE_THRESHOLD;

    /* 根据 fps 把持续时间转成帧数，至少 1 帧 */
    logic->eye_closed_frames_thresh = (int)(EAR_DURATION_S * fps);
    if (logic->eye_closed_frames_thresh < 1) {
        logic->eye_closed_frames_thresh = 1;
    }

    logic->yawn_frames_thresh = (int)(MAR_DURATION_S * fps);
    if (logic->yawn_frames_thresh < 1) {
        logic->yawn_frames_thresh = 1;
    }

    logic->head_down_frames_thresh = (int)(HEAD_DOWN_DURATION_S * fps);
    if (logic->head_down_frames_thresh < 1) {
        logic->head_down_frames_thresh = 1;
    }

    log_info("DMS 疲劳规则初始化: fps=%.1f, eye_closed_thresh=%d, yawn_thresh=%d, head_down_thresh=%d",
             fps,
             logic->eye_closed_frames_thresh,
             logic->yawn_frames_thresh,
             logic->head_down_frames_thresh);

    return true;
}

const char* dms_fatigue_logic_update(dms_fatigue_logic_t *logic,
                                      float ear,
                                      float mar,
                                      float head_down_score,
                                      int face_found)
{
    if (!logic) {
        return "ERROR";
    }

    /* 未检测到人脸 */
    if (!face_found) {
        logic->eye_closed_counter = 0;
        logic->yawn_counter = 0;
        logic->head_down_counter = 0;
        return "NO_FACE";
    }

    /* EAR 计数器 */
    if (ear < logic->ear_threshold) {
        logic->eye_closed_counter++;
    } else {
        logic->eye_closed_counter = 0;
    }

    /* MAR 计数器 */
    if (mar > logic->mar_threshold) {
        logic->yawn_counter++;
    } else {
        logic->yawn_counter = 0;
    }

    /* 低头计数器 */
    if (head_down_score > logic->head_down_score_threshold) {
        logic->head_down_counter++;
    } else {
        logic->head_down_counter = 0;
    }

    bool eye_closed_active = logic->eye_closed_counter >= logic->eye_closed_frames_thresh;
    bool yawn_active       = logic->yawn_counter       >= logic->yawn_frames_thresh;
    bool head_down_active  = logic->head_down_counter  >= logic->head_down_frames_thresh;

    /* 组合判断：任意两项同时出现视为 FATIGUE */
    int active_count = (eye_closed_active ? 1 : 0) +
                       (yawn_active       ? 1 : 0) +
                       (head_down_active  ? 1 : 0);

    if (active_count >= 2) {
        return "FATIGUE";
    }

    if (eye_closed_active) return "EYE_CLOSED";
    if (yawn_active)       return "YAWN";
    if (head_down_active)  return "HEAD_DOWN";

    return "NORMAL";
}
