#ifndef VEHICLE_MCU_TEST_COMMON_H
#define VEHICLE_MCU_TEST_COMMON_H

/*
 * test_common.h - vehicle_mcu 单元测试公共宏
 *
 * 极简断言宏：每个用例打印一行 "PASS: ..." 或 "FAIL: ..."，
 * 由 run_tests.sh 统计总数。各测试程序返回码 0=全过，1=有失败。
 */

#include <stdio.h>
#include <math.h>

static int g_pass_count = 0;
static int g_fail_count = 0;

#define CHECK_TRUE(cond, name)                                                  \
    do {                                                                        \
        if (cond) {                                                             \
            printf("PASS: %s\n", name);                                         \
            g_pass_count++;                                                     \
        } else {                                                                \
            printf("FAIL: %s (条件不满足) [%s:%d]\n", name, __FILE__, __LINE__); \
            g_fail_count++;                                                     \
        }                                                                       \
    } while (0)

#define CHECK_NEAR(val, expect, tol, name)                                      \
    do {                                                                        \
        float _v = (float)(val);                                                \
        float _e = (float)(expect);                                             \
        float _t = (float)(tol);                                                \
        if (fabsf(_v - _e) <= _t) {                                             \
            printf("PASS: %s (%.4f 在 %.4f±%.4f 内)\n", name,                   \
                   (double)_v, (double)_e, (double)_t);                         \
            g_pass_count++;                                                     \
        } else {                                                                \
            printf("FAIL: %s (实测 %.4f, 期望 %.4f±%.4f) [%s:%d]\n",            \
                   name, (double)_v, (double)_e, (double)_t,                    \
                   __FILE__, __LINE__);                                         \
            g_fail_count++;                                                     \
        }                                                                       \
    } while (0)

#define TEST_SUMMARY(mod)                                                       \
    do {                                                                        \
        printf("[%s] 通过 %d, 失败 %d\n", mod, g_pass_count, g_fail_count);     \
        return (g_fail_count == 0) ? 0 : 1;                                     \
    } while (0)

#endif /* VEHICLE_MCU_TEST_COMMON_H */
