#ifndef DMS_AI_THREAD_H
#define DMS_AI_THREAD_H

#include "common.h"
#include "dms_infer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS AI 推理线程。
 *
 * 视频采集线程把最新 JPEG 通过 dms_ai_thread_submit_frame() 提交到单槽缓冲，
 * 然后立即继续采集；AI 线程在后台异步处理最新帧，永远不会阻塞 MJPEG 推流。
 */

bool dms_ai_thread_start(void);
void dms_ai_thread_stop(void);

/* 视频采集线程每抓到一帧调用一次。 */
void dms_ai_thread_submit_frame(const uint8_t *jpg, size_t jpg_size,
                                 int frame_id, uint64_t timestamp_us);

/* 获取最近一次 AI 推理结果（拷贝）。 */
bool dms_ai_thread_get_latest_result(dms_result_t *result);

/* 视频流每推送一帧调用一次，用于统计 stream_fps。 */
void dms_ai_thread_inc_stream_frames(void);

/* 每 5 秒打印一次性能统计（内部自动限频）。 */
void dms_ai_thread_print_stats(void);

/* 获取最近一次统计周期内的 AI FPS。 */
float dms_ai_thread_get_ai_fps(void);

#ifdef __cplusplus
}
#endif

#endif
