#include "dms_retinaface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <time.h>

#include "sys_logger.h"
#include "rknn_api.h"

/* 严格对照 retinaface.cc 的超参数 */
#define RETINAFACE_SCORE_THRESHOLD  0.5f
#define RETINAFACE_NMS_THRESHOLD    0.2f
#define RETINAFACE_VARIANCES_0      0.1f
#define RETINAFACE_VARIANCES_1      0.2f
#define RETINAFACE_MAX_OUTPUTS      4

/* 支持的模型输入尺寸 */
#define RETINAFACE_INPUT_UNKNOWN    0
#define RETINAFACE_INPUT_320        320
#define RETINAFACE_INPUT_640        640

/* 每个 prior 对应 2 个 min_size，3 个 feature map */
#define RETINAFACE_NUM_FEATURE_MAPS 3
#define RETINAFACE_NUM_MIN_SIZES    2

#define RETINAFACE_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

typedef struct {
    rknn_context ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr input_attr;
    rknn_tensor_attr output_attrs[RETINAFACE_MAX_OUTPUTS];

    int model_w;
    int model_h;
    int model_c;

    float *priors;          /* [num_priors * 4]: cx, cy, w, h (normalized) */
    int num_priors;

    rknn_tensor_mem *input_mem;
    rknn_tensor_mem *output_mems[RETINAFACE_MAX_OUTPUTS];

    /* 解码时复用的缓冲区，避免每帧 malloc */
    int *decode_indices;
    float *decode_props;
    float *decode_loc_fp32;
    float *decode_landms_fp32;

    /* 输出索引识别结果 */
    int loc_idx;
    int score_idx;
    int landms_idx;

    bool selftest_passed;

    /* 性能统计 */
    uint64_t inference_total_us;
    uint64_t inference_count;
    uint64_t inference_max_us;
    uint64_t last_inference_us;
} retinaface_ctx_t;

static retinaface_ctx_t g_rf = { 0 };

/* ======================================================================== */
/* 调试与分项耗时统计                                                        */
/* ======================================================================== */

/* 设为 1 时，前 RETINAFACE_DEBUG_PRINT_COUNT 次真实推理会打印坐标反算诊断 */
#define RETINAFACE_DEBUG_COORDS          1
#define RETINAFACE_DEBUG_PRINT_COUNT     2

static int s_debug_print_count = 0;
static dms_retinaface_timing_t g_last_timing = {0};
static dms_retinaface_timing_t g_avg_timing = {0};
static uint64_t g_timing_count = 0;

static uint64_t get_mono_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static inline float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale)
{
    return ((float)qnt - (float)zp) * scale;
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
    log_info("%s: name=%s dims=[%s] n_dims=%u n_elems=%u size=%u size_with_stride=%u fmt=%s type=%s qnt=%s zp=%d scale=%f",
             prefix,
             attr->name,
             dims_str,
             attr->n_dims,
             attr->n_elems,
             attr->size,
             attr->size_with_stride,
             get_format_string(attr->fmt),
             get_type_string(attr->type),
             get_qnt_type_string(attr->qnt_type),
             attr->zp,
             attr->scale);
}

/*
 * 生成 RetinaFace prior boxes，与 rknn_box_priors.h 中预生成的一致。
 * min_sizes: [[16,32], [64,128], [256,512]]
 * steps:     [8, 16, 32]
 */
static bool generate_priors(int model_size, float **priors_out, int *num_priors_out)
{
    int min_sizes[RETINAFACE_NUM_FEATURE_MAPS][RETINAFACE_NUM_MIN_SIZES] = {
        {16, 32},
        {64, 128},
        {256, 512}
    };
    int steps[RETINAFACE_NUM_FEATURE_MAPS] = {8, 16, 32};

    int num_priors = 0;
    for (int k = 0; k < RETINAFACE_NUM_FEATURE_MAPS; k++) {
        int fm = model_size / steps[k];
        num_priors += fm * fm * RETINAFACE_NUM_MIN_SIZES;
    }

    float *priors = (float *)calloc((size_t)num_priors * 4, sizeof(float));
    if (!priors) {
        log_error("retinaface: 无法分配 prior 内存 (%d)", num_priors);
        return false;
    }

    int idx = 0;
    for (int k = 0; k < RETINAFACE_NUM_FEATURE_MAPS; k++) {
        int fm = model_size / steps[k];
        for (int i = 0; i < fm; i++) {
            for (int j = 0; j < fm; j++) {
                for (int s = 0; s < RETINAFACE_NUM_MIN_SIZES; s++) {
                    float cx = (j + 0.5f) * steps[k] / model_size;
                    float cy = (i + 0.5f) * steps[k] / model_size;
                    float w = (float)min_sizes[k][s] / model_size;
                    float h = (float)min_sizes[k][s] / model_size;
                    priors[idx * 4 + 0] = cx;
                    priors[idx * 4 + 1] = cy;
                    priors[idx * 4 + 2] = w;
                    priors[idx * 4 + 3] = h;
                    idx++;
                }
            }
        }
    }

    if (idx != num_priors) {
        log_error("retinaface: prior 生成数量异常 %d != %d", idx, num_priors);
        free(priors);
        return false;
    }

    *priors_out = priors;
    *num_priors_out = num_priors;
    return true;
}

