#include "sys_storage.h"
#include "sys_logger.h"
#include "sys_time.h"

#ifdef DMS_MODE
#include "sys_dms_storage.h"
#endif

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    frame_data_t frames[CAMERA_FPS];
    int count;
    char second_name[MAX_NAME_LEN];
} second_batch_t;

static ring_buffer_t g_rb;
static pthread_t g_storage_thread;
static bool g_storage_thread_created = false;
static volatile bool g_storage_running = false;
static volatile bool g_session_active = false;

static char g_base_path[MAX_PATH_LEN] = SDCARD_BASE_PATH;
static second_batch_t g_batch;
static pthread_mutex_t g_batch_mutex = PTHREAD_MUTEX_INITIALIZER;

#define ENCODER_BATCH_CAPACITY    (ENCODER_FPS + 20)

typedef struct {
    encoder_sample_t samples[ENCODER_BATCH_CAPACITY];
    int count;
    char second_name[MAX_NAME_LEN];
} encoder_second_batch_t;

static encoder_second_batch_t g_enc_batch;
static pthread_mutex_t g_enc_batch_mutex = PTHREAD_MUTEX_INITIALIZER;



static void frame_free(frame_data_t *f)
{
    if (!f) return;
    SAFE_FREE(f->img_data);
    f->img_size = 0;
}

static bool mkdir_recursive(const char *path)
{
    if (!path || path[0] == '\0') return false;

    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
                log_error("创建目录失败: %s, errno=%d", tmp, errno);
                return false;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        log_error("创建目录失败: %s, errno=%d", tmp, errno);
        return false;
    }

    return true;
}

static void fsync_parent_dir(const char *path)
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

static bool write_file_atomic(const char *path, const void *data, size_t len)
{
    char tmp_path[MAX_PATH_LEN];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    int fd = open(tmp_path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (fd < 0) {
        log_error("打开临时文件失败: %s, errno=%d", tmp_path, errno);
        return false;
    }

    const uint8_t *p = (const uint8_t *)data;
    size_t left = len;

    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_error("写文件失败: %s, errno=%d", tmp_path, errno);
            close(fd);
            unlink(tmp_path);
            return false;
        }
        if (n == 0) {
            log_error("写文件返回 0: %s", tmp_path);
            close(fd);
            unlink(tmp_path);
            return false;
        }

        p += n;
        left -= (size_t)n;
    }

    fsync(fd);
    close(fd);

    if (rename(tmp_path, path) != 0) {
        log_error("rename 失败: %s -> %s, errno=%d", tmp_path, path, errno);
        unlink(tmp_path);
        return false;
    }

    /* 严格掉电保护：sync 父目录 */
    fsync_parent_dir(path);

    return true;
}

static bool write_text_atomic(const char *path, const char *text)
{
    return write_file_atomic(path, text, strlen(text));
}

static bool write_csv_atomic(const char *csv_path, const second_batch_t *batch)
{
    char tmp_path[MAX_PATH_LEN];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", csv_path);

    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        log_error("打开 CSV 临时文件失败: %s", tmp_path);
        return false;
    }

    fprintf(fp, "index,frame_id,img_timestamp_us,img_name\n");

    for (int i = 0; i < batch->count; i++) {
        const frame_data_t *f = &batch->frames[i];
        fprintf(fp, "%d,%d,%llu,%03d.jpg\n",
                i + 1,
                f->frame_id,
                (unsigned long long)f->timestamp_us,
                i + 1);
    }

    if (ferror(fp)) {
        log_error("写入 CSV 失败: %s", tmp_path);
        fclose(fp);
        unlink(tmp_path);
        return false;
    }

    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    if (rename(tmp_path, csv_path) != 0) {
        log_error("CSV rename 失败: %s -> %s, errno=%d", tmp_path, csv_path, errno);
        unlink(tmp_path);
        return false;
    }

    fsync_parent_dir(csv_path);
    return true;
}

static bool jpeg_is_valid(const uint8_t *data, size_t size)
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

