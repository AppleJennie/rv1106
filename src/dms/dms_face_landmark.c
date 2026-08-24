#include "dms_face_landmark.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <time.h>

#include "sys_logger.h"
#include "dms_image_utils.h"
#include "rknn_api.h"

#define HEADPOSE_INPUT_W  56
#define HEADPOSE_INPUT_H  56
#define HEADPOSE_MAX_OUTPUTS 4
#define HEADPOSE_BIN_NUM  66

#define HEADPOSE_PITCH_DOWN_THRESHOLD 25.0f /* degrees */

typedef struct {
    rknn_context ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr input_attr;
    rknn_tensor_attr output_attrs[HEADPOSE_MAX_OUTPUTS];

    int model_w;
    int model_h;

    rknn_tensor_mem *input_mem;
    rknn_tensor_mem *output_mems[HEADPOSE_MAX_OUTPUTS];

    bool mock;
    bool fatal_error;      /* 出现致命 RKNN 错误后永久禁用 */
    bool fatal_logged;
} headpose_ctx_t;

static headpose_ctx_t g_hp = { 0 };

static uint64_t get_mono_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static inline float dequantize_int8(int8_t q, int32_t zp, float scale)
{
    return ((float)q - (float)zp) * scale;
}

static void dump_tensor_attr(const char *prefix, const rknn_tensor_attr *attr)
{
    char dims_str[128] = {0};
    int pos = 0;
    for (uint32_t i = 0; i < attr->n_dims && i < 4; i++) {
        pos += snprintf(dims_str + pos, sizeof(dims_str) - pos, "%d", attr->dims[i]);
        if (i + 1 < attr->n_dims && i + 1 < 4) {
            pos += snprintf(dims_str + pos, sizeof(dims_str) - pos, ",");
        }
    }
    log_info("%s: name=%s dims=[%s] n_elems=%u size=%u size_with_stride=%u fmt=%s type=%s qnt=%s zp=%d scale=%f",
             prefix,
             attr->name,
             dims_str,
             attr->n_elems,
             attr->size,
             attr->size_with_stride,
             get_format_string(attr->fmt),
             get_type_string(attr->type),
             get_qnt_type_string(attr->qnt_type),
             attr->zp,
             attr->scale);
}

static bool allocate_io_mem(void)
{
    g_hp.input_attr.index = 0;
    int ret = rknn_query(g_hp.ctx, RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR, &g_hp.input_attr, sizeof(g_hp.input_attr));
    if (ret != RKNN_SUCC) {
        log_error("rknn_query headpose native NHWC input attr 失败: %d", ret);
        return false;
    }
    dump_tensor_attr("headpose input", &g_hp.input_attr);

    g_hp.input_attr.type = RKNN_TENSOR_UINT8;
    g_hp.input_attr.fmt = RKNN_TENSOR_NHWC;

    g_hp.input_mem = rknn_create_mem(g_hp.ctx, g_hp.input_attr.size_with_stride);
    if (!g_hp.input_mem) {
        log_error("rknn_create_mem headpose input 失败");
        return false;
    }

    ret = rknn_set_io_mem(g_hp.ctx, g_hp.input_mem, &g_hp.input_attr);
    if (ret != RKNN_SUCC) {
        log_error("rknn_set_io_mem headpose input 失败: %d", ret);
        return false;
    }

    for (uint32_t i = 0; i < g_hp.io_num.n_output; i++) {
        memset(&g_hp.output_attrs[i], 0, sizeof(g_hp.output_attrs[i]));
        g_hp.output_attrs[i].index = i;
        ret = rknn_query(g_hp.ctx, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR, &g_hp.output_attrs[i], sizeof(g_hp.output_attrs[i]));
        if (ret != RKNN_SUCC) {
            log_error("rknn_query headpose native NHWC output attr %d 失败: %d", i, ret);
            return false;
        }
        dump_tensor_attr("headpose output", &g_hp.output_attrs[i]);

        g_hp.output_mems[i] = rknn_create_mem(g_hp.ctx, g_hp.output_attrs[i].size_with_stride);
        if (!g_hp.output_mems[i]) {
            log_error("rknn_create_mem headpose output %d 失败", i);
            return false;
        }

        ret = rknn_set_io_mem(g_hp.ctx, g_hp.output_mems[i], &g_hp.output_attrs[i]);
        if (ret != RKNN_SUCC) {
            log_error("rknn_set_io_mem headpose output %d 失败: %d", i, ret);
            return false;
        }
    }
    return true;
}

