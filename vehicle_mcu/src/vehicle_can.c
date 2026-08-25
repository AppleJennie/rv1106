/*
 * vehicle_can.c - 车辆 CAN 总线信号抽象层实现
 *
 * 只维护已解码信号的值/有效性/时效，不解析原始报文。
 * 严禁编造 CAN ID；真实报文由 BSP/HAL 解码后注入。
 *
 * 详见 vehicle_can.h 头部注释。仅依赖 C 标准库。
 */
#include "vehicle_can.h"

#include <string.h>

/* ==================== 内部状态 ==================== */

typedef struct {
    vehicle_can_config_t cfg;
    vehicle_can_state_t  st;

    /* 各路信号最近一次注入时间戳 */
    uint32_t ts_speed;
    uint32_t ts_brake;
    uint32_t ts_accel;
    uint32_t ts_door;
    uint32_t ts_soc;

    bool initialized;
} can_module_state_t;

static can_module_state_t s_can;

/* ==================== 默认配置 ==================== */

void vehicle_can_get_default_config(vehicle_can_config_t *config)
{
    if (!config) return;

    memset(config, 0, sizeof(*config));
    config->speed_stale_ms = 500;    /* 车速 500ms 不刷新视为过期 */
    config->pedal_stale_ms = 500;    /* 踏板 500ms */
    config->door_stale_ms  = 2000;   /* 车门 2000ms */
    config->soc_stale_ms   = 5000;   /* SOC 5000ms */
}

/* ==================== 对外接口 ==================== */

int vehicle_can_init(const vehicle_can_config_t *config)
{
    vehicle_can_config_t cfg;
    if (config) {
        cfg = *config;
    } else {
        vehicle_can_get_default_config(&cfg);
    }

    if (cfg.speed_stale_ms == 0 || cfg.pedal_stale_ms == 0
        || cfg.door_stale_ms == 0 || cfg.soc_stale_ms == 0) {
        return -1;
    }

    memset(&s_can, 0, sizeof(s_can));
    s_can.cfg = cfg;
    s_can.st.door_state = VEHICLE_DOOR_UNKNOWN;
    s_can.initialized = true;
    return 0;
}

void vehicle_can_update_speed(float speed_kph, uint32_t now_ms)
{
    if (!s_can.initialized) return;
    s_can.st.speed_kph = speed_kph;
    s_can.st.valid_flags |= VEHICLE_CAN_VALID_SPEED;
    s_can.ts_speed = now_ms;
}

void vehicle_can_update_brake_pedal(bool pressed, uint32_t now_ms)
{
    if (!s_can.initialized) return;
    s_can.st.brake_pedal = pressed;
    s_can.st.valid_flags |= VEHICLE_CAN_VALID_BRAKE;
    s_can.ts_brake = now_ms;
}

void vehicle_can_update_accelerator_pedal(float percent, uint32_t now_ms)
{
    if (!s_can.initialized) return;
    s_can.st.accelerator_pedal = percent;
    s_can.st.valid_flags |= VEHICLE_CAN_VALID_ACCEL;
    s_can.ts_accel = now_ms;
}

void vehicle_can_update_door(vehicle_door_state_t state, uint32_t now_ms)
{
    if (!s_can.initialized) return;
    s_can.st.door_state = state;
    s_can.st.valid_flags |= VEHICLE_CAN_VALID_DOOR;
    s_can.ts_door = now_ms;
}

void vehicle_can_update_soc(float percent, uint32_t now_ms)
{
    if (!s_can.initialized) return;
    s_can.st.soc = percent;
    s_can.st.valid_flags |= VEHICLE_CAN_VALID_SOC;
    s_can.ts_soc = now_ms;
}

/* 信号是否过期（无符号减法，天然容忍时间戳回绕） */
static bool signal_stale(uint32_t last_ms, uint32_t timeout_ms, uint32_t now_ms)
{
    return (uint32_t)(now_ms - last_ms) > timeout_ms;
}

bool vehicle_can_get_state(vehicle_can_state_t *out, uint32_t now_ms)
{
    if (!out) return false;

    memset(out, 0, sizeof(*out));
    out->door_state = VEHICLE_DOOR_UNKNOWN;

    if (!s_can.initialized) return false;

    uint8_t flags = 0;

    if ((s_can.st.valid_flags & VEHICLE_CAN_VALID_SPEED)
        && !signal_stale(s_can.ts_speed, s_can.cfg.speed_stale_ms, now_ms)) {
        out->speed_kph = s_can.st.speed_kph;
        flags |= VEHICLE_CAN_VALID_SPEED;
    }
    if ((s_can.st.valid_flags & VEHICLE_CAN_VALID_BRAKE)
        && !signal_stale(s_can.ts_brake, s_can.cfg.pedal_stale_ms, now_ms)) {
        out->brake_pedal = s_can.st.brake_pedal;
        flags |= VEHICLE_CAN_VALID_BRAKE;
    }
    if ((s_can.st.valid_flags & VEHICLE_CAN_VALID_ACCEL)
        && !signal_stale(s_can.ts_accel, s_can.cfg.pedal_stale_ms, now_ms)) {
        out->accelerator_pedal = s_can.st.accelerator_pedal;
        flags |= VEHICLE_CAN_VALID_ACCEL;
    }
    if ((s_can.st.valid_flags & VEHICLE_CAN_VALID_DOOR)
        && !signal_stale(s_can.ts_door, s_can.cfg.door_stale_ms, now_ms)) {
        out->door_state = s_can.st.door_state;
        flags |= VEHICLE_CAN_VALID_DOOR;
    }
    if ((s_can.st.valid_flags & VEHICLE_CAN_VALID_SOC)
        && !signal_stale(s_can.ts_soc, s_can.cfg.soc_stale_ms, now_ms)) {
        out->soc = s_can.st.soc;
        flags |= VEHICLE_CAN_VALID_SOC;
    }

    out->valid_flags = flags;
    return flags != 0;
}

/*
 * 硬件层 stub：默认无硬件实现。
 * 移植到目标硬件时由 BSP 重新实现本函数：读取 CAN 控制器报文、
 * 按整车厂 DBC 解码，然后调用 vehicle_can_update_*() 注入。
 */
bool vehicle_can_hal_poll(uint32_t now_ms)
{
    (void)now_ms;
    return false;
}
