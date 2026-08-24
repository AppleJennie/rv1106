#ifndef NET_TRANSFER_H
#define NET_TRANSFER_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

bool upload_init(void);

/* 开始上传，内部应该周期性检查 cancel_flag */
bool upload_all_sessions(const char *base_path, volatile int *cancel_flag);

/* 请求取消上传 */
void upload_request_cancel(void);

/* 上传模块释放 */
void upload_deinit(void);

/* 查询待上传组数量（用于状态机显示） */
int upload_count_pending(const char *base_path);

#ifdef __cplusplus
}
#endif

#endif