bool dms_face_landmark_init(const char *model_path)
{
    memset(&g_hp, 0, sizeof(g_hp));

    if (!model_path || access(model_path, F_OK) != 0) {
        log_warn("headpose 模型不存在: %s, 使用 mock", model_path ? model_path : "(null)");
        g_hp.mock = true;
        return true;
    }

    int ret = rknn_init(&g_hp.ctx, (char *)model_path, 0, 0, NULL);
    if (ret < 0) {
        log_error("rknn_init headpose 失败: %d", ret);
        g_hp.mock = true;
        return true;
    }

    ret = rknn_query(g_hp.ctx, RKNN_QUERY_IN_OUT_NUM, &g_hp.io_num, sizeof(g_hp.io_num));
    if (ret != RKNN_SUCC || g_hp.io_num.n_input != 1) {
        log_error("headpose 模型输入输出数量异常: in=%d out=%d",
                  g_hp.io_num.n_input, g_hp.io_num.n_output);
        goto fail;
    }
    log_info("headpose model input num: %d, output num: %d",
             g_hp.io_num.n_input, g_hp.io_num.n_output);

    if (!allocate_io_mem()) {
        goto fail;
    }

    if (g_hp.input_attr.n_dims >= 4) {
        g_hp.model_h = (int)g_hp.input_attr.dims[1];
        g_hp.model_w = (int)g_hp.input_attr.dims[2];
    } else {
        g_hp.model_w = HEADPOSE_INPUT_W;
        g_hp.model_h = HEADPOSE_INPUT_H;
    }

    log_info("headpose 模型: %dx%d", g_hp.model_w, g_hp.model_h);
    log_info("headpose 模型初始化成功");
    return true;

fail:
    dms_face_landmark_deinit();
    g_hp.mock = true;
    return true;
}

void dms_face_landmark_deinit(void)
{
    rknn_context tmp_ctx = g_hp.ctx;

    /* 先释放 tensor memory，再 destroy context，顺序不能反。 */
    if (g_hp.input_mem) {
        rknn_destroy_mem(tmp_ctx, g_hp.input_mem);
        /* rknn_create_mem 创建的 mem 由 rknn_destroy_mem 内部 free，不再重复 free。 */
        g_hp.input_mem = NULL;
    }
    for (uint32_t i = 0; i < g_hp.io_num.n_output; i++) {
        if (g_hp.output_mems[i]) {
            rknn_destroy_mem(tmp_ctx, g_hp.output_mems[i]);
            g_hp.output_mems[i] = NULL;
        }
    }

    if (tmp_ctx != 0) {
        rknn_destroy(tmp_ctx);
        g_hp.ctx = 0;
    }

    memset(&g_hp, 0, sizeof(g_hp));
}

bool dms_face_landmark_is_mock(void)
{
    return g_hp.mock;
}

static void preprocess_face_crop(const dms_image_t *src,
                                 int crop_x, int crop_y, int crop_w, int crop_h)
{
    dms_image_t crop = { 0 };
    if (!dms_crop_and_resize(src, crop_x, crop_y, crop_w, crop_h,
                             g_hp.model_w, g_hp.model_h, &crop)) {
        log_error("人脸裁剪缩放失败");
        return;
    }

    if (crop.channels == 3) {
        if (!dms_rgb_to_gray(&crop)) {
            log_error("RGB 转灰度失败");
            dms_free_image(&crop);
            return;
        }
    }

    memcpy(g_hp.input_mem->virt_addr, crop.data, (size_t)g_hp.model_w * g_hp.model_h);
    dms_free_image(&crop);
}

