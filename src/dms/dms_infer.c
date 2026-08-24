#include "dms_infer.h"
#include "dms_face_detect.h"
#include "dms_face_landmark.h"
#include "dms_fatigue_logic.h"
#include "dms_fatigue_features.h"
#include "dms_retinaface.h"
#include "sys_logger.h"

#include <string.h>

#if DMS_HW_PREPROCESS
static dms_infer_hw_timing_t g_hw_timing;
#endif

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
    dms_fatigue_features_reset();

    log_info("DMS 推理模块初始化...");

    if (!dms_face_detect_init(FACE_DETECT_MODEL_PATH)) {
        log_error("DMS 人脸检测模块初始化失败");
        return false;
    }

#if DMS_HW_PREPROCESS
    /*
     * V2-A：用 RetinaFace 真实输入尺寸初始化 RGA 硬件预处理模块。
     * RGA 不可用时 init 返回 false，AI 线程自动回退软件 JPEG 路径。
     */
    {
        int rf_w = 0, rf_h = 0;
        if (dms_retinaface_get_input_size(&rf_w, &rf_h)) {
            if (!dms_rga_preprocess_init(rf_w, rf_h)) {
                log_warn("DMS RGA 硬件预处理不可用，AI 走软件 JPEG 路径");
            }
        } else {
            log_warn("DMS 无法获取 RetinaFace 输入尺寸，RGA 预处理未启用");
        }
    }
#endif

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
#if DMS_ENABLE_LANDMARK_106
        dms_face_landmark_106_deinit();
#endif
#if DMS_ENABLE_HEADPOSE
        dms_face_landmark_deinit();
#endif
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
    dms_image_t reused_img;
    memset(&reused_img, 0, sizeof(reused_img));
#endif

    /* 1. 人脸检测；106 开启时复用同一张已解码 RGB 图，避免重复 JPEG decode。 */
#if DMS_ENABLE_LANDMARK_106
    if (!dms_face_detect_process_ex(frame, &face_result, &reused_img)) {
        log_error("DMS 人脸检测推理失败");
        if (reused_img.data) {
            dms_free_image(&reused_img);
        }
        return false;
    }
#else
    if (!dms_face_detect_process(frame, &face_result)) {
        log_error("DMS 人脸检测推理失败");
        return false;
    }
#endif

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
    if (!dms_face_landmark_106_process_image(&reused_img, &face_result, &landmark_106_result)) {
        log_error("DMS 106 点关键点推理失败");
        if (reused_img.data) {
            dms_free_image(&reused_img);
        }
        return false;
    }
    result->landmark_106 = landmark_106_result;
    if (reused_img.data) {
        dms_free_image(&reused_img);
    }
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
    /* 不启用旧 headpose；106 开启时用 106 点计算 EAR/MAR/head_down_score。 */
    result->ear = 0.0f;
    result->mar = 0.0f;
    result->head_down_score = 0.0f;
    result->yaw = result->pitch = result->roll = 0.0f;

    if (result->face_found) {
        snprintf(result->status, sizeof(result->status), "FACE");
    } else {
        snprintf(result->status, sizeof(result->status), "NO_FACE");
    }

    dms_fatigue_features_update(result);
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

#if DMS_HW_PREPROCESS
    dms_rga_preprocess_deinit();
#endif

    memset(&g_fatigue_logic, 0, sizeof(g_fatigue_logic));
    dms_fatigue_features_reset();

    g_infer_mock = true;
    g_infer_inited = false;

    log_info("DMS 推理模块已关闭");
}

bool dms_infer_is_mock_mode(void)
{
    return g_infer_mock;
}

#if DMS_HW_PREPROCESS
/*
 * V2-A 硬件预处理路径。
 * 与 dms_infer_process_frame() 的差异仅在取图方式：
 *   - RetinaFace：直接用 RGA 产出的 640x640 BGR（stretch），不做 JPEG 解码/CPU resize。
 *   - 106 点：源图为 RGA 产出的全分辨率 RGB（与已验收软件路径完全等价），
 *     crop 仍用 CPU dms_crop_and_resize，compute_loose_crop 逻辑不变，坐标无需缩放。
 * 疲劳特征/状态机调用与软件路径完全一致。
 */
