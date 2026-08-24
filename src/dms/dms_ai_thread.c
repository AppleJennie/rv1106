#include "dms_ai_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "sys_logger.h"
#include "dms_infer.h"
#include "dms_visualize.h"
#include "dms_stream_server.h"
#include "dms_retinaface.h"

#if DMS_HW_PREPROCESS
#include "dms_rga_preprocess.h"
#endif

/* Latest-frame single-slot buffer. */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;

    uint8_t  *data;
    size_t    size;
    size_t    cap;
    int       frame_id;
    uint64_t  timestamp_us;
    bool      valid;
    bool      consumed;
} latest_frame_t;

static latest_frame_t g_latest = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond  = PTHREAD_COND_INITIALIZER,
};

static pthread_t      g_ai_tid;
static volatile bool  g_ai_running = false;
static bool           g_ai_thread_created = false;

/* Last successful AI result, protected by g_latest.mutex. */
static dms_result_t   g_last_result;

/* Performance counters. */
static volatile uint64_t g_stat_camera_frames       = 0;
static volatile uint64_t g_stat_ai_frames           = 0;
static volatile uint64_t g_stat_ai_drops            = 0;
static volatile uint64_t g_stat_stream_frames       = 0;
static volatile uint64_t g_stat_debug_stream_frames = 0;
static volatile float    g_last_ai_fps = 0.0f;

static uint64_t get_mono_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static void reset_latest_frame(void)
{
    if (g_latest.data) {
        free(g_latest.data);
        g_latest.data = NULL;
    }
    g_latest.size = 0;
    g_latest.cap = 0;
    g_latest.frame_id = 0;
    g_latest.timestamp_us = 0;
    g_latest.valid = false;
    g_latest.consumed = true;
}

static bool copy_latest_to_buffer(const uint8_t *jpg, size_t jpg_size,
                                   int frame_id, uint64_t timestamp_us)
{
    pthread_mutex_lock(&g_latest.mutex);

    if (jpg_size > g_latest.cap) {
        uint8_t *new_buf = (uint8_t *)realloc(g_latest.data, jpg_size);
        if (!new_buf) {
            pthread_mutex_unlock(&g_latest.mutex);
            log_error("AI latest-frame buffer realloc 失败");
            return false;
        }
        g_latest.data = new_buf;
        g_latest.cap = jpg_size;
    }

    memcpy(g_latest.data, jpg, jpg_size);
    g_latest.size = jpg_size;
    g_latest.frame_id = frame_id;
    g_latest.timestamp_us = timestamp_us;
    g_latest.valid = true;
    g_latest.consumed = false;

    pthread_cond_signal(&g_latest.cond);
    pthread_mutex_unlock(&g_latest.mutex);
    return true;
}

static bool take_latest_frame(uint8_t **out_data, size_t *out_size,
                               int *out_frame_id, uint64_t *out_ts)
{
    pthread_mutex_lock(&g_latest.mutex);

    while (g_ai_running && (!g_latest.valid || g_latest.consumed)) {
        pthread_cond_wait(&g_latest.cond, &g_latest.mutex);
    }

    if (!g_ai_running) {
        pthread_mutex_unlock(&g_latest.mutex);
        return false;
    }

    *out_data = (uint8_t *)malloc(g_latest.size);
    if (!*out_data) {
        pthread_mutex_unlock(&g_latest.mutex);
        log_error("AI take_latest_frame malloc 失败");
        return false;
    }

    memcpy(*out_data, g_latest.data, g_latest.size);
    *out_size = g_latest.size;
    *out_frame_id = g_latest.frame_id;
    *out_ts = g_latest.timestamp_us;
    g_latest.consumed = true;

    pthread_mutex_unlock(&g_latest.mutex);
    return true;
}

static void update_last_result(const dms_result_t *r)
{
    pthread_mutex_lock(&g_latest.mutex);
    g_last_result = *r;
    pthread_mutex_unlock(&g_latest.mutex);
}

#if DMS_HW_PREPROCESS
/*
 * HW 模式下 AI 线程不从 g_latest 消费 JPEG（推理走 RGA prepared 帧），
 * 但 debug 可视化仍需要一张 JPEG。这里非阻塞地 peek 最新 JPEG（拷贝），
 * 不改变 valid/consumed 状态。
 */
static bool peek_latest_jpeg(uint8_t **out_data, size_t *out_size)
{
    pthread_mutex_lock(&g_latest.mutex);

    if (!g_latest.valid || !g_latest.data || g_latest.size == 0) {
        pthread_mutex_unlock(&g_latest.mutex);
        return false;
    }

    uint8_t *buf = (uint8_t *)malloc(g_latest.size);
    if (!buf) {
        pthread_mutex_unlock(&g_latest.mutex);
        log_error("AI peek_latest_jpeg malloc 失败");
        return false;
    }

    memcpy(buf, g_latest.data, g_latest.size);
    *out_data = buf;
    *out_size = g_latest.size;

    pthread_mutex_unlock(&g_latest.mutex);
    return true;
}
#endif /* DMS_HW_PREPROCESS */

