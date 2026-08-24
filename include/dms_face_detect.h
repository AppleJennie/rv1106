#ifndef DMS_FACE_DETECT_H
#define DMS_FACE_DETECT_H

#include "common.h"
#include "dms_image_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 人脸检测模块接口。
 * 真实实现加载 /mnt/sdcard/dms/models/retinaface.rknn 做人脸 bbox 检测。
 * 模型不存在时初始化失败，不会进入 mock。
 *
 * RetinaFace 输出 5 个关键点（双眼、鼻尖、左右嘴角）。
 */

#define DMS_FACE_KPT_NUM 5

typedef struct {
    int   found;
    float score;
    int   x;
    int   y;
    int   w;
    int   h;

    /* 5 个关键点在原始图像中的坐标。未检测到时值为 0。 */
    float kpt[DMS_FACE_KPT_NUM * 2]; /* x0,y0,x1,y1,... */
} dms_face_detect_result_t;

bool dms_face_detect_init(const char *model_path);
bool dms_face_detect_process(const frame_data_t *frame, dms_face_detect_result_t *result);

/*
 * 同 dms_face_detect_process，但当 out_img 非 NULL 时把已解码 RGB 图所有权转给调用者，
 * 供后续 landmark 复用，避免同一帧重复 JPEG decode。调用者负责 dms_free_image(out_img)。
 */
bool dms_face_detect_process_ex(const frame_data_t *frame,
                                dms_face_detect_result_t *result,
                                dms_image_t *out_img);
void dms_face_detect_deinit(void);
bool dms_face_detect_is_mock(void);

#ifdef __cplusplus
}
#endif

#endif
