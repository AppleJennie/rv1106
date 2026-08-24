#include "state_machine.h"
#include "common.h"
#include "sys_logger.h"
#include "sys_storage.h"
#if APP_MODE_DMS
#include "sys_dms_storage.h"
#include "dms_infer.h"
#include "dms_stream_server.h"
#include "dms_ai_thread.h"
#endif
#include "hal_gpio.h"
#include "hal_led.h"
#include "hal_camera.h"
#include "hal_encoder.h"
#include "hal_beep.h"
#include "net_transfer.h"
#include "hal_power.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>


volatile int g_run_flag = 1;
volatile system_state_e g_system_state = STATE_BOOT;

static pthread_t g_capture_thread;
static volatile bool g_capture_running = false;
static volatile int g_capture_error = 0;
static bool g_capture_thread_created = false;
static int g_frame_id = 0;

static pthread_t g_upload_thread;
static volatile bool g_upload_running = false;
static volatile int g_upload_cancel = 0;
static volatile int g_upload_done = 0;
static volatile int g_upload_result = 0;
static bool g_upload_thread_created = false;

static pthread_t g_encoder_thread;
static volatile bool g_encoder_running = false;
static bool g_encoder_thread_created = false;
static volatile int g_encoder_frame_id = 0;

static pthread_t g_init_led_thread;
static volatile bool g_init_led_running = false;
static bool g_init_led_thread_created = false;

static pthread_mutex_t g_encoder_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_latest_enc_angle[ENC_COUNT] = {0};
static uint64_t g_latest_enc_ts = 0;
static bool g_latest_enc_valid = false;

static bool g_power_low_led_active = false;

// ---------- 工具函数声明 ----------
static void apply_led_by_state(system_state_e st);
static void apply_power_low_led(void);
static void apply_led_effective(void);
static void power_monitor_tick(void);
static const char* state_to_string(system_state_e st);
static void* init_led_thread_func(void *arg);
static void init_led_blink_start(void);
static void init_led_blink_stop(void);
static void* encoder_thread_func(void *arg);
static void apply_power_low_led(void)
{
    /* 低电量双红灯闪烁逻辑已关闭 */
#if 0
    /*
     * 低电状态最高优先级：
     * 两个灯全部红色闪烁。
     */
    led_set_mode(LED_IDX_SYSTEM, LED_MODE_BLINK, LED_COLOR_RED, 300);
    led_set_mode(LED_IDX_STATUS, LED_MODE_BLINK, LED_COLOR_RED, 300);

    log_warn("LED状态: 低电报警，两个灯=红色闪烁");
#endif
}

static void apply_led_effective(void)
{
    /* 低电量LED报警已关闭，直接按状态显示 */
    // if (g_power_low_led_active) {
    //     apply_power_low_led();
    // } else {
        apply_led_by_state(g_system_state);
    // }
}

static void get_latest_encoder(float *angle_buf, uint64_t *ts, bool *valid);
static void* capture_thread_func(void *arg);
static bool do_upload_mock(void);
static void* upload_thread_func(void *arg);
static bool log_rate_limit_us(uint64_t *last_ts, uint64_t interval_us);

// ---------- 状态字符串 ----------
static const char* state_to_string(system_state_e st)
{
    switch (st) {
        case STATE_BOOT:        return "BOOT";
        case STATE_READY:       return "READY";
        case STATE_RECORDING:   return "RECORDING";
        case STATE_UPLOADING:   return "UPLOADING";
        case STATE_UPLOAD_FAIL: return "UPLOAD_FAIL";
        case STATE_ERROR:       return "ERROR";
        default:                return "UNKNOWN";
    }
}

static uint64_t get_mono_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static uint64_t get_wall_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

