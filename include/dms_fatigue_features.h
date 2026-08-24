#ifndef DMS_FATIGUE_FEATURES_H
#define DMS_FATIGUE_FEATURES_H

#include "common.h"
#include "dms_infer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 基于 106 点关键点的疲劳特征计算：
 * EAR / MAR / head_down_score + 2s 个人基线 + 事件状态机。
 *
 * 说明：2d106det 语义索引通过索引调试图 + 几何聚类确认为“段范围”：
 *  - 轮廓/下巴：0~32
 *  - 眼 A：33~42，眼 B：87~96（左右由图像 x 坐标动态区分）
 *  - 眉毛：43~51 / 97~105
 *  - 鼻：72~86
 *  - 嘴：52~71
 * 具体 EAR/MAR 不依赖单个死索引，而是在段内按 min/max x/y 动态选取角点与上下唇/眼睑。
 */
void dms_fatigue_features_reset(void);
void dms_fatigue_features_update(dms_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
