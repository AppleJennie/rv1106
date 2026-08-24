#include "dms_infer.h"
#include "dms_face_detect.h"
#include "dms_face_landmark.h"
#include "dms_fatigue_logic.h"
#include "sys_logger.h"

#include <string.h>

#define FACE_DETECT_MODEL_PATH       "/mnt/sdcard/dms/models/retinaface.rknn"
#define FACE_LANDMARK_MODEL_PATH     "/mnt/sdcard/dms/models/face_landmark.rknn"
#define FACE_LANDMARK_106_MODEL_PATH "/mnt/sdcard/dms/models/2d106det.rknn"

static bool            g_infer_inited = false;
static bool            g_infer_mock = true;
static dms_fatigue_logic_t g_fatigue_logic;

bool dms_infer_init(void)
{
    if (g_infer_inited) {
        return true;
    }

    memset(&g_fatigue_logic, 0, sizeof(g_fatigue_logic));

    log_info("DMS 推理模块初始化...");

    if (!dms_face_detect_init(FACE_DETECT_MODEL_PATH)) {
        log_error("DMS 人脸检测模块初始化失败");
        return false;
    }

#if DMS_ENABLE_HEADPOSE
    if (!dms_face_landmark_init(FACE_LANDMARK_MODEL_PATH)) {
        log_error("DMS 人脸关键点模块初始化失败");
        dms_face_detect_deinit();
        return false;
    }
#endif

#if DMS_ENABLE_LANDMARK_106
    if (!dms_face_landmark_106_init(FACE_LANDMARK_106_MODEL_PATH)) {
        log_error("DMS 106 点关键点模块初始化失败");
        dms_face_detect_deinit();
#if DMS_ENABLE_HEADPOSE
        dms_face_landmark_deinit();
#endif
        return false;
    }
#endif

    /* 任一模型缺失都进入 mock 模式；headpose/landmark106 禁用时只看人脸检测 */
    g_infer_mock = dms_face_detect_is_mock();
#if DMS_ENABLE_HEADPOSE
    g_infer_mock = g_infer_mock || dms_face_landmark_is_mock();
#endif
#if DMS_ENABLE_LANDMARK_106
    g_infer_mock = g_infer_mock || dms_face_landmark_106_is_mock();
#endif

    if (g_infer_mock) {
        log_warn("DMS RKNN model missing, run in mock mode");
    } else {
        log_info("DMS RKNN 模型加载成功，进入真实推理模式");
    }

    /* 疲劳规则状态机，按 CAMERA_FPS 初始化 */
    if (!dms_fatigue_logic_init(&g_fatigue_logic, (float)CAMERA_FPS)) {
        log_error("DMS 疲劳规则状态机初始化失败");
        dms_face_landmark_deinit();
        dms_face_detect_deinit();
        return false;
    }

    g_infer_inited = true;
    log_info("DMS 推理模块初始化完成");
    return true;
}

bool dms_infer_process_frame(const frame_data_t *frame, dms_result_t *result)
{
    if (!g_infer_inited || !result) {
        return false;
    }

    memset(result, 0, sizeof(*result));

    dms_face_detect_result_t face_result;
#if DMS_ENABLE_HEADPOSE
    dms_face_landmark_result_t landmark_result;
#endif
#if DMS_ENABLE_LANDMARK_106
    dms_face_landmark_106_result_t landmark_106_result;
#endif

    /* 1. 人脸检测 */
    if (!dms_face_detect_process(frame, &face_result)) {
        log_error("DMS 人脸检测推理失败");
        return false;
    }

    result->face_found = face_result.found;
    result->face_score = face_result.score;
    result->face_x = face_result.x;
    result->face_y = face_result.y;
    result->face_w = face_result.w;
    result->face_h = face_result.h;

    for (int k = 0; k < 10; k++) {
        result->face_kpt[k] = face_result.kpt[k];
    }

#if DMS_ENABLE_LANDMARK_106
    if (!dms_face_landmark_106_process(frame, &face_result, &landmark_106_result)) {
        log_error("DMS 106 点关键点推理失败");
        return false;
    }
    result->landmark_106 = landmark_106_result;
#endif

#if DMS_ENABLE_HEADPOSE
    /* 2. 人脸关键点检测 */
    if (!dms_face_landmark_process(frame, &face_result, &landmark_result)) {
        log_error("DMS 人脸关键点推理失败");
        return false;
    }

    if (landmark_result.found) {
        result->ear = landmark_result.ear;
        result->mar = landmark_result.mar;
        result->head_down_score = landmark_result.head_down_score;
        result->yaw = landmark_result.yaw;
        result->pitch = landmark_result.pitch;
        result->roll = landmark_result.roll;
    } else {
        result->ear = 0.0f;
        result->mar = 0.0f;
        result->head_down_score = 0.0f;
        result->yaw = result->pitch = result->roll = 0.0f;
    }

    /* 3. 疲劳规则判断 */
    const char *status = dms_fatigue_logic_update(&g_fatigue_logic,
                                                   result->ear,
                                                   result->mar,
                                                   result->head_down_score,
                                                   result->face_found);
    snprintf(result->status, sizeof(result->status), "%s", status);

    result->eye_closed = (strcmp(status, "EYE_CLOSED") == 0 || strcmp(status, "FATIGUE") == 0) ? 1 : 0;
    result->yawn       = (strcmp(status, "YAWN") == 0       || strcmp(status, "FATIGUE") == 0) ? 1 : 0;
    result->head_down  = (strcmp(status, "HEAD_DOWN") == 0  || strcmp(status, "FATIGUE") == 0) ? 1 : 0;
    result->fatigue    = (strcmp(status, "FATIGUE") == 0) ? 1 : 0;
#else
    /* Phase 3A：只验证 BlazeFace，不调用 headpose，不输出疲劳状态 */
    result->ear = 0.30f;
    result->mar = 0.20f;
    result->head_down_score = 0.0f;
    result->yaw = result->pitch = result->roll = 0.0f;

    if (result->face_found) {
        snprintf(result->status, sizeof(result->status), "FACE");
    } else {
        snprintf(result->status, sizeof(result->status), "NO_FACE");
    }
#endif

    return true;
}

void dms_infer_deinit(void)
{
    if (!g_infer_inited) {
        return;
    }

#if DMS_ENABLE_HEADPOSE
    dms_face_landmark_deinit();
#endif
#if DMS_ENABLE_LANDMARK_106
    dms_face_landmark_106_deinit();
#endif
    dms_face_detect_deinit();

    memset(&g_fatigue_logic, 0, sizeof(g_fatigue_logic));

    g_infer_mock = true;
    g_infer_inited = false;

    log_info("DMS 推理模块已关闭");
}

bool dms_infer_is_mock_mode(void)
{
    return g_infer_mock;
}
