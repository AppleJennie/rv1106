#ifndef SYS_TIME_H
#define SYS_TIME_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

// 获取微秒级时间戳
uint64_t time_get_timestamp_us(void);

// 获取毫秒级时间戳
uint64_t time_get_timestamp_ms(void);

// 生成时间戳文件夹名（格式：YYYY-MM-DD_HH-MM-SS）
void time_get_folder_name(char *buf, int buf_len);

// 从机同步主控时间（右手主控可忽略，但保留接口）
bool time_sync_with_master(const char *master_ip, int port);

#ifdef __cplusplus
}
#endif

#endif