static bool write_file_fast(const char *path, const void *data, size_t len)
{
    if (!path || !data || len == 0) {
        log_error("拒绝写入空图片: path=%s, len=%zu",
                  path ? path : "NULL", len);
        return false;
    }

    char tmp_path[MAX_PATH_LEN];

    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (n <= 0 || n >= (int)sizeof(tmp_path)) {
        log_error("图片 tmp 路径过长: %s", path);
        return false;
    }

    /*
     * 关键：
     * 先写 .tmp，不直接写正式 jpg。
     * 这样即使中途失败，也不会产生 0KB 的 001.jpg。
     */
    int fd = open(tmp_path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (fd < 0) {
        log_error("打开图片临时文件失败: %s, errno=%d", tmp_path, errno);
        return false;
    }

    const uint8_t *p = (const uint8_t *)data;
    size_t left = len;

    while (left > 0) {
        ssize_t w = write(fd, p, left);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }

            log_error("写图片临时文件失败: %s, errno=%d", tmp_path, errno);
            close(fd);
            unlink(tmp_path);
            return false;
        }

        if (w == 0) {
            log_error("写图片临时文件返回0: %s", tmp_path);
            close(fd);
            unlink(tmp_path);
            return false;
        }

        p += w;
        left -= (size_t)w;
    }

    if (fsync(fd) != 0) {
        log_error("图片 fsync 失败: %s, errno=%d", tmp_path, errno);
        close(fd);
        unlink(tmp_path);
        return false;
    }

    if (close(fd) != 0) {
        log_error("关闭图片临时文件失败: %s, errno=%d", tmp_path, errno);
        unlink(tmp_path);
        return false;
    }

    /*
     * 写完整并刷盘后，才原子替换成正式 jpg。
     */
    if (rename(tmp_path, path) != 0) {
        log_error("图片 rename 失败: %s -> %s, errno=%d",
                  tmp_path, path, errno);
        unlink(tmp_path);
        return false;
    }

    return true;
}

static bool write_encoder_csv_atomic(const char *csv_path, const encoder_second_batch_t *batch)
{
    char tmp_path[MAX_PATH_LEN];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", csv_path);

    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        log_error("打开编码器 CSV 临时文件失败: %s", tmp_path);
        return false;
    }

    fprintf(fp, "index,enc_frame_id,timestamp_us,enc1,enc2,enc3,valid1,valid2,valid3\n");

    for (int i = 0; i < batch->count; i++) {
        const encoder_sample_t *s = &batch->samples[i];

        fprintf(fp, "%d,%d,%llu,%.3f,%.3f,%.3f,%d,%d,%d\n",
                i + 1,
                s->enc_frame_id,
                (unsigned long long)s->timestamp_us,
                s->enc_angle[0],
                s->enc_angle[1],
                s->enc_angle[2],
                s->enc_valid[0],
                s->enc_valid[1],
                s->enc_valid[2]);
    }

    if (ferror(fp)) {
        log_error("写入编码器 CSV 失败: %s", tmp_path);
        fclose(fp);
        unlink(tmp_path);
        return false;
    }

    fflush(fp);

    if (fsync(fileno(fp)) != 0) {
        log_error("编码器 CSV fsync 失败: %s, errno=%d", tmp_path, errno);
        fclose(fp);
        unlink(tmp_path);
        return false;
    }

    if (fclose(fp) != 0) {
        log_error("关闭编码器 CSV 失败: %s, errno=%d", tmp_path, errno);
        unlink(tmp_path);
        return false;
    }

    if (rename(tmp_path, csv_path) != 0) {
        log_error("编码器 CSV rename 失败: %s -> %s, errno=%d", tmp_path, csv_path, errno);
        unlink(tmp_path);
        return false;
    }

    fsync_parent_dir(csv_path);
    return true;
}

static uint64_t storage_get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static void timestamp_to_second_name(uint64_t ts_us, char *buf, size_t len)
{
    time_t sec = (time_t)(ts_us / 1000000ULL);
    struct tm tm_now;
    localtime_r(&sec, &tm_now);
    strftime(buf, len, "%Y-%m-%d_%H-%M-%S", &tm_now);
}

static void clear_batch(second_batch_t *batch)
{
    if (!batch) return;

    for (int i = 0; i < batch->count; i++) {
        frame_free(&batch->frames[i]);
    }

    memset(batch, 0, sizeof(*batch));
}

static void clear_encoder_batch(encoder_second_batch_t *batch)
{
    if (!batch) return;
    memset(batch, 0, sizeof(*batch));
}

static bool flush_encoder_batch(encoder_second_batch_t *batch)
{
    if (!batch || batch->count <= 0) return true;

    char csv_path[MAX_PATH_LEN];

    snprintf(csv_path, sizeof(csv_path), "%s/code/%s_enc.csv",
             g_base_path, batch->second_name);

    if (!write_encoder_csv_atomic(csv_path, batch)) {
        log_error("写编码器 CSV 失败: %s", csv_path);
        return false;
    }

    log_info("一秒编码器数据落盘完成: %s, samples=%d",
             batch->second_name, batch->count);

    clear_encoder_batch(batch);
    return true;
}

