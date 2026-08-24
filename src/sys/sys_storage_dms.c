#include "sys_storage_dms.h"
#include "sys_logger.h"
#include "hal_camera.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

/* DMS 缓存 4 秒 @ 15fps，也可覆盖到 30fps */
#define DMS_RING_BUFFER_SIZE    64

typedef struct {
    frame_data_t frames[DMS_RING_BUFFER_SIZE];
    int head;
    int tail;
    int count;
    int inflight_count;
    int dropped_count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    pthread_cond_t drained;
} dms_ring_buffer_t;

static dms_ring_buffer_t g_dms_rb;
static pthread_t g_dms_storage_thread;
static bool g_dms_storage_thread_created = false;
static volatile bool g_dms_storage_running = false;
static volatile bool g_dms_session_active = false;

static char g_dms_session_path[MAX_PATH_LEN];
static char g_dms_frames_path[MAX_PATH_LEN];
static int g_dms_frame_counter = 0;
static pthread_mutex_t g_dms_frame_counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_dms_csv_fp = NULL;

static void dms_frame_free(frame_data_t *f)
{
    if (!f) return;
    SAFE_FREE(f->img_data);
    f->img_size = 0;
}

static bool dms_mkdir_recursive(const char *path)
{
    if (!path || path[0] == '\0') return false;

    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
                log_error("DMS 创建目录失败: %s, errno=%d", tmp, errno);
                return false;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        log_error("DMS 创建目录失败: %s, errno=%d", tmp, errno);
        return false;
    }

    return true;
}

static void dms_fsync_parent_dir(const char *path)
{
    char dir[MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s", path);

    char *slash = strrchr(dir, '/');
    if (!slash) return;

    *slash = '\0';

    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
}

static bool dms_write_file_atomic(const char *path, const void *data, size_t len)
{
    if (!path || !data || len == 0) {
        log_error("DMS 拒绝写入空文件: path=%s, len=%zu",
                  path ? path : "NULL", len);
        return false;
    }

    char tmp_path[MAX_PATH_LEN];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (n <= 0 || n >= (int)sizeof(tmp_path)) {
        log_error("DMS 临时文件路径过长: %s", path);
        return false;
    }

    int fd = open(tmp_path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (fd < 0) {
        log_error("DMS 打开临时文件失败: %s, errno=%d", tmp_path, errno);
        return false;
    }

    const uint8_t *p = (const uint8_t *)data;
    size_t left = len;

    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            log_error("DMS 写临时文件失败: %s, errno=%d", tmp_path, errno);
            close(fd);
            unlink(tmp_path);
            return false;
        }
        if (w == 0) {
            log_error("DMS 写临时文件返回 0: %s", tmp_path);
            close(fd);
            unlink(tmp_path);
            return false;
        }
        p += w;
        left -= (size_t)w;
    }

    if (fsync(fd) != 0) {
        log_error("DMS fsync 临时文件失败: %s, errno=%d", tmp_path, errno);
        close(fd);
        unlink(tmp_path);
        return false;
    }

    if (close(fd) != 0) {
        log_error("DMS 关闭临时文件失败: %s, errno=%d", tmp_path, errno);
        unlink(tmp_path);
        return false;
    }

    if (rename(tmp_path, path) != 0) {
        log_error("DMS rename 失败: %s -> %s, errno=%d", tmp_path, path, errno);
        unlink(tmp_path);
        return false;
    }

    return true;
}

static bool dms_jpeg_is_valid(const uint8_t *data, size_t size)
{
    if (!data || size < 4) {
        return false;
    }

    if (data[0] != 0xFF || data[1] != 0xD8) {
        return false;
    }

    if (data[size - 2] != 0xFF || data[size - 1] != 0xD9) {
        return false;
    }

    return true;
}

static uint64_t dms_get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static void dms_session_name_from_time(char *buf, size_t len)
{
    time_t sec = time(NULL);
    struct tm tm_now;
    localtime_r(&sec, &tm_now);
    strftime(buf, len, "%Y%m%d_%H%M%S", &tm_now);
}

