#include "dms_stream_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>

#include "sys_logger.h"

#define DMS_STREAM_MAX_CLIENTS  4
#define DMS_STREAM_BACKLOG      4
#define DMS_STREAM_BUF_SIZE     4096
#define DMS_STREAM_BOUNDARY     "dmsframe"

typedef struct {
    int listen_fd;
    int port;
    volatile int running;
    pthread_t accept_thread;

    pthread_mutex_t data_mutex;
    uint8_t *jpg_buf;
    size_t jpg_size;
    size_t jpg_cap;
    dms_result_t result;
    volatile int data_valid;

    /* Debug visualization stream. */
    uint8_t *debug_jpg_buf;
    size_t debug_jpg_size;
    size_t debug_jpg_cap;
    volatile int debug_valid;
    int debug_client_count;
} stream_server_t;

static stream_server_t g_ss = { 0 };

static const char *html_page =
    "<!DOCTYPE html>"
    "<html><head><meta charset=\"UTF-8\"><title>DMS Live</title>"
    "<style>"
    "body{font-family:sans-serif;background:#111;color:#eee;text-align:center;margin:20px}"
    "img{border:2px solid #444;max-width:95%;background:#000}"
    "#status{margin-top:15px;font-size:18px;color:#0f0}"
    "</style></head><body>"
    "<h1>DMS 实时监控</h1>"
    "<img id=\"live\" src=\"/stream.mjpg\" alt=\"live feed\">"
    "<div><a href=\"/stream_debug.mjpg\" style=\"color:#0f0;\">debug 叠加流 (带人脸框与 HUD)</a></div>"
    "<div id=\"status\">加载中...</div>"
    "<script>"
    "function updateStatus(){"
    "fetch('/status.json').then(r=>r.json()).then(d=>{"
    "const s=document.getElementById('status');"
    "s.innerText='状态:'+d.status+' 人脸:'+(d.face_found?'有':'无')+"
    "' 置信:'+d.face_score.toFixed(2)+' pitch:'+d.pitch.toFixed(1)+"
    "' mar:'+d.mar.toFixed(2)+' 疲劳:'+(d.fatigue?'是':'否');"
    "}).catch(e=>{console.log(e);});"
    "}"
    "setInterval(updateStatus,1000);"
    "updateStatus();"
    "</script></body></html>";

