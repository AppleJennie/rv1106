#include "common.h"
#include "hal_beep.h"
#include "sys_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/statvfs.h>

static int g_beep_gpio_fd = -1;
static pthread_t g_beep_thread;
static volatile bool g_beep_running = false;
static bool g_beep_thread_created = false;

/* ==================== GPIO 工具函数 ==================== */

static int gpio_export_unexport(int gpio, bool do_export)
{
    const char *path = do_export ? "/sys/class/gpio/export" : "/sys/class/gpio/unexport";
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", gpio);
    write(fd, buf, strlen(buf));
    close(fd);
    return 0;
}

static int gpio_set_direction_out(int gpio)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    write(fd, "out", 3);
    close(fd);
    return 0;
}

static int gpio_open_value_fd(int gpio)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    return open(path, O_WRONLY);
}

static void gpio_write_value(int fd, int val)
{
    if (fd < 0) return;
    const char *v = val ? "1\n" : "0\n";
    write(fd, v, strlen(v));
}

/* ==================== SD 卡剩余空间 ==================== */

static long get_sd_free_mb(void)
{
    struct statvfs vfs;

    if (statvfs(SDCARD_BASE_PATH, &vfs) != 0) {
        log_warn("读取 SD 卡剩余空间失败: %s, errno=%d",
                 SDCARD_BASE_PATH, errno);
        return -1;
    }

    unsigned long long free_bytes =
        (unsigned long long)vfs.f_bavail *
        (unsigned long long)vfs.f_bsize;

    return (long)(free_bytes / 1024ULL / 1024ULL);
}

/* ==================== 蜂鸣器动作 ==================== */

static void beep_once(void)
{
    gpio_write_value(g_beep_gpio_fd, 1);
    usleep(BEEP_ON_TIME_MS * 1000);
    gpio_write_value(g_beep_gpio_fd, 0);
}

/* ==================== 监控线程 ==================== */

static void *beep_monitor_thread_func(void *arg)
{
    (void)arg;
    int warn_count = 0;

    log_info("蜂鸣器 SD 卡容量监控线程启动，剩余空间阈值=%d MB，最多报警%d次",
             STORAGE_FREE_WARN_THRESHOLD_MB, BEEP_MAX_WARN_COUNT);

    while (g_beep_running && g_run_flag) {
        long free_mb = get_sd_free_mb();

        if (free_mb < 0) {
            sleep(STORAGE_CHECK_INTERVAL_SEC);
            continue;
        }

        if (free_mb < STORAGE_FREE_WARN_THRESHOLD_MB) {
            if (warn_count < BEEP_MAX_WARN_COUNT) {
                log_warn("SD 卡剩余空间 %ld MB 低于阈值 %d MB，报警第 %d/%d 次",
                         free_mb, STORAGE_FREE_WARN_THRESHOLD_MB,
                         warn_count + 1, BEEP_MAX_WARN_COUNT);
                beep_once();
                warn_count++;
            }
            usleep(BEEP_OFF_TIME_MS * 1000);
        } else {
            if (warn_count > 0) {
                log_info("SD 卡剩余空间恢复到 %ld MB，报警计数重置", free_mb);
            }
            warn_count = 0;
            sleep(STORAGE_CHECK_INTERVAL_SEC);
        }
    }

    gpio_write_value(g_beep_gpio_fd, 0);
    log_info("蜂鸣器 SD 卡容量监控线程退出");
    return NULL;
}

/* ==================== 对外接口 ==================== */

bool beep_init(void)
{
    log_info("初始化蜂鸣器 GPIO%d...", BEEP_GPIO_NUM);

    gpio_export_unexport(BEEP_GPIO_NUM, true);
    usleep(100000);

    if (gpio_set_direction_out(BEEP_GPIO_NUM) != 0) {
        log_error("蜂鸣器 GPIO%d 方向设置失败", BEEP_GPIO_NUM);
        return false;
    }

    g_beep_gpio_fd = gpio_open_value_fd(BEEP_GPIO_NUM);
    if (g_beep_gpio_fd < 0) {
        log_error("蜂鸣器 GPIO%d value 文件打开失败", BEEP_GPIO_NUM);
        return false;
    }

    gpio_write_value(g_beep_gpio_fd, 0);

    g_beep_running = true;
    if (pthread_create(&g_beep_thread, NULL, beep_monitor_thread_func, NULL) != 0) {
        log_error("创建蜂鸣器监控线程失败");
        g_beep_running = false;
        close(g_beep_gpio_fd);
        g_beep_gpio_fd = -1;
        return false;
    }

    g_beep_thread_created = true;
    log_info("蜂鸣器初始化完成");
    return true;
}

void beep_trigger_once(void)
{
    beep_once();
}

void beep_deinit(void)
{
    g_beep_running = false;

    if (g_beep_thread_created) {
        pthread_join(g_beep_thread, NULL);
        g_beep_thread_created = false;
    }

    if (g_beep_gpio_fd >= 0) {
        gpio_write_value(g_beep_gpio_fd, 0);
        close(g_beep_gpio_fd);
        g_beep_gpio_fd = -1;
    }

    gpio_export_unexport(BEEP_GPIO_NUM, false);

    log_info("蜂鸣器已关闭");
}