static void power_monitor_tick(void)
{
    static uint64_t last_check_us = 0;

    uint64_t now = get_mono_time_us();

    if (last_check_us != 0 &&
        now - last_check_us < POWER_CHECK_INTERVAL_US) {
        return;
    }

    last_check_us = now;

    /* 低电量LED报警已关闭，不再检测状态变化并切换LED */
#if 0
    bool low = power_poll();

    if (low != g_power_low_led_active) {
        g_power_low_led_active = low;

        if (low) {
            log_error("电源低电报警触发: adc_raw=%d",
                      power_get_last_raw());
        } else {
            log_info("电源低电报警解除: adc_raw=%d",
                     power_get_last_raw());
        }

        /*
         * 低电状态变化后，立刻重新应用 LED。
         */
        apply_led_effective();
    }
#endif

}

static bool log_rate_limit_us(uint64_t *last_ts, uint64_t interval_us)
{
    uint64_t now = get_mono_time_us();

    if (*last_ts == 0 || now - *last_ts >= interval_us) {
        *last_ts = now;
        return true;
    }

    return false;
}

static void apply_led_by_state(system_state_e st)
{
    switch (st) {
        case STATE_BOOT:
            led_set_mode(LED_IDX_SYSTEM, LED_MODE_BLINK, LED_COLOR_YELLOW, 500);
            led_set_mode(LED_IDX_STATUS, LED_MODE_OFF, LED_COLOR_OFF, 0);
            log_info("LED状态: 系统灯=黄色闪烁, 状态灯=熄灭");
            break;

        case STATE_READY:
            led_set_mode(LED_IDX_SYSTEM, LED_MODE_ON, LED_COLOR_GREEN, 0);
            led_set_mode(LED_IDX_STATUS, LED_MODE_OFF, LED_COLOR_OFF, 0);
            log_info("LED状态: 系统灯=绿色常亮, 状态灯=熄灭");
            break;

        case STATE_RECORDING:
            led_set_mode(LED_IDX_SYSTEM, LED_MODE_BLINK, LED_COLOR_GREEN, 500);
            led_set_mode(LED_IDX_STATUS, LED_MODE_OFF, LED_COLOR_OFF, 0);
            log_info("LED状态: 系统灯=绿色闪烁, 状态灯=熄灭");
            break;

        case STATE_UPLOADING:
            led_set_mode(LED_IDX_SYSTEM, LED_MODE_ON, LED_COLOR_GREEN, 0);
            led_set_mode(LED_IDX_STATUS, LED_MODE_BLINK, LED_COLOR_BLUE, 300);
            log_info("LED状态: 系统灯=绿色常亮, 状态灯=蓝色闪烁");
            break;

        case STATE_UPLOAD_FAIL:
            led_set_mode(LED_IDX_SYSTEM, LED_MODE_ON, LED_COLOR_GREEN, 0);
            led_set_mode(LED_IDX_STATUS, LED_MODE_ON, LED_COLOR_RED, 0);
            log_info("LED状态: 系统灯=绿色常亮, 状态灯=红色常亮");
            break;

        case STATE_ERROR:
        default:
            led_set_mode(LED_IDX_SYSTEM, LED_MODE_ON, LED_COLOR_RED, 0);
            led_set_mode(LED_IDX_STATUS, LED_MODE_ON, LED_COLOR_RED, 0);
            log_info("LED状态: 系统灯=红色常亮, 状态灯=红色常亮");
            break;
    }
}

bool state_machine_set_state(system_state_e new_state)
{
    system_state_e old_state = g_system_state;

    g_system_state = new_state;

    log_info("状态切换: %s -> %s",
             state_to_string(old_state),
             state_to_string(new_state));

    /* 低电量LED报警已关闭，直接按状态显示 */
    apply_led_effective();

    return true;
}

system_state_e state_machine_get_state(void)
{
    return g_system_state;
}

static void get_latest_encoder(float *angle_buf, uint64_t *ts, bool *valid)
{
    pthread_mutex_lock(&g_encoder_lock);

    if (angle_buf) {
        for (int i = 0; i < ENC_COUNT; i++) {
            angle_buf[i] = g_latest_enc_angle[i];
        }
    }

    if (ts) {
        *ts = g_latest_enc_ts;
    }

    if (valid) {
        *valid = g_latest_enc_valid;
    }

    pthread_mutex_unlock(&g_encoder_lock);
}

