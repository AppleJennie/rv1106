#include "dms_face_landmark_106.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <time.h>

#include "sys_logger.h"
#include "dms_image_utils.h"
#include "rknn_api.h"

#define LANDMARK_106_INPUT_W  192
#define LANDMARK_106_INPUT_H  192
#define LANDMARK_106_OUTPUT_LEN (DMS_LANDMARK_106_NUM * 2)

/* 松散裁剪倍数：与 InsightFace 2d106det 训练时保持一致（max(w,h)*1.5） */
#define LANDMARK_106_CROP_SCALE 1.5f

typedef struct {
    rknn_context ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr input_attr;
    rknn_tensor_attr output_attr;

    rknn_tensor_mem *input_mem;
    rknn_tensor_mem *output_mem;

    bool mock;
    bool fatal_error;
    bool fatal_logged;
} landmark_106_ctx_t;

static landmark_106_ctx_t g_lm106 = { 0 };

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
    log_info("%s: name=%s dims=[%s] n_elems=%u size=%u fmt=%s type=%s qnt=%s zp=%d scale=%f",
             prefix,
             attr->name,
             dims_str,
             attr->n_elems,
             attr->size,
             get_format_string(attr->fmt),
             get_type_string(attr->type),
             get_qnt_type_string(attr->qnt_type),
             attr->zp,
             attr->scale);
}

static bool allocate_io_mem(void)
{
    g_lm106.input_attr.index = 0;
    int ret = rknn_query(g_lm106.ctx, RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR,
                         &g_lm106.input_attr, sizeof(g_lm106.input_attr));
    if (ret != RKNN_SUCC) {
        log_error("rknn_query landmark_106 native NHWC input attr 失败: %d", ret);
        return false;
    }
    dump_tensor_attr("landmark_106 input", &g_lm106.input_attr);

    /* 模型输入为 RGB 192x192，与 dms_decode_jpeg 输出格式一致。 */
    g_lm106.input_attr.type = RKNN_TENSOR_UINT8;
    g_lm106.input_attr.fmt = RKNN_TENSOR_NHWC;

    g_lm106.input_mem = rknn_create_mem(g_lm106.ctx, g_lm106.input_attr.size_with_stride);
    if (!g_lm106.input_mem) {
        log_error("rknn_create_mem landmark_106 input 失败");
        return false;
    }

    ret = rknn_set_io_mem(g_lm106.ctx, g_lm106.input_mem, &g_lm106.input_attr);
    if (ret != RKNN_SUCC) {
        log_error("rknn_set_io_mem landmark_106 input 失败: %d", ret);
        return false;
    }

    memset(&g_lm106.output_attr, 0, sizeof(g_lm106.output_attr));
    g_lm106.output_attr.index = 0;
    ret = rknn_query(g_lm106.ctx, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR,
                     &g_lm106.output_attr, sizeof(g_lm106.output_attr));
    if (ret != RKNN_SUCC) {
        log_error("rknn_query landmark_106 native NHWC output attr 失败: %d", ret);
        return false;
    }
    dump_tensor_attr("landmark_106 output", &g_lm106.output_attr);

    g_lm106.output_mem = rknn_create_mem(g_lm106.ctx, g_lm106.output_attr.size_with_stride);
    if (!g_lm106.output_mem) {
        log_error("rknn_create_mem landmark_106 output 失败");
        return false;
    }

    ret = rknn_set_io_mem(g_lm106.ctx, g_lm106.output_mem, &g_lm106.output_attr);
    if (ret != RKNN_SUCC) {
        log_error("rknn_set_io_mem landmark_106 output 失败: %d", ret);
        return false;
    }

    return true;
}