static bool flush_batch(second_batch_t *batch)
{
    if (!batch || batch->count <= 0) return true;

    uint64_t t0 = storage_get_time_us();

    char photo_dir[MAX_PATH_LEN];
    char csv_path[MAX_PATH_LEN];
    char ready_path[MAX_PATH_LEN];

    snprintf(photo_dir, sizeof(photo_dir), "%s/photo/%s", g_base_path, batch->second_name);
    snprintf(csv_path, sizeof(csv_path), "%s/code/%s.csv", g_base_path, batch->second_name);
    snprintf(ready_path, sizeof(ready_path), "%s/code/%s.ready", g_base_path, batch->second_name);

    if (!mkdir_recursive(photo_dir)) {
        return false;
    }

    /*
     * 确保 code/ 目录也存在（storage_init 时创建过，但运行时可能被删）
     */
    char code_dir[MAX_PATH_LEN];
    snprintf(code_dir, sizeof(code_dir), "%s/code", g_base_path);
    if (!mkdir_recursive(code_dir)) {
        return false;
    }

    for (int i = 0; i < batch->count; i++) {
        frame_data_t *f = &batch->frames[i];

        char img_name[64];
        char img_path[MAX_PATH_LEN];

        snprintf(img_name, sizeof(img_name), "%03d.jpg", i + 1);
        snprintf(img_path, sizeof(img_path), "%s/%s", photo_dir, img_name);

        if (!f->img_data || f->img_size <= 1024 ||
            !jpeg_is_valid((const uint8_t *)f->img_data, (size_t)f->img_size)) {
            log_warn("跳过异常图片帧: second=%s, index=%d, frame_id=%d, size=%d",
                     batch->second_name,
                     i + 1,
                     f->frame_id,
                     f->img_size);
            continue;
        }

        if (!write_file_fast(img_path, f->img_data, (size_t)f->img_size)) {
            log_error("写图片失败: %s", img_path);
            return false;
        }
    }

    if (!write_csv_atomic(csv_path, batch)) {
        log_error("写 CSV 失败: %s", csv_path);
        return false;
    }

    if (!write_text_atomic(ready_path, "ready\n")) {
        log_error("写 ready 标记失败: %s", ready_path);
        return false;
    }

    uint64_t cost_us = storage_get_time_us() - t0;

    log_info("一秒数据落盘完成: %s, frames=%d, cost=%llu us",
             batch->second_name,
             batch->count,
             (unsigned long long)cost_us);

    if (cost_us > 800000ULL) {
        log_warn("存储 flush 耗时过长: %llu us，建议检查 TF 卡速度或降低图片质量",
                 (unsigned long long)cost_us);
    }

    clear_batch(batch);
    return true;
}

static bool rb_init(ring_buffer_t *rb)
{
    memset(rb, 0, sizeof(*rb));
    pthread_mutex_init(&rb->mutex, NULL);
    pthread_cond_init(&rb->not_empty, NULL);
    pthread_cond_init(&rb->not_full, NULL);
    pthread_cond_init(&rb->drained, NULL);
    return true;
}

static void rb_destroy(ring_buffer_t *rb)
{
    pthread_mutex_lock(&rb->mutex);

    for (int i = 0; i < RING_BUFFER_SIZE; i++) {
        frame_free(&rb->buffer[i]);
    }

    pthread_mutex_unlock(&rb->mutex);

    pthread_mutex_destroy(&rb->mutex);
    pthread_cond_destroy(&rb->not_empty);
    pthread_cond_destroy(&rb->not_full);
    pthread_cond_destroy(&rb->drained);
}

static void rb_clear_locked(ring_buffer_t *rb)
{
    for (int i = 0; i < RING_BUFFER_SIZE; i++) {
        frame_free(&rb->buffer[i]);
    }
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    rb->inflight_count = 0;
    /* dropped_count 不清零，保留历史统计 */
}

