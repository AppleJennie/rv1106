#include "common.h"
#include "hal_encoder.h"
#include "sys_logger.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>

/*
 * hal_encoder.c
 *
 * 修改目标：
 * 1. 支持 90Hz 编码器采样。
 * 2. TCA9548A 切通道延时从 200us 降低到 20us。
 * 3. MT6701 角度读取改为连续读取 0x03/0x04 两个寄存器。
 * 4. 失败日志限频，避免 90Hz 下刷屏。
 */

#define TCA_SWITCH_DELAY_US       20
#define ENCODER_READ_RETRY        2
#define ENCODER_RETRY_DELAY_US    300
#define ENCODER_WARN_INTERVAL_MS  1000

#define MT6701_REG_ANGLE_H        0x03
#define MT6701_RAW_MAX            16384.0f

/* 通道映射 */
static const int tca_ch_map[ENC_COUNT] = {
    TCA_CHANNEL_1,
    TCA_CHANNEL_2,
    TCA_CHANNEL_3
};

/* 编码器名称 */
static const char* enc_name[ENC_COUNT] = {
    "编码器1",
    "编码器2",
    "编码器3"
};

static int i2c_fd = -1;
static uint64_t g_last_warn_ms[ENC_COUNT] = {0};

static bool encoder_channel_active(uint8_t ch)
{
    if (ch >= ENC_COUNT) {
        return false;
    }

    return (ENC_ACTIVE_MASK & (1U << ch)) != 0;
}

/* ==================== 工具函数 ==================== */

static uint64_t get_time_ms_local(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

static void encoder_warn_limited(uint8_t ch, const char *msg)
{
    if (ch >= ENC_COUNT) {
        return;
    }

    uint64_t now = get_time_ms_local();

    if (now - g_last_warn_ms[ch] >= ENCODER_WARN_INTERVAL_MS) {
        g_last_warn_ms[ch] = now;
        log_warn("%s %s", enc_name[ch], msg);
    }
}

static int i2c_write_full(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t left = len;

    while (left > 0) {
        ssize_t n = write(i2c_fd, p, left);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (n == 0) {
            return -1;
        }

        p += n;
        left -= (size_t)n;
    }

    return 0;
}

/*
 * 使用 I2C_RDWR 完成：
 * write 1 byte register address + read N bytes
 *
 * 好处：
 * 1. 比 write()+read() 更标准。
 * 2. 可以形成 repeated start。
 * 3. 适合读取连续寄存器。
 */
static int i2c_read_regs(uint8_t dev_addr, uint8_t reg, uint8_t *buf, size_t len)
{
    if (i2c_fd < 0 || !buf || len == 0) {
        return -1;
    }

    struct i2c_msg msgs[2];
    struct i2c_rdwr_ioctl_data data;

    memset(msgs, 0, sizeof(msgs));
    memset(&data, 0, sizeof(data));

    msgs[0].addr  = dev_addr;
    msgs[0].flags = 0;
    msgs[0].len   = 1;
    msgs[0].buf   = &reg;

    msgs[1].addr  = dev_addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len   = (unsigned short)len;
    msgs[1].buf   = buf;

    data.msgs  = msgs;
    data.nmsgs = 2;

    if (ioctl(i2c_fd, I2C_RDWR, &data) < 0) {
        return -1;
    }

    return 0;
}

static bool wait_device_ready(const char *dev, int timeout_ms)
{
    int waited = 0;
    struct stat st;

    while (waited < timeout_ms) {
        if (stat(dev, &st) == 0) {
            return true;
        }

        usleep(100000);
        waited += 100;
    }

    return false;
}

/* ==================== I2C 总线初始化 ==================== */

static int i2c_init_bus(void)
{
    if (i2c_fd >= 0) {
        close(i2c_fd);
        i2c_fd = -1;
    }

    i2c_fd = open(I2C_DEVICE, O_RDWR);
    if (i2c_fd < 0) {
        log_error("I2C 总线打开失败: %s, errno=%d", I2C_DEVICE, errno);
        return -1;
    }

    if (ioctl(i2c_fd, I2C_TENBIT, 0) < 0) {
        log_warn("I2C_TENBIT 设置失败，继续运行: errno=%d", errno);
    }

    return 0;
}

/* ==================== 选择 TCA9548A 通道 ==================== */

static int tca_select_channel(int ch)
{
    uint8_t value = (uint8_t)ch;

    if (i2c_fd < 0) {
        return -1;
    }

    if (ioctl(i2c_fd, I2C_SLAVE, TCA9548A_ADDR) < 0) {
        return -1;
    }

    if (i2c_write_full(&value, 1) != 0) {
        return -1;
    }

    /*
     * 原来是 usleep(200)。
     * 这里先保守改成 20us，不建议第一版完全去掉。
     * 如果实测稳定，可以后续再改成 5us 或 0us。
     */
    if (TCA_SWITCH_DELAY_US > 0) {
        usleep(TCA_SWITCH_DELAY_US);
    }

    return 0;
}

/* ==================== 读取 MT6701 角度 ==================== */

static float mt6701_read_angle(void)
{
    uint8_t buf[2] = {0};

    /*
     * 连续读取 0x03 和 0x04。
     * raw = 高 8 位左移 6 + 低字节低 6 位。
     */
    if (i2c_read_regs(MT6701_ADDR, MT6701_REG_ANGLE_H, buf, 2) != 0) {
        return -1.0f;
    }

    uint16_t raw = ((uint16_t)buf[0] << 6) | (buf[1] & 0x3F);

    if (raw >= 16384) {
        return -1.0f;
    }

    return (float)raw * 360.0f / MT6701_RAW_MAX;
}

/* ==================== 对外接口：编码器初始化 ==================== */

bool encoder_init(void)
{
    log_info("初始化 I2C & 编码器...");

    if (!wait_device_ready(I2C_DEVICE, 10000)) {
        log_error("I2C 设备 10 秒内未就绪: %s", I2C_DEVICE);
        return false;
    }

    if (i2c_init_bus() != 0) {
        return false;
    }

    log_info("编码器预热中...");

    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < ENC_COUNT; j++) {
            if (!encoder_channel_active((uint8_t)j)) {
                continue;
            }

            if (tca_select_channel(tca_ch_map[j]) == 0) {
                (void)mt6701_read_angle();
            }
            usleep(500);
        }
    }

    for (int i = 0; i < ENC_COUNT; i++) {
        if (!encoder_channel_active((uint8_t)i)) {
            log_info("%s：未启用，跳过检测", enc_name[i]);
            continue;
        }

        if (tca_select_channel(tca_ch_map[i]) != 0) {
            log_warn("%s：TCA 通道选择失败", enc_name[i]);
            continue;
        }

        float ang = mt6701_read_angle();

        if (ang >= 0.0f && ang < 360.0f) {
            log_info("%s：正常 %.1f°", enc_name[i], ang);
        } else {
            log_warn("%s：读取失败", enc_name[i]);
        }
    }

    log_info("编码器初始化完成");
    return true;
}