static void *ai_thread_func(void *arg)
{
    (void)arg;
    log_info("DMS AI 线程启动");

    while (g_ai_running) {
#if DMS_HW_PREPROCESS
        /*
         * V2-A 硬件预处理路径：RGA 可用时走 prepared 帧，
         * AI 主路径无软件 JPEG 解码 / CPU resize。
         * RGA 不可用（init 失败或连续失败自锁）时自动回退下面的软件 JPEG 路径。
         */
        if (dms_rga_preprocess_is_ready()) {
            dms_prepared_frame_t prep;
            memset(&prep, 0, sizeof(prep));

            if (!dms_rga_preprocess_take(&prep, 500)) {
                continue;   /* 超时，重新检查 g_ai_running / ready */
            }

            uint64_t t0 = get_mono_time_us();

            dms_result_t result;
            memset(&result, 0, sizeof(result));
            strncpy(result.status, "AI_ERROR", sizeof(result.status) - 1);

            bool ok = dms_infer_process_prepared(&prep, &result);
            if (!ok) {
                strncpy(result.status, "AI_ERROR", sizeof(result.status) - 1);
                result.face_found = 0;
            }

            update_last_result(&result);
            g_stat_ai_frames++;

            /* debug 可视化：peek 最新 JPEG 画 HUD（仅在有 debug 客户端时） */
            if (dms_stream_server_debug_active()) {
                uint8_t *jpg = NULL;
                size_t jpg_size = 0;
                if (peek_latest_jpeg(&jpg, &jpg_size)) {
                    float ai_fps = dms_ai_thread_get_ai_fps();
                    size_t debug_size = 0;
                    uint8_t *debug_jpg = dms_visualize_generate(jpg, jpg_size, &result,
                                                                ai_fps, &debug_size);
                    if (debug_jpg) {
                        dms_stream_server_update_debug_frame(debug_jpg, debug_size, &result);
                        g_stat_debug_stream_frames++;
                        free(debug_jpg);
                    }
                    free(jpg);
                }
            }

            uint64_t cost_ms = (get_mono_time_us() - t0) / 1000ULL;
            if (cost_ms > 1000ULL) {
                log_warn("DMS AI 单帧处理耗时过长 (hw): %llums", (unsigned long long)cost_ms);
            }
            continue;
        }
#endif /* DMS_HW_PREPROCESS */

        uint8_t *jpg = NULL;
        size_t jpg_size = 0;
        int frame_id = 0;
        uint64_t ts_us = 0;

        if (!take_latest_frame(&jpg, &jpg_size, &frame_id, &ts_us)) {
            continue;
        }

        uint64_t t0 = get_mono_time_us();

        frame_data_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.img_data = jpg;
        frame.img_size = (int)jpg_size;
        frame.frame_id = frame_id;
        frame.timestamp_us = ts_us;

        dms_result_t result;
        memset(&result, 0, sizeof(result));
        strncpy(result.status, "AI_ERROR", sizeof(result.status) - 1);

        /* 1. 完整 AI 推理阶段（含 JPEG 解码 + RetinaFace + 疲劳状态机） */
        uint64_t t_infer_0 = get_mono_time_us();
        bool ok = dms_infer_process_frame(&frame, &result);
        if (!ok) {
            strncpy(result.status, "AI_ERROR", sizeof(result.status) - 1);
            result.face_found = 0;
        }
        uint64_t t_infer_1 = get_mono_time_us();
        uint64_t ai_total_us = t_infer_1 - t_infer_0;

        update_last_result(&result);
        g_stat_ai_frames++;

        /* 2. 生成带 HUD 的 debug JPEG，不阻塞视频采集线程。
         * 只在有 debug 流客户端时进行昂贵的 JPEG 编解码，避免拖慢 AI FPS。 */
        uint64_t debug_total_us = 0;
        if (dms_stream_server_debug_active()) {
            float ai_fps = dms_ai_thread_get_ai_fps();
            size_t debug_size = 0;
            uint64_t t_vis_0 = get_mono_time_us();
            uint8_t *debug_jpg = dms_visualize_generate(jpg, jpg_size, &result, ai_fps, &debug_size);
            uint64_t t_vis_1 = get_mono_time_us();
            debug_total_us = t_vis_1 - t_vis_0;
            if (debug_jpg) {
                dms_stream_server_update_debug_frame(debug_jpg, debug_size, &result);
                g_stat_debug_stream_frames++;
                free(debug_jpg);
            }
        }

        uint64_t cost_ms = (get_mono_time_us() - t0) / 1000ULL;
        if (cost_ms > 1000ULL) {
            log_warn("DMS AI 单帧处理耗时过长: %llums", (unsigned long long)cost_ms);
        }

        free(jpg);
    }

    log_info("DMS AI 线程退出");
    return NULL;
}

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

