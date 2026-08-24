#include "sys_dms_storage.h"
#include "sys_logger.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#define DMS_BASE_PATH           "/mnt/sdcard/dms"
#define DMS_SESSIONS_PATH       DMS_BASE_PATH "/sessions"

static char g_session_path[MAX_PATH_LEN];
static char g_frames_path[MAX_PATH_LEN];
static char g_csv_tmp_path[MAX_PATH_LEN];
static char g_csv_path[MAX_PATH_LEN];
static char g_json_path[MAX_PATH_LEN];

static int g_frame_counter = 0;
static FILE *g_csv_fp = NULL;

static pthread_mutex_t g_dms_mutex = PTHREAD_MUTEX_INITIALIZER;

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

    dms_fsync_parent_dir(path);
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

static void dms_session_name_from_time(char *buf, size_t len)
{
    time_t sec = time(NULL);
    struct tm tm_now;
    localtime_r(&sec, &tm_now);
    strftime(buf, len, "%Y%m%d_%H%M%S", &tm_now);
}

static void dms_time_str_now(char *buf, size_t len)
{
    time_t sec = time(NULL);
    struct tm tm_now;
    localtime_r(&sec, &tm_now);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm_now);
}

static bool dms_write_session_info_json(void)
{
    char tmp_path[MAX_PATH_LEN];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_json_path);

    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        log_error("DMS 打开 session_info.json 临时文件失败: %s", tmp_path);
        return false;
    }

    char time_buf[64];
    dms_time_str_now(time_buf, sizeof(time_buf));

    fprintf(fp, "{\n");
    fprintf(fp, "  \"mode\": \"driver_fatigue_capture\",\n");
    fprintf(fp, "  \"camera_width\": %d,\n", CAMERA_WIDTH);
    fprintf(fp, "  \"camera_height\": %d,\n", CAMERA_HEIGHT);
    fprintf(fp, "  \"fps\": %d,\n", CAMERA_FPS);
    fprintf(fp, "  \"device\": \"RV1106\",\n");
    fprintf(fp, "  \"start_time\": \"%s\"\n", time_buf);
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

    if (rename(tmp_path, g_json_path) != 0) {
        log_error("DMS rename session_info.json 失败: %s -> %s, errno=%d",
                  tmp_path, g_json_path, errno);
        unlink(tmp_path);
        return false;
    }

    dms_fsync_parent_dir(g_json_path);
    return true;
}

static bool dms_open_csv_tmp(void)
{
    g_csv_fp = fopen(g_csv_tmp_path, "w");
    if (!g_csv_fp) {
        log_error("DMS 打开 CSV 临时文件失败: %s", g_csv_tmp_path);
        return false;
    }

    fprintf(g_csv_fp, "frame_id,timestamp_us,jpeg_name,jpeg_size\n");
    fflush(g_csv_fp);
    fsync(fileno(g_csv_fp));
    dms_fsync_parent_dir(g_csv_tmp_path);

    return true;
}

static void dms_close_csv_tmp(void)
{
    if (g_csv_fp) {
        fflush(g_csv_fp);
        fsync(fileno(g_csv_fp));
        fclose(g_csv_fp);
        g_csv_fp = NULL;
    }
}

static bool dms_rename_csv_to_final(void)
{
    if (rename(g_csv_tmp_path, g_csv_path) != 0) {
        log_error("DMS rename CSV 失败: %s -> %s, errno=%d",
                  g_csv_tmp_path, g_csv_path, errno);
        return false;
    }

    dms_fsync_parent_dir(g_csv_path);
    return true;
}

bool dms_storage_init(void)
{
    pthread_mutex_lock(&g_dms_mutex);

    g_session_path[0] = '\0';
    g_frames_path[0] = '\0';
    g_csv_tmp_path[0] = '\0';
    g_csv_path[0] = '\0';
    g_json_path[0] = '\0';
    g_frame_counter = 0;
    g_csv_fp = NULL;

    bool ok = dms_mkdir_recursive(DMS_SESSIONS_PATH);

    pthread_mutex_unlock(&g_dms_mutex);

    if (ok) {
        log_info("DMS 存储模块初始化完成: %s", DMS_BASE_PATH);
    } else {
        log_error("DMS 存储模块初始化失败");
    }

    return ok;
}