static void safe_close(int *fd)
{
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static int send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int send_string(int fd, const char *str)
{
    return send_all(fd, str, strlen(str));
}

static int read_http_request(int fd, char *buf, size_t buf_size)
{
    size_t total = 0;
    memset(buf, 0, buf_size);

    while (total < buf_size - 1) {
        ssize_t n = recv(fd, buf + total, buf_size - 1 - total, 0);
        if (n <= 0) return -1;
        total += (size_t)n;
        buf[total] = '\0';

        /* End of HTTP header. */
        if (strstr(buf, "\r\n\r\n") != NULL) break;
    }
    return (int)total;
}

static void handle_status_json(int client_fd)
{
    char body[512];
    pthread_mutex_lock(&g_ss.data_mutex);
    const dms_result_t *r = &g_ss.result;
    snprintf(body, sizeof(body),
        "{"
        "\"status\":\"%s\","
        "\"face_found\":%d,"
        "\"face_score\":%.3f,"
        "\"face_x\":%d,"
        "\"face_y\":%d,"
        "\"face_w\":%d,"
        "\"face_h\":%d,"
        "\"ear\":%.3f,"
        "\"mar\":%.3f,"
        "\"pitch\":%.3f,"
        "\"head_down\":%d,"
        "\"fatigue\":%d"
        "}",
        r->status,
        r->face_found,
        r->face_score,
        r->face_x, r->face_y, r->face_w, r->face_h,
        r->ear, r->mar, r->pitch,
        r->head_down, r->fatigue);
    pthread_mutex_unlock(&g_ss.data_mutex);

    char header[256];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", strlen(body));

    send_string(client_fd, header);
    send_string(client_fd, body);
}

static void handle_stream_mjpg(int client_fd)
{
    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" DMS_STREAM_BOUNDARY "\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";
    if (send_string(client_fd, hdr) < 0) return;

    int frame_count = 0;
    while (g_ss.running) {
        uint8_t *jpg = NULL;
        size_t jpg_size = 0;

        pthread_mutex_lock(&g_ss.data_mutex);
        if (g_ss.data_valid && g_ss.jpg_size > 0) {
            jpg = (uint8_t *)malloc(g_ss.jpg_size);
            if (jpg) {
                memcpy(jpg, g_ss.jpg_buf, g_ss.jpg_size);
                jpg_size = g_ss.jpg_size;
            }
        }
        pthread_mutex_unlock(&g_ss.data_mutex);

        if (!jpg || jpg_size == 0) {
            usleep(50000);
            continue;
        }

        char header[256];
        snprintf(header, sizeof(header),
            "--" DMS_STREAM_BOUNDARY "\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %zu\r\n"
            "\r\n", jpg_size);

        int ok = (send_string(client_fd, header) == 0) &&
                 (send_all(client_fd, (const char *)jpg, jpg_size) == 0) &&
                 (send_string(client_fd, "\r\n") == 0);

        free(jpg);

        if (!ok) break;

        /* Limit to ~15 fps to avoid saturating network. */
        usleep(66000);
        if (++frame_count > 10000) frame_count = 0; /* safety */
    }
}

static void handle_stream_debug_mjpg(int client_fd)
{
    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" DMS_STREAM_BOUNDARY "\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";

    pthread_mutex_lock(&g_ss.data_mutex);
    g_ss.debug_client_count++;
    pthread_mutex_unlock(&g_ss.data_mutex);

    if (send_string(client_fd, hdr) < 0) {
        pthread_mutex_lock(&g_ss.data_mutex);
        g_ss.debug_client_count--;
        pthread_mutex_unlock(&g_ss.data_mutex);
        return;
    }

    int frame_count = 0;
    while (g_ss.running) {
        uint8_t *jpg = NULL;
        size_t jpg_size = 0;

        pthread_mutex_lock(&g_ss.data_mutex);
        if (g_ss.debug_valid && g_ss.debug_jpg_size > 0) {
            jpg = (uint8_t *)malloc(g_ss.debug_jpg_size);
            if (jpg) {
                memcpy(jpg, g_ss.debug_jpg_buf, g_ss.debug_jpg_size);
                jpg_size = g_ss.debug_jpg_size;
            }
        }
        pthread_mutex_unlock(&g_ss.data_mutex);

        if (!jpg || jpg_size == 0) {
            usleep(50000);
            continue;
        }

        char header[256];
        snprintf(header, sizeof(header),
            "--" DMS_STREAM_BOUNDARY "\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %zu\r\n"
            "\r\n", jpg_size);

        int ok = (send_string(client_fd, header) == 0) &&
                 (send_all(client_fd, (const char *)jpg, jpg_size) == 0) &&
                 (send_string(client_fd, "\r\n") == 0);

        free(jpg);

        if (!ok) break;

        /* Debug stream follows AI FPS (~2 fps); cap at ~5 fps to save bandwidth. */
        usleep(200000);
        if (++frame_count > 10000) frame_count = 0;
    }

    pthread_mutex_lock(&g_ss.data_mutex);
    g_ss.debug_client_count--;
    pthread_mutex_unlock(&g_ss.data_mutex);
}

static void handle_root(int client_fd)
{
    size_t len = strlen(html_page);
    char header[256];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "\r\n", len);
    send_string(client_fd, header);
    send_string(client_fd, html_page);
}

static void *client_thread_func(void *arg)
{
    int client_fd = (int)(intptr_t)arg;
    char buf[DMS_STREAM_BUF_SIZE];

    if (read_http_request(client_fd, buf, sizeof(buf)) < 0) {
        safe_close(&client_fd);
        return NULL;
    }

    if (strncmp(buf, "GET /stream.mjpg", 16) == 0) {
        handle_stream_mjpg(client_fd);
    } else if (strncmp(buf, "GET /stream_debug.mjpg", 22) == 0) {
        handle_stream_debug_mjpg(client_fd);
    } else if (strncmp(buf, "GET /status.json", 16) == 0) {
        handle_status_json(client_fd);
    } else {
        handle_root(client_fd);
    }

    safe_close(&client_fd);
    return NULL;
}

static void *accept_thread_func(void *arg)
{
    (void)arg;

    while (g_ss.running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(g_ss.listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (g_ss.running) {
                log_warn("stream server accept 失败: %s", strerror(errno));
            }
            continue;
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread_func, (void *)(intptr_t)client_fd) != 0) {
            log_warn("stream server 客户端线程创建失败");
            safe_close(&client_fd);
            continue;
        }
        pthread_detach(tid);
    }

    return NULL;
}

