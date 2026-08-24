#ifndef DMS_STREAM_SERVER_H
#define DMS_STREAM_SERVER_H

#include <stdint.h>
#include <stddef.h>
#include "dms_infer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS HTTP/MJPEG 实时推流服务。
 * 用于在电脑浏览器实时查看摄像头画面和识别状态。
 */

bool dms_stream_server_init(int port);
void dms_stream_server_update_frame(const uint8_t *jpg, size_t jpg_size, const dms_result_t *result);
void dms_stream_server_update_debug_frame(const uint8_t *jpg, size_t jpg_size, const dms_result_t *result);
bool dms_stream_server_debug_active(void);
void dms_stream_server_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* DMS_STREAM_SERVER_H */
