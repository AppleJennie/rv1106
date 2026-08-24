#include "sys_logger.h"
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

static log_level_e g_log_level = LOG_LEVEL_INFO;
static const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};

bool logger_init(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    return true;
}

void logger_set_level(log_level_e level)
{
    g_log_level = level;
}

static void log_print(log_level_e level, const char *fmt, va_list args)
{
    if (level < g_log_level) return;

    struct timeval tv;
    struct tm tm;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);

    fprintf(stdout, "[%04d-%02d-%02d %02d:%02d:%02d.%03ld] [%s] ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, tv.tv_usec / 1000,
            level_str[level]);

    vfprintf(stdout, fmt, args);
    fprintf(stdout, "\n");

    /*
     * 后台启动时 stdout 通常重定向到文件，uClibc 的 _IOLBF
     * 不保证按行刷新，因此每条日志都 fflush，方便远程排障。
     */
    fflush(stdout);
}

void log_debug(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_print(LOG_LEVEL_DEBUG, fmt, args);
    va_end(args);
}

void log_info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_print(LOG_LEVEL_INFO, fmt, args);
    va_end(args);
}

void log_warn(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_print(LOG_LEVEL_WARN, fmt, args);
    va_end(args);
}

void log_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_print(LOG_LEVEL_ERROR, fmt, args);
    va_end(args);
}

void logger_deinit(void)
{
    // 无操作
}
