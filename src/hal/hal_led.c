#include "hal_led.h"
#include "sys_logger.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define BIT_0         0xC0
#define BIT_1         0xFC
#define BYTES_PER_LED 24

typedef struct {
    led_mode_e mode;
    led_color_e color;
    int blink_period_ms;
    bool blink_on;
    uint64_t last_toggle_ms;
} led_ctrl_t;

static int spi_fd = -1;
static bool g_led_available = false;
static led_ctrl_t g_leds[LED_COUNT];
static pthread_mutex_t g_led_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t get_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

static void byte_to_spi(uint8_t data, uint8_t *out_buf)
{
    for (int i = 0; i < 8; i++) {
        out_buf[i] = (data & 0x80) ? BIT_1 : BIT_0;
        data <<= 1;
    }
}

static void color_to_grb(led_color_e color, uint8_t *g, uint8_t *r, uint8_t *b)
{
    *g = 0; *r = 0; *b = 0;
    switch (color) {
        case LED_COLOR_RED:    *r = 255; break;
        case LED_COLOR_GREEN:  *g = 255; break;
        case LED_COLOR_BLUE:   *b = 255; break;
        case LED_COLOR_YELLOW: *g = 255; *r = 255; break;
        default: break;
    }
}

static led_color_e get_effective_color(int i)
{
    if (g_leds[i].mode == LED_MODE_OFF) {
        return LED_COLOR_OFF;
    }
    if (g_leds[i].mode == LED_MODE_ON) {
        return g_leds[i].color;
    }
    if (g_leds[i].mode == LED_MODE_BLINK) {
        return g_leds[i].blink_on ? g_leds[i].color : LED_COLOR_OFF;
    }
    return LED_COLOR_OFF;
}

static void ws2812_send_current(void)
{
    if (!g_led_available || spi_fd < 0) return;

    uint8_t spi_buf[LED_COUNT * BYTES_PER_LED];
    memset(spi_buf, 0, sizeof(spi_buf));

    for (int i = 0; i < LED_COUNT; i++) {
        uint8_t g, r, b;
        color_to_grb(get_effective_color(i), &g, &r, &b);

        uint8_t *p = &spi_buf[i * BYTES_PER_LED];
        byte_to_spi(g, p + 0);
        byte_to_spi(r, p + 8);
        byte_to_spi(b, p + 16);
    }

    struct spi_ioc_transfer xfer = {
        .tx_buf = (unsigned long)spi_buf,
        .len = sizeof(spi_buf),
        .speed_hz = SPI_SPEED_HZ,
        .bits_per_word = 8,
    };

    ioctl(spi_fd, SPI_IOC_MESSAGE(1), &xfer);
    usleep(100);
}

bool led_is_available(void)
{
    return g_led_available;
}

bool led_init(void)
{
    spi_fd = open(SPI_DEVICE, O_RDWR);
    if (spi_fd < 0) {
        g_led_available = false;
        log_warn("打开 SPI 失败: %s，LED 不可用", SPI_DEVICE);
        /* LED 不是关键模块，返回 true 让上层继续初始化。 */
        return true;
    }

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = SPI_SPEED_HZ;

    ioctl(spi_fd, SPI_IOC_WR_MODE, &mode);
    ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

    memset(g_leds, 0, sizeof(g_leds));
    ws2812_send_current();

    g_led_available = true;
    log_info("LED 初始化完成");
    return true;
}

void led_set_mode(int led_idx, led_mode_e mode, led_color_e color, int blink_period_ms)
{
    if (led_idx < 0 || led_idx >= LED_COUNT) return;
    if (!g_led_available) return;

    pthread_mutex_lock(&g_led_mutex);

    g_leds[led_idx].mode = mode;
    g_leds[led_idx].color = color;
    g_leds[led_idx].blink_period_ms = blink_period_ms;
    g_leds[led_idx].blink_on = true;
    g_leds[led_idx].last_toggle_ms = get_ms();

    ws2812_send_current();

    pthread_mutex_unlock(&g_led_mutex);
}

void led_tick(void)
{
    if (!g_led_available) return;

    pthread_mutex_lock(&g_led_mutex);

    uint64_t now = get_ms();
    bool changed = false;

    for (int i = 0; i < LED_COUNT; i++) {
        if (g_leds[i].mode == LED_MODE_BLINK && g_leds[i].blink_period_ms > 0) {
            int half_period = g_leds[i].blink_period_ms / 2;
            if ((int)(now - g_leds[i].last_toggle_ms) >= half_period) {
                g_leds[i].blink_on = !g_leds[i].blink_on;
                g_leds[i].last_toggle_ms = now;
                changed = true;
            }
        }
    }

    if (changed) {
        ws2812_send_current();
    }

    pthread_mutex_unlock(&g_led_mutex);
}

void led_deinit(void)
{
    if (!g_led_available) return;

    pthread_mutex_lock(&g_led_mutex);

    for (int i = 0; i < LED_COUNT; i++) {
        g_leds[i].mode = LED_MODE_OFF;
        g_leds[i].color = LED_COLOR_OFF;
    }
    ws2812_send_current();

    pthread_mutex_unlock(&g_led_mutex);

    if (spi_fd >= 0) {
        close(spi_fd);
        spi_fd = -1;
    }

    g_led_available = false;
    log_info("LED 已关闭");
}