bool dms_face_landmark_106_init(const char *model_path)
{
    memset(&g_lm106, 0, sizeof(g_lm106));

    if (!model_path || access(model_path, F_OK) != 0) {
        log_warn("landmark_106 模型不存在: %s, 使用 mock", model_path ? model_path : "(null)");
        g_lm106.mock = true;
        return true;
    }

    int ret = rknn_init(&g_lm106.ctx, (char *)model_path, 0, 0, NULL);
    if (ret < 0) {
        log_error("rknn_init landmark_106 失败: %d", ret);
        g_lm106.mock = true;
        return true;
    }

    ret = rknn_query(g_lm106.ctx, RKNN_QUERY_IN_OUT_NUM, &g_lm106.io_num, sizeof(g_lm106.io_num));
    if (ret != RKNN_SUCC || g_lm106.io_num.n_input != 1 || g_lm106.io_num.n_output != 1) {
        log_error("landmark_106 模型输入输出数量异常: in=%d out=%d",
                  g_lm106.io_num.n_input, g_lm106.io_num.n_output);
        goto fail;
    }
    log_info("landmark_106 model input num: %d, output num: %d",
             g_lm106.io_num.n_input, g_lm106.io_num.n_output);

    if (!allocate_io_mem()) {
        goto fail;
    }

    log_info("landmark_106 模型初始化成功");
    return true;

fail:
    dms_face_landmark_106_deinit();
    g_lm106.mock = true;
    return true;
}

void dms_face_landmark_106_deinit(void)
{
    rknn_context tmp_ctx = g_lm106.ctx;

    if (g_lm106.input_mem) {
        rknn_destroy_mem(tmp_ctx, g_lm106.input_mem);
        free(g_lm106.input_mem);
        g_lm106.input_mem = NULL;
    }
    if (g_lm106.output_mem) {
        rknn_destroy_mem(tmp_ctx, g_lm106.output_mem);
        free(g_lm106.output_mem);
        g_lm106.output_mem = NULL;
    }
    if (tmp_ctx != 0) {
        rknn_destroy(tmp_ctx);
        g_lm106.ctx = 0;
    }

    memset(&g_lm106, 0, sizeof(g_lm106));
}

bool dms_face_landmark_106_is_mock(void)
{
    return g_lm106.mock;
}

static inline int clamp_int(int v, int minv, int maxv)
{
    if (v < minv) return minv;
    if (v > maxv) return maxv;
    return v;
}

/*
 * 根据人脸 bbox 计算一个边长为 max(w,h)*1.5 的正方形松散裁剪框，
 * 以 bbox 中心为中心，并限制在原图范围内。
 */
static void compute_loose_crop(const dms_face_detect_result_t *face,
                               int img_w, int img_h,
                               int *crop_x, int *crop_y,
                               int *crop_w, int *crop_h)
{
    float face_cx = face->x + face->w * 0.5f;
    float face_cy = face->y + face->h * 0.5f;
    float side = (float)fmax(face->w, face->h) * LANDMARK_106_CROP_SCALE;

    int x0 = (int)roundf(face_cx - side * 0.5f);
    int y0 = (int)roundf(face_cy - side * 0.5f);
    int x1 = (int)roundf(face_cx + side * 0.5f);
    int y1 = (int)roundf(face_cy + side * 0.5f);

    x0 = clamp_int(x0, 0, img_w - 1);
    y0 = clamp_int(y0, 0, img_h - 1);
    x1 = clamp_int(x1, 0, img_w - 1);
    y1 = clamp_int(y1, 0, img_h - 1);

    *crop_x = x0;
    *crop_y = y0;
    *crop_w = x1 - x0 + 1;
    *crop_h = y1 - y0 + 1;
}

static bool preprocess_crop(const dms_image_t *src,
                            const dms_face_detect_result_t *face,
                            int *crop_x, int *crop_y,
                            int *crop_w, int *crop_h)
{
    compute_loose_crop(face, src->width, src->height, crop_x, crop_y, crop_w, crop_h);

    dms_image_t crop = { 0 };
    if (!dms_crop_and_resize(src, *crop_x, *crop_y, *crop_w, *crop_h,
                             LANDMARK_106_INPUT_W, LANDMARK_106_INPUT_H, &crop)) {
        log_error("landmark_106 人脸裁剪缩放失败");
        return false;
    }

    if (crop.channels != 3) {
        log_error("landmark_106 期望 RGB 三通道输入，实际 channels=%d", crop.channels);
        dms_free_image(&crop);
        return false;
    }

    memcpy(g_lm106.input_mem->virt_addr, crop.data,
           (size_t)LANDMARK_106_INPUT_W * LANDMARK_106_INPUT_H * 3);
    dms_free_image(&crop);

    return true;
}

