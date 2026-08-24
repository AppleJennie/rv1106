#ifndef SYS_HEALTH_H
#define SYS_HEALTH_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 启动前环境检查，任一关键项失败返回 false */
bool health_check_before_start(void);

/* 检查 SD 卡剩余空间（MB），低于阈值返回 false */
bool health_check_disk_space(const char *path, int min_free_mb);

/* 产品化新增：启动前清理残留环境（杀服务、清 socket、等节点） */
void health_prepare_runtime_environment(void);

/* 产品化新增：等待关键设备节点出现，timeout_ms 为超时时间 */
bool health_wait_device_ready(const char *path, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