bool dms_storage_start_session(void)
{
    pthread_mutex_lock(&g_dms_mutex);

    if (g_csv_fp) {
        dms_close_csv_tmp();
        unlink(g_csv_tmp_path);
    }

    char session_name[MAX_NAME_LEN];
    dms_session_name_from_time(session_name, sizeof(session_name));

    snprintf(g_session_path, sizeof(g_session_path),
             "%s/%s", DMS_SESSIONS_PATH, session_name);
    snprintf(g_frames_path, sizeof(g_frames_path),
             "%s/frames", g_session_path);
    snprintf(g_csv_tmp_path, sizeof(g_csv_tmp_path),
             "%s/frame_timestamps.csv.tmp", g_session_path);
    snprintf(g_csv_path, sizeof(g_csv_path),
             "%s/frame_timestamps.csv", g_session_path);
    snprintf(g_json_path, sizeof(g_json_path),
             "%s/session_info.json", g_session_path);

    if (!dms_mkdir_recursive(g_frames_path)) {
        pthread_mutex_unlock(&g_dms_mutex);
        log_error("DMS 创建 frames 目录失败: %s", g_frames_path);
        return false;
    }

    g_frame_counter = 0;

    if (!dms_write_session_info_json()) {
        pthread_mutex_unlock(&g_dms_mutex);
        log_error("DMS 写入 session_info.json 失败");
        return false;
    }

    if (!dms_open_csv_tmp()) {
        pthread_mutex_unlock(&g_dms_mutex);
        log_error("DMS 打开 CSV 临时文件失败");
        return false;
    }

    pthread_mutex_unlock(&g_dms_mutex);

    log_info("DMS 新会话开始: %s", g_session_path);
    return true;
}

static bool dms_write_alarm_frame_internal(const frame_data_t *frame,
                                            const char *alarm_dir,
                                            const char *reason)
{
    if (!frame || !frame->img_data || frame->img_size <= 0 || !reason) {
        return false;
    }

    if (!dms_mkdir_recursive(alarm_dir)) {
        log_error("DMS 创建 alarm 目录失败: %s", alarm_dir);
        return false;
    }

    static int alarm_counter = 0;
    alarm_counter++;

    char jpeg_name[64];
    char jpeg_path[MAX_PATH_LEN];
    snprintf(jpeg_name, sizeof(jpeg_name), "%06d_%s.jpg", alarm_counter, reason);
    snprintf(jpeg_path, sizeof(jpeg_path), "%s/%s", alarm_dir, jpeg_name);

    if (!dms_write_file_atomic(jpeg_path, frame->img_data, (size_t)frame->img_size)) {
        log_error("DMS 保存报警帧失败: %s", jpeg_path);
        return false;
    }

    log_info("DMS 报警帧已保存: %s", jpeg_path);
    return true;
}

bool dms_storage_save_alarm_frame(const frame_data_t *frame, const char *reason)
{
    pthread_mutex_lock(&g_dms_mutex);

    char alarm_dir[MAX_PATH_LEN];
    snprintf(alarm_dir, sizeof(alarm_dir), "%s/alarms", g_session_path);

    bool ok = dms_write_alarm_frame_internal(frame, alarm_dir, reason);

    pthread_mutex_unlock(&g_dms_mutex);
    return ok;
}

bool dms_storage_push_frame(const frame_data_t *frame)
{
    if (!frame || !frame->img_data || frame->img_size <= 0) {
        log_warn("DMS 推入非法帧");
        return false;
    }

    if (!dms_jpeg_is_valid((const uint8_t *)frame->img_data, (size_t)frame->img_size)) {
        log_warn("DMS 跳过非法 JPEG: frame_id=%d, size=%d",
                 frame->frame_id, frame->img_size);
        return false;
    }

    pthread_mutex_lock(&g_dms_mutex);

    if (!g_csv_fp) {
        pthread_mutex_unlock(&g_dms_mutex);
        log_error("DMS 未开始会话就收到帧");
        return false;
    }

    g_frame_counter++;
    int frame_no = g_frame_counter;

    char jpeg_name[32];
    char jpeg_path[MAX_PATH_LEN];
    snprintf(jpeg_name, sizeof(jpeg_name), "%06d.jpg", frame_no);
    snprintf(jpeg_path, sizeof(jpeg_path), "%s/%s", g_frames_path, jpeg_name);

    if (!dms_write_file_atomic(jpeg_path, frame->img_data, (size_t)frame->img_size)) {
        pthread_mutex_unlock(&g_dms_mutex);
        log_error("DMS 写 JPEG 失败: %s", jpeg_path);
        return false;
    }

    fprintf(g_csv_fp, "%d,%llu,%s,%d\n",
            frame->frame_id,
            (unsigned long long)frame->timestamp_us,
            jpeg_name,
            frame->img_size);

    fflush(g_csv_fp);
    fsync(fileno(g_csv_fp));

    pthread_mutex_unlock(&g_dms_mutex);

    return true;
}

void dms_storage_stop_session(void)
{
    pthread_mutex_lock(&g_dms_mutex);

    if (g_csv_fp) {
        dms_close_csv_tmp();

        if (!dms_rename_csv_to_final()) {
            log_error("DMS 停止会话时 CSV 落盘失败");
        } else {
            log_info("DMS 会话停止，CSV 落盘完成: %s", g_csv_path);
        }
    }

    sync();

    pthread_mutex_unlock(&g_dms_mutex);
}

void dms_storage_deinit(void)
{
    pthread_mutex_lock(&g_dms_mutex);

    if (g_csv_fp) {
        dms_close_csv_tmp();
        unlink(g_csv_tmp_path);
        g_csv_fp = NULL;
    }

    pthread_mutex_unlock(&g_dms_mutex);

    log_info("DMS 存储模块关闭");
}