bool dms_ai_thread_start(void)
{
    if (g_ai_thread_created) {
        return true;
    }

    g_ai_running = true;
    g_stat_camera_frames       = 0;
    g_stat_ai_frames           = 0;
    g_stat_ai_drops            = 0;
    g_stat_stream_frames       = 0;
    g_stat_debug_stream_frames = 0;

    pthread_mutex_lock(&g_latest.mutex);
    memset(&g_last_result, 0, sizeof(g_last_result));
    strncpy(g_last_result.status, "NO_FACE", sizeof(g_last_result.status) - 1);
    pthread_mutex_unlock(&g_latest.mutex);

    if (pthread_create(&g_ai_tid, NULL, ai_thread_func, NULL) != 0) {
        log_error("DMS AI 线程创建失败");
        g_ai_running = false;
        return false;
    }

    g_ai_thread_created = true;
    return true;
}

void dms_ai_thread_stop(void)
{
    if (!g_ai_thread_created) {
        return;
    }

    g_ai_running = false;
    pthread_cond_broadcast(&g_latest.cond);
    pthread_join(g_ai_tid, NULL);
    g_ai_thread_created = false;

    pthread_mutex_lock(&g_latest.mutex);
    reset_latest_frame();
    memset(&g_last_result, 0, sizeof(g_last_result));
    pthread_mutex_unlock(&g_latest.mutex);
}

void dms_ai_thread_submit_frame(const uint8_t *jpg, size_t jpg_size,
                                 int frame_id, uint64_t timestamp_us)
{
    if (!g_ai_running || !jpg || jpg_size == 0) {
        return;
    }

    g_stat_camera_frames++;

    pthread_mutex_lock(&g_latest.mutex);
    bool was_unconsumed = g_latest.valid && !g_latest.consumed;
    pthread_mutex_unlock(&g_latest.mutex);

#if DMS_HW_PREPROCESS
    /*
     * HW 模式下 g_latest 只作 debug 可视化的 JPEG 缓存，AI 不消费它，
     * 覆盖未消费帧不代表 AI 跟不上，不计入 ai_drop。
     */
    if (was_unconsumed && !dms_rga_preprocess_is_ready()) {
        g_stat_ai_drops++;
    }
#else
    if (was_unconsumed) {
        g_stat_ai_drops++;
    }
#endif

    if (!copy_latest_to_buffer(jpg, jpg_size, frame_id, timestamp_us)) {
        log_warn("AI latest-frame 提交失败, frame_id=%d", frame_id);
    }
}

bool dms_ai_thread_get_latest_result(dms_result_t *result)
{
    if (!result) {
        return false;
    }

    pthread_mutex_lock(&g_latest.mutex);
    *result = g_last_result;
    pthread_mutex_unlock(&g_latest.mutex);
    return true;
}

void dms_ai_thread_inc_stream_frames(void)
{
    g_stat_stream_frames++;
}