bool storage_push_frame(const frame_data_t *frame)
{
#ifdef DMS_MODE
    return dms_storage_push_frame(frame);
#else
    if (!g_session_active) {
        return false;
    }

    if (!frame || !frame->img_data || frame->img_size <= 0) {
        return false;
    }

    pthread_mutex_lock(&g_rb.mutex);

    if (g_rb.count >= RING_BUFFER_SIZE) {
        g_rb.dropped_count++;

        frame_data_t *old = &g_rb.buffer[g_rb.tail];
        frame_free(old);
        g_rb.tail = (g_rb.tail + 1) % RING_BUFFER_SIZE;
        g_rb.count--;

        log_warn("存储队列满，丢弃最旧帧，累计丢帧=%d", g_rb.dropped_count);
    }

    frame_data_t *dst = &g_rb.buffer[g_rb.head];
    memset(dst, 0, sizeof(*dst));

    dst->timestamp_us = frame->timestamp_us;
    dst->enc_timestamp_us = frame->enc_timestamp_us;
    dst->frame_id = frame->frame_id;
    dst->img_size = frame->img_size;
    memcpy(dst->enc_angle, frame->enc_angle, sizeof(dst->enc_angle));

    dst->img_data = malloc((size_t)frame->img_size);
    if (!dst->img_data) {
        pthread_mutex_unlock(&g_rb.mutex);
        log_error("存储队列 malloc 失败");
        return false;
    }

    memcpy(dst->img_data, frame->img_data, (size_t)frame->img_size);

    g_rb.head = (g_rb.head + 1) % RING_BUFFER_SIZE;
    g_rb.count++;

    pthread_cond_signal(&g_rb.not_empty);
    pthread_mutex_unlock(&g_rb.mutex);

    return true;
#endif
}

bool storage_push_encoder_sample(const encoder_sample_t *sample)
{
#if !ENABLE_ENCODER
    (void)sample;
    return true;
#else
    if (!g_session_active) {
        return false;
    }

    if (!sample) {
        return false;
    }

    pthread_mutex_lock(&g_enc_batch_mutex);

    char second_name[MAX_NAME_LEN];
    timestamp_to_second_name(sample->timestamp_us, second_name, sizeof(second_name));

    if (g_enc_batch.count > 0 && strcmp(g_enc_batch.second_name, second_name) != 0) {
        if (!flush_encoder_batch(&g_enc_batch)) {
            log_error("flush_encoder_batch 失败，清空当前编码器批次");
            clear_encoder_batch(&g_enc_batch);
        }
    }

    if (g_enc_batch.count == 0) {
        snprintf(g_enc_batch.second_name, sizeof(g_enc_batch.second_name), "%s", second_name);
    }

    if (g_enc_batch.count >= ENCODER_BATCH_CAPACITY) {
        log_warn("当前秒编码器超过缓存上限 %d，丢弃额外编码器样本",
                 ENCODER_BATCH_CAPACITY);
        pthread_mutex_unlock(&g_enc_batch_mutex);
        return false;
    }

    g_enc_batch.samples[g_enc_batch.count] = *sample;
    g_enc_batch.count++;

    pthread_mutex_unlock(&g_enc_batch_mutex);
    return true;
#endif
}

static bool rb_pop(frame_data_t *out)
{
    pthread_mutex_lock(&g_rb.mutex);

    while (g_rb.count <= 0 && g_storage_running) {
        pthread_cond_wait(&g_rb.not_empty, &g_rb.mutex);
    }

    if (g_rb.count <= 0 && !g_storage_running) {
        pthread_mutex_unlock(&g_rb.mutex);
        return false;
    }

    frame_data_t *src = &g_rb.buffer[g_rb.tail];
    memcpy(out, src, sizeof(*out));
    src->img_data = NULL;
    src->img_size = 0;

    g_rb.tail = (g_rb.tail + 1) % RING_BUFFER_SIZE;
    g_rb.count--;
    g_rb.inflight_count++;

    pthread_mutex_unlock(&g_rb.mutex);
    return true;
}

static void handle_frame_to_batch(frame_data_t *frame)
{
    pthread_mutex_lock(&g_batch_mutex);

    char second_name[MAX_NAME_LEN];
    timestamp_to_second_name(frame->timestamp_us, second_name, sizeof(second_name));

    if (g_batch.count > 0 && strcmp(g_batch.second_name, second_name) != 0) {
        if (!flush_batch(&g_batch)) {
            log_error("flush_batch 失败，丢弃当前批次，避免阻塞后续采集");
            clear_batch(&g_batch);
        }
    }

    if (g_batch.count == 0) {
        snprintf(g_batch.second_name, sizeof(g_batch.second_name), "%s", second_name);
    }

    if (g_batch.count >= CAMERA_FPS) {
        log_warn("当前秒超过 %d 帧，丢弃额外帧", CAMERA_FPS);
        pthread_mutex_unlock(&g_batch_mutex);
        return;
    }

    frame_data_t *dst = &g_batch.frames[g_batch.count];
    memset(dst, 0, sizeof(*dst));

    dst->timestamp_us = frame->timestamp_us;
    dst->enc_timestamp_us = frame->enc_timestamp_us;
    dst->frame_id = frame->frame_id;
    dst->img_size = frame->img_size;
    memcpy(dst->enc_angle, frame->enc_angle, sizeof(dst->enc_angle));

    /*
     * 关键：这里不再 malloc + memcpy。
     * rb_pop() 已经把 ring buffer 里的 img_data 所有权转移给了 frame。
     * 这里继续把所有权转移给 batch。
     */
    dst->img_data = frame->img_data;
    frame->img_data = NULL;
    frame->img_size = 0;

    g_batch.count++;

    pthread_mutex_unlock(&g_batch_mutex);
}