static bool allocate_io_mem(void)
{
    /* 参考 retinaface.cc：输入使用 RKNN_QUERY_NATIVE_INPUT_ATTR */
    g_rf.input_attr.index = 0;
    int ret = rknn_query(g_rf.ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &g_rf.input_attr, sizeof(g_rf.input_attr));
    if (ret != RKNN_SUCC) {
        log_error("retinaface: rknn_query native input attr 失败: %d", ret);
        return false;
    }
    dump_tensor_attr("retinaface input", &g_rf.input_attr);

    /* 与参考一致：uint8 NHWC，把 normalize/quantize fuse 到 NPU */
    g_rf.input_attr.type = RKNN_TENSOR_UINT8;
    g_rf.input_attr.fmt = RKNN_TENSOR_NHWC;

    g_rf.input_mem = rknn_create_mem(g_rf.ctx, g_rf.input_attr.size_with_stride);
    if (!g_rf.input_mem) {
        log_error("retinaface: rknn_create_mem input 失败");
        return false;
    }

    ret = rknn_set_io_mem(g_rf.ctx, g_rf.input_mem, &g_rf.input_attr);
    if (ret != RKNN_SUCC) {
        log_error("retinaface: rknn_set_io_mem input 失败: %d", ret);
        return false;
    }

    for (uint32_t i = 0; i < g_rf.io_num.n_output; i++) {
        memset(&g_rf.output_attrs[i], 0, sizeof(g_rf.output_attrs[i]));
        g_rf.output_attrs[i].index = i;
        ret = rknn_query(g_rf.ctx, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR, &g_rf.output_attrs[i], sizeof(g_rf.output_attrs[i]));
        if (ret != RKNN_SUCC) {
            log_error("retinaface: rknn_query native NHWC output attr %d 失败: %d", i, ret);
            return false;
        }
        dump_tensor_attr("retinaface output", &g_rf.output_attrs[i]);

        g_rf.output_mems[i] = rknn_create_mem(g_rf.ctx, g_rf.output_attrs[i].size_with_stride);
        if (!g_rf.output_mems[i]) {
            log_error("retinaface: rknn_create_mem output %d 失败", i);
            return false;
        }

        ret = rknn_set_io_mem(g_rf.ctx, g_rf.output_mems[i], &g_rf.output_attrs[i]);
        if (ret != RKNN_SUCC) {
            log_error("retinaface: rknn_set_io_mem output %d 失败: %d", i, ret);
            return false;
        }
    }
    return true;
}

static bool identify_outputs(void)
{
    g_rf.loc_idx = -1;
    g_rf.score_idx = -1;
    g_rf.landms_idx = -1;

    for (uint32_t i = 0; i < g_rf.io_num.n_output; i++) {
        uint32_t elems = g_rf.output_attrs[i].n_elems;
        if (elems == (uint32_t)(g_rf.num_priors * 4) && g_rf.loc_idx < 0) {
            g_rf.loc_idx = (int)i;
        } else if (elems == (uint32_t)(g_rf.num_priors * 2) && g_rf.score_idx < 0) {
            g_rf.score_idx = (int)i;
        } else if (elems == (uint32_t)(g_rf.num_priors * 10) && g_rf.landms_idx < 0) {
            g_rf.landms_idx = (int)i;
        }
    }

    if (g_rf.loc_idx < 0 || g_rf.score_idx < 0 || g_rf.landms_idx < 0) {
        log_error("retinaface: 无法识别输出 loc=%d score=%d landms=%d (num_priors=%d)",
                  g_rf.loc_idx, g_rf.score_idx, g_rf.landms_idx, g_rf.num_priors);
        return false;
    }

    log_info("retinaface: output mapping loc=%d score=%d landms=%d",
             g_rf.loc_idx, g_rf.score_idx, g_rf.landms_idx);
    return true;
}

