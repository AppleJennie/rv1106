#ifndef HAL_ENCODER_H
#define HAL_ENCODER_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 I2C 总线、TCA9548A 和 MT6701，并进行预热
bool encoder_init(void);

// 读取单个编码器角度（ch: 0~2），返回 0~360.0°，失败返回 -1.0
float encoder_read(uint8_t ch);

// 一次性读取所有 3 个编码器角度
bool encoder_read_all(float *angle_buf);

// 反初始化编码器
void encoder_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
