#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "common.h"

// ----------------------------
// RV1106 GPIO 控制（通用）
// ----------------------------
int gpio_export(int gpio) {
    char buf[256];
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) return -1;
    sprintf(buf, "%d", gpio);
    write(fd, buf, strlen(buf));
    close(fd);
    return 0;
}

int gpio_dir(int gpio, int out) {
    char buf[256];
    sprintf(buf, "/sys/class/gpio/gpio%d/direction", gpio);
    int fd = open(buf, O_WRONLY);
    if (fd < 0) return -1;
    write(fd, out ? "out" : "in", out ? 3 : 2);
    close(fd);
    return 0;
}

int gpio_write(int gpio, int val) {
    char buf[256];
    sprintf(buf, "/sys/class/gpio/gpio%d/value", gpio);
    int fd = open(buf, O_WRONLY);
    if (fd < 0) return -1;
    write(fd, val ? "1" : "0", 1);
    close(fd);
    return 0;
}

// ----------------------------
// 获取当前内存使用量（单位 KB）
// ----------------------------
long get_memory_used_kb() {
    FILE *fp;
    char buf[256];
    long total, available;

    fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;

    fgets(buf, sizeof(buf), fp); // MemTotal
    sscanf(buf, "MemTotal: %ld kB", &total);

    fgets(buf, sizeof(buf), fp); // MemFree
    fgets(buf, sizeof(buf), fp); // MemAvailable
    sscanf(buf, "MemAvailable: %ld kB", &available);

    fclose(fp);
    return total - available;
}

// ----------------------------
// 蜂鸣器滴滴叫
// ----------------------------
void beep_beep() {
    gpio_write(BEEP_GPIO_NUM, 1);
    usleep(BEEP_ON_TIME * 1000);
    gpio_write(BEEP_GPIO_NUM, 0);
    usleep(BEEP_OFF_TIME * 1000);
}

// ----------------------------
// 主逻辑
// ----------------------------
int main() {
    // 初始化 GPIO
    gpio_export(BEEP_GPIO_NUM);
    usleep(100000);
    gpio_dir(BEEP_GPIO_NUM, 1);
    gpio_write(BEEP_GPIO_NUM, 0); // 初始关闭

    printf("LuckFox Pico RV1106 内存监控\n");
    printf("报警阈值：%d KB\n", MEMORY_WARN_THRESHOLD_KB);
    printf("蜂鸣器引脚：GPIO1_D3 (GPIO%d)\n", BEEP_GPIO_NUM);

    while (1) {
        long used = get_memory_used_kb();

        if (used < 0) {
            sleep(1);
            continue;
        }

        printf("内存已用：%ld kB\n", used);

        // 超过阈值 → 蜂鸣器滴滴滴
        if (used > MEMORY_WARN_THRESHOLD_KB) {
            beep_beep();
        } else {
            gpio_write(BEEP_GPIO_NUM, 0);
            sleep(1);
        }
    }

    return 0;
}
