#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

#define I2C_DEVICE        "/dev/i2c-3"
#define TCA9548A_ADDR     0x70
#define MT6701_ADDR       0x06

#define TCA_CHANNEL_1     0x01
#define TCA_CHANNEL_2     0x02
#define TCA_CHANNEL_3     0x04

#define MAX_PATH_LEN      512
#define MAX_NAME_LEN      128

/* ==================== 应用模式配置 ==================== */
/*
 * APP_MODE_DMS：应用模式总开关。
 *  1 = DMS 疲劳驾驶采集模式：
 *      保存到 /mnt/sdcard/dms，按 session 存储，不启用编码器。
 *  0 = 原始 Hand Capture Right 数采仪模式：
 *      保存到 /mnt/sdcard/right，启用编码器，按秒分组存储。
 */
#define APP_MODE_DMS          1

/*
 * DMS_AUTO_START：DMS 模式下是否启动后自动开始识别。
 *  1 = 无需按键，自动创建 session、启动摄像头、开始 RKNN 识别、开启 HTTP 推流
 *  0 = 等待 GPIO0 按键触发（原始逻辑）
 */
#define DMS_AUTO_START        1

/*
 * DMS_INFER_INTERVAL_FRAMES：RKNN 推理间隔帧数。
 * 摄像头 15FPS，每 3 帧推理一次 ≈ 5FPS，降低 CPU/NPU 负载。
 */
#define DMS_INFER_INTERVAL_FRAMES  3

/*
 * DMS_SAVE_ALL_FRAMES：DMS 模式下是否每帧都写入 SD 卡。
 *  0 = 仅保存报警帧（FATIGUE 等），减少 SD 卡 IO
 *  1 = 每帧都保存（原始行为，调试用）
 */
#define DMS_SAVE_ALL_FRAMES        0

/*
 * DMS_ENABLE_HEADPOSE：是否启用 headpose RKNN 推理。
 *  1 = 启用（需要 headpose.rknn 能正常在 RV1106 上运行）
 *  0 = 禁用（Phase 3A 默认，只验证 BlazeFace 人脸检测）
 */
#define DMS_ENABLE_HEADPOSE        0

/*
 * DMS_ENABLE_LANDMARK_106：是否启用 106 点人脸关键点 RKNN 推理。
 *  1 = 启用（需要 2d106det.rknn，目前为 InsightFace 预训练模型，仅限非商业研究）
 *  0 = 禁用（默认，等 P0 验收通过后再打开）
 */
#define DMS_ENABLE_LANDMARK_106    0

/* 摄像头分辨率回退参考值，两种模式都需要定义 */
#define ORIGINAL_CAMERA_WIDTH   1920
#define ORIGINAL_CAMERA_HEIGHT  1536
#define ORIGINAL_CAMERA_FPS     30

/* DMS 路径常量始终定义，保证 DMS 源文件在非 DMS 模式下也能编译 */
#define DMS_BASE_PATH         "/mnt/sdcard/dms"

#if APP_MODE_DMS
#define DMS_MODE              1
#define ENABLE_ENCODER        0

#define DMS_CAMERA_WIDTH      1280
#define DMS_CAMERA_HEIGHT     720
#define DMS_CAMERA_FPS        15

#define SDCARD_BASE_PATH      DMS_BASE_PATH
#define CAMERA_WIDTH          DMS_CAMERA_WIDTH
#define CAMERA_HEIGHT         DMS_CAMERA_HEIGHT
#define CAMERA_FPS            DMS_CAMERA_FPS
#else
#define DMS_MODE              0
#define ENABLE_ENCODER        1

#define SDCARD_BASE_PATH      "/mnt/sdcard/right"
#define CAMERA_WIDTH          ORIGINAL_CAMERA_WIDTH
#define CAMERA_HEIGHT         ORIGINAL_CAMERA_HEIGHT
#define CAMERA_FPS            ORIGINAL_CAMERA_FPS
#endif

#define CAMERA_DEVICE     "/dev/video0"

#define ENC_COUNT         3

/* ==================== 编码器配置 ==================== */
/* ENABLE_ENCODER 已由 APP_MODE_DMS 统一控制，此处无需修改 */

#define ENCODER_FPS             90
#define ENCODER_INTERVAL_US     (1000000 / ENCODER_FPS)
#define ENC_RING_BUFFER_SIZE    (ENCODER_FPS * 3)

/*
 * 单编码器测试时只打开实际接线的那一路：
 * 0x01 = 编码器1
 * 0x02 = 编码器2
 * 0x04 = 编码器3
 *
 * 三个都接上后改成：
 * #define ENC_ACTIVE_MASK 0x07
 */
#define ENC_ACTIVE_MASK         0x07
#define ENC_INVALID_ANGLE       (-1.0f)

