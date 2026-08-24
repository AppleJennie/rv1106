#ifndef HAL_POWER_H
#define HAL_POWER_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

bool power_init(void);
void power_deinit(void);

/*
 * 周期调用。
 * 返回当前是否处于低电状态。
 */
bool power_poll(void);

/*
 * 读取当前低电状态，不触发新的 ADC 读取。
 */
bool power_is_low(void);

/*
 * 获取最近一次 ADC raw 值，调试用。
 */
int power_get_last_raw(void);

#ifdef __cplusplus
}
#endif

#endif