bool dms_retinaface_init(const char *model_path)
{
    memset(&g_rf, 0, sizeof(g_rf));
    g_rf.loc_idx = -1;
    g_rf.score_idx = -1;
    g_rf.landms_idx = -1;

    log_info("RetinaFace 初始化: %s", model_path ? model_path : "(null)");

    if (!model_path || access(model_path, F_OK) != 0) {
        log_error("retinaface: 模型不存在: %s", model_path ? model_path : "(null)");
        return false;
    }

    int ret = rknn_init(&g_rf.ctx, (char *)model_path, 0, 0, NULL);
    if (ret < 0) {
        log_error("retinaface: rknn_init 失败: %d", ret);
        return false;
    }

    rknn_sdk_version sdk_ver;
    ret = rknn_query(g_rf.ctx, RKNN_QUERY_SDK_VERSION, &sdk_ver, sizeof(sdk_ver));
    if (ret == RKNN_SUCC) {
        log_info("retinaface: RKNN api=%s driver=%s", sdk_ver.api_version, sdk_ver.drv_version);
    }

    ret = rknn_query(g_rf.ctx, RKNN_QUERY_IN_OUT_NUM, &g_rf.io_num, sizeof(g_rf.io_num));
    if (ret != RKNN_SUCC || g_rf.io_num.n_input != 1 || g_rf.io_num.n_output < 3) {
        log_error("retinaface: 模型输入输出数量异常: in=%d out=%d",
                  g_rf.io_num.n_input, g_rf.io_num.n_output);
        goto fail;
    }
    log_info("retinaface: input num=%d output num=%d",
             g_rf.io_num.n_input, g_rf.io_num.n_output);

    if (!allocate_io_mem()) {
        goto fail;
    }

    /* 根据真实 tensor 属性决定输入尺寸 */
    if (g_rf.input_attr.n_dims >= 4) {
        if (g_rf.input_attr.fmt == RKNN_TENSOR_NCHW) {
            g_rf.model_c = (int)g_rf.input_attr.dims[1];
            g_rf.model_h = (int)g_rf.input_attr.dims[2];
            g_rf.model_w = (int)g_rf.input_attr.dims[3];
        } else {
            g_rf.model_h = (int)g_rf.input_attr.dims[1];
            g_rf.model_w = (int)g_rf.input_attr.dims[2];
            g_rf.model_c = (int)g_rf.input_attr.dims[3];
        }
    } else {
        log_error("retinaface: 输入 tensor 维度异常: n_dims=%u", g_rf.input_attr.n_dims);
        goto fail;
    }

    log_info("retinaface: 模型输入尺寸 %dx%dx%d (H x W x C)",
             g_rf.model_h, g_rf.model_w, g_rf.model_c);

    if (g_rf.model_w != g_rf.model_h) {
        log_error("retinaface: 模型输入非正方形 (%dx%d)，不支持",
                  g_rf.model_w, g_rf.model_h);
        goto fail;
    }

    if (!generate_priors(g_rf.model_w, &g_rf.priors, &g_rf.num_priors)) {
        goto fail;
    }
    log_info("retinaface: 生成 priors=%d", g_rf.num_priors);

    if (!identify_outputs()) {
        goto fail;
    }

    g_rf.decode_indices = (int *)calloc((size_t)g_rf.num_priors, sizeof(int));
    g_rf.decode_props = (float *)calloc((size_t)g_rf.num_priors, sizeof(float));
    g_rf.decode_loc_fp32 = (float *)calloc((size_t)g_rf.num_priors * 4, sizeof(float));
    g_rf.decode_landms_fp32 = (float *)calloc((size_t)g_rf.num_priors * 10, sizeof(float));
    if (!g_rf.decode_indices || !g_rf.decode_props || !g_rf.decode_loc_fp32 || !g_rf.decode_landms_fp32) {
        log_error("retinaface: decode 缓冲区分配失败");
        goto fail;
    }

    /* 单帧 self-test */
    log_info("retinaface: 开始单帧 self-test");
    dms_image_t dummy = {0};
    dummy.width = 1280;
    dummy.height = 720;
    dummy.channels = 3;
    dummy.data = (uint8_t *)calloc((size_t)dummy.width * dummy.height * 3, 1);
    if (!dummy.data) {
        log_error("retinaface: self-test 内存分配失败");
        goto fail;
    }
    /* 填充一个简单梯度，避免全黑输入 */
    for (int y = 0; y < dummy.height; y++) {
        for (int x = 0; x < dummy.width; x++) {
            int v = ((x + y) * 255) / (dummy.width + dummy.height);
            dummy.data[(y * dummy.width + x) * 3 + 0] = (uint8_t)v;
            dummy.data[(y * dummy.width + x) * 3 + 1] = (uint8_t)((v * 2) % 256);
            dummy.data[(y * dummy.width + x) * 3 + 2] = (uint8_t)((v * 3) % 256);
        }
    }

    dms_face_detect_result_t self_result;
    memset(&self_result, 0, sizeof(self_result));
    bool self_ok = dms_retinaface_process(&dummy, &self_result);
    free(dummy.data);

    if (!self_ok) {
        log_error("retinaface: self-test 推理失败");
        goto fail;
    }

    g_rf.selftest_passed = true;
    log_info("RETINAFACE_SELFTEST_PASS");

    return true;

fail:
    dms_retinaface_deinit();
    log_error("RETINAFACE_SELFTEST_FAIL");
    return false;
}

