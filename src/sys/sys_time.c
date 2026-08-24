#include "sys_time.h"
#include "sys_logger.h"
#include <time.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/socket.h>

uint64_t time_get_timestamp_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

uint64_t time_get_timestamp_ms(void)
{
    return time_get_timestamp_us() / 1000;
}

void time_get_folder_name(char *buf, int buf_len)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, buf_len, "%Y-%m-%d_%H-%M-%S", &tm);
}

bool time_sync_with_master(const char *master_ip, int port)
{
    // 右手为主控，无需同步其他设备时间，此函数保留供扩展
    log_debug("右手为主控，跳过时间同步");
    return true;
}
