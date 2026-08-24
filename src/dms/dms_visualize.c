#include "dms_visualize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "sys_logger.h"

/* stb_image implementation already in dms_image_utils.c */
#define STBI_ONLY_JPEG
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define FONT_W 8
#define FONT_H 8

/* 可视化阶段耗时统计 */
static dms_visualize_timing_t g_last_vtiming = {0};
static dms_visualize_timing_t g_avg_vtiming = {0};
static uint64_t g_vtiming_count = 0;

static uint64_t get_mono_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

/*
 * Public domain 8x8 bitmap font, ASCII 0..127.
 * Each character is 8 bytes, one byte per row, MSB leftmost.
 */
static const uint8_t g_font8x8[128][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* # */
    {0x0C,0x3E,0x03,0x1E,0x60,0x3F,0x0C,0x00}, /* $ */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* % */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* & */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* ( */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* , */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* . */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* / */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 0 */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 1 */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 2 */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 3 */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 4 */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 5 */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 6 */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 7 */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 8 */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 9 */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* : */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* ; */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* < */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* = */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* > */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* ? */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* @ */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* A */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* B */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* C */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* D */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, /* E */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, /* F */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* G */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* H */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* I */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* J */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* K */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, /* L */
    {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, /* M */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* N */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* O */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, /* P */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* Q */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* R */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* S */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* T */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* U */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* V */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* W */
    {0x63,0x63,0x36,0x1C,0x36,0x63,0x63,0x00}, /* X */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* Y */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* Z */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* [ */
    {0x01,0x03,0x06,0x0C,0x18,0x30,0x60,0x00}, /* \ */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* ] */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ */
    {0x0C,0x0C,0x06,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* a */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* b */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* c */
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, /* d */
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, /* e */
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, /* f */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* g */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* h */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* i */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* j */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* k */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* l */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* m */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* n */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* o */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* p */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* q */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* r */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* s */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* t */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* u */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* v */
    {0x00,0x00,0x63,0x6B,0x7F,0x36,0x36,0x00}, /* w */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* x */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* y */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* z */
    {0x18,0x0C,0x0C,0x06,0x0C,0x0C,0x18,0x00}, /* { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | */
    {0x06,0x0C,0x0C,0x18,0x0C,0x0C,0x06,0x00}, /* } */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}  /* DEL */
};

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

static inline void set_pixel(uint8_t *rgb, int w, int h, int x, int y, rgb_t c)
{
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    int idx = (y * w + x) * 3;
    rgb[idx + 0] = c.r;
    rgb[idx + 1] = c.g;
    rgb[idx + 2] = c.b;
}

static void draw_rect(uint8_t *rgb, int w, int h, int x, int y, int rw, int rh, rgb_t c, int thickness)
{
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x + rw > w) rw = w - x;
    if (y + rh > h) rh = h - y;
    if (rw <= 0 || rh <= 0) return;

    int x2 = x + rw - 1;
    int y2 = y + rh - 1;

    for (int t = 0; t < thickness; t++) {
        int xx1 = x + t;
        int yy1 = y + t;
        int xx2 = x2 - t;
        int yy2 = y2 - t;
        if (xx1 > xx2 || yy1 > yy2) break;
        for (int i = xx1; i <= xx2; i++) {
            set_pixel(rgb, w, h, i, yy1, c);
            set_pixel(rgb, w, h, i, yy2, c);
        }
        for (int j = yy1; j <= yy2; j++) {
            set_pixel(rgb, w, h, xx1, j, c);
            set_pixel(rgb, w, h, xx2, j, c);
        }
    }
}

static void draw_filled_circle(uint8_t *rgb, int w, int h, int cx, int cy, int r, rgb_t c)
{
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) {
                set_pixel(rgb, w, h, x, y, c);
            }
        }
    }
}

static void draw_char(uint8_t *rgb, int w, int h, int x, int y, char ch, rgb_t c)
{
    if ((unsigned char)ch > 127) return;
    const uint8_t *bm = g_font8x8[(unsigned char)ch];
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = bm[row];
        for (int col = 0; col < FONT_W; col++) {
            if (bits & (0x80 >> col)) {
                set_pixel(rgb, w, h, x + col, y + row, c);
            }
        }
    }
}

static void draw_string(uint8_t *rgb, int w, int h, int x, int y, const char *str, rgb_t c)
{
    while (*str) {
        draw_char(rgb, w, h, x, y, *str, c);
        x += FONT_W;
        str++;
    }
}

/* ======================================================================== */
/* stbi_write_jpg_to_func callback context                                  */
/* ======================================================================== */

typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
} jpg_buf_t;

static void jpg_write_func(void *context, void *data, int size)
{
    jpg_buf_t *buf = (jpg_buf_t *)context;
    if (size <= 0 || !buf) return;

    size_t need = buf->size + (size_t)size;
    if (need > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap * 2 : 4096;
        while (new_cap < need) new_cap *= 2;
        uint8_t *new_data = (uint8_t *)realloc(buf->data, new_cap);
        if (!new_data) return;
        buf->data = new_data;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->size, data, (size_t)size);
    buf->size += (size_t)size;
}

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

uint8_t *dms_visualize_generate(const uint8_t *jpg, size_t jpg_size,
                                 const dms_result_t *result,
                                 float ai_fps, size_t *out_size)
{
    if (!jpg || jpg_size == 0 || !out_size) return NULL;

    uint64_t t0 = get_mono_time_us();

    int w = 0, h = 0, channels = 0;
    uint8_t *rgb = stbi_load_from_memory(jpg, (int)jpg_size, &w, &h, &channels, 3);
    if (!rgb) {
        log_warn("visualize: JPEG 解码失败");
        return NULL;
    }

    uint64_t t_decode = get_mono_time_us();

    rgb_t green = {0, 255, 0};
    rgb_t yellow = {255, 255, 0};
    rgb_t red = {255, 0, 0};
    rgb_t white = {255, 255, 255};
    rgb_t magenta = {255, 0, 255};

    bool face_found = result && result->face_found;
    bool ai_error = !result || (strcmp(result->status, "AI_ERROR") == 0);

    /* Draw face bbox. */
    if (face_found && result) {
        draw_rect(rgb, w, h,
                  result->face_x, result->face_y,
                  result->face_w, result->face_h,
                  green, 3);

        /* Draw 5 RetinaFace landmarks: eyes, nose, mouth corners. */
        rgb_t cyan = {0, 255, 255};
        for (int k = 0; k < 5; k++) {
            int kx = (int)(result->face_kpt[k * 2 + 0] + 0.5f);
            int ky = (int)(result->face_kpt[k * 2 + 1] + 0.5f);
            draw_filled_circle(rgb, w, h, kx, ky, 4, cyan);
        }

#if DMS_ENABLE_LANDMARK_106
        /* 叠加 106 点：先全部画成洋红小点，用于肉眼校验贴合度。 */
        if (result->landmark_106.found) {
            for (int i = 0; i < DMS_LANDMARK_106_NUM; i++) {
                int px = (int)(result->landmark_106.points[i * 2 + 0] + 0.5f);
                int py = (int)(result->landmark_106.points[i * 2 + 1] + 0.5f);
#if DMS_DEBUG_LANDMARK_INDEX
                char idx[8];
                snprintf(idx, sizeof(idx), "%d", i);
                draw_filled_circle(rgb, w, h, px, py, 2, yellow);
                draw_string(rgb, w, h, px + 3, py - 4, idx, white);
#else
                draw_filled_circle(rgb, w, h, px, py, 2, magenta);
#endif
            }
        }
#endif
    }

    /* HUD background bar at top-left. */
    for (int y = 0; y < 5 + FONT_H * 9; y++) {
        for (int x = 0; x < 230; x++) {
            set_pixel(rgb, w, h, x, y, (rgb_t){0, 0, 0});
        }
    }

    rgb_t status_color = ai_error ? red : (face_found ? green : yellow);
    const char *status_str = result ? result->status : "INIT";

    char line[80];
    int line_y = 4;
    int line_x = 4;

    snprintf(line, sizeof(line), "STATE: %s", status_str);
    draw_string(rgb, w, h, line_x, line_y, line, status_color);
    line_y += FONT_H + 2;

    if (face_found && result) {
        snprintf(line, sizeof(line), "FACE: %.2f", result->face_score);
        draw_string(rgb, w, h, line_x, line_y, line, green);
    } else if (ai_error) {
        draw_string(rgb, w, h, line_x, line_y, "AI ERROR", red);
    } else {
        draw_string(rgb, w, h, line_x, line_y, "NO FACE", yellow);
    }
    line_y += FONT_H + 2;

    snprintf(line, sizeof(line), "EAR: %.3f L%.2f R%.2f",
             result ? result->ear : 0.0f,
             result ? result->left_ear : 0.0f,
             result ? result->right_ear : 0.0f);
    draw_string(rgb, w, h, line_x, line_y, line, white);
    line_y += FONT_H + 2;

    snprintf(line, sizeof(line), "EAR TH: %.3f", result ? result->ear_threshold : 0.0f);
    draw_string(rgb, w, h, line_x, line_y, line, white);
    line_y += FONT_H + 2;

    snprintf(line, sizeof(line), "MAR: %.3f TH %.2f",
             result ? result->mar : 0.0f,
             result ? result->mar_threshold : 0.0f);
    draw_string(rgb, w, h, line_x, line_y, line, white);
    line_y += FONT_H + 2;

    snprintf(line, sizeof(line), "HEAD: %.2f", result ? result->head_down_score : 0.0f);
    draw_string(rgb, w, h, line_x, line_y, line, white);
    line_y += FONT_H + 2;

#if DMS_ENABLE_LANDMARK_106
    if (face_found && result && !result->feature_calibrated) {
        draw_string(rgb, w, h, line_x, line_y, "CALIBRATING...", yellow);
    } else if (face_found && result) {
        draw_string(rgb, w, h, line_x, line_y, "CALIBRATED", green);
    } else {
        draw_string(rgb, w, h, line_x, line_y, "CAL: --", white);
    }
    line_y += FONT_H + 2;

    if (face_found && result && result->landmark_106.found) {
        snprintf(line, sizeof(line), "LM106: OK %.1fms", result->landmark_106.total_us / 1000.0f);
        draw_string(rgb, w, h, line_x, line_y, line, magenta);
    } else {
        draw_string(rgb, w, h, line_x, line_y, "LM106: --", white);
    }
    line_y += FONT_H + 2;
#endif

    snprintf(line, sizeof(line), "AI FPS: %.1f", ai_fps);
    draw_string(rgb, w, h, line_x, line_y, line, white);
    line_y += FONT_H + 2;

    /* Optional label above bbox. */
    if (face_found && result) {
        char label[32];
        snprintf(label, sizeof(label), "FACE %.2f", result->face_score);
        int label_x = result->face_x;
        int label_y = result->face_y - FONT_H - 2;
        if (label_y < 0) label_y = result->face_y + result->face_h + 2;
        /* small black background */
        int label_w = (int)strlen(label) * FONT_W;
        for (int yy = label_y - 1; yy < label_y + FONT_H + 1; yy++) {
            for (int xx = label_x - 1; xx < label_x + label_w + 1; xx++) {
                set_pixel(rgb, w, h, xx, yy, (rgb_t){0, 0, 0});
            }
        }
        draw_string(rgb, w, h, label_x, label_y, label, green);
    }

    uint64_t t_draw = get_mono_time_us();

    jpg_buf_t out = {0};
    int ok = stbi_write_jpg_to_func(jpg_write_func, &out, w, h, 3, rgb, 85);
    stbi_image_free(rgb);
    uint64_t t_encode = get_mono_time_us();

    /* 更新可视化阶段耗时统计 */
    g_last_vtiming.jpeg_decode_us = t_decode - t0;
    g_last_vtiming.draw_us        = t_draw - t_decode;
    g_last_vtiming.jpeg_encode_us = t_encode - t_draw;
    g_last_vtiming.total_us       = t_encode - t0;

    g_avg_vtiming.jpeg_decode_us = (g_avg_vtiming.jpeg_decode_us * g_vtiming_count + g_last_vtiming.jpeg_decode_us) / (g_vtiming_count + 1);
    g_avg_vtiming.draw_us        = (g_avg_vtiming.draw_us        * g_vtiming_count + g_last_vtiming.draw_us)        / (g_vtiming_count + 1);
    g_avg_vtiming.jpeg_encode_us = (g_avg_vtiming.jpeg_encode_us * g_vtiming_count + g_last_vtiming.jpeg_encode_us) / (g_vtiming_count + 1);
    g_avg_vtiming.total_us       = (g_avg_vtiming.total_us       * g_vtiming_count + g_last_vtiming.total_us)       / (g_vtiming_count + 1);
    g_vtiming_count++;

    if (!ok || out.size == 0) {
        free(out.data);
        log_warn("visualize: JPEG 编码失败");
        return NULL;
    }

    *out_size = out.size;
    return out.data;
}

const dms_visualize_timing_t *dms_visualize_last_timing(void)
{
    return &g_last_vtiming;
}

const dms_visualize_timing_t *dms_visualize_avg_timing(void)
{
    if (g_vtiming_count == 0) return NULL;
    return &g_avg_vtiming;
}

void dms_visualize_reset_timing_stats(void)
{
    memset(&g_last_vtiming, 0, sizeof(g_last_vtiming));
    memset(&g_avg_vtiming, 0, sizeof(g_avg_vtiming));
    g_vtiming_count = 0;
}