static void *storage_thread_func(void *arg)
{
    (void)arg;

    log_info("存储线程启动");

    while (g_storage_running) {
        frame_data_t frame;
        memset(&frame, 0, sizeof(frame));

        if (rb_pop(&frame)) {
            handle_frame_to_batch(&frame);
            frame_free(&frame);

            /* 处理完成后递减 inflight，若队列和 inflight 均为空则广播 */
            pthread_mutex_lock(&g_rb.mutex);
            g_rb.inflight_count--;
            if (g_rb.count == 0 && g_rb.inflight_count == 0) {
                pthread_cond_broadcast(&g_rb.drained);
            }
            pthread_mutex_unlock(&g_rb.mutex);
        }
    }

    /* 线程即将退出前，排空剩余帧 */
    while (1) {
        frame_data_t frame;
        memset(&frame, 0, sizeof(frame));

        if (!rb_pop(&frame)) {
            break;
        }

        handle_frame_to_batch(&frame);
        frame_free(&frame);

        pthread_mutex_lock(&g_rb.mutex);
        g_rb.inflight_count--;
        if (g_rb.count == 0 && g_rb.inflight_count == 0) {
            pthread_cond_broadcast(&g_rb.drained);
        }
        pthread_mutex_unlock(&g_rb.mutex);
    }

    pthread_mutex_lock(&g_batch_mutex);
    if (g_batch.count > 0) {
        if (!flush_batch(&g_batch)) {
            log_error("存储线程退出前 flush 失败，清空当前批次");
            clear_batch(&g_batch);
        }
    }
    pthread_mutex_unlock(&g_batch_mutex);

    log_info("存储线程退出");
    return NULL;
}