void dms_ai_thread_print_stats(void)
{
    static uint64_t last_stat_ts = 0;
    static uint64_t last_camera = 0;
    static uint64_t last_ai = 0;
    static uint64_t last_stream = 0;
    static uint64_t last_debug_stream = 0;

    uint64_t now = get_mono_time_us();
    if (now - last_stat_ts < 5000000ULL) {
        return;
    }

    uint64_t dt_us = now - last_stat_ts;
    if (last_stat_ts == 0) {
        dt_us = 5000000ULL;
    }
    last_stat_ts = now;

    uint64_t cam = g_stat_camera_frames;
    uint64_t ai = g_stat_ai_frames;
    uint64_t stream = g_stat_stream_frames;
    uint64_t debug_stream = g_stat_debug_stream_frames;
    uint64_t drops = g_stat_ai_drops;

    float cam_fps = (float)(cam - last_camera) * 1000000.0f / (float)dt_us;
    float ai_fps  = (float)(ai - last_ai) * 1000000.0f / (float)dt_us;
    float stream_fps = (float)(stream - last_stream) * 1000000.0f / (float)dt_us;
    float debug_stream_fps = (float)(debug_stream - last_debug_stream) * 1000000.0f / (float)dt_us;
    g_last_ai_fps = ai_fps;

    uint64_t retina_avg_us = dms_retinaface_avg_inference_us();
    uint64_t retina_max_us = dms_retinaface_max_inference_us();

    const dms_retinaface_timing_t *rf_avg = dms_retinaface_avg_timing();
    const dms_visualize_timing_t  *vis_avg = dms_visualize_avg_timing();

    float rf_jpeg_ms     = rf_avg ? (float)rf_avg->jpeg_decode_us / 1000.0f : 0.0f;
    float rf_pre_ms      = rf_avg ? (float)rf_avg->preprocess_us / 1000.0f : 0.0f;
    float rf_rknn_ms     = rf_avg ? (float)rf_avg->rknn_run_us / 1000.0f : 0.0f;
    float rf_post_ms     = rf_avg ? (float)rf_avg->postprocess_us / 1000.0f : 0.0f;
    float rf_total_ms    = rf_avg ? (float)rf_avg->total_us / 1000.0f : 0.0f;

    float vis_dec_ms     = vis_avg ? (float)vis_avg->jpeg_decode_us / 1000.0f : 0.0f;
    float vis_draw_ms    = vis_avg ? (float)vis_avg->draw_us / 1000.0f : 0.0f;
    float vis_enc_ms     = vis_avg ? (float)vis_avg->jpeg_encode_us / 1000.0f : 0.0f;
    float vis_total_ms   = vis_avg ? (float)vis_avg->total_us / 1000.0f : 0.0f;

    log_info("[DMS PERF] camera_fps=%.1f raw_stream_fps=%.1f debug_stream_fps=%.1f ai_fps=%.1f "
             "retinaface_avg_ms=%.1f retinaface_max_ms=%.1f "
             "rf_jpeg_decode_ms=%.1f rf_preprocess_ms=%.1f rf_rknn_ms=%.1f rf_post_ms=%.1f rf_total_ms=%.1f "
             "vis_jpeg_decode_ms=%.1f vis_draw_ms=%.1f vis_encode_ms=%.1f vis_total_ms=%.1f "
             "camera_frames=%llu ai_frames=%llu ai_drop_latest=%llu",
             cam_fps, stream_fps, debug_stream_fps, ai_fps,
             (float)retina_avg_us / 1000.0f, (float)retina_max_us / 1000.0f,
             rf_jpeg_ms, rf_pre_ms, rf_rknn_ms, rf_post_ms, rf_total_ms,
             vis_dec_ms, vis_draw_ms, vis_enc_ms, vis_total_ms,
             (unsigned long long)cam,
             (unsigned long long)ai,
             (unsigned long long)drops);

    dms_retinaface_reset_stats();
    dms_retinaface_reset_timing_stats();
    dms_visualize_reset_timing_stats();

#if DMS_HW_PREPROCESS
    if (dms_rga_preprocess_is_ready()) {
        dms_rga_preprocess_stats_t rs;
        memset(&rs, 0, sizeof(rs));
        dms_rga_preprocess_get_stats(&rs);

        dms_infer_hw_timing_t ht;
        memset(&ht, 0, sizeof(ht));
        dms_infer_get_hw_timing(&ht);

        uint64_t pn = rs.produced_frames ? rs.produced_frames : 1;
        uint64_t hn = ht.frames ? ht.frames : 1;

        /*
         * HW 模式下：rf_pre_ms 即 retina_copy_ms（RGA 已替代 CPU resize），
         * rf_jpeg_ms 即 cpu_jpeg_decode_ms，必须≈0。
         * 计时用本周期均值；计数打累计总值，与主 PERF 行口径一致。
         */
        log_info("[DMS HW PERF] camera_fps=%.1f raw_stream_fps=%.1f ai_fps=%.1f "
                 "vi_get_ms=%.1f rga_retina_ms=%.1f rga_source_ms=%.1f "
                 "retina_copy_ms=%.1f retina_rknn_ms=%.1f retina_post_ms=%.1f "
                 "landmark106_ms=%.1f fatigue_feature_ms=%.1f cpu_jpeg_decode_ms=%.1f "
                 "rga_produced_total=%llu rga_dropped_total=%llu",
                 cam_fps, stream_fps, ai_fps,
                 (float)rs.vi_get_us / (float)pn / 1000.0f,
                 (float)rs.rga_retina_us / (float)pn / 1000.0f,
                 (float)rs.rga_source_us / (float)pn / 1000.0f,
                 rf_pre_ms, rf_rknn_ms, rf_post_ms,
                 (float)ht.landmark106_us / (float)hn / 1000.0f,
                 (float)ht.fatigue_feature_us / (float)hn / 1000.0f,
                 rf_jpeg_ms,
                 (unsigned long long)rs.produced_total,
                 (unsigned long long)rs.dropped_total);

        dms_rga_preprocess_reset_stats();
        dms_infer_reset_hw_timing();
    }
#endif /* DMS_HW_PREPROCESS */

    last_camera = cam;
    last_ai = ai;
    last_stream = stream;
    last_debug_stream = debug_stream;
}

float dms_ai_thread_get_ai_fps(void)
{
    return g_last_ai_fps;
}