bool dms_stream_server_init(int port)
{
    memset(&g_ss, 0, sizeof(g_ss));
    g_ss.port = port;
    pthread_mutex_init(&g_ss.data_mutex, NULL);

    g_ss.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_ss.listen_fd < 0) {
        log_error("stream server socket 创建失败: %s", strerror(errno));
        return false;
    }

    int reuse = 1;
    setsockopt(g_ss.listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(g_ss.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        log_error("stream server bind 端口 %d 失败: %s", port, strerror(errno));
        safe_close(&g_ss.listen_fd);
        return false;
    }

    if (listen(g_ss.listen_fd, DMS_STREAM_BACKLOG) != 0) {
        log_error("stream server listen 失败: %s", strerror(errno));
        safe_close(&g_ss.listen_fd);
        return false;
    }

    g_ss.running = 1;
    if (pthread_create(&g_ss.accept_thread, NULL, accept_thread_func, NULL) != 0) {
        log_error("stream server 接受线程创建失败");
        g_ss.running = 0;
        safe_close(&g_ss.listen_fd);
        return false;
    }

    log_info("DMS stream server started: http://0.0.0.0:%d/", port);
    return true;
}

void dms_stream_server_update_frame(const uint8_t *jpg, size_t jpg_size, const dms_result_t *result)
{
    if (!jpg || jpg_size == 0) return;

    pthread_mutex_lock(&g_ss.data_mutex);

    if (jpg_size > g_ss.jpg_cap) {
        uint8_t *new_buf = (uint8_t *)realloc(g_ss.jpg_buf, jpg_size);
        if (new_buf) {
            g_ss.jpg_buf = new_buf;
            g_ss.jpg_cap = jpg_size;
        }
    }

    if (g_ss.jpg_cap >= jpg_size) {
        memcpy(g_ss.jpg_buf, jpg, jpg_size);
        g_ss.jpg_size = jpg_size;
        if (result) {
            g_ss.result = *result;
        }
        g_ss.data_valid = 1;
    }

    pthread_mutex_unlock(&g_ss.data_mutex);
}

void dms_stream_server_update_debug_frame(const uint8_t *jpg, size_t jpg_size, const dms_result_t *result)
{
    if (!jpg || jpg_size == 0) return;

    pthread_mutex_lock(&g_ss.data_mutex);

    if (jpg_size > g_ss.debug_jpg_cap) {
        uint8_t *new_buf = (uint8_t *)realloc(g_ss.debug_jpg_buf, jpg_size);
        if (new_buf) {
            g_ss.debug_jpg_buf = new_buf;
            g_ss.debug_jpg_cap = jpg_size;
        }
    }

    if (g_ss.debug_jpg_cap >= jpg_size) {
        memcpy(g_ss.debug_jpg_buf, jpg, jpg_size);
        g_ss.debug_jpg_size = jpg_size;
        if (result) {
            g_ss.result = *result;
        }
        g_ss.debug_valid = 1;
    }

    pthread_mutex_unlock(&g_ss.data_mutex);
}

bool dms_stream_server_debug_active(void)
{
    pthread_mutex_lock(&g_ss.data_mutex);
    bool active = g_ss.debug_client_count > 0;
    pthread_mutex_unlock(&g_ss.data_mutex);
    return active;
}

void dms_stream_server_deinit(void)
{
    g_ss.running = 0;

    if (g_ss.listen_fd >= 0) {
        /* Force accept() to return. */
        shutdown(g_ss.listen_fd, SHUT_RDWR);
        close(g_ss.listen_fd);
        g_ss.listen_fd = -1;
    }

    if (g_ss.accept_thread) {
        pthread_join(g_ss.accept_thread, NULL);
        g_ss.accept_thread = 0;
    }

    pthread_mutex_lock(&g_ss.data_mutex);
    if (g_ss.jpg_buf) {
        free(g_ss.jpg_buf);
        g_ss.jpg_buf = NULL;
    }
    g_ss.jpg_size = 0;
    g_ss.jpg_cap = 0;
    g_ss.data_valid = 0;

    if (g_ss.debug_jpg_buf) {
        free(g_ss.debug_jpg_buf);
        g_ss.debug_jpg_buf = NULL;
    }
    g_ss.debug_jpg_size = 0;
    g_ss.debug_jpg_cap = 0;
    g_ss.debug_valid = 0;
    g_ss.debug_client_count = 0;
    pthread_mutex_unlock(&g_ss.data_mutex);

    pthread_mutex_destroy(&g_ss.data_mutex);
    log_info("DMS stream server 已关闭");
}