bool storage_check_sdcard(void)
{
    struct stat st;

    if (stat(g_base_path, &st) != 0) {
        log_warn("基础目录不存在，尝试创建: %s", g_base_path);
        if (!mkdir_recursive(g_base_path)) {
            return false;
        }
    }

    if (stat(g_base_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        log_error("SD 卡目录异常: %s", g_base_path);
        return false;
    }

    char test_path[MAX_PATH_LEN];
    snprintf(test_path, sizeof(test_path), "%s/.write_test", g_base_path);

    if (!write_text_atomic(test_path, "test\n")) {
        log_error("SD 卡写入测试失败: %s", g_base_path);
        return false;
    }

    unlink(test_path);
    return true;
}

static void cleanup_tmp_files(void)
{
    char cmd[MAX_PATH_LEN + 128];

    int n = snprintf(cmd, sizeof(cmd),
                     "find %s -name '*.tmp' -type f -delete 2>/dev/null",
                     g_base_path);

    if (n > 0 && n < (int)sizeof(cmd)) {
        system(cmd);
    }
}

bool storage_init(void)
{
#ifdef DMS_MODE
    return dms_storage_init();
#else
    if (!storage_check_sdcard()) {
        return false;
    }

    cleanup_tmp_files();

    char photo_dir[MAX_PATH_LEN];
    char code_dir[MAX_PATH_LEN];

    snprintf(photo_dir, sizeof(photo_dir), "%s/photo", g_base_path);
    snprintf(code_dir, sizeof(code_dir), "%s/code", g_base_path);

    if (!mkdir_recursive(photo_dir)) return false;
    if (!mkdir_recursive(code_dir)) return false;

    rb_init(&g_rb);
    clear_batch(&g_batch);

    pthread_mutex_lock(&g_enc_batch_mutex);
    clear_encoder_batch(&g_enc_batch);
    pthread_mutex_unlock(&g_enc_batch_mutex);

    g_storage_running = true;

    if (pthread_create(&g_storage_thread, NULL, storage_thread_func, NULL) != 0) {
        g_storage_running = false;
        rb_destroy(&g_rb);
        log_error("创建存储线程失败");
        return false;
    }

    g_storage_thread_created = true;
    log_info("存储系统初始化完成: %s", g_base_path);
    return true;
#endif
}

bool storage_start_session(void)
{
#ifdef DMS_MODE
    return dms_storage_start_session();
#else
    if (!storage_check_sdcard()) {
        return false;
    }

    /* 先等存储线程排空正在处理的帧，再清队列，避免 inflight_count 被清零后又被减 */
    pthread_mutex_lock(&g_rb.mutex);
    while (g_rb.count > 0 || g_rb.inflight_count > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;

        int ret = pthread_cond_timedwait(&g_rb.drained, &g_rb.mutex, &ts);
        if (ret == ETIMEDOUT) {
            log_warn("启动新会话前等待 drained 超时，强制清理 count=%d, inflight=%d",
                     g_rb.count, g_rb.inflight_count);
            break;
        }
    }
    rb_clear_locked(&g_rb);
    pthread_mutex_unlock(&g_rb.mutex);

    pthread_mutex_lock(&g_batch_mutex);
    clear_batch(&g_batch);
    pthread_mutex_unlock(&g_batch_mutex);

    pthread_mutex_lock(&g_enc_batch_mutex);
    clear_encoder_batch(&g_enc_batch);
    pthread_mutex_unlock(&g_enc_batch_mutex);

    g_session_active = true;
    log_info("开始新的采集会话");
    return true;
#endif
}

void storage_stop_session(void)
{
#ifdef DMS_MODE
    dms_storage_stop_session();
#else
    g_session_active = false;

    /* 等待存储线程把队列里的帧全部取完并处理完，带 5 秒超时防死等 */
    pthread_mutex_lock(&g_rb.mutex);
    while (g_rb.count > 0 || g_rb.inflight_count > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5;

        int ret = pthread_cond_timedwait(&g_rb.drained, &g_rb.mutex, &ts);
        if (ret == ETIMEDOUT) {
            log_error("等待存储队列 drained 超时，count=%d, inflight=%d",
                      g_rb.count, g_rb.inflight_count);
            break;
        }
    }
    pthread_mutex_unlock(&g_rb.mutex);

    /* 加锁 flush 最后一批 */
    pthread_mutex_lock(&g_batch_mutex);
    if (g_batch.count > 0) {
        if (!flush_batch(&g_batch)) {
            log_error("停止采集时 flush 失败，清空当前批次");
            clear_batch(&g_batch);
        }
    }
    pthread_mutex_unlock(&g_batch_mutex);

    pthread_mutex_lock(&g_enc_batch_mutex);
    if (g_enc_batch.count > 0) {
        if (!flush_encoder_batch(&g_enc_batch)) {
            log_error("停止采集时编码器 flush 失败，清空当前批次");
            clear_encoder_batch(&g_enc_batch);
        }
    }
    pthread_mutex_unlock(&g_enc_batch_mutex);

    /*
     * 停止采集时做一次全局刷盘。
     * 解决最后一两个文件夹刚 rename 完但 TF 卡缓存未落盘的问题。
     */
    sync();
    usleep(300000);

    log_info("采集会话停止，已执行 sync 刷盘");
#endif
}

void storage_deinit(void)
{
#ifdef DMS_MODE
    dms_storage_deinit();
#else
    g_storage_running = false;

    pthread_mutex_lock(&g_rb.mutex);
    pthread_cond_broadcast(&g_rb.not_empty);
    pthread_cond_broadcast(&g_rb.drained);
    pthread_mutex_unlock(&g_rb.mutex);

    if (g_storage_thread_created) {
        pthread_join(g_storage_thread, NULL);
        g_storage_thread_created = false;
    }

    pthread_mutex_lock(&g_batch_mutex);
    clear_batch(&g_batch);
    pthread_mutex_unlock(&g_batch_mutex);

    pthread_mutex_lock(&g_enc_batch_mutex);
    clear_encoder_batch(&g_enc_batch);
    pthread_mutex_unlock(&g_enc_batch_mutex);

    rb_destroy(&g_rb);

    log_info("存储系统关闭");
#endif
}

int storage_get_dropped_count(void)
{
    int count;
    pthread_mutex_lock(&g_rb.mutex);
    count = g_rb.dropped_count;
    pthread_mutex_unlock(&g_rb.mutex);
    return count;
}
