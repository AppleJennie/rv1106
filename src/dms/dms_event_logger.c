#include "dms_event_logger.h"
#include "sys_logger.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ==================== 内部常量 ==================== */
#define EVENT_LOG_DIR       "/mnt/sdcard/dms/events"
#define EVENT_CSV_FILENAME  "events.csv"
#define EVENT_CSV_HEADER    "timestamp,event_type,risk_level,risk_score,duration_ms,ear,ear_baseline,mar,mar_baseline,head_down_score,face_score,vehicle_speed,route_id,driver_id,shift_id\n"

/* ==================== 内部状态 ==================== */
static FILE *s_csv_fp = NULL;
static bool  s_initialized = false;
static char  s_csv_path[MAX_PATH_LEN];
static char  s_log_dir[MAX_PATH_LEN] = EVENT_LOG_DIR;

/* ==================== 内部辅助函数 ==================== */

/* 创建目录（递归） */
static bool ensure_dir(const char *path)
{
    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s", path);

    size_t len = strlen(tmp);
    if (len == 0) return false;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

/* 检查文件是否为空（需要写 header） */
static bool file_is_empty(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return true;  /* 文件不存在 */
    return (st.st_size == 0);
}

/* ==================== 公开 API ==================== */

bool dms_event_logger_init(void)
{
    return dms_event_logger_init_with_dir(EVENT_LOG_DIR);
}

bool dms_event_logger_init_with_dir(const char *dir)
{
    if (s_initialized) return true;
    if (!dir || dir[0] == '\0') return false;

    snprintf(s_log_dir, sizeof(s_log_dir), "%s", dir);

    /* 创建事件目录 */
    if (!ensure_dir(s_log_dir)) {
        log_error("[EventLogger] failed to create dir: %s", s_log_dir);
        return false;
    }

    /* 拼接 CSV 路径 */
    snprintf(s_csv_path, sizeof(s_csv_path), "%.400s/%s", s_log_dir, EVENT_CSV_FILENAME);

    /* 检查是否需要写 header */
    bool need_header = file_is_empty(s_csv_path);

    /* 以追加模式打开 CSV */
    s_csv_fp = fopen(s_csv_path, "a");
    if (!s_csv_fp) {
        log_error("[EventLogger] failed to open: %s", s_csv_path);
        return false;
    }

    /* 写入 CSV header */
    if (need_header) {
        fputs(EVENT_CSV_HEADER, s_csv_fp);
        fflush(s_csv_fp);
    }

    s_initialized = true;
    log_info("[EventLogger] initialized, csv: %s", s_csv_path);
    return true;
}

bool dms_event_logger_write(const dms_event_record_t *record)
{
    if (!s_initialized || !s_csv_fp || !record) return false;

    /* NORMAL 事件不记录 */
    if (record->risk_level == DMS_RISK_NORMAL) return true;

    /* 格式化时间戳 */
    time_t sec = (time_t)(record->timestamp_ms / 1000);
    struct tm *tm_info = localtime(&sec);
    char time_str[32];
    if (tm_info) {
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    } else {
        snprintf(time_str, sizeof(time_str), "%llu", (unsigned long long)record->timestamp_ms);
    }

    /* 写入 CSV 行 */
    fprintf(s_csv_fp, "%s.%03d,%s,%s,%d,%llu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.1f,%s,%s,%s\n",
            time_str,
            (int)(record->timestamp_ms % 1000),
            record->event_type,
            dms_risk_level_to_string(record->risk_level),
            record->risk_score,
            (unsigned long long)record->duration_ms,
            record->ear,
            record->ear_baseline,
            record->mar,
            record->mar_baseline,
            record->head_down_score,
            record->face_score,
            record->vehicle_speed,
            record->route_id,
            record->driver_id,
            record->shift_id);

    return true;
}

bool dms_event_logger_save_snapshot(const char *event_type,
                                    const uint8_t *jpeg,
                                    size_t jpeg_size)
{
    if (!s_initialized || !event_type || !jpeg || jpeg_size == 0) return false;

    /* 生成文件名：YYYYMMDD_HHMMSS_EVENT_TYPE.jpg */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char filename[MAX_PATH_LEN];

    if (tm_info) {
        snprintf(filename, sizeof(filename),
                 "%.400s/%04d%02d%02d_%02d%02d%02d_%.32s.jpg",
                 s_log_dir,
                 tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
                 tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
                 event_type);
    } else {
        snprintf(filename, sizeof(filename),
                 "%.400s/%llu_%.32s.jpg",
                 s_log_dir,
                 (unsigned long long)now,
                 event_type);
    }

    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        log_error("[EventLogger] failed to save snapshot: %s", filename);
        return false;
    }

    size_t written = fwrite(jpeg, 1, jpeg_size, fp);
    fclose(fp);

    if (written != jpeg_size) {
        log_error("[EventLogger] snapshot write incomplete: %s (%zu/%zu)",
                  filename, written, jpeg_size);
        return false;
    }

    log_info("[EventLogger] snapshot saved: %s", filename);
    return true;
}

void dms_event_logger_flush(void)
{
    if (s_csv_fp) {
        fflush(s_csv_fp);
    }
}

void dms_event_logger_deinit(void)
{
    if (s_csv_fp) {
        fflush(s_csv_fp);
        fclose(s_csv_fp);
        s_csv_fp = NULL;
    }
    s_initialized = false;
    log_info("[EventLogger] deinitialized");
}
