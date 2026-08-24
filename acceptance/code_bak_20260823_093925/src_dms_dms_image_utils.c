#include "dms_image_utils.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#include "stb_image.h"

bool dms_decode_jpeg(const uint8_t *jpeg_buf, size_t jpeg_size, dms_image_t *out_img)
{
    if (!jpeg_buf || jpeg_size == 0 || !out_img) return false;

    memset(out_img, 0, sizeof(*out_img));

    int w = 0, h = 0, c = 0;
    uint8_t *data = stbi_load_from_memory(jpeg_buf, (int)jpeg_size, &w, &h, &c, 3);
    if (!data) return false;

    out_img->width = w;
    out_img->height = h;
    out_img->channels = 3;
    out_img->data = data;
    return true;
}

void dms_free_image(dms_image_t *img)
{
    if (img && img->data) {
        stbi_image_free(img->data);
        img->data = NULL;
        img->width = img->height = img->channels = 0;
    }
}

bool dms_rgb_to_gray(dms_image_t *img)
{
    if (!img || !img->data || img->channels != 3) return false;

    int pixels = img->width * img->height;
    uint8_t *src = img->data;
    uint8_t *dst = img->data;

    for (int i = 0; i < pixels; i++) {
        uint8_t r = src[i * 3 + 0];
        uint8_t g = src[i * 3 + 1];
        uint8_t b = src[i * 3 + 2];
        dst[i] = (uint8_t)((76 * r + 150 * g + 29 * b) >> 8);
    }

    img->channels = 1;
    return true;
}

bool dms_resize_nn(const dms_image_t *src, int dst_w, int dst_h, dms_image_t *out_img)
{
    if (!src || !src->data || !out_img || dst_w <= 0 || dst_h <= 0) return false;

    memset(out_img, 0, sizeof(*out_img));

    int dst_size = dst_w * dst_h * src->channels;
    uint8_t *dst = (uint8_t *)malloc((size_t)dst_size);
    if (!dst) return false;

    float x_ratio = (float)src->width / (float)dst_w;
    float y_ratio = (float)src->height / (float)dst_h;

    for (int y = 0; y < dst_h; y++) {
        int src_y = (int)(y * y_ratio);
        if (src_y >= src->height) src_y = src->height - 1;
        for (int x = 0; x < dst_w; x++) {
            int src_x = (int)(x * x_ratio);
            if (src_x >= src->width) src_x = src->width - 1;
            for (int c = 0; c < src->channels; c++) {
                dst[(y * dst_w + x) * src->channels + c] =
                    src->data[(src_y * src->width + src_x) * src->channels + c];
            }
        }
    }

    out_img->width = dst_w;
    out_img->height = dst_h;
    out_img->channels = src->channels;
    out_img->data = dst;
    return true;
}

bool dms_crop_and_resize(const dms_image_t *src,
                         int crop_x, int crop_y, int crop_w, int crop_h,
                         int dst_w, int dst_h, dms_image_t *out_img)
{
    if (!src || !src->data || !out_img || crop_w <= 0 || crop_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return false;

    memset(out_img, 0, sizeof(*out_img));

    /* Clamp crop region to source bounds. */
    if (crop_x < 0) crop_x = 0;
    if (crop_y < 0) crop_y = 0;
    if (crop_x + crop_w > src->width) crop_w = src->width - crop_x;
    if (crop_y + crop_h > src->height) crop_h = src->height - crop_y;
    if (crop_w <= 0 || crop_h <= 0) return false;

    int dst_size = dst_w * dst_h * src->channels;
    uint8_t *dst = (uint8_t *)malloc((size_t)dst_size);
    if (!dst) return false;

    float x_ratio = (float)crop_w / (float)dst_w;
    float y_ratio = (float)crop_h / (float)dst_h;

    for (int y = 0; y < dst_h; y++) {
        int src_y = crop_y + (int)(y * y_ratio);
        if (src_y >= src->height) src_y = src->height - 1;
        for (int x = 0; x < dst_w; x++) {
            int src_x = crop_x + (int)(x * x_ratio);
            if (src_x >= src->width) src_x = src->width - 1;
            for (int c = 0; c < src->channels; c++) {
                dst[(y * dst_w + x) * src->channels + c] =
                    src->data[(src_y * src->width + src_x) * src->channels + c];
            }
        }
    }

    out_img->width = dst_w;
    out_img->height = dst_h;
    out_img->channels = src->channels;
    out_img->data = dst;
    return true;
}
