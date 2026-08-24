#ifndef DMS_RETINAFACE_H
#define DMS_RETINAFACE_H

#include "common.h"
#include "dms_face_detect.h"
#include "dms_image_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RetinaFace RGB 人脸检测模块。
 *
 * 实现严格参考：
 *   /home/jennie/hhh/luckfox_pico_rknn_example/example/luckfox_pico_retinaface_facenet/src/retinaface.cc
 *
 * 模型路径由调用者指定，典型值：
 *   /mnt/sdcard/dms/models/retinaface.rknn
 */

/*
 * 单次推理各阶段耗时（微秒）。
 * 用于 P0 性能基线校准，也便于定位是 NPU 慢还是预处理/后处理慢。
 */
typedef struct {
    uint64_t jpeg_decode_us;   /* JPEG 解码（由 dms_face_detect.c 设置） */
    uint64_t preprocess_us;    /* RGB→BGR + bilinear resize */
    uint64_t rknn_run_us;      /* NPU 推理 */
    uint64_t postprocess_us;   /* dequantize + decode + NMS + 坐标反算 */
    uint64_t total_us;         /* 整个人脸检测阶段（jpeg + preprocess + rknn + post） */
} dms_retinaface_timing_t;

/* 初始化 RetinaFace 模型并执行单帧 self-test。 */
bool dms_retinaface_init(const char *model_path);

/* 释放模型资源。 */
void dms_retinaface_deinit(void);

/*
 * 对一帧 RGB 图像做人脸检测。
 * src 必须是 dms_decode_jpeg 解码出的 RGB 图像（channels == 3）。
 * 输出坐标已经映射回 src 的原始分辨率，并已 clamp 到图像范围内。
 */
bool dms_retinaface_process(const dms_image_t *src, dms_face_detect_result_t *result);

/*
 * V2-A 硬件预处理路径：bgr 为 RGA 产出的 model 输入尺寸 BGR 图（stretch），
 * 函数内部只做 memcpy + rknn_run + decode，坐标反算使用 orig_w/orig_h。
 */
bool dms_retinaface_process_prepared(const uint8_t *bgr, int orig_w, int orig_h,
                                     dms_face_detect_result_t *result);

/* 查询模型输入尺寸（用于 RGA 预处理模块初始化）。 */
bool dms_retinaface_get_input_size(int *out_w, int *out_h);

/* self-test 是否通过。 */
bool dms_retinaface_selftest_passed(void);

/* 最近一次推理耗时（微秒）。 */
uint64_t dms_retinaface_last_inference_us(void);

/* 自上次 reset 以来的平均推理耗时（微秒）。 */
uint64_t dms_retinaface_avg_inference_us(void);

/* 自上次 reset 以来的最大推理耗时（微秒）。 */
uint64_t dms_retinaface_max_inference_us(void);

/* 重置统计（avg/max）。 */
void dms_retinaface_reset_stats(void);

/* 分项耗时统计 */
const dms_retinaface_timing_t *dms_retinaface_last_timing(void);
const dms_retinaface_timing_t *dms_retinaface_avg_timing(void);
void dms_retinaface_set_last_jpeg_decode_us(uint64_t us);
void dms_retinaface_reset_timing_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* DMS_RETINAFACE_H */