void dms_retinaface_deinit(void)
{
    rknn_context tmp_ctx = g_rf.ctx;

    if (g_rf.input_mem) {
        rknn_destroy_mem(tmp_ctx, g_rf.input_mem);
        /* rknn_create_mem 创建的 mem（ALLOC_INSIDE）由 rknn_destroy_mem 内部 free，
         * 这里再 free 会 double-free。 */
        g_rf.input_mem = NULL;
    }
    for (uint32_t i = 0; i < g_rf.io_num.n_output; i++) {
        if (g_rf.output_mems[i]) {
            rknn_destroy_mem(tmp_ctx, g_rf.output_mems[i]);
            g_rf.output_mems[i] = NULL;
        }
    }

    if (tmp_ctx != 0) {
        rknn_destroy(tmp_ctx);
        g_rf.ctx = 0;
    }

    if (g_rf.priors) {
        free(g_rf.priors);
        g_rf.priors = NULL;
    }
    free(g_rf.decode_indices);
    free(g_rf.decode_props);
    free(g_rf.decode_loc_fp32);
    free(g_rf.decode_landms_fp32);
    g_rf.decode_indices = NULL;
    g_rf.decode_props = NULL;
    g_rf.decode_loc_fp32 = NULL;
    g_rf.decode_landms_fp32 = NULL;

    memset(&g_rf, 0, sizeof(g_rf));
    g_rf.loc_idx = -1;
    g_rf.score_idx = -1;
    g_rf.landms_idx = -1;
}

bool dms_retinaface_selftest_passed(void)
{
    return g_rf.selftest_passed;
}

uint64_t dms_retinaface_last_inference_us(void)
{
    return g_rf.last_inference_us;
}

uint64_t dms_retinaface_avg_inference_us(void)
{
    if (g_rf.inference_count == 0) return 0;
    return g_rf.inference_total_us / g_rf.inference_count;
}

uint64_t dms_retinaface_max_inference_us(void)
{
    return g_rf.inference_max_us;
}

void dms_retinaface_reset_stats(void)
{
    g_rf.inference_total_us = 0;
    g_rf.inference_count = 0;
    g_rf.inference_max_us = 0;
}

const dms_retinaface_timing_t *dms_retinaface_last_timing(void)
{
    return &g_last_timing;
}

const dms_retinaface_timing_t *dms_retinaface_avg_timing(void)
{
    if (g_timing_count == 0) return NULL;
    return &g_avg_timing;
}

void dms_retinaface_set_last_jpeg_decode_us(uint64_t us)
{
    g_last_timing.jpeg_decode_us = us;
}

void dms_retinaface_reset_timing_stats(void)
{
    memset(&g_last_timing, 0, sizeof(g_last_timing));
    memset(&g_avg_timing, 0, sizeof(g_avg_timing));
    g_timing_count = 0;
}

/*
 * 打印坐标反算诊断信息。
 * 用于 P0 验收时核对 bbox 与 5 个 landmark 是否走了完全一致的 scale/pad 路径。
 */
static void print_coord_diagnostics(const dms_image_t *src,
                                    const dms_face_detect_result_t *model_coords,
                                    const dms_face_detect_result_t *orig_coords)
{
    if (!src || !model_coords || !orig_coords) return;

    float scale_x = (float)src->width / (float)g_rf.model_w;
    float scale_y = (float)src->height / (float)g_rf.model_h;

    log_info("[RETINAFACE COORD] src=%dx%d model=%dx%d scale_x=%.3f scale_y=%.3f "
             "pad=none(stretch)",
             src->width, src->height, g_rf.model_w, g_rf.model_h,
             scale_x, scale_y);

    log_info("[RETINAFACE COORD] bbox model=(%d,%d,%d,%d) orig=(%d,%d,%d,%d) "
             "score=%.3f found=%d",
             model_coords->x, model_coords->y, model_coords->w, model_coords->h,
             orig_coords->x, orig_coords->y, orig_coords->w, orig_coords->h,
             orig_coords->score, orig_coords->found);

    for (int k = 0; k < DMS_FACE_KPT_NUM; k++) {
        log_info("[RETINAFACE COORD] kpt%d model=(%.1f,%.1f) orig=(%.1f,%.1f)",
                 k,
                 model_coords->kpt[k * 2 + 0], model_coords->kpt[k * 2 + 1],
                 orig_coords->kpt[k * 2 + 0], orig_coords->kpt[k * 2 + 1]);
    }
}

/*
 * BGR 顺序 bilinear resize，同时完成 RGB -> BGR 转换。
 * 参考 retinaface.cc 中 cv::resize(bgr, retina_input, ..., cv::INTER_LINEAR)。
 */
