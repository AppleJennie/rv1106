#ifndef HAL_BEEP_H
#define HAL_BEEP_H

#include <stdbool.h>

bool beep_init(void);
void beep_deinit(void);

/* 对外触发一次蜂鸣器，用于报警 */
void beep_trigger_once(void);

#endif