#define RECORD_BTN_GPIO   0
#define UPLOAD_BTN_GPIO   1

/* ==================== 蜂鸣器配置 ==================== */
#define BEEP_GPIO_NUM            59
#define BEEP_ON_TIME_MS          100
#define BEEP_OFF_TIME_MS         100
#define BEEP_MAX_WARN_COUNT      10
#define STORAGE_FREE_WARN_THRESHOLD_MB     4096
#define STORAGE_CHECK_INTERVAL_SEC         3

#define LED_COUNT         2
#define LED_IDX_SYSTEM    0
#define LED_IDX_STATUS    1

#define CAPTURE_INTERVAL_US   (1000000 / CAMERA_FPS)

/* 建议至少缓存 3 秒数据，SD 卡偶发卡顿时不容易丢帧 */
#define RING_BUFFER_SIZE      90

/* 你的实际路径如果是 /mnt/sd/right，就改这里 */
#ifdef DMS_MODE
#define UPLOAD_SERVER_IP      "192.168.4.17"
#else
#define UPLOAD_SERVER_IP      "192.168.4.17"
#endif
#define UPLOAD_SERVER_PORT    9000

/* ==================== RKMPI 摄像头配置 ==================== */
#define RK_VI_DEV_ID                 0
#define RK_VI_CHN_ID                 0
#define RK_VENC_CHN_ID               0
#define RK_GETSTREAM_TIMEOUT_MS      1000

#define CAMERA_IQ_DIR                "/etc/iqfiles"
#define CAMERA_AIQ_WARMUP_MS         1000
#define CAMERA_WARMUP_FRAMES         10

/* ==================== 摄像头故障阈值 ==================== */
#define GRAB_FAIL_RESTART_THRESHOLD  10
#define GRAB_FAIL_FATAL_THRESHOLD    30
#define STAT_PRINT_INTERVAL_MS       1000
#define CAMERA_RESTART_MAX_PER_SESSION 3

/* ==================== 网络上传配置 ==================== */
#define RECV_BUF_SIZE                1024
#define SEND_BUF_SIZE                4096

/* ==================== LED SPI 配置 ==================== */
#define SPI_DEVICE                   "/dev/spidev0.0"
#define SPI_SPEED_HZ                 8000000

/* ==================== SD 卡健康检查 ==================== */
#define MIN_FREE_MB                  100

/* ==================== 电源 ADC 低电检测 ==================== */
/*
 * POWER_ADC 当前确认路径：
 * /sys/bus/iio/devices/iio:device0/in_voltage1_raw
 *
 * 当前正常 raw 约 805。
 * raw < 760 连续 3 次：低电
 * raw > 800：恢复
 */
#define POWER_ADC_RAW_PATH              "/sys/bus/iio/devices/iio:device0/in_voltage1_raw"
#define POWER_LOW_RAW_THRESHOLD         760
#define POWER_RECOVER_RAW_THRESHOLD     800
#define POWER_LOW_CONFIRM_COUNT         3
#define POWER_CHECK_INTERVAL_US         1000000ULL

#define SAFE_FREE(p)                  \
    do {                              \
        if ((p) != NULL) {            \
            free(p);                  \
            (p) = NULL;               \
        }                             \
    } while (0)

// ==================== LED 颜色 ====================
typedef enum {
    LED_COLOR_OFF = 0,
    LED_COLOR_RED,
    LED_COLOR_GREEN,
    LED_COLOR_BLUE,
    LED_COLOR_YELLOW
} led_color_e;

// ==================== 系统状态 ====================
typedef enum {
    STATE_BOOT = 0,        // 上电初始化
    STATE_READY,           // 待机，可开始采集
    STATE_RECORDING,       // 采集中
    STATE_UPLOADING,       // 上传中
    STATE_UPLOAD_FAIL,     // 上传失败
    STATE_ERROR            // 硬件/系统错误
} system_state_e;

// ==================== 帧数据 ====================
typedef struct {
    uint64_t timestamp_us;
    int enc_frame_id;
    float enc_angle[ENC_COUNT];
    uint8_t enc_valid[ENC_COUNT];   // 1=该编码器有效，0=读取失败
} encoder_sample_t;

typedef struct {
    uint64_t timestamp_us;
    uint64_t enc_timestamp_us;
    float enc_angle[ENC_COUNT];
    uint8_t *img_data;
    int img_size;
    int frame_id;
} frame_data_t;

// ==================== 全局运行标志 ====================
extern volatile int g_run_flag;
extern volatile system_state_e g_system_state;

// ==================== 安全释放指针 ====================
static inline void safe_free_ptr(void **pp)
{
    if (pp && *pp) {
        free(*pp);
        *pp = NULL;
    }
}

#ifdef __cplusplus
}
#endif

#endif
