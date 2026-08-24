#ifndef DMS_FACE_LANDMARK_H
#define DMS_FACE_LANDMARK_H

#include "common.h"
#include "dms_face_detect.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 人脸关键点/姿态检测模块接口。
 * 真实实现加载 /mnt/sdcard/dms/models/face_landmark.rknn（headpose 模型）。
 * mock 实现返回固定 EAR / MAR / head_down_score。
 */

#define DMS_HEADPOSE_LANDMARK_NUM 66

typedef struct {
    int   found;             /* 是否检测到关键点/姿态 */
    float ear;               /* 左眼+右眼平均 EAR（若模型支持） */
    float mar;               /* 嘴巴 MAR（若模型支持） */
    float head_down_score;   /* 低头分数 0.0~1.0 */

    /* 头部姿态角（度）。valid 为 true 时表示模型输出了角度。 */
    int   angles_valid;
    float yaw;
    float pitch;
    float roll;

    /* 若模型输出 3D 密集关键点，最多保存 66 个。 */
    int   landmarks_3d_num;
    float landmarks_3d[DMS_HEADPOSE_LANDMARK_NUM * 3]; /* x,y,z */
} dms_face_landmark_result_t;

bool dms_face_landmark_init(const char *model_path);
bool dms_face_landmark_process(const frame_data_t *frame,
                               const dms_face_detect_result_t *face,
                               dms_face_landmark_result_t *result);
void dms_face_landmark_deinit(void);
bool dms_face_landmark_is_mock(void);

#ifdef __cplusplus
}
#endif

#endif
