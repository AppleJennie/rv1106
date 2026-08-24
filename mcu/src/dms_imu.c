#include "dms_imu.h"

#include <string.h>

/*
 * DMS IMU Interface - stub 实现。
 *
 * 当前只提供接口定义，不连接真实 IMU 传感器。
 */

void dms_imu_init(void)
{
    /* STUB */
}

bool dms_imu_get_sample(imu_sample_t *sample)
{
    if (!sample) return false;
    memset(sample, 0, sizeof(*sample));
    return false;  /* 无有效数据 */
}

bool dms_imu_get_driving_events(imu_driving_event_t *events)
{
    if (!events) return false;
    memset(events, 0, sizeof(*events));
    return false;  /* 无事件 */
}
