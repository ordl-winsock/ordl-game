/*
 * ORDL UI — BMP image decoder from scratch
 * Pure C23, zero external dependencies.
 *
 * Supports:
 *   - BITMAPINFOHEADER (40 bytes)
 *   - 24-bit and 32-bit RGB (uncompressed)
 *   - Bottom-up and top-down row order
 *   - Outputs RGBA8888
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

ui_image_t *ui_image_load_bmp(const uint8_t *data, size_t len) {
    if (!data || len < 54) return NULL;

    /* BMP file header (14 bytes) */
    if (data[0] != 'B' || data[1] != 'M') return NULL;
    uint32_t off_bits = (uint32_t)data[10] | ((uint32_t)data[11] << 8)
                      | ((uint32_t)data[12] << 16) | ((uint32_t)data[13] << 24);

    /* DIB header size at offset 14 */
    uint32_t dib_size = (uint32_t)data[14] | ((uint32_t)data[15] << 8)
                      | ((uint32_t)data[16] << 16) | ((uint32_t)data[17] << 24);
    if (dib_size < 40) return NULL;

    /* BITMAPINFOHEADER fields */
    int32_t width  = (int32_t)((uint32_t)data[18] | ((uint32_t)data[19] << 8)
                    | ((uint32_t)data[20] << 16) | ((uint32_t)data[21] << 24));
    int32_t height = (int32_t)((uint32_t)data[22] | ((uint32_t)data[23] << 8)
                    | ((uint32_t)data[24] << 16) | ((uint32_t)data[25] << 24));
    uint16_t planes = (uint16_t)(data[26] | ((uint16_t)data[27] << 8));
    uint16_t bit_count = (uint16_t)(data[28] | ((uint16_t)data[29] << 8));
    uint32_t compression = (uint32_t)data[30] | ((uint32_t)data[31] << 8)
                         | ((uint32_t)data[32] << 16) | ((uint32_t)data[33] << 24);

    if (planes != 1) return NULL;
    if (compression != 0) return NULL; /* No RLE */
    if (bit_count != 24 && bit_count != 32) return NULL;

    int w = (int)width;
    if (height == INT32_MIN) return NULL;
    int h = height < 0 ? -height : height;
    bool top_down = height < 0;
    if (w <= 0 || h <= 0) return NULL;

    ui_image_t *img = calloc(1, sizeof(ui_image_t));
    if (!img) return NULL;
    img->w = w;
    img->h = h;
    img->pixels = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!img->pixels) { free(img); return NULL; }

    int bytes_per_pixel = bit_count / 8;
    if (w > INT_MAX / bytes_per_pixel) { ui_image_free(img); return NULL; }
    int row_bytes = ((w * bytes_per_pixel + 3) / 4) * 4;
    size_t src_needed = (size_t)row_bytes * (size_t)h;
    if (off_bits + src_needed > len) { ui_image_free(img); return NULL; }

    for (int y = 0; y < h; y++) {
        int dst_y = top_down ? y : (h - 1 - y);
        const uint8_t *row = data + off_bits + (size_t)y * (size_t)row_bytes;
        uint32_t *dst = img->pixels + (size_t)dst_y * (size_t)w;
        for (int x = 0; x < w; x++) {
            uint8_t b = row[x * bytes_per_pixel + 0];
            uint8_t g = row[x * bytes_per_pixel + 1];
            uint8_t r = row[x * bytes_per_pixel + 2];
            uint8_t a = (bytes_per_pixel == 4) ? row[x * bytes_per_pixel + 3] : 255;
            dst[x] = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
        }
    }
    return img;
}

void ui_image_free(ui_image_t *img) {
    if (!img) return;
    free(img->pixels);
    free(img);
}
