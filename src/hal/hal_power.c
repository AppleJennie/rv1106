#include "hal_power.h"
#include "sys_logger.h"

#include <stdio.h>
#include <errno.h>

static bool g_power_low = false;
static int g_low_count = 0;
static int g_last_raw = -1;
static bool g_power_inited = false;

static int read_power_adc_raw(void)
{
    FILE *fp = fopen(POWER_ADC_RAW_PATH, "r");
    if (!fp) {
        log_warn("POWER_ADC 打开失败: %s, errno=%d",
                 POWER_ADC_RAW_PATH, errno);
        return -1;
    }

    int raw = -1;

    if (fscanf(fp, "%d", &raw) != 1) {
        raw = -1;
    }

    fclose(fp);
    return raw;
}

bool power_init(void)
{
    g_power_low = false;
    g_low_count = 0;
    g_last_raw = -1;
    g_power_inited = true;

    int raw = read_power_adc_raw();
    if (raw < 0) {
        log_warn("电源 ADC 初始化读取失败，后续继续重试: %s",
                 POWER_ADC_RAW_PATH);
        return true;
    }

    g_last_raw = raw;

    log_info("电源 ADC 初始化完成: path=%s raw=%d low_th=%d recover_th=%d",
             POWER_ADC_RAW_PATH,
             raw,
             POWER_LOW_RAW_THRESHOLD,
             POWER_RECOVER_RAW_THRESHOLD);

    return true;
}

void power_deinit(void)
{
    g_power_inited = false;
    g_power_low = false;
    g_low_count = 0;
    g_last_raw = -1;
    log_info("电源 ADC 检测关闭");
}

bool power_poll(void)
{
    if (!g_power_inited) {
        return false;
    }

    int raw = read_power_adc_raw();
    if (raw < 0) {
        return g_power_low;
    }

    g_last_raw = raw;

    if (!g_power_low) {
        /*
         * 正常状态下，连续 POWER_LOW_CONFIRM_COUNT 次低于阈值，
         * 才确认低电，避免 ADC 抖动误触发。
         */
        if (raw < POWER_LOW_RAW_THRESHOLD) {
            g_low_count++;

            log_warn("POWER_ADC 偏低: raw=%d count=%d/%d",
                     raw,
                     g_low_count,
                     POWER_LOW_CONFIRM_COUNT);

            if (g_low_count >= POWER_LOW_CONFIRM_COUNT) {
                g_power_low = true;
                log_error("电源低电确认: raw=%d，进入低电报警状态", raw);
            }
        } else {
            g_low_count = 0;
        }
    } else {
        /*
         * 低电状态下，必须高于恢复阈值才退出。
         * 760/800 形成滞回，防止反复闪烁切换。
         */
        if (raw > POWER_RECOVER_RAW_THRESHOLD) {
            g_power_low = false;
            g_low_count = 0;

            log_info("电源电压恢复: raw=%d，退出低电报警状态", raw);
        }
    }

    return g_power_low;
}

bool power_is_low(void)
{
    return g_power_low;
}

int power_get_last_raw(void)
{
    return g_last_raw;
}