bool dms_infer_process_prepared(const dms_prepared_frame_t *prep, dms_result_t *result)
{
    if (!g_infer_inited || !result || !prep || !prep->retina_bgr) {
        return false;
    }

    memset(result, 0, sizeof(*result));

    /* 一次性 dump 调试：SD 卡存在 /mnt/sdcard/dms/DUMP_RGA 时本帧 dump */
    bool do_dump = dms_hw_dump_check_and_consume();

    dms_face_detect_result_t face_result;

    /* 1. 人脸检测（RGA 预处理输入，无软件 JPEG 解码） */
    if (!dms_retinaface_process_prepared(prep->retina_bgr,
                                         prep->orig_w, prep->orig_h,
                                         &face_result)) {
        log_error("DMS 人脸检测推理失败 (prepared)");
        return false;
    }

    if (do_dump) {
        char name[128];
        snprintf(name, sizeof(name), "rga_source_%dx%d.ppm", prep->src_w, prep->src_h);
        dms_hw_dump_ppm(name, prep->src_rgb, prep->src_w, prep->src_h, false);
        snprintf(name, sizeof(name), "retina_in_%dx%d.ppm", prep->retina_w, prep->retina_h);
        dms_hw_dump_ppm(name, prep->retina_bgr, prep->retina_w, prep->retina_h, true);
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

    if (face_result.found) {
        log_info("FACE_DETECT_OK face=1 score=%.3f bbox=%d,%d,%d,%d (hw)",
                 face_result.score, face_result.x, face_result.y,
                 face_result.w, face_result.h);
    }

#if DMS_ENABLE_LANDMARK_106
    dms_face_landmark_106_result_t landmark_106_result;
    memset(&landmark_106_result, 0, sizeof(landmark_106_result));

    if (prep->src_rgb && prep->src_w > 0 && prep->src_h > 0) {
        /*
         * 全分辨率 RGB 源图（与软件路径 stb 解码结果等价），
         * 人脸 bbox 与 106 结果点都在原图坐标系，无需缩放映射。
         * 仅包装，不持有内存：process_image 内部 crop 自行 malloc/free。
         */
        dms_image_t src_img;
        memset(&src_img, 0, sizeof(src_img));
        src_img.width = prep->src_w;
        src_img.height = prep->src_h;
        src_img.channels = 3;
        src_img.data = (uint8_t *)prep->src_rgb;

        if (!dms_face_landmark_106_process_image(&src_img, &face_result, &landmark_106_result)) {
            log_error("DMS 106 点关键点推理失败 (prepared)");
            return false;
        }

        if (do_dump && landmark_106_result.found) {
            const uint8_t *lm_in = dms_face_landmark_106_last_input();
            if (lm_in) {
                dms_hw_dump_ppm("lm106_crop_192x192.ppm", lm_in, 192, 192, false);
            }
        }

        result->landmark_106 = landmark_106_result;
        g_hw_timing.landmark106_us += landmark_106_result.total_us;
    }
#endif

    /* 2. 疲劳特征更新（与软件路径一致） */
    result->ear = 0.0f;
    result->mar = 0.0f;
    result->head_down_score = 0.0f;
    result->yaw = result->pitch = result->roll = 0.0f;

    if (result->face_found) {
        snprintf(result->status, sizeof(result->status), "FACE");
    } else {
        snprintf(result->status, sizeof(result->status), "NO_FACE");
    }

    dms_fatigue_features_update(result);
    g_hw_timing.fatigue_feature_us += result->feature_cost_us;
    g_hw_timing.frames++;

    return true;
}

void dms_infer_get_hw_timing(dms_infer_hw_timing_t *out)
{
    if (!out) {
        return;
    }
    *out = g_hw_timing;
}

void dms_infer_reset_hw_timing(void)
{
    memset(&g_hw_timing, 0, sizeof(g_hw_timing));
}
#endif /* DMS_HW_PREPROCESS */
