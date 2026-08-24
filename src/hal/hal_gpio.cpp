#include "hal_gpio.h"
#include "common.h"
#include "sys_logger.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

/*
 * hal_gpio.cpp
 *
 * 修改目标：
 * 1. 不再每次读取 GPIO 都 ifstream open/close。
 * 2. gpio_init() 时打开 value 文件，后续只 lseek + read。
 * 3. 降低 100Hz 轮询时的系统调用和 C++ 流开销。
 * 4. 保持原来的接口不变：
 *      gpio_init()
 *      gpio_record_is_pressed()
 *      gpio_upload_is_pressed()
 *      gpio_deinit()
 */

static int g_record_fd = -1;
static int g_upload_fd = -1;

static char g_record_value_path[128];
static char g_upload_value_path[128];

static bool write_text_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return false;
    }

    size_t len = strlen(text);
    const char *p = text;

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            close(fd);
            return false;
        }

        if (n == 0) {
            close(fd);
            return false;
        }

        p += n;
        len -= (size_t)n;
    }

    close(fd);
    return true;
}

static bool path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool wait_path_ready(const char *path, int timeout_ms)
{
    int waited = 0;

    while (waited < timeout_ms) {
        if (path_exists(path)) {
            return true;
        }

        usleep(10000);
        waited += 10;
    }

    return false;
}

static bool gpio_export_one(int gpio)
{
    char gpio_dir[128];
    char gpio_num[32];

    snprintf(gpio_dir, sizeof(gpio_dir), "/sys/class/gpio/gpio%d", gpio);

    if (path_exists(gpio_dir)) {
        return true;
    }

    snprintf(gpio_num, sizeof(gpio_num), "%d", gpio);

    if (!write_text_file("/sys/class/gpio/export", gpio_num)) {
        /*
         * 如果已经 export，写 export 可能返回 EBUSY。
         * 这里再检查一次目录，存在就认为成功。
         */
        if (path_exists(gpio_dir)) {
            return true;
        }

        log_error("GPIO%d export 失败, errno=%d", gpio, errno);
        return false;
    }

    if (!wait_path_ready(gpio_dir, 1000)) {
        log_error("GPIO%d export 后目录未就绪: %s", gpio, gpio_dir);
        return false;
    }

    return true;
}

static bool gpio_unexport_one(int gpio)
{
    char gpio_num[32];

    snprintf(gpio_num, sizeof(gpio_num), "%d", gpio);

    if (!write_text_file("/sys/class/gpio/unexport", gpio_num)) {
        /*
         * unexport 失败通常不是致命问题：
         * 可能已经被系统释放，或者权限不允许。
         */
        log_warn("GPIO%d unexport 失败或已释放, errno=%d", gpio, errno);
        return false;
    }

    return true;
}

static bool gpio_set_direction_in(int gpio)
{
    char path[128];

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);

    if (!wait_path_ready(path, 1000)) {
        log_error("GPIO%d direction 节点未就绪: %s", gpio, path);
        return false;
    }

    if (!write_text_file(path, "in")) {
        log_error("GPIO%d 设置 direction=in 失败, errno=%d", gpio, errno);
        return false;
    }

    return true;
}

static int gpio_open_value_fd(int gpio, char *out_path, size_t out_len)
{
    snprintf(out_path, out_len, "/sys/class/gpio/gpio%d/value", gpio);

    if (!wait_path_ready(out_path, 1000)) {
        log_error("GPIO%d value 节点未就绪: %s", gpio, out_path);
        return -1;
    }

    int fd = open(out_path, O_RDONLY);
    if (fd < 0) {
        log_error("GPIO%d value 打开失败: %s, errno=%d", gpio, out_path, errno);
        return -1;
    }

    return fd;
}

static bool gpio_read_value_fd(int fd)
{
    if (fd < 0) {
        return false;
    }

    char buf[8] = {0};

    if (lseek(fd, 0, SEEK_SET) < 0) {
        return false;
    }

    ssize_t n;

    do {
        n = read(fd, buf, sizeof(buf) - 1);
    } while (n < 0 && errno == EINTR);

    if (n <= 0) {
        return false;
    }

    /*
     * sysfs GPIO value 通常返回：
     * "0\n" 或 "1\n"
     */
    return buf[0] == '1';
}

static void gpio_close_fd(int *fd)
{
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

bool gpio_init(void)
{
    log_info("初始化 GPIO 按键/控制输入...");

    g_record_fd = -1;
    g_upload_fd = -1;

    if (!gpio_export_one(RECORD_BTN_GPIO)) {
        log_error("录制 GPIO export 失败: gpio%d", RECORD_BTN_GPIO);
        goto fail;
    }

    if (!gpio_set_direction_in(RECORD_BTN_GPIO)) {
        log_error("录制 GPIO 设置输入失败: gpio%d", RECORD_BTN_GPIO);
        goto fail;
    }

    g_record_fd = gpio_open_value_fd(RECORD_BTN_GPIO,
                                     g_record_value_path,
                                     sizeof(g_record_value_path));
    if (g_record_fd < 0) {
        goto fail;
    }

    if (!gpio_export_one(UPLOAD_BTN_GPIO)) {
        log_error("上传 GPIO export 失败: gpio%d", UPLOAD_BTN_GPIO);
        goto fail;
    }

    if (!gpio_set_direction_in(UPLOAD_BTN_GPIO)) {
        log_error("上传 GPIO 设置输入失败: gpio%d", UPLOAD_BTN_GPIO);
        goto fail;
    }

    g_upload_fd = gpio_open_value_fd(UPLOAD_BTN_GPIO,
                                     g_upload_value_path,
                                     sizeof(g_upload_value_path));
    if (g_upload_fd < 0) {
        goto fail;
    }

    log_info("GPIO 初始化完成: record=gpio%d, upload=gpio%d",
             RECORD_BTN_GPIO, UPLOAD_BTN_GPIO);

    return true;

fail:
    gpio_close_fd(&g_record_fd);
    gpio_close_fd(&g_upload_fd);

    /*
     * 失败时尝试释放，避免下次启动残留。
     */
    gpio_unexport_one(RECORD_BTN_GPIO);
    gpio_unexport_one(UPLOAD_BTN_GPIO);

    return false;
}

bool gpio_record_is_pressed(void)
{
    /*
     * 高电平有效，保持原逻辑不变。
     */
    return gpio_read_value_fd(g_record_fd);
}

bool gpio_upload_is_pressed(void)
{
    /*
     * 高电平有效，保持原逻辑不变。
     */
    return gpio_read_value_fd(g_upload_fd);
}

void gpio_deinit(void)
{
    gpio_close_fd(&g_record_fd);
    gpio_close_fd(&g_upload_fd);

    gpio_unexport_one(RECORD_BTN_GPIO);
    gpio_unexport_one(UPLOAD_BTN_GPIO);

    log_info("GPIO 已释放");
}
