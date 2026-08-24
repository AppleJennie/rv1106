#ifndef DMS_VEHICLE_H
#define DMS_VEHICLE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMS Vehicle Interface - 车辆 CAN 总线预留接口。
 *
 * 当前只定义抽象接口和 stub 实现。
 * 不编造具体 CAN ID，等拿到真实车型 DBC/协议后再实现。
 */

/* ==================== 车辆状态 ==================== */
typedef struct {
    float    speed_kmh;         /* 车速 km/h */
    bool     brake_active;      /* 刹车激活 */
    float    accel_pedal_pct;   /* 油门踏板开度 0~100% */
    bool     door_open;         /* 车门开启 */
    bool     valid;             /* 数据有效标志 */
} dms_vehicle_state_t;

/* ==================== API（当前全部为 stub） ==================== */

/* 初始化车辆接口 */
void dms_vehicle_init(void);

/* 获取车速（stub，返回 0） */
float dms_vehicle_get_speed(void);

/* 获取刹车状态（stub，返回 false） */
bool dms_vehicle_get_brake(void);

/* 获取油门踏板（stub，返回 0） */
float dms_vehicle_get_accel_pedal(void);

/* 获取车门状态（stub，返回 false） */
bool dms_vehicle_get_door_state(void);

/* 获取完整车辆状态 */
void dms_vehicle_get_state(dms_vehicle_state_t *state);

/*
 * 通过 CAN 发送 DMS 状态（stub）。
 * 未来将 DMS 风险等级、报警状态等发送到车辆 CAN 总线。
 */
bool dms_vehicle_send_dms_status(uint8_t risk_level, uint8_t alarm_level);

#ifdef __cplusplus
}
#endif

#endif