static void softmax(const float *src, int n, float *dst)
{
    float max_val = src[0];
    for (int i = 1; i < n; i++) {
        if (src[i] > max_val) max_val = src[i];
    }
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        dst[i] = expf(src[i] - max_val);
        sum += dst[i];
    }
    if (sum > 0.0f) {
        for (int i = 0; i < n; i++) {
            dst[i] /= sum;
        }
    }
}

static float expectation_over_bins(const float *probs, int n)
{
    /* Bins cover [-99, +99] degrees with step 3 degrees. */
    float angle = 0.0f;
    for (int i = 0; i < n; i++) {
        float bin_center = (float)i * 3.0f - 99.0f + 1.5f;
        angle += probs[i] * bin_center;
    }
    return angle;
}

static bool parse_headpose_outputs(dms_face_landmark_result_t *result)
{
    result->found = 1;
    result->angles_valid = 0;
    result->landmarks_3d_num = 0;

    /* Heuristic: if we have 3 outputs each with HEADPOSE_BIN_NUM elements,
       treat them as yaw/pitch/roll binned classifiers. */
    if (g_hp.io_num.n_output == 3) {
        int all_bins = 1;
        for (uint32_t i = 0; i < 3; i++) {
            if (g_hp.output_attrs[i].n_elems != (uint32_t)HEADPOSE_BIN_NUM) {
                all_bins = 0;
                break;
            }
        }

        if (all_bins) {
            float logits[3][HEADPOSE_BIN_NUM];
            float probs[3][HEADPOSE_BIN_NUM];

            for (int i = 0; i < 3; i++) {
                int32_t zp = g_hp.output_attrs[i].zp;
                float scale = g_hp.output_attrs[i].scale;
                if (g_hp.output_attrs[i].type == RKNN_TENSOR_FLOAT32) {
                    float *f = (float *)g_hp.output_mems[i]->virt_addr;
                    for (int j = 0; j < HEADPOSE_BIN_NUM; j++) logits[i][j] = f[j];
                } else {
                    int8_t *q = (int8_t *)g_hp.output_mems[i]->virt_addr;
                    for (int j = 0; j < HEADPOSE_BIN_NUM; j++) logits[i][j] = dequantize_int8(q[j], zp, scale);
                }
                softmax(logits[i], HEADPOSE_BIN_NUM, probs[i]);
            }

            result->yaw = expectation_over_bins(probs[0], HEADPOSE_BIN_NUM);
            result->pitch = expectation_over_bins(probs[1], HEADPOSE_BIN_NUM);
            result->roll = expectation_over_bins(probs[2], HEADPOSE_BIN_NUM);
            result->angles_valid = 1;
            return true;
        }
    }

    /* Fallback: if each output has 198 elements, store as raw 3D landmarks. */
    int total_landmarks = 0;
    for (uint32_t i = 0; i < g_hp.io_num.n_output && total_landmarks < DMS_HEADPOSE_LANDMARK_NUM; i++) {
        uint32_t elems = g_hp.output_attrs[i].n_elems;
        int32_t zp = g_hp.output_attrs[i].zp;
        float scale = g_hp.output_attrs[i].scale;
        if (g_hp.output_attrs[i].type == RKNN_TENSOR_FLOAT32) {
            float *f = (float *)g_hp.output_mems[i]->virt_addr;
            for (uint32_t j = 0; j < elems && total_landmarks < DMS_HEADPOSE_LANDMARK_NUM; j++, total_landmarks++) {
                result->landmarks_3d[total_landmarks * 3 + 0] = f[j];
            }
        } else {
            int8_t *q = (int8_t *)g_hp.output_mems[i]->virt_addr;
            for (uint32_t j = 0; j < elems && total_landmarks < DMS_HEADPOSE_LANDMARK_NUM; j++, total_landmarks++) {
                result->landmarks_3d[total_landmarks * 3 + 0] = dequantize_int8(q[j], zp, scale);
            }
        }
    }
    result->landmarks_3d_num = total_landmarks;
    return true;
}