static void preprocess_rgb_to_bgr_bilinear(const uint8_t *src_rgb, int src_w, int src_h,
                                           uint8_t *dst_bgr, int dst_w, int dst_h)
{
    float x_ratio = (float)src_w / dst_w;
    float y_ratio = (float)src_h / dst_h;

    for (int y = 0; y < dst_h; y++) {
        float sy = (y + 0.5f) * y_ratio - 0.5f;
        int y0 = (int)floorf(sy);
        int y1 = y0 + 1;
        float dy = sy - y0;
        y0 = RETINAFACE_CLAMP(y0, 0, src_h - 1);
        y1 = RETINAFACE_CLAMP(y1, 0, src_h - 1);

        for (int x = 0; x < dst_w; x++) {
            float sx = (x + 0.5f) * x_ratio - 0.5f;
            int x0 = (int)floorf(sx);
            int x1 = x0 + 1;
            float dx = sx - x0;
            x0 = RETINAFACE_CLAMP(x0, 0, src_w - 1);
            x1 = RETINAFACE_CLAMP(x1, 0, src_w - 1);

            const uint8_t *p00 = src_rgb + (y0 * src_w + x0) * 3;
            const uint8_t *p01 = src_rgb + (y0 * src_w + x1) * 3;
            const uint8_t *p10 = src_rgb + (y1 * src_w + x0) * 3;
            const uint8_t *p11 = src_rgb + (y1 * src_w + x1) * 3;

            int dst_off = (y * dst_w + x) * 3;
            /* RGB -> BGR */
            for (int c = 0; c < 3; c++) {
                float v00 = p00[c];
                float v01 = p01[c];
                float v10 = p10[c];
                float v11 = p11[c];
                float v0 = v00 + dx * (v01 - v00);
                float v1 = v10 + dx * (v11 - v10);
                float v = v0 + dy * (v1 - v0);
                int dst_c;
                if (c == 0) dst_c = 2;      /* R -> B */
                else if (c == 1) dst_c = 1; /* G -> G */
                else dst_c = 0;             /* B -> R */
                dst_bgr[dst_off + dst_c] = (uint8_t)(v + 0.5f);
            }
        }
    }
}

static bool run_preprocess(const dms_image_t *src)
{
    if (!src || !src->data || src->channels != 3) {
        log_error("retinaface: preprocess 需要 3 通道 RGB 图");
        return false;
    }

    uint8_t *dst = (uint8_t *)g_rf.input_mem->virt_addr;
    preprocess_rgb_to_bgr_bilinear(src->data, src->width, src->height,
                                   dst, g_rf.model_w, g_rf.model_h);
    return true;
}

static float calc_overlap(float xmin0, float ymin0, float xmax0, float ymax0,
                          float xmin1, float ymin1, float xmax1, float ymax1)
{
    float w = fmaxf(0.0f, fminf(xmax0, xmax1) - fmaxf(xmin0, xmin1) + 1.0f);
    float h = fmaxf(0.0f, fminf(ymax0, ymax1) - fmaxf(ymin0, ymin1) + 1.0f);
    float i = w * h;
    float u = (xmax0 - xmin0 + 1.0f) * (ymax0 - ymin0 + 1.0f) +
              (xmax1 - xmin1 + 1.0f) * (ymax1 - ymin1 + 1.0f) - i;
    return u <= 0.0f ? 0.0f : i / u;
}

static int quick_sort_indice_inverse(float *input, int left, int right, int *indices)
{
    float key;
    int key_index;
    int low = left;
    int high = right;
    if (left < right) {
        key_index = indices[left];
        key = input[left];
        while (low < high) {
            while (low < high && input[high] <= key) {
                high--;
            }
            input[low] = input[high];
            indices[low] = indices[high];
            while (low < high && input[low] >= key) {
                low++;
            }
            input[high] = input[low];
            indices[high] = indices[low];
        }
        input[low] = key;
        indices[low] = key_index;
        quick_sort_indice_inverse(input, left, low - 1, indices);
        quick_sort_indice_inverse(input, low + 1, right, indices);
    }
    return low;
}

static void nms(int valid_count, float *boxes, int *order, float threshold)
{
    for (int i = 0; i < valid_count; ++i) {
        if (order[i] == -1) continue;
        int n = order[i];
        for (int j = i + 1; j < valid_count; ++j) {
            int m = order[j];
            if (m == -1) continue;
            float iou = calc_overlap(
                boxes[n * 4 + 0], boxes[n * 4 + 1], boxes[n * 4 + 2], boxes[n * 4 + 3],
                boxes[m * 4 + 0], boxes[m * 4 + 1], boxes[m * 4 + 2], boxes[m * 4 + 3]);
            if (iou > threshold) {
                order[j] = -1;
            }
        }
    }
}

