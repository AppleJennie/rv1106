#include "net_transfer.h"
#include "sys_logger.h"

#include <dirent.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h>


static int g_sock = -1;
static volatile int *g_cancel_flag = NULL;

static bool path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
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

static bool connect_server(void)
{
    struct sockaddr_in addr;
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) {
        log_error("创建上传 socket 失败");
        return false;
    }

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(g_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UPLOAD_SERVER_PORT);
    if (inet_pton(AF_INET, UPLOAD_SERVER_IP, &addr.sin_addr) <= 0) {
        log_error("上传服务器 IP 无效: %s", UPLOAD_SERVER_IP);
        close(g_sock);
        g_sock = -1;
        return false;
    }

    if (connect(g_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_error("连接上传服务器失败: %s:%d", UPLOAD_SERVER_IP, UPLOAD_SERVER_PORT);
        close(g_sock);
        g_sock = -1;
        return false;
    }

    log_info("已连接上传服务器: %s:%d", UPLOAD_SERVER_IP, UPLOAD_SERVER_PORT);
    return true;
}

static bool connect_server_retry(int retry)
{
    for (int i = 0; i < retry; i++) {
        if (connect_server()) {
            return true;
        }
        if (i + 1 < retry) {
            log_warn("连接服务器失败 %d/%d，1秒后重试", i + 1, retry);
            sleep(1);
        }
    }
    log_error("连接服务器失败，已重试 %d 次", retry);
    return false;
}

static void close_server(void)
{
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
}

static bool send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char*)buf;

    while (len > 0) {
        if (g_cancel_flag && *g_cancel_flag) {
            log_warn("send_all: 检测到取消请求");
            return false;
        }

        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);
                continue;
            }

            log_error("send 失败 errno=%d", errno);
            return false;
        }

        if (n == 0) {
            log_error("send 返回 0，连接可能已断开");
            return false;
        }

        p += n;
        len -= (size_t)n;
    }

    return true;
}

static bool recv_ok(int fd)
{
    if (g_cancel_flag && *g_cancel_flag) {
        return false;
    }

    char buf[RECV_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    while (1) {
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                log_error("等待服务端 OK 超时");
                return false;
            }

            log_error("recv 失败 errno=%d", errno);
            return false;
        }

        if (n == 0) {
            log_error("服务端关闭连接");
            return false;
        }

        buf[n] = '\0';

        if (strncmp(buf, "OK", 2) == 0) {
            return true;
        }

        log_error("服务端返回非 OK: %s", buf);
        return false;
    }
}

