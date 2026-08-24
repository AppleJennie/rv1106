#include "sys_health.h"
#include "sys_logger.h"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>

/*
 * 修改要点：
 * 1. 不再 kill rkisp / rkaiq_3A_server，避免破坏 ISP/AIQ/白平衡链路。
 * 2. 只停止 rkipc / mpp_service 这类可能占用媒体资源的业务进程。
 * 3. 仍然清理 Rockit 临时 socket，解决 Address already in use。
 * 4. 环境检查仍保持：设备节点、库文件、I2C、SD 卡、磁盘空间、网络接口。
 */

static bool mkdir_recursive(const char *path)
{
    if (!path || path[0] == '\0') return false;

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        return false;
    }

    return true;
}

static bool check_dev_node(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        log_error("设备节点不存在: %s", path);
        return false;
    }
    log_info("设备节点正常: %s", path);
    return true;
}

static bool check_dev_node_warn(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        log_warn("未发现设备节点: %s，请确认 SDK 设备节点命名", path);
        return false;
    }
    log_info("设备节点正常: %s", path);
    return true;
}

static bool check_library(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        log_error("库文件不存在: %s", path);
        return false;
    }
    log_info("库文件正常: %s", path);
    return true;
}

static bool check_dir_writable(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        log_warn("目录不存在，尝试创建: %s", path);
        if (!mkdir_recursive(path)) {
            log_error("创建目录失败: %s, errno=%d", path, errno);
            return false;
        }
        if (stat(path, &st) != 0) {
            log_error("创建后仍无法访问: %s", path);
            return false;
        }
    }

    if (!S_ISDIR(st.st_mode)) {
        log_error("路径不是目录: %s", path);
        return false;
    }

    char test_path[512];
    snprintf(test_path, sizeof(test_path), "%s/.health_test_%d", path, (int)getpid());

    int fd = open(test_path, O_CREAT | O_WRONLY, 0666);
    if (fd < 0) {
        log_error("目录不可写: %s, errno=%d", path, errno);
        return false;
    }

    close(fd);
    unlink(test_path);

    log_info("目录可写: %s", path);
    return true;
}

static void check_optional_net_iface(const char *iface)
{
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s", iface);

    if (access(path, F_OK) == 0) {
        log_info("网络接口存在: %s", iface);
    } else {
        log_warn("网络接口不存在: %s，上传功能可能不可用", iface);
    }
}

bool health_wait_device_ready(const char *path, int timeout_ms)
{
    int waited = 0;
    struct stat st;

    while (waited < timeout_ms) {
        if (stat(path, &st) == 0) {
            log_info("设备已就绪: %s", path);
            return true;
        }

        usleep(100000);
        waited += 100;
    }

    log_error("等待设备超时: %s, timeout=%dms", path, timeout_ms);
    return false;
}

bool health_check_disk_space(const char *path, int min_free_mb)
{
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0) {
        log_error("获取磁盘空间失败: %s, errno=%d", path, errno);
        return false;
    }

    unsigned long long free_mb = (unsigned long long)vfs.f_bavail * vfs.f_bsize / (1024 * 1024);

    if (free_mb < (unsigned long long)min_free_mb) {
        log_error("SD 卡剩余空间不足: %llu MB < %d MB", free_mb, min_free_mb);
        return false;
    }

    log_info("SD 卡剩余空间: %llu MB", free_mb);
    return true;
}

static void health_cleanup_rockit_tmp(void)
{
    log_info("清理 Rockit 临时 socket/缓存");

    system("rm -f /tmp/UNIX.domain* 2>/dev/null");
    system("rm -f /tmp/rk* 2>/dev/null");
    system("rm -f /tmp/rt* 2>/dev/null");

    /*
     * 启动阶段不做 sync，避免拖慢开机。
     * 真正采集停止时，在 storage_stop_session() 里 sync。
     */
}

static void health_stop_default_camera_services(void)
{
    log_info("停止可能占用摄像头的默认业务进程");

    /*
     * 不调用 RkLunch-stop.sh。
     * 部分固件没有这个脚本；另外它可能会连带影响 ISP/AIQ 相关服务。
     *
     * 不杀 rkisp / rkaiq_3A_server。
     * 你的绿图问题与 ISP/AIQ/白平衡链路高度相关，强杀它们会让问题更难排查。
     */
    system("killall rkipc 2>/dev/null");
    system("killall mpp_service 2>/dev/null");

    usleep(100000);

    system("killall -9 rkipc 2>/dev/null");
    system("killall -9 mpp_service 2>/dev/null");
}

void health_prepare_runtime_environment(void)
{
    log_info("准备运行环境...");

    /*
     * 原来这里 sleep(2)，会导致黄灯至少晚 2 秒。
     * 改成短等待，真正设备是否存在交给后面的 wait/check。
     */
    usleep(300000);

    health_stop_default_camera_services();
    health_cleanup_rockit_tmp();

    log_info("清理上次异常退出残留的 tmp 文件");
    {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "find %s -name '*.tmp' -type f -delete 2>/dev/null",
                 SDCARD_BASE_PATH);
        system(cmd);
    }

    /*
     * 原来这里再等 500ms，保守缩短到 100ms。
     */
    usleep(100000);

    /*
     * 等待关键设备。
     * vvi 在不同 SDK 里可能没有，不要等太久。
     * venc / vsys / I2C 是关键节点，但也不要每个等 5 秒。
     */
    health_wait_device_ready("/dev/mpi/vvi", 500);
    health_wait_device_ready("/dev/mpi/venc", 1500);
    health_wait_device_ready("/dev/mpi/vsys", 1500);
    health_wait_device_ready(I2C_DEVICE, 1500);

    log_info("运行环境准备完成");
}

bool health_check_before_start(void)
{
    bool ok = true;

    log_info("==================================");
    log_info(" 运行前环境检查开始");
    log_info("==================================");

    /* RKMPI 设备节点（vvi 不同 SDK 命名可能不同，仅 warn） */
    check_dev_node_warn("/dev/mpi/vvi");

    /* venc / vsys 是核心，缺失则 fatal */
    if (!check_dev_node("/dev/mpi/venc")) ok = false;
    if (!check_dev_node("/dev/mpi/vsys")) ok = false;

    /* 库文件 */
    if (!check_library("/oem/usr/lib/librockit.so"))        ok = false;
    if (!check_library("/oem/usr/lib/librockchip_mpp.so"))  ok = false;
    if (!check_library("/oem/usr/lib/librga.so"))           ok = false;

    /* I2C：关闭编码器时不依赖 I2C，仅作为警告 */
#if ENABLE_ENCODER
    if (!check_dev_node(I2C_DEVICE)) ok = false;
#else
    check_dev_node_warn(I2C_DEVICE);
#endif

    /* SD 卡 */
    if (!check_dir_writable(SDCARD_BASE_PATH)) ok = false;

    /* 磁盘空间 */
    if (!health_check_disk_space(SDCARD_BASE_PATH, MIN_FREE_MB)) ok = false;

    /* 网络（可选，不 fatal） */
    check_optional_net_iface("wlan0");

    log_info("==================================");
    if (ok) {
        log_info(" 运行前环境检查通过");
    } else {
        log_error(" 运行前环境检查失败");
    }
    log_info("==================================");

    return ok;
}
