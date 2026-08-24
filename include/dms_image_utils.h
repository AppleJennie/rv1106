#ifndef DMS_IMAGE_UTILS_H
#define DMS_IMAGE_UTILS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int width;
    int height;
    int channels; /* 1 or 3 */
    uint8_t *data;
} dms_image_t;

/* Decode JPEG buffer to RGB (channels=3). Returns true on success. */
bool dms_decode_jpeg(const uint8_t *jpeg_buf, size_t jpeg_size, dms_image_t *out_img);

/* Free image data allocated by dms_decode_jpeg. */
void dms_free_image(dms_image_t *img);

/* Convert RGB image to grayscale in-place (channels becomes 1). */
bool dms_rgb_to_gray(dms_image_t *img);

/* Resize image to dst_w x dst_h using nearest-neighbor. out_img must be freed. */
bool dms_resize_nn(const dms_image_t *src, int dst_w, int dst_h, dms_image_t *out_img);

/* Crop a region from src and resize to dst_w x dst_h. out_img must be freed. */
bool dms_crop_and_resize(const dms_image_t *src,
                         int crop_x, int crop_y, int crop_w, int crop_h,
                         int dst_w, int dst_h, dms_image_t *out_img);

#endif /* DMS_IMAGE_UTILS_H */