static bool run_decode(dms_face_detect_result_t *result)
{
    if (g_rf.loc_idx < 0 || g_rf.score_idx < 0 || g_rf.landms_idx < 0) {
        log_error("retinaface: 输出索引未初始化");
        return false;
    }

    uint8_t *location_u8 = (uint8_t *)g_rf.output_mems[g_rf.loc_idx]->virt_addr;
    uint8_t *scores_u8   = (uint8_t *)g_rf.output_mems[g_rf.score_idx]->virt_addr;
    uint8_t *landms_u8   = (uint8_t *)g_rf.output_mems[g_rf.landms_idx]->virt_addr;

    int32_t loc_zp = g_rf.output_attrs[g_rf.loc_idx].zp;
    float loc_scale = g_rf.output_attrs[g_rf.loc_idx].scale;
    int32_t scores_zp = g_rf.output_attrs[g_rf.score_idx].zp;
    float scores_scale = g_rf.output_attrs[g_rf.score_idx].scale;
    int32_t landms_zp = g_rf.output_attrs[g_rf.landms_idx].zp;
    float landms_scale = g_rf.output_attrs[g_rf.landms_idx].scale;

    const float variances[2] = {RETINAFACE_VARIANCES_0, RETINAFACE_VARIANCES_1};

    int *filter_indices = g_rf.decode_indices;
    float *props = g_rf.decode_props;
    float *loc_fp32 = g_rf.decode_loc_fp32;
    float *landms_fp32 = g_rf.decode_landms_fp32;

    /* 清空本帧需要的区域（最多 num_priors） */
    memset(filter_indices, 0, (size_t)g_rf.num_priors * sizeof(int));
    memset(props, 0, (size_t)g_rf.num_priors * sizeof(float));
    memset(loc_fp32, 0, (size_t)g_rf.num_priors * 4 * sizeof(float));
    memset(landms_fp32, 0, (size_t)g_rf.num_priors * 10 * sizeof(float));

    int valid_count = 0;
    for (int i = 0; i < g_rf.num_priors; i++) {
        float face_score = deqnt_affine_to_f32((int8_t)scores_u8[i * 2 + 1], scores_zp, scores_scale);
        if (face_score > RETINAFACE_SCORE_THRESHOLD) {
            filter_indices[valid_count] = i;
            props[valid_count] = face_score;

            int offset = i * 4;
            uint8_t *bbox = location_u8 + offset;

            float box_cx = deqnt_affine_to_f32((int8_t)bbox[0], loc_zp, loc_scale)
                           * variances[0] * g_rf.priors[i * 4 + 2] + g_rf.priors[i * 4 + 0];
            float box_cy = deqnt_affine_to_f32((int8_t)bbox[1], loc_zp, loc_scale)
                           * variances[0] * g_rf.priors[i * 4 + 3] + g_rf.priors[i * 4 + 1];
            float box_w = expf(deqnt_affine_to_f32((int8_t)bbox[2], loc_zp, loc_scale) * variances[1])
                          * g_rf.priors[i * 4 + 2];
            float box_h = expf(deqnt_affine_to_f32((int8_t)bbox[3], loc_zp, loc_scale) * variances[1])
                          * g_rf.priors[i * 4 + 3];

            float xmin = box_cx - box_w * 0.5f;
            float ymin = box_cy - box_h * 0.5f;
            float xmax = xmin + box_w;
            float ymax = ymin + box_h;

            loc_fp32[offset + 0] = xmin;
            loc_fp32[offset + 1] = ymin;
            loc_fp32[offset + 2] = xmax;
            loc_fp32[offset + 3] = ymax;

            for (int j = 0; j < 5; j++) {
                landms_fp32[i * 10 + 2 * j + 0] =
                    deqnt_affine_to_f32((int8_t)landms_u8[i * 10 + 2 * j + 0], landms_zp, landms_scale)
                    * variances[0] * g_rf.priors[i * 4 + 2] + g_rf.priors[i * 4 + 0];
                landms_fp32[i * 10 + 2 * j + 1] =
                    deqnt_affine_to_f32((int8_t)landms_u8[i * 10 + 2 * j + 1], landms_zp, landms_scale)
                    * variances[0] * g_rf.priors[i * 4 + 3] + g_rf.priors[i * 4 + 1];
            }

            ++valid_count;
        }
    }

    if (valid_count == 0) {
        result->found = 0;
        goto decode_free;
    }

    quick_sort_indice_inverse(props, 0, valid_count - 1, filter_indices);
    nms(valid_count, loc_fp32, filter_indices, RETINAFACE_NMS_THRESHOLD);

    /* 只取最高分的一个（DMS 当前只关注单个人脸） */
    int best_idx = -1;
    float best_score = 0.0f;
    for (int i = 0; i < valid_count; i++) {
        if (filter_indices[i] == -1 || props[i] < RETINAFACE_SCORE_THRESHOLD) {
            continue;
        }
        best_idx = filter_indices[i];
        best_score = props[i];
        break;
    }

    if (best_idx < 0) {
        result->found = 0;
        goto decode_free;
    }

    float x1 = loc_fp32[best_idx * 4 + 0];
    float y1 = loc_fp32[best_idx * 4 + 1];
    float x2 = loc_fp32[best_idx * 4 + 2];
    float y2 = loc_fp32[best_idx * 4 + 3];

    /* clamp 到模型输入范围内 */
    x1 = RETINAFACE_CLAMP(x1, 0.0f, 1.0f);
    y1 = RETINAFACE_CLAMP(y1, 0.0f, 1.0f);
    x2 = RETINAFACE_CLAMP(x2, 0.0f, 1.0f);
    y2 = RETINAFACE_CLAMP(y2, 0.0f, 1.0f);

    result->found = 1;
    result->score = best_score;
    result->x = (int)(x1 * g_rf.model_w);
    result->y = (int)(y1 * g_rf.model_h);
    result->w = (int)((x2 - x1) * g_rf.model_w);
    result->h = (int)((y2 - y1) * g_rf.model_h);

    for (int j = 0; j < 5; j++) {
        float px = landms_fp32[best_idx * 10 + 2 * j + 0];
        float py = landms_fp32[best_idx * 10 + 2 * j + 1];
        px = RETINAFACE_CLAMP(px, 0.0f, 1.0f);
        py = RETINAFACE_CLAMP(py, 0.0f, 1.0f);
        result->kpt[j * 2 + 0] = px * g_rf.model_w;
        result->kpt[j * 2 + 1] = py * g_rf.model_h;
    }

decode_free:
    return true;
}

