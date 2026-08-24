#ifndef HAL_CAMERA_H
#define HAL_CAMERA_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

bool camera_init(void);
bool camera_grab_jpeg(uint8_t **jpeg_buf, int *jpeg_size);
void camera_deinit(void);

/* 产品化新增：重启摄像头链路 */
bool camera_restart(void);

/* 产品化新增：查询摄像头健康状态 */
bool camera_health_ok(void);

/* 产品化新增：查询当前实际分辨率/FPS */
void camera_get_resolution(int *width, int *height, int *fps);

/* 产品化新增：获取统计信息 */
typedef struct {
    bool initialized;
    int grab_ok_count;          /* 成功抓取帧数 */
    int grab_fail_count;        /* 当前连续失败计数 */
    int total_fail_count;       /* 累计失败次数 */
    int timeout_count;          /* GetStream 超时次数 */
    int restart_count;          /* 链路重启次数 */
    int restart_fail_count;     /* 重启失败次数 */
    int last_fps;               /* 上一秒实际 FPS */
    size_t last_jpeg_size;      /* 上一帧 JPEG 大小 */
    size_t avg_jpeg_size;       /* 平均 JPEG 大小 */
} camera_stats_t;

void camera_get_stats(camera_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif
