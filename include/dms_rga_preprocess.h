#ifndef DMS_RGA_PREPROCESS_H
#define DMS_RGA_PREPROCESS_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS Hardware Video Pipeline V2-A：RGA 硬件预处理模块。
 *
 * 在 VI 帧 Release 之前，用 RGA 直接从 NV12 DMABUF 产出：
 *   1. model_w x model_h BGR（RetinaFace 输入，stretch，与 CPU 版语义一致）
 *   2. 全分辨率 RGB 源图（106 点 crop 的源图，与已验收软件路径完全等价，
 *      坐标无需缩放映射）
 *
 * 线程模型：
 *   - 生产者：capture 线程，调 dms_rga_preprocess_from_vi()（同步，单次应 <10ms）。
 *   - 消费者：AI 线程（唯一），调 dms_rga_preprocess_take()（带超时的 latest-only 取帧）。
 *
 * 所有权：三缓冲（FREE/READY/IN_USE）。
 *   capture 写之前把槽位置为 IN_USE，写完置 READY；
 *   AI 取帧时把最新 READY 置 IN_USE，拷贝完置 FREE；
 *   AI 跟不上时 capture 覆盖最旧 READY（保持 latest-frame-only 语义）。
 *   capture 线程绝不会覆盖 AI 正在读的 buffer。
 *
 * DMS_HW_PREPROCESS=0 时本模块退化为空实现（always not-ready），
 * AI 线程自动走软件 JPEG 路径，便于 A/B 回归。
 */

#if DMS_HW_PREPROCESS

/* 前向声明，避免 dms 头文件依赖 RKMPI 头文件 */
struct rkVIDEO_FRAME_INFO_S;

/* 一帧准备好的 AI 输入（take 返回，指针有效期到下一次 take） */
typedef struct {
    const uint8_t *retina_bgr;   /* retina_w x retina_h x 3 BGR（stretch） */
    int            retina_w;
    int            retina_h;
    const uint8_t *src_rgb;      /* src_w x src_h x 3 RGB 全分辨率源图（= orig 尺寸） */
    int            src_w;
    int            src_h;
    int            orig_w;       /* VI 原始分辨率 */
    int            orig_h;
    int            frame_id;     /* RGA 模块内部递增序号 */
    uint64_t       timestamp_us; /* CLOCK_MONOTONIC，RGA 完成时刻 */
    uint64_t       vi_get_us;    /* RK_MPI_VI_GetChnFrame 耗时 */
    uint64_t       rga_retina_us;/* RGA NV12->BGR+resize 耗时 */
    uint64_t       rga_source_us;/* RGA NV12->RGB 半幅 耗时 */
} dms_prepared_frame_t;

/* 自上次 reset 以来的累计统计（perf 打印用）；produced/dropped 另维护累计总值 */
typedef struct {
    uint64_t produced_frames;    /* 本周期 from_vi 成功产出的帧数 */
    uint64_t dropped_frames;     /* 本周期覆盖最旧 READY 的次数（AI 跟不上） */
    uint64_t produced_total;     /* 累计产出帧数（不随 reset 清零） */
    uint64_t dropped_total;      /* 累计覆盖次数（不随 reset 清零） */
    uint64_t vi_get_us;
    uint64_t rga_retina_us;
    uint64_t rga_source_us;
} dms_rga_preprocess_stats_t;

/*
 * 初始化 RGA 预处理模块。
 * model_w/model_h 为 RetinaFace 输入尺寸（640x640）。
 * 打印 RGA 版本；RGA 不可用时返回 false（上层自动回退软件路径）。
 */
bool dms_rga_preprocess_init(int model_w, int model_h);

/*
 * 在 VI 帧 ReleaseChnFrame 之前调用（capture 线程）。
 * vi_get_us 为调用方测得的 RK_MPI_VI_GetChnFrame 耗时，仅用于统计。
 */
bool dms_rga_preprocess_from_vi(const struct rkVIDEO_FRAME_INFO_S *vi_frame,
                                uint64_t vi_get_us);

/*
 * AI 线程取最新准备好的帧（latest-only，超时返回 false）。
 * 返回的指针指向模块内部消费缓冲，有效期到下一次 take。
 */
bool dms_rga_preprocess_take(dms_prepared_frame_t *out, int timeout_ms);

/* RGA 模块是否可用（init 成功且未连续失败自锁）。 */
bool dms_rga_preprocess_is_ready(void);

void dms_rga_preprocess_get_stats(dms_rga_preprocess_stats_t *out);
void dms_rga_preprocess_reset_stats(void);

void dms_rga_preprocess_deinit(void);

/*
 * 一次性 dump 调试：AI 线程每帧调 dms_hw_dump_check_and_consume()，
 * SD 卡存在 /mnt/sdcard/dms/DUMP_RGA 时删除标志并返回 true（本帧 dump）。
 * dms_hw_dump_ppm() 写 P6 PPM 到 /mnt/sdcard/dms/live/，is_bgr 时做 BGR->RGB。
 */
bool dms_hw_dump_check_and_consume(void);
bool dms_hw_dump_ppm(const char *name, const uint8_t *data, int w, int h, bool is_bgr);

#else /* !DMS_HW_PREPROCESS */

static inline bool dms_rga_preprocess_is_ready(void) { return false; }

#endif /* DMS_HW_PREPROCESS */

#ifdef __cplusplus
}
#endif

#endif /* DMS_RGA_PREPROCESS_H */
