#ifndef DMS_INFER_H
#define DMS_INFER_H

#include "common.h"
#include "dms_face_landmark_106.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS 板端推理结果。
 * 后续真实 RKNN 模型推理时填充这些字段，
 * mock 模式下返回固定 NORMAL 值。
 */
typedef struct {
    int   face_found;        /* 0/1 是否检测到人脸 */
    float face_score;        /* 人脸置信度 */

    int   face_x;            /* 人脸框左上角 x */
    int   face_y;            /* 人脸框左上角 y */
    int   face_w;            /* 人脸框宽度 */
    int   face_h;            /* 人脸框高度 */

    /* RetinaFace 5 个关键点在原图坐标：双眼、鼻尖、两个嘴角 */
    float face_kpt[10];      /* x0,y0,x1,y1,... */

    float ear;               /* Eye Aspect Ratio */
    float mar;               /* Mouth Aspect Ratio */
    float head_down_score;   /* 低头程度，0.0~1.0 */

    float yaw;               /* 头部偏航角（度），模型输出有效时填充 */
    float pitch;             /* 头部俯仰角（度） */
    float roll;              /* 头部翻滚角（度） */

    int   eye_closed;        /* 0/1 是否闭眼 */
    int   yawn;              /* 0/1 是否打哈欠 */
    int   head_down;         /* 0/1 是否低头 */
    int   fatigue;           /* 0/1 是否疲劳 */

    char  status[32];        /* 字符串状态，如 NORMAL / EYE_CLOSED / FATIGUE */

#if DMS_ENABLE_LANDMARK_106
    /* 106 点人脸关键点结果（仅当 DMS_ENABLE_LANDMARK_106=1 时存在） */
    dms_face_landmark_106_result_t landmark_106;
#endif
} dms_result_t;

/*
 * 初始化 DMS 推理模块。
 * 尝试加载 /mnt/sdcard/dms/models/retinaface.rknn
 * 和 /mnt/sdcard/dms/models/face_landmark.rknn（仅 DMS_ENABLE_HEADPOSE=1 时）。
 * RetinaFace 初始化失败返回 false。
 */
bool dms_infer_init(void);

/*
 * 处理一帧图像，输出疲劳判断结果。
 * mock 模式下返回固定 NORMAL 状态。
 */
bool dms_infer_process_frame(const frame_data_t *frame, dms_result_t *result);

/*
 * 释放 DMS 推理模块资源。
 */
void dms_infer_deinit(void);

/*
 * 查询当前是否处于 mock 模式。
 */
bool dms_infer_is_mock_mode(void);

#ifdef __cplusplus
}
#endif

#endif
