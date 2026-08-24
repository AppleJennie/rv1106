#ifndef __COMMON_H__
#define __COMMON_H__

// 内存阈值：超过这个值就报警（单位：KB）
// 1GB = 1024*1024 = 1048576 KB
#define MEMORY_WARN_THRESHOLD_KB    1048576

// 蜂鸣器 GPIO
#define BEEP_GPIO_NUM  35   // GPIO1_D3 = 35

// 蜂鸣器鸣叫间隔（毫秒）
#define BEEP_ON_TIME   300
#define BEEP_OFF_TIME  700

#endif