static bool send_file(const char *full_path, const char *relative_path)
{
    if (g_cancel_flag && *g_cancel_flag) {
        return false;
    }

    struct stat st;
    if (stat(full_path, &st) != 0) {
        log_error("获取文件信息失败: %s", full_path);
        return false;
    }

    FILE *fp = fopen(full_path, "rb");
    if (!fp) {
        log_error("打开文件失败: %s", full_path);
        return false;
    }

    char header[1024];
    int header_len = snprintf(header, sizeof(header), "FILE %s %lld\n",
                              relative_path, (long long)st.st_size);
    if (header_len <= 0 || header_len >= (int)sizeof(header)) {
        fclose(fp);
        return false;
    }

    if (!send_all(g_sock, header, (size_t)header_len)) {
        log_error("发送文件头失败: %s", relative_path);
        fclose(fp);
        return false;
    }

    char buf[SEND_BUF_SIZE];
    size_t nread;
    while ((nread = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (g_cancel_flag && *g_cancel_flag) {
            log_warn("send_file: 检测到取消请求");
            fclose(fp);
            return false;
        }
        if (!send_all(g_sock, buf, nread)) {
            log_error("发送文件内容失败: %s", relative_path);
            fclose(fp);
            return false;
        }
    }

    if (ferror(fp)) {
        log_error("读取文件失败: %s", full_path);
        fclose(fp);
        return false;
    }

    fclose(fp);

    if (!recv_ok(g_sock)) {
        log_error("服务端未确认文件: %s", relative_path);
        return false;
    }

    log_info("上传成功: %s", relative_path);
    return true;
}

static int cmp_str(const void *a, const void *b)
{
    const char * const *sa = (const char * const *)a;
    const char * const *sb = (const char * const *)b;
    return strcmp(*sa, *sb);
}

/* 基于 base_path 生成相对路径 */
static const char *make_relative_path(const char *base_path, const char *full_path)
{
    size_t base_len = strlen(base_path);

    if (strncmp(full_path, base_path, base_len) == 0) {
        const char *p = full_path + base_len;
        if (*p == '/') p++;
        return p;
    }

    return full_path;
}

/* ==================== 三态状态管理 ==================== */

static bool should_upload_group(const char *base_path, const char *group_name)
{
    char ready_path[MAX_PATH_LEN];
    char uploading_path[MAX_PATH_LEN];
    char uploaded_path[MAX_PATH_LEN];

    snprintf(ready_path, sizeof(ready_path), "%s/code/%s.ready", base_path, group_name);
    snprintf(uploading_path, sizeof(uploading_path), "%s/code/%s.uploading", base_path, group_name);
    snprintf(uploaded_path, sizeof(uploaded_path), "%s/code/%s.uploaded", base_path, group_name);

    /* 已上传的不再上传，同时清理历史残留 */
    if (path_exists(uploaded_path)) {
        if (path_exists(ready_path)) {
            unlink(ready_path);
        }

        if (path_exists(uploading_path)) {
            unlink(uploading_path);
        }

        return false;
    }

    /* 有 ready 或 uploading 都可以尝试上传（uploading 表示上次断电中断） */
    return path_exists(ready_path) || path_exists(uploading_path);
}

static bool mark_group_uploading(const char *base_path, const char *group_name)
{
    char ready_path[MAX_PATH_LEN];
    char uploading_path[MAX_PATH_LEN];
    char uploaded_path[MAX_PATH_LEN];

    snprintf(ready_path, sizeof(ready_path), "%s/code/%s.ready", base_path, group_name);
    snprintf(uploading_path, sizeof(uploading_path), "%s/code/%s.uploading", base_path, group_name);
    snprintf(uploaded_path, sizeof(uploaded_path), "%s/code/%s.uploaded", base_path, group_name);

    /*
     * 如果已经 uploaded，说明这个组已经完成。
     * 清理可能残留的 ready/uploading。
     */
    if (path_exists(uploaded_path)) {
        unlink(ready_path);
        unlink(uploading_path);
        return false;
    }

    /*
     * ready -> uploading
     */
    if (path_exists(ready_path)) {
        if (rename(ready_path, uploading_path) != 0) {
            log_error("rename ready->uploading 失败: %s, errno=%d", group_name, errno);
            return false;
        }

        fsync_parent_dir(uploading_path);
        return true;
    }

    /*
     * 没有 ready 但有 uploading，说明上次上传中断，继续上传。
     */
    if (path_exists(uploading_path)) {
        return true;
    }

    return false;
}

static bool mark_group_uploaded(const char *base_path, const char *group_name)
{
    char tmp_path[MAX_PATH_LEN];
    char ready_path[MAX_PATH_LEN];
    char uploading_path[MAX_PATH_LEN];
    char uploaded_path[MAX_PATH_LEN];

    snprintf(tmp_path, sizeof(tmp_path), "%s/code/%s.uploaded.tmp", base_path, group_name);
    snprintf(ready_path, sizeof(ready_path), "%s/code/%s.ready", base_path, group_name);
    snprintf(uploading_path, sizeof(uploading_path), "%s/code/%s.uploading", base_path, group_name);
    snprintf(uploaded_path, sizeof(uploaded_path), "%s/code/%s.uploaded", base_path, group_name);

    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        log_error("写 uploaded 临时标记失败: %s", tmp_path);
        return false;
    }

    fprintf(fp, "uploaded\n");
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    if (rename(tmp_path, uploaded_path) != 0) {
        log_error("rename uploaded 标记失败: %s -> %s, errno=%d",
                  tmp_path, uploaded_path, errno);
        unlink(tmp_path);
        return false;
    }

    /*
     * uploaded 已经写成功后，再清理 ready/uploading。
     * 即使清理失败，也不能影响 uploaded 状态。
     */
    if (unlink(uploading_path) != 0 && errno != ENOENT) {
        log_warn("清理 uploading 标记失败: %s, errno=%d", uploading_path, errno);
    }

    if (unlink(ready_path) != 0 && errno != ENOENT) {
        log_warn("清理 ready 标记失败: %s, errno=%d", ready_path, errno);
    }

    fsync_parent_dir(uploaded_path);
    return true;
}

static void rollback_group_uploading(const char *base_path, const char *group_name)
{
    char ready_path[MAX_PATH_LEN];
    char uploading_path[MAX_PATH_LEN];
    char uploaded_path[MAX_PATH_LEN];

    snprintf(ready_path, sizeof(ready_path), "%s/code/%s.ready", base_path, group_name);
    snprintf(uploading_path, sizeof(uploading_path), "%s/code/%s.uploading", base_path, group_name);
    snprintf(uploaded_path, sizeof(uploaded_path), "%s/code/%s.uploaded", base_path, group_name);

    /*
     * 如果已经 uploaded，不回滚，避免把已完成组重新变成 ready。
     */
    if (path_exists(uploaded_path)) {
        unlink(ready_path);
        unlink(uploading_path);
        return;
    }

    if (path_exists(uploading_path)) {
        /*
         * 避免 ready 已存在导致状态混乱。
         */
        if (path_exists(ready_path)) {
            unlink(ready_path);
        }

        if (rename(uploading_path, ready_path) != 0) {
            log_error("rename uploading->ready 回滚失败: %s, errno=%d", group_name, errno);
        } else {
            fsync_parent_dir(ready_path);
            log_warn("上传失败回滚: %s -> ready", group_name);
        }
    }
}