/* 将模型坐标映射回原始图像坐标。参考 retinaface.cc 的 mapCoordinates（简单缩放）。 */
static void map_to_original(const dms_image_t *src, dms_face_detect_result_t *result)
{
    if (!result->found) return;

    float scale_x = (float)src->width / (float)g_rf.model_w;
    float scale_y = (float)src->height / (float)g_rf.model_h;

    float x1 = result->x * scale_x;
    float y1 = result->y * scale_y;
    float x2 = (result->x + result->w) * scale_x;
    float y2 = (result->y + result->h) * scale_y;

    x1 = RETINAFACE_CLAMP(x1, 0.0f, (float)src->width);
    y1 = RETINAFACE_CLAMP(y1, 0.0f, (float)src->height);
    x2 = RETINAFACE_CLAMP(x2, 0.0f, (float)src->width);
    y2 = RETINAFACE_CLAMP(y2, 0.0f, (float)src->height);

    result->x = (int)x1;
    result->y = (int)y1;
    result->w = (int)(x2 - x1);
    result->h = (int)(y2 - y1);

    for (int k = 0; k < DMS_FACE_KPT_NUM * 2; k += 2) {
        result->kpt[k + 0] = RETINAFACE_CLAMP(result->kpt[k + 0] * scale_x, 0.0f, (float)src->width);
        result->kpt[k + 1] = RETINAFACE_CLAMP(result->kpt[k + 1] * scale_y, 0.0f, (float)src->height);
    }
}

bool dms_retinaface_get_input_size(int *out_w, int *out_h)
{
    if (!g_rf.ctx || g_rf.model_w <= 0 || g_rf.model_h <= 0) {
        return false;
    }
    if (out_w) *out_w = g_rf.model_w;
    if (out_h) *out_h = g_rf.model_h;
    return true;
}

/*
 * V2-A 硬件预处理路径：输入已是 RGA 产出的 model_w x model_h BGR（stretch），
 * 直接 memcpy 进 NPU 输入内存，跳过 preprocess_rgb_to_bgr_bilinear。
 * decode / NMS / 坐标反算与 dms_retinaface_process() 完全一致。
 */
bool dms_retinaface_process_prepared(const uint8_t *bgr, int orig_w, int orig_h,
                                     dms_face_detect_result_t *result)
{
    if (!result) return false;
    memset(result, 0, sizeof(*result));

    if (!g_rf.ctx) {
        log_error("retinaface: 模型未初始化");
        return false;
    }
    if (!bgr || orig_w <= 0 || orig_h <= 0) {
        log_error("retinaface: prepared 输入非法 bgr=%p orig=%dx%d", (const void *)bgr, orig_w, orig_h);
        return false;
    }

    dms_face_detect_result_t model_coords;
    memset(&model_coords, 0, sizeof(model_coords));

    /* 1. 预处理 = 纯 memcpy（RGA 已完成 resize + RGB->BGR） */
    uint64_t t_preprocess_0 = get_mono_time_us();
    memcpy(g_rf.input_mem->virt_addr, bgr, (size_t)g_rf.model_w * g_rf.model_h * 3);
    uint64_t t_preprocess_1 = get_mono_time_us();

    /* 2. NPU 推理 */
    uint64_t t_rknn_0 = get_mono_time_us();
    int ret = rknn_run(g_rf.ctx, NULL);
    uint64_t t_rknn_1 = get_mono_time_us();

    uint64_t rknn_us = t_rknn_1 - t_rknn_0;
    g_rf.last_inference_us = rknn_us;
    g_rf.inference_total_us += rknn_us;
    g_rf.inference_count++;
    if (rknn_us > g_rf.inference_max_us) {
        g_rf.inference_max_us = rknn_us;
    }

    if (ret != RKNN_SUCC) {
        log_error("retinaface: rknn_run 失败 ret=%d", ret);
        return false;
    }

    /* 3. 后处理：decode + NMS + 坐标反算（与软件路径一致，stretch 语义） */
    dms_image_t orig_ref;
    memset(&orig_ref, 0, sizeof(orig_ref));
    orig_ref.width = orig_w;
    orig_ref.height = orig_h;
    orig_ref.channels = 3;

    uint64_t t_post_0 = get_mono_time_us();
    if (!run_decode(&model_coords)) {
        return false;
    }
    *result = model_coords;  /* 复制模型坐标系结果 */
    map_to_original(&orig_ref, result);
    uint64_t t_post_1 = get_mono_time_us();

    /* 4. 更新分项耗时统计（HW 路径无软件 JPEG 解码，jpeg_decode_us=0） */
    g_last_timing.jpeg_decode_us = 0;
    g_last_timing.preprocess_us = t_preprocess_1 - t_preprocess_0;
    g_last_timing.rknn_run_us   = rknn_us;
    g_last_timing.postprocess_us = t_post_1 - t_post_0;
    g_last_timing.total_us = g_last_timing.jpeg_decode_us +
                             g_last_timing.preprocess_us +
                             g_last_timing.rknn_run_us +
                             g_last_timing.postprocess_us;

    g_avg_timing.jpeg_decode_us  = (g_avg_timing.jpeg_decode_us  * g_timing_count + g_last_timing.jpeg_decode_us)  / (g_timing_count + 1);
    g_avg_timing.preprocess_us   = (g_avg_timing.preprocess_us   * g_timing_count + g_last_timing.preprocess_us)   / (g_timing_count + 1);
    g_avg_timing.rknn_run_us     = (g_avg_timing.rknn_run_us     * g_timing_count + g_last_timing.rknn_run_us)     / (g_timing_count + 1);
    g_avg_timing.postprocess_us  = (g_avg_timing.postprocess_us  * g_timing_count + g_last_timing.postprocess_us)  / (g_timing_count + 1);
    g_avg_timing.total_us        = (g_avg_timing.total_us        * g_timing_count + g_last_timing.total_us)        / (g_timing_count + 1);
    g_timing_count++;

    /* 5. P0 诊断打印：前 RETINAFACE_DEBUG_PRINT_COUNT 次真实推理 */
#if RETINAFACE_DEBUG_COORDS
    if (s_debug_print_count < RETINAFACE_DEBUG_PRINT_COUNT && result->found) {
        print_coord_diagnostics(&orig_ref, &model_coords, result);
        s_debug_print_count++;
    }
#endif

    return true;
}

