#ifndef VEHICLE_CAN_H
#define VEHICLE_CAN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * vehicle_can.h - 车辆 CAN 总线信号抽象层
 *
 * 定位：
 *   - 本模块只维护"已解码"的车辆信号状态及其有效性/时效，
 *     不解析原始 CAN 报文。
 *   - 严禁在本模块中编造任何 CAN ID。真实报文 ID / 信号布局必须
 *     来自整车厂提供的 DBC / 通讯矩阵文档，由底层驱动（BSP/HAL）
 *     完成收发与解码后，调用 vehicle_can_update_*() 注入。
 *   - CAN 缺失（从未注入或信号过期）时，所有依赖本模块的上层
 *     功能必须继续可用：valid_flags 全 0 即可，不得阻塞。
 *
 * 依赖：仅 C 标准库，自包含。
 */

/* 信号有效标志位（vehicle_can_state_t.valid_flags） */
#define VEHICLE_CAN_VALID_SPEED   0x01u   /* 车速有效 */
#define VEHICLE_CAN_VALID_BRAKE   0x02u   /* 刹车踏板有效 */
#define VEHICLE_CAN_VALID_ACCEL   0x04u   /* 油门开度有效 */
#define VEHICLE_CAN_VALID_DOOR    0x08u   /* 车门状态有效 */
#define VEHICLE_CAN_VALID_SOC     0x10u   /* 电池 SOC 有效 */

/* 车门状态 */
typedef enum {
    VEHICLE_DOOR_UNKNOWN = 0,
    VEHICLE_DOOR_CLOSED,
    VEHICLE_DOOR_OPEN
} vehicle_door_state_t;

/* 已解码车辆信号快照 */
typedef struct {
    float                speed_kph;          /* 车速 (km/h) */
    bool                 brake_pedal;        /* 刹车踏板：true=踩下 */
    float                accelerator_pedal;  /* 油门开度 (0~100 %) */
    vehicle_door_state_t door_state;         /* 车门状态 */
    float                soc;                /* 电池 SOC (0~100 %) */
    uint8_t              valid_flags;        /* VEHICLE_CAN_VALID_* 位组合 */
} vehicle_can_state_t;

/*
 * CAN 配置（阈值集中管理）。
 * 注意：以下默认值仅为工程初始值，需按整车通讯矩阵实车标定。
 */
typedef struct {
    uint32_t speed_stale_ms;   /* 车速信号过期时间，默认 500ms */
    uint32_t pedal_stale_ms;   /* 踏板信号过期时间，默认 500ms */
    uint32_t door_stale_ms;    /* 车门信号过期时间，默认 2000ms */
    uint32_t soc_stale_ms;     /* SOC 信号过期时间，默认 5000ms */
} vehicle_can_config_t;

/* 获取默认配置 */
void vehicle_can_get_default_config(vehicle_can_config_t *config);

/* 初始化。config 传 NULL 使用默认配置。返回 0 成功，-1 参数非法 */
int vehicle_can_init(const vehicle_can_config_t *config);

/* --- 信号注入（由 BSP/HAL 解码真实报文后调用） --- */
void vehicle_can_update_speed(float speed_kph, uint32_t now_ms);
void vehicle_can_update_brake_pedal(bool pressed, uint32_t now_ms);
void vehicle_can_update_accelerator_pedal(float percent, uint32_t now_ms);
void vehicle_can_update_door(vehicle_door_state_t state, uint32_t now_ms);
void vehicle_can_update_soc(float percent, uint32_t now_ms);

/*
 * 读取信号快照（带回放过期检查）。
 * 过期信号的对应 valid_flags 位清零、字段值清零/置 UNKNOWN，绝不阻塞。
 * 返回 true 表示至少一路信号有效；false 表示 CAN 完全缺失。
 */
bool vehicle_can_get_state(vehicle_can_state_t *out, uint32_t now_ms);

/*
 * 硬件层 stub：从真实 CAN 控制器轮询报文并解码注入。
 * 默认实现：无硬件，直接返回 false。移植到目标硬件时由 BSP 重新实现。
 * 再次强调：禁止编造 CAN ID，ID 必须来自整车厂 DBC 文档。
 */
bool vehicle_can_hal_poll(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* VEHICLE_CAN_H */