static bool run_inference(dms_face_landmark_result_t *result)
{
    uint64_t t0 = get_mono_time_us();
    int ret = rknn_run(g_hp.ctx, NULL);
    uint64_t cost_ms = (get_mono_time_us() - t0) / 1000ULL;

    if (ret != RKNN_SUCC || cost_ms > 1000ULL) {
        if (!g_hp.fatal_logged) {
            g_hp.fatal_logged = true;
            log_error("HEADPOSE disabled after fatal RKNN error (ret=%d, cost=%llums)",
                      ret, (unsigned long long)cost_ms);
        }
        g_hp.fatal_error = true;
        return false;
    }

    return parse_headpose_outputs(result);
}

static void compute_ear_mar_from_keypoints(const dms_face_detect_result_t *face,
                                           dms_face_landmark_result_t *result)
{
    /*
     * BlazeFace 输出 5 个关键点（双眼、鼻尖、嘴角），缺少眼睑轮廓，
     * 因此无法计算标准 EAR。这里用 mouth_width / face_width 作为近似 MAR，
     * 并把 EAR 固定为正常值，避免误报。后续若换用 68/106 点 landmark 模型，
     * 可在此实现真实 EAR。
     */
    float *kpt = (float *)face->kpt;
    float mouth_width = hypotf(kpt[8] - kpt[6], kpt[9] - kpt[7]);
    float face_w = (float)face->w;

    result->ear = 0.30f; /* placeholder: BlazeFace 关键点不足以计算真实 EAR */
    result->mar = (face_w > 0.0f) ? (mouth_width / face_w) : 0.0f;
}

static void compute_head_down(dms_face_landmark_result_t *result)
{
    if (result->angles_valid) {
        /* Pitch > threshold means looking down. */
        float down = (result->pitch - 0.0f) / HEADPOSE_PITCH_DOWN_THRESHOLD;
        if (down < 0.0f) down = 0.0f;
        if (down > 1.0f) down = 1.0f;
        result->head_down_score = down;
    } else {
        result->head_down_score = 0.0f;
    }
}

static void mock_result(dms_face_landmark_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->found = 1;
    result->ear = 0.30f;
    result->mar = 0.20f;
    result->head_down_score = 0.0f;
}

bool dms_face_landmark_process(const frame_data_t *frame,
                               const dms_face_detect_result_t *face,
                               dms_face_landmark_result_t *result)
{
    memset(result, 0, sizeof(*result));

    if (g_hp.fatal_error) {
        /* 已触发熔断，返回占位结果，避免继续卡 NPU。 */
        mock_result(result);
        return true;
    }

    if (g_hp.mock || !face || !face->found) {
        mock_result(result);
        return true;
    }

    if (!frame || !frame->img_data || frame->img_size == 0) {
        log_warn("headpose 输入帧为空");
        return false;
    }

    dms_image_t img = { 0 };
    if (!dms_decode_jpeg((const uint8_t *)frame->img_data, (size_t)frame->img_size, &img)) {
        log_warn("JPEG 解码失败");
        return false;
    }

    /* Add small margin around face for headpose input. */
    int margin_x = (int)(face->w * 0.15f);
    int margin_y = (int)(face->h * 0.2f);
    int crop_x = face->x - margin_x;
    int crop_y = face->y - margin_y;
    int crop_w = face->w + margin_x * 2;
    int crop_h = face->h + margin_y * 2;

    preprocess_face_crop(&img, crop_x, crop_y, crop_w, crop_h);
    dms_free_image(&img);

    if (!run_inference(result)) {
        return false;
    }

    compute_ear_mar_from_keypoints(face, result);
    compute_head_down(result);

    return true;
}