bool dms_retinaface_process(const dms_image_t *src, dms_face_detect_result_t *result)
{
    if (!result) return false;
    memset(result, 0, sizeof(*result));

    if (!g_rf.ctx) {
        log_error("retinaface: 模型未初始化");
        return false;
    }

    dms_face_detect_result_t model_coords;
    memset(&model_coords, 0, sizeof(model_coords));

    /* 1. 预处理 */
    uint64_t t_preprocess_0 = get_mono_time_us();
    if (!run_preprocess(src)) {
        return false;
    }
    uint64_t t_preprocess_1 = get_mono_time_us();

    /* 2. NPU 推理 */
    uint64_t t_rknn_0 = get_mono_time_us();
    int ret = rknn_run(g_rf.ctx, NULL);
    uint64_t t_rknn_1 = get_mono_time_us();

    uint64_t rknn_us = t_rknn_1 - t_rknn_0;
    g_rf.last_inference_us = rknn_us;
    g_rf.inference_total_us += rknn_us;
    g_rf.inference_count++;
    if (rknn_us > g_rf.inference_max_us) {
        g_rf.inference_max_us = rknn_us;
    }

    if (ret != RKNN_SUCC) {
        log_error("retinaface: rknn_run 失败 ret=%d", ret);
        return false;
    }

    /* 3. 后处理：decode + NMS + 坐标反算 */
    uint64_t t_post_0 = get_mono_time_us();
    if (!run_decode(&model_coords)) {
        return false;
    }
    *result = model_coords;  /* 复制模型坐标系结果 */
    map_to_original(src, result);
    uint64_t t_post_1 = get_mono_time_us();

    /* 4. 更新分项耗时统计 */
    g_last_timing.preprocess_us = t_preprocess_1 - t_preprocess_0;
    g_last_timing.rknn_run_us   = rknn_us;
    g_last_timing.postprocess_us = t_post_1 - t_post_0;
    g_last_timing.total_us = g_last_timing.jpeg_decode_us +
                             g_last_timing.preprocess_us +
                             g_last_timing.rknn_run_us +
                             g_last_timing.postprocess_us;

    g_avg_timing.jpeg_decode_us  = (g_avg_timing.jpeg_decode_us  * g_timing_count + g_last_timing.jpeg_decode_us)  / (g_timing_count + 1);
    g_avg_timing.preprocess_us   = (g_avg_timing.preprocess_us   * g_timing_count + g_last_timing.preprocess_us)   / (g_timing_count + 1);
    g_avg_timing.rknn_run_us     = (g_avg_timing.rknn_run_us     * g_timing_count + g_last_timing.rknn_run_us)     / (g_timing_count + 1);
    g_avg_timing.postprocess_us  = (g_avg_timing.postprocess_us  * g_timing_count + g_last_timing.postprocess_us)  / (g_timing_count + 1);
    g_avg_timing.total_us        = (g_avg_timing.total_us        * g_timing_count + g_last_timing.total_us)        / (g_timing_count + 1);
    g_timing_count++;

    /* 5. P0 诊断打印：前 RETINAFACE_DEBUG_PRINT_COUNT 次真实推理 */
#if RETINAFACE_DEBUG_COORDS
    if (s_debug_print_count < RETINAFACE_DEBUG_PRINT_COUNT && result->found) {
        print_coord_diagnostics(src, &model_coords, result);
        s_debug_print_count++;
    }
#endif

    return true;
}