static void* init_led_thread_func(void *arg)
{
    (void)arg;

    while (g_init_led_running && g_run_flag) {
        led_tick();
        usleep(50000);
    }

    return NULL;
}

static void init_led_blink_start(void)
{
    if (g_init_led_thread_created) {
        return;
    }

    g_init_led_running = true;

    if (pthread_create(&g_init_led_thread, NULL, init_led_thread_func, NULL) == 0) {
        g_init_led_thread_created = true;
    } else {
        g_init_led_running = false;
        log_warn("初始化 LED tick 线程创建失败，启动阶段闪烁可能不可见");
    }
}

static void init_led_blink_stop(void)
{
    g_init_led_running = false;

    if (g_init_led_thread_created) {
        pthread_join(g_init_led_thread, NULL);
        g_init_led_thread_created = false;
    }
}

static void* encoder_thread_func(void *arg)
{
    (void)arg;

    int enc_fail = 0;
    int enc_ok = 0;

    uint64_t next_tick = get_mono_time_us();
    uint64_t stat_ts = get_mono_time_us();
    int stat_count = 0;

    log_info("编码器90Hz采集线程启动");
    log_info("ENCODER CONFIG: fps=%d interval=%d us",
             ENCODER_FPS, ENCODER_INTERVAL_US);

    while (g_encoder_running && g_run_flag) {
        encoder_sample_t sample;
        memset(&sample, 0, sizeof(sample));

        sample.timestamp_us = get_wall_time_us();
        sample.enc_frame_id = ++g_encoder_frame_id;

        /* 默认无效 */
        for (int i = 0; i < ENC_COUNT; i++) {
            sample.enc_angle[i] = ENC_INVALID_ANGLE;
            sample.enc_valid[i] = 0;
        }

        bool ok = encoder_read_all(sample.enc_angle);

        /* 根据角度判断每一路是否有效 */
        for (int i = 0; i < ENC_COUNT; i++) {
            if (sample.enc_angle[i] >= 0.0f && sample.enc_angle[i] < 360.0f) {
                sample.enc_valid[i] = 1;
            }
        }

        if (ok) {
            pthread_mutex_lock(&g_encoder_lock);
            for (int i = 0; i < ENC_COUNT; i++) {
                g_latest_enc_angle[i] = sample.enc_angle[i];
            }
            g_latest_enc_ts = sample.timestamp_us;
            g_latest_enc_valid = true;
            pthread_mutex_unlock(&g_encoder_lock);

            enc_ok++;
        } else {
            enc_fail++;

            static uint64_t last_enc_fail_log_us = 0;
            if (log_rate_limit_us(&last_enc_fail_log_us, 1000000ULL)) {
                log_warn("本轮三个编码器均读取失败，最近累计 enc_fail=%d", enc_fail);
            }
        }

        /*
         * 关键：
         * 不管读取是否完全成功，每个 tick 都存。
         * 这样一秒目标就是 90 行左右。
         */
        storage_push_encoder_sample(&sample);

        stat_count++;

        uint64_t now = get_mono_time_us();
        if (now - stat_ts >= 1000000ULL) {
            log_info("[ENC] fps=%d, enc_frame_id=%d, ok=%d, fail=%d",
                     stat_count, sample.enc_frame_id, enc_ok, enc_fail);

            stat_count = 0;
            enc_ok = 0;
            enc_fail = 0;
            stat_ts = now;
        }

        next_tick += ENCODER_INTERVAL_US;

        now = get_mono_time_us();
        if (next_tick > now) {
            usleep((useconds_t)(next_tick - now));
        } else {
            uint64_t lag = now - next_tick;

            static uint64_t last_enc_lag_log_us = 0;
            if (lag > 5000ULL && log_rate_limit_us(&last_enc_lag_log_us, 1000000ULL)) {
                log_warn("编码器采集超时: lag=%llu us",
                         (unsigned long long)lag);
            }

            next_tick = now + ENCODER_INTERVAL_US;
        }
    }

    log_info("编码器90Hz采集线程退出，共采集 %d 帧", g_encoder_frame_id);
    return NULL;
}

