#ifndef DMS_VISUALIZE_H
#define DMS_VISUALIZE_H

#include "common.h"
#include "dms_infer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 把原始 JPEG + dms_result_t 叠加 bbox 与 HUD 后重新编码为 JPEG。
 * 返回的 buffer 需要调用者 free()。
 *
 * out_size: 输出 JPEG 字节数
 * ai_fps:   显示在 HUD 上的 AI 帧率
 *
 * 返回 NULL 表示失败。
 */
uint8_t *dms_visualize_generate(const uint8_t *jpg, size_t jpg_size,
                                 const dms_result_t *result,
                                 float ai_fps, size_t *out_size);

/* 可视化阶段耗时（微秒）：JPEG 解码、绘制、JPEG 编码、总耗时 */
typedef struct {
    uint64_t jpeg_decode_us;
    uint64_t draw_us;
    uint64_t jpeg_encode_us;
    uint64_t total_us;
} dms_visualize_timing_t;

const dms_visualize_timing_t *dms_visualize_last_timing(void);
const dms_visualize_timing_t *dms_visualize_avg_timing(void);
void dms_visualize_reset_timing_stats(void);

#ifdef __cplusplus
}
#endif

#endif