static bool dms_write_session_info_json(void)
{
    int width = 0, height = 0, fps = 0;
    camera_get_resolution(&width, &height, &fps);

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/session_info.json", g_dms_session_path);

    char tmp_path[MAX_PATH_LEN];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        log_error("DMS 打开 session_info.json 临时文件失败: %s", tmp_path);
        return false;
    }

    time_t sec = time(NULL);
    struct tm tm_now;
    localtime_r(&sec, &tm_now);
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", &tm_now);

    fprintf(fp, "{\n");
    fprintf(fp, "  \"mode\": \"dms\",\n");
    fprintf(fp, "  \"camera_width\": %d,\n", width);
    fprintf(fp, "  \"camera_height\": %d,\n", height);
    fprintf(fp, "  \"fps\": %d,\n", fps);
    fprintf(fp, "  \"start_time\": \"%s\",\n", time_buf);
    fprintf(fp, "  \"device\": \"luckfox_pico_rv1106\"\n");
    fprintf(fp, "}\n");

    if (ferror(fp)) {
        log_error("DMS 写入 session_info.json 失败");
        fclose(fp);
        unlink(tmp_path);
        return false;
    }

    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    if (rename(tmp_path, path) != 0) {
        log_error("DMS rename session_info.json 失败: %s -> %s, errno=%d",
                  tmp_path, path, errno);
        unlink(tmp_path);
        return false;
    }

    dms_fsync_parent_dir(path);
    return true;
}

static bool dms_open_csv(void)
{
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/frame_timestamps.csv", g_dms_session_path);

    g_dms_csv_fp = fopen(path, "w");
    if (!g_dms_csv_fp) {
        log_error("DMS 打开 frame_timestamps.csv 失败: %s", path);
        return false;
    }

    fprintf(g_dms_csv_fp, "frame_id,timestamp_us,jpeg_name,jpeg_size\n");
    fflush(g_dms_csv_fp);
    fsync(fileno(g_dms_csv_fp));
    dms_fsync_parent_dir(path);

    return true;
}

static void dms_close_csv(void)
{
    if (g_dms_csv_fp) {
        fflush(g_dms_csv_fp);
        fsync(fileno(g_dms_csv_fp));
        fclose(g_dms_csv_fp);
        g_dms_csv_fp = NULL;
    }
}

static bool dms_write_frame(const frame_data_t *frame, int frame_no)
{
    if (!dms_jpeg_is_valid((const uint8_t *)frame->img_data, (size_t)frame->img_size)) {
        log_warn("DMS 跳过异常 JPEG: frame_id=%d, size=%d",
                 frame->frame_id, frame->img_size);
        return false;
    }

    char jpeg_name[32];
    char jpeg_path[MAX_PATH_LEN];
    snprintf(jpeg_name, sizeof(jpeg_name), "%06d.jpg", frame_no);
    snprintf(jpeg_path, sizeof(jpeg_path), "%s/%s", g_dms_frames_path, jpeg_name);

    if (!dms_write_file_atomic(jpeg_path, frame->img_data, (size_t)frame->img_size)) {
        return false;
    }

    if (g_dms_csv_fp) {
        fprintf(g_dms_csv_fp, "%d,%llu,%s,%d\n",
                frame->frame_id,
                (unsigned long long)frame->timestamp_us,
                jpeg_name,
                frame->img_size);
    }

    return true;
}

static void dms_rb_init(dms_ring_buffer_t *rb)
{
    memset(rb, 0, sizeof(*rb));
    pthread_mutex_init(&rb->mutex, NULL);
    pthread_cond_init(&rb->not_empty, NULL);
    pthread_cond_init(&rb->not_full, NULL);
    pthread_cond_init(&rb->drained, NULL);
}

static void dms_rb_destroy(dms_ring_buffer_t *rb)
{
    if (!rb) return;

    pthread_mutex_lock(&rb->mutex);
    for (int i = 0; i < DMS_RING_BUFFER_SIZE; i++) {
        dms_frame_free(&rb->frames[i]);
    }
    pthread_mutex_unlock(&rb->mutex);

    pthread_mutex_destroy(&rb->mutex);
    pthread_cond_destroy(&rb->not_empty);
    pthread_cond_destroy(&rb->not_full);
    pthread_cond_destroy(&rb->drained);
}

static void dms_rb_clear_locked(dms_ring_buffer_t *rb)
{
    for (int i = 0; i < DMS_RING_BUFFER_SIZE; i++) {
        dms_frame_free(&rb->frames[i]);
    }
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    rb->inflight_count = 0;
}