static void* capture_thread_func(void *arg)
{
    (void)arg;

    int camera_fail = 0;
    int pushed_fail = 0;
    int restart_total = 0;

    uint64_t next_tick = get_mono_time_us();
    uint64_t stat_ts = get_mono_time_us();
    int stat_frames = 0;

    log_info("采集线程启动");
    log_info("CAPTURE CONFIG: width=%d height=%d fps=%d interval=%d us",
         CAMERA_WIDTH, CAMERA_HEIGHT, CAMERA_FPS, CAPTURE_INTERVAL_US);

    while (g_capture_running && g_run_flag) {
        frame_data_t frame;
        memset(&frame, 0, sizeof(frame));

        frame.timestamp_us = get_wall_time_us();
        frame.frame_id = ++g_frame_id;

        frame.enc_timestamp_us = 0;
        for (int i = 0; i < ENC_COUNT; i++) {
            frame.enc_angle[i] = ENC_INVALID_ANGLE;
        }

        /* 抓取 JPEG */
        if (!camera_grab_jpeg(&frame.img_data, &frame.img_size)) {
            camera_fail++;

            if (camera_fail >= 10) {
                restart_total++;

                if (restart_total > CAMERA_RESTART_MAX_PER_SESSION) {
                    log_error("摄像头重启超过 %d 次仍失败，通知主状态机进入 ERROR",
                              CAMERA_RESTART_MAX_PER_SESSION);
                    g_capture_error = 1;
                    g_capture_running = false;
                    break;
                }

                log_error("摄像头连续失败 %d 次，尝试重启 RKMPI (restart_total=%d/%d)",
                          camera_fail, restart_total, CAMERA_RESTART_MAX_PER_SESSION);

                if (!camera_restart()) {
                    log_error("摄像头重启失败，通知主状态机进入 ERROR");
                    g_capture_error = 1;
                    g_capture_running = false;
                    break;
                }

                camera_fail = 0;
            }

            usleep(5000);
            continue;
        }

        camera_fail = 0;

#if APP_MODE_DMS
        /*
         * Phase 3A：视频采集与 AI 推理完全解耦。
         * capture thread 只负责：抓图 → 推 MJPEG → 把最新帧 submit 给 AI thread。
         * 绝不等待 RKNN。
         */
        dms_result_t dms_result;
        memset(&dms_result, 0, sizeof(dms_result));
        strncpy(dms_result.status, "INIT", sizeof(dms_result.status) - 1);
        dms_ai_thread_get_latest_result(&dms_result);

        /* 每帧都更新 MJPEG 推流 */
        dms_stream_server_update_frame(frame.img_data, (size_t)frame.img_size, &dms_result);
        dms_ai_thread_inc_stream_frames();

        /* 把最新帧交给 AI thread 后台处理 */
        dms_ai_thread_submit_frame(frame.img_data, (size_t)frame.img_size,
                                    frame.frame_id, frame.timestamp_us);

        /* 根据最近一次 AI 结果更新 LED（Phase 3A 只显示是否有人脸） */
        if (dms_result.face_found) {
            led_set_mode(LED_IDX_STATUS, LED_MODE_ON, LED_COLOR_GREEN, 0);
        } else {
            led_set_mode(LED_IDX_STATUS, LED_MODE_OFF, LED_COLOR_OFF, 0);
        }

        /* 每秒打印一次 DMS 状态 */
        static uint64_t last_dms_status_log_us = 0;
        if (log_rate_limit_us(&last_dms_status_log_us, 1000000ULL)) {
            log_info("DMS status=%s face=%d score=%.2f",
                     dms_result.status,
                     dms_result.face_found,
                     dms_result.face_score);
        }

        /* Phase 3A 不保存任何普通帧，也不触发报警 */
        (void)pushed_fail;
#else
        /* 推入存储队列 */
        if (!storage_push_frame(&frame)) {
            pushed_fail++;

            static uint64_t last_push_fail_log_us = 0;
            if (log_rate_limit_us(&last_push_fail_log_us, 1000000ULL)) {
                log_warn("帧推入存储队列失败，frame_id=%d, pushed_fail=%d",
                         frame.frame_id, pushed_fail);
            }
        }
#endif

        SAFE_FREE(frame.img_data);

        stat_frames++;

        /* 每秒打印采集统计 */
        uint64_t now = get_mono_time_us();
        if (now - stat_ts >= 1000000ULL) {
#if APP_MODE_DMS
            log_info("[CAP] fps=%d, frame_id=%d, cam_fail=%d, push_fail=%d",
                     stat_frames, frame.frame_id, camera_fail, pushed_fail);
            dms_ai_thread_print_stats();
#else
            log_info("[CAP] fps=%d, frame_id=%d, cam_fail=%d, push_fail=%d, dropped=%d",
                     stat_frames, frame.frame_id, camera_fail, pushed_fail,
                     storage_get_dropped_count());
#endif
            stat_frames = 0;
            stat_ts = now;
        }

        next_tick += CAPTURE_INTERVAL_US;

        now = get_mono_time_us();
        if (next_tick > now) {
            usleep((useconds_t)(next_tick - now));
        } else {
            uint64_t lag = now - next_tick;
            if (lag > 10000ULL) {
                log_warn("采集处理严重超时: lag=%llu us",
                         (unsigned long long)lag);
            }
            next_tick = now + CAPTURE_INTERVAL_US;
        }
    }

    log_info("采集线程退出，本次共采集 %d 帧", g_frame_id);
    return NULL;
}

