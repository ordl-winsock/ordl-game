/*
 * ORDL UI — GIF image decoder from scratch
 * Pure C23, zero external dependencies.
 *
 * Supports GIF87a/GIF89a:
 *   - Global color table
 *   - LZW decompression (variable 2..12 bit codes)
 *   - Single frame output (first frame of animation)
 *   - Interlaced and non-interlaced images
 * Outputs RGBA8888.
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Bit reader for LZW                                                         */
/* -------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint32_t buf;
    int bits;
} gif_bits_t;

static void gif_bits_refill(gif_bits_t *b) {
    while (b->bits <= 24 && b->pos < b->len) {
        b->buf |= (uint32_t)b->data[b->pos] << b->bits;
        b->bits += 8;
        b->pos++;
    }
}

static uint32_t gif_bits_read(gif_bits_t *b, int n) {
    gif_bits_refill(b);
    uint32_t v = b->buf & ((1u << n) - 1);
    b->buf >>= n;
    b->bits -= n;
    return v;
}

/* -------------------------------------------------------------------------- */
/* LZW dictionary                                                             */
/* -------------------------------------------------------------------------- */

#define GIF_LZW_MAX_CODES 4096
#define GIF_LZW_MAX_STR   4096

typedef struct {
    uint16_t prefix[GIF_LZW_MAX_CODES];
    uint8_t  suffix[GIF_LZW_MAX_CODES];
    uint16_t len[GIF_LZW_MAX_CODES];
    int next_code;
    int code_size;
    int min_code_size;
    uint16_t clear_code;
    uint16_t end_code;
} gif_lzw_dict_t;

static void gif_lzw_init(gif_lzw_dict_t *d, int min_code_size) {
    d->min_code_size = min_code_size;
    d->clear_code = (uint16_t)(1 << min_code_size);
    d->end_code = (uint16_t)(d->clear_code + 1);
    d->next_code = d->end_code + 1;
    d->code_size = min_code_size + 1;
    for (int i = 0; i < d->clear_code; i++) {
        d->prefix[i] = 0xFFFF;
        d->suffix[i] = (uint8_t)i;
        d->len[i] = 1;
    }
}

static void gif_lzw_clear(gif_lzw_dict_t *d) {
    d->next_code = d->end_code + 1;
    d->code_size = d->min_code_size + 1;
}

static bool gif_lzw_add(gif_lzw_dict_t *d, uint16_t prefix, uint8_t suffix) {
    if (d->next_code >= GIF_LZW_MAX_CODES) return false;
    d->prefix[d->next_code] = prefix;
    d->suffix[d->next_code] = suffix;
    d->len[d->next_code] = (prefix == 0xFFFF) ? 1 : d->len[prefix] + 1;
    d->next_code++;
    if (d->next_code > (1 << d->code_size) && d->code_size < 12) {
        d->code_size++;
    }
    return true;
}

static void gif_lzw_output(uint16_t code, const gif_lzw_dict_t *d,
                           uint8_t *out, size_t *out_pos, size_t out_cap) {
    if (code >= d->next_code) return;
    /* Build string in reverse using a stack */
    uint8_t stack[GIF_LZW_MAX_STR];
    int sp = 0;
    uint16_t c = code;
    while (c != 0xFFFF && sp < GIF_LZW_MAX_STR) {
        stack[sp++] = d->suffix[c];
        c = d->prefix[c];
    }
    while (sp > 0 && *out_pos < out_cap) {
        out[(*out_pos)++] = stack[--sp];
    }
}

/* -------------------------------------------------------------------------- */
/* LZW decompress                                                             */
/* -------------------------------------------------------------------------- */