static bool dms_rb_pop(dms_ring_buffer_t *rb, frame_data_t *out)
{
    pthread_mutex_lock(&rb->mutex);

    while (rb->count <= 0 && g_dms_storage_running) {
        pthread_cond_wait(&rb->not_empty, &rb->mutex);
    }

    if (rb->count <= 0) {
        pthread_mutex_unlock(&rb->mutex);
        return false;
    }

    frame_data_t *src = &rb->frames[rb->tail];
    memcpy(out, src, sizeof(*out));
    src->img_data = NULL;
    src->img_size = 0;

    rb->tail = (rb->tail + 1) % DMS_RING_BUFFER_SIZE;
    rb->count--;
    rb->inflight_count++;

    pthread_mutex_unlock(&rb->mutex);
    return true;
}

static void *dms_storage_thread_func(void *arg)
{
    (void)arg;

    log_info("DMS 存储线程启动");

    while (g_dms_storage_running) {
        frame_data_t frame;
        memset(&frame, 0, sizeof(frame));

        if (dms_rb_pop(&g_dms_rb, &frame)) {
            int frame_no;
            pthread_mutex_lock(&g_dms_frame_counter_mutex);
            g_dms_frame_counter++;
            frame_no = g_dms_frame_counter;
            pthread_mutex_unlock(&g_dms_frame_counter_mutex);

            if (!dms_write_frame(&frame, frame_no)) {
                log_error("DMS 写帧失败: frame_id=%d", frame.frame_id);
            }

            dms_frame_free(&frame);

            pthread_mutex_lock(&g_dms_rb.mutex);
            g_dms_rb.inflight_count--;
            if (g_dms_rb.count == 0 && g_dms_rb.inflight_count == 0) {
                pthread_cond_broadcast(&g_dms_rb.drained);
            }
            pthread_mutex_unlock(&g_dms_rb.mutex);
        }
    }

    /* 退出前排空剩余帧 */
    while (1) {
        frame_data_t frame;
        memset(&frame, 0, sizeof(frame));

        if (!dms_rb_pop(&g_dms_rb, &frame)) {
            break;
        }

        int frame_no;
        pthread_mutex_lock(&g_dms_frame_counter_mutex);
        g_dms_frame_counter++;
        frame_no = g_dms_frame_counter;
        pthread_mutex_unlock(&g_dms_frame_counter_mutex);

        if (!dms_write_frame(&frame, frame_no)) {
            log_error("DMS 写帧失败: frame_id=%d", frame.frame_id);
        }
        dms_frame_free(&frame);

        pthread_mutex_lock(&g_dms_rb.mutex);
        g_dms_rb.inflight_count--;
        if (g_dms_rb.count == 0 && g_dms_rb.inflight_count == 0) {
            pthread_cond_broadcast(&g_dms_rb.drained);
        }
        pthread_mutex_unlock(&g_dms_rb.mutex);
    }

    log_info("DMS 存储线程退出");
    return NULL;
}

bool dms_storage_init(void)
{
    g_dms_session_path[0] = '\0';
    g_dms_frames_path[0] = '\0';
    g_dms_frame_counter = 0;
    g_dms_csv_fp = NULL;

    if (!dms_mkdir_recursive(DMS_BASE_PATH "/sessions")) {
        log_error("DMS 创建基础目录失败: %s", DMS_BASE_PATH "/sessions");
        return false;
    }

    dms_rb_init(&g_dms_rb);

    g_dms_storage_running = true;
    if (pthread_create(&g_dms_storage_thread, NULL, dms_storage_thread_func, NULL) != 0) {
        g_dms_storage_running = false;
        dms_rb_destroy(&g_dms_rb);
        log_error("DMS 创建存储线程失败");
        return false;
    }

    g_dms_storage_thread_created = true;
    log_info("DMS 存储系统初始化完成: %s", DMS_BASE_PATH);
    return true;
}

