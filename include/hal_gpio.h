#ifndef HAL_GPIO_H
#define HAL_GPIO_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 GPIO
bool gpio_init(void);

// 读取录制按键状态（GPIO1），true=按下
bool gpio_record_is_pressed(void);

// 读取上传按键状态（GPIO2），true=按下
bool gpio_upload_is_pressed(void);

// 反初始化 GPIO
void gpio_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