static uint8_t *gif_lzw_decompress(const uint8_t *data, size_t len,
                                   int min_code_size, size_t *out_len) {
    gif_bits_t bits = { data, len, 0, 0, 0 };
    gif_lzw_dict_t dict;
    gif_lzw_init(&dict, min_code_size);

    size_t cap = 65536;
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) return NULL;
    size_t pos = 0;

    uint16_t old_code = 0xFFFF;
    bool first_code = true;

    for (;;) {
        uint16_t code = (uint16_t)gif_bits_read(&bits, dict.code_size);
        if (code == dict.clear_code) {
            gif_lzw_clear(&dict);
            old_code = 0xFFFF;
            first_code = true;
            continue;
        }
        if (code == dict.end_code) {
            break;
        }
        if (first_code) {
            gif_lzw_output(code, &dict, out, &pos, cap);
            old_code = code;
            first_code = false;
            continue;
        }

        if (code < dict.next_code) {
            gif_lzw_output(code, &dict, out, &pos, cap);
            /* Add old_code + first_char_of_code */
            uint8_t first_char = 0;
            uint16_t t = code;
            while (t != 0xFFFF) {
                first_char = dict.suffix[t];
                t = dict.prefix[t];
            }
            gif_lzw_add(&dict, old_code, first_char);
            old_code = code;
        } else {
            /* Code not in dict yet: output old_code + first_char_of_old_code */
            uint8_t first_char = 0;
            uint16_t t = old_code;
            while (t != 0xFFFF) {
                first_char = dict.suffix[t];
                t = dict.prefix[t];
            }
            gif_lzw_output(old_code, &dict, out, &pos, cap);
            if (pos < cap) out[pos++] = first_char;
            gif_lzw_add(&dict, old_code, first_char);
            old_code = code;
        }

        if (pos >= cap) {
            size_t nc = cap * 2;
            uint8_t *nb = (uint8_t *)realloc(out, nc);
            if (!nb) { free(out); return NULL; }
            out = nb;
            cap = nc;
        }
    }

    *out_len = pos;
    return out;
}

/* -------------------------------------------------------------------------- */
/* GIF parser                                                                 */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t r, g, b;
} gif_color_t;

static bool gif_read_subblocks(const uint8_t *data, size_t len, size_t *pos,
                               uint8_t **out, size_t *out_len) {
    size_t cap = 4096;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return false;
    size_t total = 0;

    while (*pos < len) {
        uint8_t block_size = data[(*pos)++];
        if (block_size == 0) break;
        if (*pos + block_size > len) { free(buf); return false; }
        if (total + block_size > cap) {
            while (cap < total + block_size) cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(buf, cap);
            if (!nb) { free(buf); return false; }
            buf = nb;
        }
        memcpy(buf + total, data + *pos, block_size);
        total += block_size;
        *pos += block_size;
    }

    *out = buf;
    *out_len = total;
    return true;
}

/* Interlace row order: pass 0 = every 8th starting 0, pass 1 = every 8th starting 4,
   pass 2 = every 4th starting 2, pass 3 = every 2nd starting 1 */
static const int gif_interlace_start[4] = {0, 4, 2, 1};
static const int gif_interlace_step[4]  = {8, 8, 4, 2};