bool dms_storage_start_session(void)
{
    char session_name[MAX_NAME_LEN];
    dms_session_name_from_time(session_name, sizeof(session_name));

    snprintf(g_dms_session_path, sizeof(g_dms_session_path),
             "%s/sessions/%s", DMS_BASE_PATH, session_name);
    snprintf(g_dms_frames_path, sizeof(g_dms_frames_path),
             "%s/frames", g_dms_session_path);

    if (!dms_mkdir_recursive(g_dms_frames_path)) {
        log_error("DMS 创建 frames 目录失败: %s", g_dms_frames_path);
        return false;
    }

    pthread_mutex_lock(&g_dms_frame_counter_mutex);
    g_dms_frame_counter = 0;
    pthread_mutex_unlock(&g_dms_frame_counter_mutex);

    if (!dms_write_session_info_json()) {
        log_error("DMS 写入 session_info.json 失败");
        return false;
    }

    if (!dms_open_csv()) {
        log_error("DMS 打开 frame_timestamps.csv 失败");
        return false;
    }

    pthread_mutex_lock(&g_dms_rb.mutex);
    dms_rb_clear_locked(&g_dms_rb);
    pthread_mutex_unlock(&g_dms_rb.mutex);

    g_dms_session_active = true;
    log_info("DMS 新会话开始: %s", g_dms_session_path);
    return true;
}

void dms_storage_stop_session(void)
{
    g_dms_session_active = false;

    /* 等待存储线程把队列里的帧全部取完并处理完，带 5 秒超时 */
    pthread_mutex_lock(&g_dms_rb.mutex);
    while (g_dms_rb.count > 0 || g_dms_rb.inflight_count > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5;

        int ret = pthread_cond_timedwait(&g_dms_rb.drained, &g_dms_rb.mutex, &ts);
        if (ret == ETIMEDOUT) {
            log_error("DMS 等待存储队列 drained 超时，count=%d, inflight=%d",
                      g_dms_rb.count, g_dms_rb.inflight_count);
            break;
        }
    }
    pthread_mutex_unlock(&g_dms_rb.mutex);

    dms_close_csv();

    sync();
    usleep(300000);

    log_info("DMS 会话停止，已执行 sync 刷盘");
}

bool dms_storage_push_frame(const frame_data_t *frame)
{
    if (!g_dms_session_active) {
        return false;
    }

    if (!frame || !frame->img_data || frame->img_size <= 0) {
        return false;
    }

    pthread_mutex_lock(&g_dms_rb.mutex);

    if (g_dms_rb.count >= DMS_RING_BUFFER_SIZE) {
        g_dms_rb.dropped_count++;

        frame_data_t *old = &g_dms_rb.frames[g_dms_rb.tail];
        dms_frame_free(old);
        g_dms_rb.tail = (g_dms_rb.tail + 1) % DMS_RING_BUFFER_SIZE;
        g_dms_rb.count--;

        log_warn("DMS 存储队列满，丢弃最旧帧，累计丢帧=%d", g_dms_rb.dropped_count);
    }

    frame_data_t *dst = &g_dms_rb.frames[g_dms_rb.head];
    memset(dst, 0, sizeof(*dst));

    dst->timestamp_us = frame->timestamp_us;
    dst->frame_id = frame->frame_id;
    dst->img_size = frame->img_size;

    dst->img_data = malloc((size_t)frame->img_size);
    if (!dst->img_data) {
        pthread_mutex_unlock(&g_dms_rb.mutex);
        log_error("DMS 存储队列 malloc 失败");
        return false;
    }

    memcpy(dst->img_data, frame->img_data, (size_t)frame->img_size);

    g_dms_rb.head = (g_dms_rb.head + 1) % DMS_RING_BUFFER_SIZE;
    g_dms_rb.count++;

    pthread_cond_signal(&g_dms_rb.not_empty);
    pthread_mutex_unlock(&g_dms_rb.mutex);

    return true;
}

void dms_storage_deinit(void)
{
    g_dms_storage_running = false;
    g_dms_session_active = false;

    pthread_mutex_lock(&g_dms_rb.mutex);
    pthread_cond_broadcast(&g_dms_rb.not_empty);
    pthread_cond_broadcast(&g_dms_rb.drained);
    pthread_mutex_unlock(&g_dms_rb.mutex);

    if (g_dms_storage_thread_created) {
        pthread_join(g_dms_storage_thread, NULL);
        g_dms_storage_thread_created = false;
    }

    dms_close_csv();

    pthread_mutex_lock(&g_dms_rb.mutex);
    dms_rb_clear_locked(&g_dms_rb);
    pthread_mutex_unlock(&g_dms_rb.mutex);

    dms_rb_destroy(&g_dms_rb);

    log_info("DMS 存储系统关闭");
}
