#include "dms_face_detect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <time.h>

#include "sys_logger.h"
#include "dms_image_utils.h"
#include "dms_retinaface.h"

#define FACE_DETECT_MODEL_PATH "/mnt/sdcard/dms/models/retinaface.rknn"

static bool g_initialized = false;

static uint64_t get_mono_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

bool dms_face_detect_init(const char *model_path)
{
    const char *path = (model_path && model_path[0]) ? model_path : FACE_DETECT_MODEL_PATH;
    g_initialized = false;

    if (!dms_retinaface_init(path)) {
        log_error("dms_face_detect: RetinaFace 初始化失败");
        return false;
    }

    g_initialized = true;
    log_info("dms_face_detect: RetinaFace 人脸检测初始化成功");
    return true;
}

void dms_face_detect_deinit(void)
{
    dms_retinaface_deinit();
    g_initialized = false;
}

bool dms_face_detect_is_mock(void)
{
    /* RetinaFace 初始化失败时上层应视为不可用，不进入 mock。 */
    return !g_initialized;
}

bool dms_face_detect_process(const frame_data_t *frame, dms_face_detect_result_t *result)
{
    if (!result) return false;
    memset(result, 0, sizeof(*result));

    if (!g_initialized) {
        log_warn("dms_face_detect: 未初始化");
        return false;
    }

    if (!frame || !frame->img_data || frame->img_size == 0) {
        log_warn("dms_face_detect: 输入帧为空");
        return false;
    }

    dms_image_t img = {0};
    uint64_t t_jpeg_0 = get_mono_time_us();
    bool dec_ok = dms_decode_jpeg((const uint8_t *)frame->img_data, (size_t)frame->img_size, &img);
    uint64_t t_jpeg_1 = get_mono_time_us();
    dms_retinaface_set_last_jpeg_decode_us(t_jpeg_1 - t_jpeg_0);

    if (!dec_ok) {
        log_warn("dms_face_detect: JPEG 解码失败");
        return false;
    }

    if (img.channels != 3) {
        log_error("dms_face_detect: 只支持 RGB 输入，当前 channels=%d", img.channels);
        dms_free_image(&img);
        return false;
    }

    dms_face_detect_result_t rf_result;
    memset(&rf_result, 0, sizeof(rf_result));
    bool ok = dms_retinaface_process(&img, &rf_result);

    if (ok && rf_result.found) {
        *result = rf_result;
        log_info("FACE_DETECT_OK face=1 score=%.3f bbox=%d,%d,%d,%d",
                 result->score, result->x, result->y, result->w, result->h);
    } else if (ok) {
        result->found = 0;
        log_info("FACE_DETECT_OK no_face");
    }

    dms_free_image(&img);
    return ok;
}
