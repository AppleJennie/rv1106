#include "dms_vehicle.h"

#include <string.h>

/*
 * DMS Vehicle Interface - stub 实现。
 *
 * 当前所有函数返回默认值，不连接真实 CAN 总线。
 * 等拿到真实车型 DBC/协议后再实现。
 */

void dms_vehicle_init(void)
{
    /* STUB */
}

float dms_vehicle_get_speed(void)
{
    /* STUB: 无真实数据时返回 0 */
    return 0.0f;
}

bool dms_vehicle_get_brake(void)
{
    /* STUB */
    return false;
}

float dms_vehicle_get_accel_pedal(void)
{
    /* STUB */
    return 0.0f;
}

bool dms_vehicle_get_door_state(void)
{
    /* STUB */
    return false;
}

void dms_vehicle_get_state(dms_vehicle_state_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->valid = false;  /* stub 数据无效 */
}

bool dms_vehicle_send_dms_status(uint8_t risk_level, uint8_t alarm_level)
{
    /* STUB: 不实际发送 CAN 帧 */
    (void)risk_level;
    (void)alarm_level;
    return false;
}