static bool do_upload_mock(void)
{
    int pending = upload_count_pending(SDCARD_BASE_PATH);
    log_info("开始上传: %s 下共 %d 组待上传数据", SDCARD_BASE_PATH, pending);
    return upload_all_sessions(SDCARD_BASE_PATH, &g_upload_cancel);
}

static void* upload_thread_func(void *arg)
{
    (void)arg;

    g_upload_done = 0;
    g_upload_result = 0;
    g_upload_cancel = 0;
    g_upload_running = true;

    bool ok = do_upload_mock();

    if (g_upload_cancel) {
        log_warn("上传被用户取消");
        g_upload_result = 0;
    } else if (ok) {
        log_info("上传成功");
        g_upload_result = 1;
    } else {
        log_error("上传失败");
        g_upload_result = 0;
    }

    g_upload_done = 1;
    g_upload_running = false;

    return NULL;
}

bool state_machine_init(void)
{
    bool ok_gpio = false;
    bool ok_led = false;
    bool ok_camera = false;
    bool ok_encoder = false;
    bool ok_storage = false;
    bool ok_dms_infer = false;
    bool ok_upload = false;

#if APP_MODE_DMS
    log_info("APP MODE: DMS DRIVER FATIGUE CAPTURE");
#else
    log_info("APP MODE: HAND CAPTURE RIGHT");
#endif

    log_info("状态机初始化开始");

    if (!gpio_init()) {
#if APP_MODE_DMS && DMS_AUTO_START
        log_warn("GPIO 初始化失败，DMS_AUTO_START 模式下继续运行");
        ok_gpio = false;
#else
        log_error("GPIO 初始化失败");
        goto fail;
#endif
    } else {
        log_info("GPIO 初始化成功");
        ok_gpio = true;
    }

    if (!led_init()) {
        log_error("LED 初始化失败");
        goto fail;
    }

    if (led_is_available()) {
        log_info("LED available=1");
        ok_led = true;
    } else {
#if APP_MODE_DMS
        log_warn("LED unavailable, continue without LED in DMS mode");
#else
        log_warn("LED unavailable, continue without LED");
#endif
        ok_led = false;
    }

    if (!power_init()) {
        log_warn("电源 ADC 检测初始化失败，继续运行");
    }

    /* g_power_low_led_active = power_poll(); */
    g_power_low_led_active = false;

    state_machine_set_state(STATE_BOOT);
    init_led_blink_start();

    if (!camera_init()) {
        log_error("摄像头初始化失败");
        goto fail;
    }
    log_info("摄像头初始化成功");
    ok_camera = true;

#if ENABLE_ENCODER
    if (!encoder_init()) {
        log_error("编码器初始化失败");
        goto fail;
    }
    log_info("编码器初始化成功");
    ok_encoder = true;
#else
    log_info("编码器模块已关闭，DMS 模式跳过编码器初始化");
    ok_encoder = true;
#endif

#if APP_MODE_DMS
    if (!dms_storage_init()) {
        log_error("DMS 存储系统初始化失败");
        goto fail;
    }
    log_info("DMS 存储系统初始化成功");

    /* 创建 DMS 辅助目录（live 用于日志，events 用于报警事件）。 */
    mkdir(DMS_BASE_PATH "/live", 0777);
    mkdir(DMS_BASE_PATH "/events", 0777);

    if (!dms_infer_init()) {
        log_error("DMS 推理模块初始化失败");
        goto fail;
    }
    log_info("DMS 推理模块初始化成功");
    ok_dms_infer = true;
#else
    if (!storage_init()) {
        log_error("存储系统初始化失败");
        goto fail;
    }
    log_info("存储系统初始化成功");
#endif
    ok_storage = true;

    if (!upload_init()) {
        log_error("上传模块初始化失败");
        goto fail;
    }
    log_info("上传模块初始化成功");
    ok_upload = true;

    if (!beep_init()) {
        log_warn("蜂鸣器初始化失败，继续运行（非关键模块）");
    }

#if APP_MODE_DMS && DMS_AUTO_START
    if (!dms_stream_server_init(8090)) {
        log_warn("DMS 推流服务初始化失败，继续运行");
    }
#endif

    init_led_blink_stop();

    state_machine_set_state(STATE_READY);
    log_info("系统进入待机状态，等待按键触发");
    return true;

fail:
    init_led_blink_stop();
    state_machine_set_state(STATE_ERROR);

    if (ok_upload) upload_deinit();
#if APP_MODE_DMS
    if (ok_dms_infer) dms_infer_deinit();
    if (ok_storage) dms_storage_deinit();
#else
    if (ok_storage) storage_deinit();
#endif
    if (ok_encoder) encoder_deinit();
    if (ok_camera) camera_deinit();
    if (ok_led) led_deinit();
    if (ok_gpio) gpio_deinit();

    return false;
}