ui_image_t *ui_image_load_gif(const uint8_t *data, size_t len) {
    if (!data || len < 13) return NULL;
    if (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0) return NULL;

    uint16_t width  = (uint16_t)(data[6] | (data[7] << 8));
    uint16_t height = (uint16_t)(data[8] | (data[9] << 8));
    uint8_t packed  = data[10];
    /* uint8_t bgcolor = data[11]; */
    /* uint8_t aspect  = data[12]; */

    bool has_gct = (packed & 0x80) != 0;
    int gct_size = 1 << ((packed & 0x07) + 1);
    gif_color_t gct[256] = {0};
    size_t pos = 13;

    if (has_gct) {
        if (pos + gct_size * 3 > len) return NULL;
        for (int i = 0; i < gct_size; i++) {
            gct[i].r = data[pos++];
            gct[i].g = data[pos++];
            gct[i].b = data[pos++];
        }
    }

    /* Allocate output image */
    ui_image_t *img = (ui_image_t *)calloc(1, sizeof(ui_image_t));
    if (!img) return NULL;
    img->w = (int)width;
    img->h = (int)height;
    img->pixels = (uint32_t *)calloc((size_t)width * (size_t)height, sizeof(uint32_t));
    if (!img->pixels) { free(img); return NULL; }

    bool done = false;
    int transparent_index = -1;
    gif_color_t *active_ct = gct;
    int active_ct_size = gct_size;

    while (!done && pos < len) {
        uint8_t sentinel = data[pos++];
        if (sentinel == 0x3B) { /* Trailer */
            break;
        }

        if (sentinel == 0x21) { /* Extension */
            if (pos >= len) { done = true; break; }
            uint8_t ext_label = data[pos++];
            if (ext_label == 0xF9) { /* Graphic Control Extension */
                if (pos + 6 > len) { done = true; break; }
                uint8_t block_size = data[pos++];
                if (block_size == 4) {
                    uint8_t gcp = data[pos++];
                    /* uint16_t delay = (uint16_t)(data[pos] | (data[pos+1] << 8)); */
                    pos += 2;
                    uint8_t ti = data[pos++];
                    if (gcp & 0x01) transparent_index = ti;
                    else transparent_index = -1;
                }
                /* Skip remaining subblocks */
                while (pos < len && data[pos] != 0) {
                    uint8_t bs = data[pos++];
                    pos += bs;
                }
                if (pos < len) pos++; /* skip 0x00 */
            } else {
                /* Skip unknown extension */
                while (pos < len && data[pos] != 0) {
                    uint8_t bs = data[pos++];
                    pos += bs;
                }
                if (pos < len) pos++; /* skip 0x00 */
            }
            continue;
        }

        if (sentinel == 0x2C) { /* Image Descriptor */
            if (pos + 9 > len) { done = true; break; }
            uint16_t left   = (uint16_t)(data[pos] | (data[pos+1] << 8)); pos += 2;
            uint16_t top    = (uint16_t)(data[pos] | (data[pos+1] << 8)); pos += 2;
            uint16_t iwidth = (uint16_t)(data[pos] | (data[pos+1] << 8)); pos += 2;
            uint16_t iheight= (uint16_t)(data[pos] | (data[pos+1] << 8)); pos += 2;
            uint8_t ipacked = data[pos++];
            bool has_lct = (ipacked & 0x80) != 0;
            bool interlaced = (ipacked & 0x40) != 0;
            int lct_size = 1 << ((ipacked & 0x07) + 1);

            if (has_lct) {
                if (pos + lct_size * 3 > len) { done = true; break; }
                active_ct = (gif_color_t *)malloc(sizeof(gif_color_t) * 256);
                if (!active_ct) { done = true; break; }
                for (int i = 0; i < lct_size; i++) {
                    active_ct[i].r = data[pos++];
                    active_ct[i].g = data[pos++];
                    active_ct[i].b = data[pos++];
                }
                active_ct_size = lct_size;
            } else {
                active_ct = gct;
                active_ct_size = gct_size;
            }

            if (pos >= len) { done = true; break; }
            uint8_t min_code_size = data[pos++];

            uint8_t *compressed = NULL;
            size_t compressed_len = 0;
            if (!gif_read_subblocks(data, len, &pos, &compressed, &compressed_len)) {
                done = true; break;
            }

            size_t pixel_count = 0;
            uint8_t *pixels = gif_lzw_decompress(compressed, compressed_len,
                                                  min_code_size, &pixel_count);
            free(compressed);
            if (!pixels) { done = true; break; }

            /* Copy pixels to image, handling interlace and transparency */
            size_t p = 0;
            if (interlaced) {
                for (int pass = 0; pass < 4; pass++) {
                    for (int y = gif_interlace_start[pass];
                         y < (int)iheight && p < pixel_count;
                         y += gif_interlace_step[pass]) {
                        int dy = top + y;
                        if (dy >= (int)height) continue;
                        for (int x = 0; x < (int)iwidth && p < pixel_count; x++) {
                            int dx = left + x;
                            if (dx < 0 || dx >= (int)width) { p++; continue; }
                            uint8_t idx = pixels[p++];
                            if (idx < active_ct_size && idx != transparent_index) {
                                gif_color_t c = active_ct[idx];
                                img->pixels[dy * width + dx] =
                                    0xFF000000u | ((uint32_t)c.b << 16) |
                                    ((uint32_t)c.g << 8) | c.r;
                            }
                        }
                    }
                }
            } else {
                for (int y = 0; y < (int)iheight && p < pixel_count; y++) {
                    int dy = top + y;
                    if (dy < 0 || dy >= (int)height) {
                        p += iwidth;
                        continue;
                    }
                    for (int x = 0; x < (int)iwidth && p < pixel_count; x++) {
                        int dx = left + x;
                        if (dx < 0 || dx >= (int)width) { p++; continue; }
                        uint8_t idx = pixels[p++];
                        if (idx < active_ct_size && idx != transparent_index) {
                            gif_color_t c = active_ct[idx];
                            img->pixels[dy * width + dx] =
                                0xFF000000u | ((uint32_t)c.b << 16) |
                                ((uint32_t)c.g << 8) | c.r;
                        }
                    }
                }
            }

            free(pixels);
            if (active_ct != gct) { free(active_ct); active_ct = gct; }

            /* For now, only decode the first frame */
            done = true;
        }
    }

    if (active_ct != gct) { free(active_ct); active_ct = gct; }

    return img;
}
