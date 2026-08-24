#include "common.h"
#include "sys_logger.h"
#include "sys_health.h"
#include "state_machine.h"

#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

static volatile sig_atomic_t g_exit_signal = 0;

/*
 * 当 stdout 不是终端时（如被 start-stop-daemon 重定向到 /dev/null），
 * 把 stdout/stderr 重定向到固定日志文件，保证部署后能看到启动日志。
 */
static void redirect_stdout_to_log_if_needed(void)
{
    if (isatty(STDOUT_FILENO)) {
        return;
    }

    const char *log_path = "/mnt/sdcard/dms/live/console.log";
    int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return;
    }

    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) {
        close(fd);
    }
}

static void signal_handler(int sig)
{
    g_exit_signal = sig;
    g_run_flag = 0;
}

#define START_RETRY_DELAY_SEC       3
#define START_RETRY_MAX_DELAY_SEC   30

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    redirect_stdout_to_log_if_needed();

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    logger_init();
    logger_set_level(LOG_LEVEL_INFO);

    log_info("==================================");
    log_info(" Hand Capture Right - Industrial");
    log_info("==================================");

    int retry_count = 0;
    int delay_sec = START_RETRY_DELAY_SEC;

    while (g_run_flag) {
        log_info("启动尝试: retry=%d", retry_count);

        /* 每次启动前都先清理残留环境 */
        health_prepare_runtime_environment();

        if (!health_check_before_start()) {
            log_error("环境检查失败，%d 秒后重试", delay_sec);
            sleep(delay_sec);

            retry_count++;
            if (delay_sec < START_RETRY_MAX_DELAY_SEC) {
                delay_sec *= 2;
                if (delay_sec > START_RETRY_MAX_DELAY_SEC) {
                    delay_sec = START_RETRY_MAX_DELAY_SEC;
                }
            }
            continue;
        }

        if (!state_machine_init()) {
            log_error("state_machine_init 失败，释放资源后 %d 秒重试", delay_sec);

            state_machine_deinit();

            sleep(delay_sec);

            retry_count++;
            if (delay_sec < START_RETRY_MAX_DELAY_SEC) {
                delay_sec *= 2;
                if (delay_sec > START_RETRY_MAX_DELAY_SEC) {
                    delay_sec = START_RETRY_MAX_DELAY_SEC;
                }
            }
            continue;
        }

        /* 初始化成功，清空重试计数 */
        retry_count = 0;
        delay_sec = START_RETRY_DELAY_SEC;

        state_machine_run();

        /* 如果是收到退出信号，就真正退出 */
        state_machine_deinit();

        if (!g_run_flag) {
            break;
        }

        log_warn("状态机退出但未收到退出信号，%d 秒后尝试重启", delay_sec);
        sleep(delay_sec);
    }

    if (g_exit_signal != 0) {
        log_warn("收到退出信号: %d", g_exit_signal);
    }

    log_info("程序退出");
    logger_deinit();
    return 0;
}
