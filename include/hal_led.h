#ifndef HAL_LED_H
#define HAL_LED_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_ON,
    LED_MODE_BLINK
} led_mode_e;

bool led_init(void);
void led_deinit(void);
bool led_is_available(void);

// blink_period_ms 仅在 BLINK 模式有效
void led_set_mode(int led_idx, led_mode_e mode, led_color_e color, int blink_period_ms);

// 主循环里定时调用
void led_tick(void);

#ifdef __cplusplus
}
#endif

#endif