/* ==================== 对外接口：读取单个编码器 ==================== */

float encoder_read(uint8_t ch)
{
    if (ch >= ENC_COUNT) {
        return ENC_INVALID_ANGLE;
    }

    if (!encoder_channel_active(ch)) {
        return ENC_INVALID_ANGLE;
    }

    for (int retry = 0; retry < ENCODER_READ_RETRY; retry++) {
        if (tca_select_channel(tca_ch_map[ch]) == 0) {
            float angle = mt6701_read_angle();

            if (angle >= 0.0f && angle < 360.0f) {
                return angle;
            }
        }

        if (retry + 1 < ENCODER_READ_RETRY) {
            usleep(ENCODER_RETRY_DELAY_US);
        }
    }

    encoder_warn_limited(ch, "读取失败");
    return ENC_INVALID_ANGLE;
}

/* ==================== 对外接口：读取所有编码器 ==================== */

bool encoder_read_all(float *angle_buf)
{
    if (!angle_buf) {
        return false;
    }

    bool any_ok = false;

    for (int i = 0; i < ENC_COUNT; i++) {
        angle_buf[i] = ENC_INVALID_ANGLE;

        /*
         * 如果以后还想屏蔽某一路，可以用 ENC_ACTIVE_MASK。
         * 现在正式采三个编码器时，ENC_ACTIVE_MASK = 0x07。
         */
        if ((ENC_ACTIVE_MASK & (1U << i)) == 0) {
            continue;
        }

        float angle = encoder_read((uint8_t)i);

        if (angle >= 0.0f && angle < 360.0f) {
            angle_buf[i] = angle;
            any_ok = true;
        } else {
            angle_buf[i] = ENC_INVALID_ANGLE;
        }
    }

    /*
     * 返回值表示：这一轮有没有至少一个编码器成功。
     * 但注意：即使返回 false，外层也应该照样存这一行。
     */
    return any_ok;
}

/* ==================== 对外接口：反初始化 ==================== */

void encoder_deinit(void)
{
    if (i2c_fd >= 0) {
        close(i2c_fd);
        i2c_fd = -1;
    }

    log_info("编码器已关闭");
}
