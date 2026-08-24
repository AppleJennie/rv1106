#ifndef SYS_STORAGE_H
#define SYS_STORAGE_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    frame_data_t buffer[RING_BUFFER_SIZE];
    int head;
    int tail;
    int count;
    int inflight_count;   /* 已取出但未处理完的帧数 */
    int dropped_count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    pthread_cond_t drained;   /* 队列和 inflight 均为空时触发 */
} ring_buffer_t;

bool storage_init(void);
bool storage_start_session(void);
void storage_stop_session(void);
bool storage_push_frame(const frame_data_t *frame);
bool storage_push_encoder_sample(const encoder_sample_t *sample);
void storage_deinit(void);

bool storage_check_sdcard(void);

/* 获取当前丢帧统计 */
int storage_get_dropped_count(void);

#ifdef __cplusplus
}
#endif

#endif
