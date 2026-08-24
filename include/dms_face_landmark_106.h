#ifndef DMS_FACE_LANDMARK_106_H
#define DMS_FACE_LANDMARK_106_H

#include "common.h"
#include "dms_face_detect.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 106 点人脸关键点模块（InsightFace 2d106det 适配）。
 * 输入：RetinaFace 输出的人脸框 + 原始 JPEG 帧。
 * 输出：106 个 2D 关键点在原图坐标系下的 x,y。
 *
 * 通过 common.h 中的 DMS_ENABLE_LANDMARK_106 控制编译开关。
 */

#define DMS_LANDMARK_106_NUM 106

typedef struct {
    int   found;                              /* 0/1 是否检测到关键点 */
    float points[DMS_LANDMARK_106_NUM * 2];   /* x0,y0,x1,y1,... 原图坐标 */

    /* 简单耗时统计（微秒） */
    uint64_t preprocess_us;
    uint64_t rknn_run_us;
    uint64_t postprocess_us;
    uint64_t total_us;
} dms_face_landmark_106_result_t;

bool dms_face_landmark_106_init(const char *model_path);
/* 兼容接口：内部仍会解码 JPEG。新代码优先用 process_image 复用已解码帧。 */
bool dms_face_landmark_106_process(const frame_data_t *frame,
                                   const dms_face_detect_result_t *face,
                                   dms_face_landmark_106_result_t *result);

/* 推荐接口：直接传入 RetinaFace 阶段已解码的 RGB 图，避免重复 JPEG decode。 */
bool dms_face_landmark_106_process_image(const dms_image_t *img,
                                         const dms_face_detect_result_t *face,
                                         dms_face_landmark_106_result_t *result);
void dms_face_landmark_106_deinit(void);
bool dms_face_landmark_106_is_mock(void);

/*
 * 最近一次成功 preprocess 后模型实际输入的 192x192 RGB 数据
 * （内部 input_mem 的虚拟地址，只读，有效期到下一次 process_image）。
 * 未初始化/mock 时返回 NULL。用于 HW 路径 dump 调试。
 */
const uint8_t *dms_face_landmark_106_last_input(void);

#ifdef __cplusplus
}
#endif

#endif /* DMS_FACE_LANDMARK_106_H */