void state_machine_run(void)
{
    bool last_record_level = gpio_record_is_pressed();
    bool last_upload_level = gpio_upload_is_pressed();

    uint64_t record_debounce_ts = 0;
    uint64_t upload_debounce_ts = 0;

#if APP_MODE_DMS && DMS_AUTO_START
    bool dms_auto_triggered = false;
    log_info("DMS_AUTO_START enabled");
#endif

    while (g_run_flag) {
        power_monitor_tick();
        led_tick();

        bool record_high = gpio_record_is_pressed();
        bool upload_high = gpio_upload_is_pressed();
        uint64_t now = get_mono_time_us();

#if APP_MODE_DMS && DMS_AUTO_START
        /* 无按键时，首次进入 READY 自动开始识别。 */
        if (!dms_auto_triggered && g_system_state == STATE_READY) {
            dms_auto_triggered = true;
            record_high = true;
            last_record_level = false;
            log_info("DMS_AUTO_START: 自动开始采集");
        }
#endif

        // ========= 录制逻辑：点按切换 =========
        if (record_high && !last_record_level) {
            if (now - record_debounce_ts > 300000) {
                record_debounce_ts = now;

                if (g_system_state == STATE_READY || g_system_state == STATE_UPLOAD_FAIL) {
                    log_info("录制按键点按: 开始采集");
                    g_frame_id = 0;
                    g_capture_error = 0;

#if APP_MODE_DMS
                    if (!dms_storage_start_session()) {
                        log_error("DMS 存储会话启动失败");
                        state_machine_set_state(STATE_ERROR);
                    } else {
                        log_info("DMS session started");
                        g_frame_id = 0;
                        g_encoder_frame_id = 0;
                        g_capture_error = 0;
#else
                    if (!storage_start_session()) {
                        log_error("存储会话启动失败");
                        state_machine_set_state(STATE_ERROR);
                    } else {
                        g_frame_id = 0;
                        g_encoder_frame_id = 0;
                        g_capture_error = 0;
#endif

                        pthread_mutex_lock(&g_encoder_lock);
                        g_latest_enc_valid = false;
                        g_latest_enc_ts = 0;
                        for (int i = 0; i < ENC_COUNT; i++) {
                            g_latest_enc_angle[i] = 0.0f;
                        }
                        pthread_mutex_unlock(&g_encoder_lock);

                        /* ENABLE_ENCODER=0 时不启动编码器线程 */
                        bool encoder_ok = true;
#if ENABLE_ENCODER
                        g_encoder_running = true;
                        encoder_ok = (pthread_create(&g_encoder_thread, NULL, encoder_thread_func, NULL) == 0);
                        if (encoder_ok) {
                            g_encoder_thread_created = true;
                        }
#endif

                        if (!encoder_ok) {
                            log_error("创建编码器90Hz采集线程失败");
#if ENABLE_ENCODER
                            g_encoder_running = false;
#endif
                            g_encoder_thread_created = false;
#if APP_MODE_DMS
                            dms_storage_stop_session();
#else
                            storage_stop_session();
#endif
                            state_machine_set_state(STATE_ERROR);
                        } else {
                            g_capture_running = true;
                            if (pthread_create(&g_capture_thread, NULL, capture_thread_func, NULL) != 0) {
                                log_error("创建相机采集线程失败");

                                g_capture_running = false;
                                g_capture_thread_created = false;

#if ENABLE_ENCODER
                                g_encoder_running = false;
                                if (g_encoder_thread_created) {
                                    pthread_join(g_encoder_thread, NULL);
                                    g_encoder_thread_created = false;
                                }
#endif

#if APP_MODE_DMS
                                dms_storage_stop_session();
#else
                                storage_stop_session();
#endif
                                state_machine_set_state(STATE_ERROR);
                            } else {
                                g_capture_thread_created = true;
#if APP_MODE_DMS
                                if (!dms_ai_thread_start()) {
                                    log_error("DMS AI 线程启动失败");
                                    g_capture_running = false;
                                    pthread_join(g_capture_thread, NULL);
                                    g_capture_thread_created = false;
                                    dms_storage_stop_session();
                                    state_machine_set_state(STATE_ERROR);
                                    continue;
                                }
#endif
                                state_machine_set_state(STATE_RECORDING);
                            }
                        }
                    }
                }
                else if (g_system_state == STATE_RECORDING) {
                    log_info("录制按键点按: 停止采集");
                    g_capture_running = false;
#if APP_MODE_DMS
                    dms_ai_thread_stop();
#endif
                    if (g_capture_thread_created) {
                        pthread_join(g_capture_thread, NULL);
                        g_capture_thread_created = false;
                    }

#if ENABLE_ENCODER
                    g_encoder_running = false;
                    if (g_encoder_thread_created) {
                        pthread_join(g_encoder_thread, NULL);
                        g_encoder_thread_created = false;
                    }
#endif

#if APP_MODE_DMS
                    dms_storage_stop_session();
                    log_info("DMS session stopped");
#else
                    storage_stop_session();
#endif
                    state_machine_set_state(STATE_READY);
                }
                else {
                    log_warn("当前状态 %s 下，录制输入无效", state_to_string(g_system_state));
                }
            }
        }

        /* 采集线程错误处理：由主循环统一进入 ERROR */
        if (g_system_state == STATE_RECORDING && g_capture_error) {
            log_error("采集线程上报错误，停止采集并进入 ERROR");

            g_capture_running = false;
#if APP_MODE_DMS
            dms_ai_thread_stop();
#endif
            if (g_capture_thread_created) {
                pthread_join(g_capture_thread, NULL);
                g_capture_thread_created = false;
            }

#if ENABLE_ENCODER
            g_encoder_running = false;
            if (g_encoder_thread_created) {
                pthread_join(g_encoder_thread, NULL);
                g_encoder_thread_created = false;
            }
#endif

#if APP_MODE_DMS
            dms_storage_stop_session();
#else
            storage_stop_session();
#endif

            g_capture_error = 0;
            state_machine_set_state(STATE_ERROR);
        }

        // ========= 上传逻辑 =========
        if (upload_high && !last_upload_level) {
            if (now - upload_debounce_ts > 300000) {
                upload_debounce_ts = now;

                if (g_system_state == STATE_READY || g_system_state == STATE_UPLOAD_FAIL) {
#ifdef DMS_MODE
                    log_warn("DMS 模式暂不支持按键上传，请用 adb pull 导出数据");
#else
                    log_info("上传控制输入变高: 开始上传");

                    g_upload_cancel = 0;
                    g_upload_done = 0;
                    g_upload_result = 0;

                    if (pthread_create(&g_upload_thread, NULL, upload_thread_func, NULL) != 0) {
                        log_error("创建上传线程失败");
                        g_upload_thread_created = false;
                        state_machine_set_state(STATE_UPLOAD_FAIL);
                    } else {
                        g_upload_thread_created = true;
                        state_machine_set_state(STATE_UPLOADING);
                    }
#endif
                }
                else if (g_system_state == STATE_UPLOADING) {
                    log_warn("上传控制输入变高: 请求取消上传");
                    g_upload_cancel = 1;
                    upload_request_cancel();
                }
                else {
                    log_warn("当前状态 %s 下，上传输入无效", state_to_string(g_system_state));
                }
            }
        }

        /* 检查上传线程是否结束 */
        if (g_system_state == STATE_UPLOADING && g_upload_done) {
            pthread_join(g_upload_thread, NULL);
            g_upload_thread_created = false;
            g_upload_done = 0;

            if (g_upload_cancel) {
                log_warn("上传已取消，返回待机状态");
                state_machine_set_state(STATE_READY);
            } else if (g_upload_result) {
                log_info("上传完成，返回待机状态");
                state_machine_set_state(STATE_READY);
            } else {
                log_error("上传失败，进入失败状态");
                state_machine_set_state(STATE_UPLOAD_FAIL);
            }
        }

        last_record_level = record_high;
        last_upload_level = upload_high;

        usleep(10000);
    }
}

void state_machine_deinit(void)
{
#if APP_MODE_DMS && DMS_AUTO_START
    dms_stream_server_deinit();
#endif
    init_led_blink_stop();

    if (g_capture_thread_created) {
        g_capture_running = false;
#if APP_MODE_DMS
        dms_ai_thread_stop();
#endif
        pthread_join(g_capture_thread, NULL);
        g_capture_thread_created = false;
    }

    if (g_encoder_thread_created) {
        g_encoder_running = false;
        pthread_join(g_encoder_thread, NULL);
        g_encoder_thread_created = false;
    }

    if (g_upload_thread_created) {
        log_warn("程序退出: 正在取消上传线程");
        g_upload_cancel = 1;
        upload_request_cancel();
        pthread_join(g_upload_thread, NULL);
        g_upload_thread_created = false;
    }

#if APP_MODE_DMS
    dms_infer_deinit();
    dms_storage_deinit();
#else
    storage_deinit();
#endif

    camera_deinit();
#if ENABLE_ENCODER
    encoder_deinit();
#endif
    power_deinit();
    led_deinit();
    gpio_deinit();
    upload_deinit();

    log_info("状态机已退出");
}