static bool group_exists(char **groups, int group_count, const char *name)
{
    for (int i = 0; i < group_count; i++) {
        if (strcmp(groups[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static bool upload_one_group(const char *base_path, const char *group_name)
{
    if (g_cancel_flag && *g_cancel_flag) {
        return false;
    }

    if (!should_upload_group(base_path, group_name)) {
        log_info("跳过未就绪或已上传组: %s", group_name);
        return true;
    }

    char csv_path[MAX_PATH_LEN];
    char photo_dir[MAX_PATH_LEN];

    snprintf(csv_path, sizeof(csv_path), "%s/code/%s.csv", base_path, group_name);
    snprintf(photo_dir, sizeof(photo_dir), "%s/photo/%s", base_path, group_name);

    if (!path_exists(csv_path)) {
        log_warn("组缺少 CSV，跳过: %s", group_name);
        return true;
    }

    /* 1. 状态迁移: ready -> uploading */
    if (!mark_group_uploading(base_path, group_name)) {
        log_error("标记 uploading 失败: %s", group_name);
        return false;
    }

    /* 2. 上传 CSV */
    const char *rel_csv = make_relative_path(base_path, csv_path);

    log_info("开始上传组: %s", group_name);

    if (!send_file(csv_path, rel_csv)) {
        log_error("上传组 CSV 失败: %s", group_name);
        rollback_group_uploading(base_path, group_name);
        return false;
    }

    /* 3. 上传编码器 CSV */
    char enc_csv_path[MAX_PATH_LEN];
    snprintf(enc_csv_path, sizeof(enc_csv_path), "%s/code/%s_enc.csv", base_path, group_name);

    if (!path_exists(enc_csv_path)) {
        log_error("组缺少编码器 CSV，拒绝上传该组: %s", enc_csv_path);
        rollback_group_uploading(base_path, group_name);
        return false;
    }

    const char *rel_enc_csv = make_relative_path(base_path, enc_csv_path);

    if (!send_file(enc_csv_path, rel_enc_csv)) {
        log_error("上传编码器 CSV 失败: %s", group_name);
        rollback_group_uploading(base_path, group_name);
        return false;
    }

    /* 4. 上传 photo 目录下 jpg */
    DIR *dir = opendir(photo_dir);
    if (!dir) {
        log_error("打开照片目录失败: %s", photo_dir);
        rollback_group_uploading(base_path, group_name);
        return false;
    }

    char *names[256];
    int count = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (g_cancel_flag && *g_cancel_flag) {
            closedir(dir);
            for (int j = 0; j < count; j++) free(names[j]);
            rollback_group_uploading(base_path, group_name);
            return false;
        }

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        const char *dot = strrchr(ent->d_name, '.');
        if (!dot || strcmp(dot, ".jpg") != 0) continue;

        if (count < 256) {
            names[count] = strdup(ent->d_name);
            if (names[count]) count++;
        }
    }
    closedir(dir);

    if (count <= 0) {
        log_error("照片目录为空，拒绝标记 uploaded: %s", photo_dir);
        for (int j = 0; j < count; j++) free(names[j]);
        rollback_group_uploading(base_path, group_name);
        return false;
    }

    qsort(names, count, sizeof(char*), cmp_str);

    bool upload_ok = true;
    for (int i = 0; i < count; i++) {
        if (g_cancel_flag && *g_cancel_flag) {
            upload_ok = false;
            break;
        }

        char img_path[MAX_PATH_LEN];
        snprintf(img_path, sizeof(img_path), "%s/%s", photo_dir, names[i]);

        const char *rel_img = make_relative_path(base_path, img_path);

        if (!send_file(img_path, rel_img)) {
            upload_ok = false;
            break;
        }
    }

    for (int i = 0; i < count; i++) free(names[i]);

    if (!upload_ok) {
        rollback_group_uploading(base_path, group_name);
        return false;
    }

    /* 全部文件上传成功后，写 uploaded 标记 */
    if (!mark_group_uploaded(base_path, group_name)) {
        log_error("uploaded 标记写入失败，回滚为 ready: %s", group_name);
        rollback_group_uploading(base_path, group_name);
        return false;
    }

    log_info("组上传完成并已标记: %s", group_name);
    return true;
}

void upload_request_cancel(void)
{
    if (g_cancel_flag) {
        *g_cancel_flag = 1;
    }

    /* 用 shutdown 打断阻塞的 send/recv，不直接 close（close 由上传线程自己做） */
    if (g_sock >= 0) {
        shutdown(g_sock, SHUT_RDWR);
    }
}

bool upload_all_sessions(const char *base_path, volatile int *cancel_flag)
{
    g_cancel_flag = cancel_flag;

    if (!base_path) return false;

    if (g_cancel_flag && *g_cancel_flag) return false;

    char code_dir[MAX_PATH_LEN];
    snprintf(code_dir, sizeof(code_dir), "%s/code", base_path);

    DIR *dir = opendir(code_dir);
    if (!dir) {
        log_error("无法打开 code 目录: %s", code_dir);
        return false;
    }

    /* 收集所有 .ready 和 .uploading 组名 */
    char *groups[512];
    int group_count = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (g_cancel_flag && *g_cancel_flag) {
            closedir(dir);
            for (int j = 0; j < group_count; j++) free(groups[j]);
            g_cancel_flag = NULL;
            close_server();
            return false;
        }

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        const char *dot = strrchr(ent->d_name, '.');
        if (!dot) continue;

        /* 只处理 .ready 和 .uploading */
        if (strcmp(dot, ".ready") != 0 && strcmp(dot, ".uploading") != 0) {
            continue;
        }

        size_t len = (size_t)(dot - ent->d_name);
        if (len == 0 || len >= MAX_NAME_LEN) continue;

        /* 检查是否已有 .uploaded */
        char uploaded_path[MAX_PATH_LEN];
        snprintf(uploaded_path, sizeof(uploaded_path), "%s/code/%.*s.uploaded",
                 base_path, (int)len, ent->d_name);
        if (path_exists(uploaded_path)) {
            continue;
        }

        char name[MAX_NAME_LEN];
        memcpy(name, ent->d_name, len);
        name[len] = '\0';

        /* 去重 */
        if (group_exists(groups, group_count, name)) {
            continue;
        }

        if (group_count < 512) {
            groups[group_count] = (char*)malloc(len + 1);
            if (!groups[group_count]) continue;
            memcpy(groups[group_count], name, len + 1);
            group_count++;
        }
    }
    closedir(dir);

    if (group_count == 0) {
        log_warn("没有发现待上传的数据组");
        g_cancel_flag = NULL;
        return true;
    }

    if (!connect_server_retry(3)) {
        for (int i = 0; i < group_count; i++) {
            free(groups[i]);
        }
        g_cancel_flag = NULL;
        return false;
    }

    qsort(groups, group_count, sizeof(char*), cmp_str);

    bool ok = true;
    for (int i = 0; i < group_count; i++) {
        if (g_cancel_flag && *g_cancel_flag) {
            ok = false;
            break;
        }
        if (!upload_one_group(base_path, groups[i])) {
            ok = false;
            break;
        }
    }

    for (int i = 0; i < group_count; i++) {
        free(groups[i]);
    }

    g_cancel_flag = NULL;
    close_server();
    return ok;
}

int upload_count_pending(const char *base_path)
{
    if (!base_path) return 0;

    char code_dir[MAX_PATH_LEN];
    snprintf(code_dir, sizeof(code_dir), "%s/code", base_path);

    DIR *dir = opendir(code_dir);
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;
    char seen[512][MAX_NAME_LEN];
    int seen_count = 0;

    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        const char *dot = strrchr(ent->d_name, '.');
        if (!dot) continue;

        if (strcmp(dot, ".ready") != 0 && strcmp(dot, ".uploading") != 0) {
            continue;
        }

        size_t len = (size_t)(dot - ent->d_name);
        if (len == 0 || len >= MAX_NAME_LEN) continue;

        char uploaded_path[MAX_PATH_LEN];
        snprintf(uploaded_path, sizeof(uploaded_path), "%s/code/%.*s.uploaded",
                 base_path, (int)len, ent->d_name);
        if (path_exists(uploaded_path)) {
            continue;
        }

        char name[MAX_NAME_LEN];
        memcpy(name, ent->d_name, len);
        name[len] = '\0';

        /* 去重 */
        bool dup = false;
        for (int i = 0; i < seen_count; i++) {
            if (strcmp(seen[i], name) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        if (seen_count < 512) {
            memcpy(seen[seen_count], name, len + 1);
            seen_count++;
        }
        count++;
    }

    closedir(dir);
    return count;
}

bool upload_init(void)
{
    return true;
}

void upload_deinit(void)
{
    close_server();
}