static bool read_output_floats(float *out, int count)
{
    if (count > (int)g_lm106.output_attr.n_elems) {
        log_warn("landmark_106 输出元素数不足: expect %d, got %u", count, g_lm106.output_attr.n_elems);
        count = (int)g_lm106.output_attr.n_elems;
    }

    if (g_lm106.output_attr.type == RKNN_TENSOR_FLOAT32) {
        float *f = (float *)g_lm106.output_mem->virt_addr;
        memcpy(out, f, sizeof(float) * count);
    } else if (g_lm106.output_attr.type == RKNN_TENSOR_INT8) {
        int8_t *q = (int8_t *)g_lm106.output_mem->virt_addr;
        int32_t zp = g_lm106.output_attr.zp;
        float scale = g_lm106.output_attr.scale;
        for (int i = 0; i < count; i++) {
            out[i] = dequantize_int8(q[i], zp, scale);
        }
    } else {
        log_error("landmark_106 不支持的输出类型: %s", get_type_string(g_lm106.output_attr.type));
        return false;
    }
    return true;
}

static bool run_inference(landmark_106_ctx_t *ctx)
{
    uint64_t t0 = get_mono_time_us();
    int ret = rknn_run(ctx->ctx, NULL);
    uint64_t cost_ms = (get_mono_time_us() - t0) / 1000ULL;

    if (ret != RKNN_SUCC || cost_ms > 1000ULL) {
        if (!ctx->fatal_logged) {
            ctx->fatal_logged = true;
            log_error("landmark_106 disabled after fatal RKNN error (ret=%d, cost=%llums)",
                      ret, (unsigned long long)cost_ms);
        }
        ctx->fatal_error = true;
        return false;
    }
    return true;
}

static void mock_result(dms_face_landmark_106_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->found = 0;
}

bool dms_face_landmark_106_process(const frame_data_t *frame,
                                   const dms_face_detect_result_t *face,
                                   dms_face_landmark_106_result_t *result)
{
    memset(result, 0, sizeof(*result));

    if (g_lm106.fatal_error) {
        mock_result(result);
        return true;
    }

    if (g_lm106.mock || !face || !face->found) {
        mock_result(result);
        return true;
    }

    if (!frame || !frame->img_data || frame->img_size == 0) {
        log_warn("landmark_106 输入帧为空");
        return false;
    }

    uint64_t t_total0 = get_mono_time_us();

    dms_image_t img = { 0 };
    if (!dms_decode_jpeg((const uint8_t *)frame->img_data, (size_t)frame->img_size, &img)) {
        log_warn("landmark_106 JPEG 解码失败");
        return false;
    }

    int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;

    uint64_t t_pre0 = get_mono_time_us();
    if (!preprocess_crop(&img, face, &crop_x, &crop_y, &crop_w, &crop_h)) {
        dms_free_image(&img);
        return false;
    }
    dms_free_image(&img);
    result->preprocess_us = get_mono_time_us() - t_pre0;

    uint64_t t_rknn0 = get_mono_time_us();
    if (!run_inference(&g_lm106)) {
        mock_result(result);
        return true;
    }
    result->rknn_run_us = get_mono_time_us() - t_rknn0;

    uint64_t t_post0 = get_mono_time_us();

    float raw[LANDMARK_106_OUTPUT_LEN];
    if (!read_output_floats(raw, LANDMARK_106_OUTPUT_LEN)) {
        return false;
    }

    /* 2d106det 后处理：pred += 1; pred *= 96; */
    for (int i = 0; i < DMS_LANDMARK_106_NUM; i++) {
        float mx = raw[i * 2 + 0] + 1.0f;
        float my = raw[i * 2 + 1] + 1.0f;
        float px = mx * (LANDMARK_106_INPUT_W / 2);
        float py = my * (LANDMARK_106_INPUT_H / 2);

        /* 从 192x192 crop 映射回原图 */
        result->points[i * 2 + 0] = (float)crop_x + px * (float)crop_w / (float)LANDMARK_106_INPUT_W;
        result->points[i * 2 + 1] = (float)crop_y + py * (float)crop_h / (float)LANDMARK_106_INPUT_H;
    }

    result->found = 1;
    result->postprocess_us = get_mono_time_us() - t_post0;
    result->total_us = get_mono_time_us() - t_total0;

    log_info("LANDMARK_106_OK found=1 preprocess_ms=%.1f rknn_ms=%.1f post_ms=%.1f total_ms=%.1f",
             result->preprocess_us / 1000.0f,
             result->rknn_run_us / 1000.0f,
             result->postprocess_us / 1000.0f,
             result->total_us / 1000.0f);

    return true;
}
