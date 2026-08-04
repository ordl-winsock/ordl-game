/*
 * ORDL UI — JPEG baseline decoder from scratch
 * Pure C23, zero external dependencies.
 *
 * Supports:
 *   - Baseline JPEG (SOF0): 8-bit, 1-3 components, Huffman coding
 *   - 4:4:4, 4:2:0, 4:2:2 chroma subsampling
 *   - Restart markers (RST0-RST7) with DRI
 *   - Outputs RGBA8888
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* -------------------------------------------------------------------------- */
/* Bitstream (clean entropy data, no markers)                                 */
/* -------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint32_t buf;
    int nbits;
} jpeg_bits_t;

static void bits_fill(jpeg_bits_t *b) {
    while (b->nbits <= 24 && b->pos < b->len) {
        b->buf = (b->buf << 8) | (uint32_t)b->data[b->pos++];
        b->nbits += 8;
    }
}

static uint32_t bits_peek(jpeg_bits_t *b, int n) {
    bits_fill(b);
    if (b->nbits < n) return 0;
    return (b->buf >> (b->nbits - n)) & ((1u << n) - 1);
}

static void bits_consume(jpeg_bits_t *b, int n) {
    b->nbits -= n;
}

static uint32_t bits_read(jpeg_bits_t *b, int n) {
    uint32_t v = bits_peek(b, n);
    bits_consume(b, n);
    return v;
}

/* -------------------------------------------------------------------------- */
/* Huffman decoder (16-bit direct lookup, MSB-first)                          */
/* -------------------------------------------------------------------------- */

#define HUFF_LUT_BITS 16
#define HUFF_LUT_SIZE (1u << HUFF_LUT_BITS)

typedef struct {
    uint16_t table[HUFF_LUT_SIZE]; /* (symbol << 5) | len; len=0 = invalid */
} jpeg_huff_table_t;

static void jpeg_huff_build(jpeg_huff_table_t *t,
                            const uint8_t *bits,
                            const uint8_t *values, int num_values) {
    memset(t->table, 0, sizeof(t->table));

    int bl_count[17] = {0};
    for (int i = 0; i < 16; i++) {
        if (bits[i] > 0) bl_count[i + 1] = (int)bits[i];
    }

    int next_code[17] = {0};
    int code = 0;
    bl_count[0] = 0;
    for (int len = 1; len <= 16; len++) {
        code = (code + bl_count[len - 1]) << 1;
        next_code[len] = code;
    }

    int vi = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bl_count[len]; i++) {
            if (vi >= num_values) return;
            int c = next_code[len]++;
            uint16_t entry = (uint16_t)((values[vi] << 5) | len);
            int prefix = c << (16 - len);
            int count = 1 << (16 - len);
            if (prefix < 0 || prefix + count > (int)HUFF_LUT_SIZE) return; /* bounds check */
            for (int j = 0; j < count; j++) {
                t->table[prefix + j] = entry;
            }
            vi++;
        }
    }
}

static int jpeg_huff_decode(const jpeg_huff_table_t *t, jpeg_bits_t *b) {
    bits_fill(b);
    if (b->nbits == 0) return -1;
    uint32_t v = bits_peek(b, HUFF_LUT_BITS);
    uint16_t entry = t->table[v];
    int len = entry & 31;
    if (len == 0 || len > b->nbits) return -1;
    bits_consume(b, len);
    return (int)(entry >> 5);
}

/* -------------------------------------------------------------------------- */
/* IDCT (floating-point, precomputed cosine table)                            */
/* -------------------------------------------------------------------------- */

static double idct_cos[8][8];
static bool idct_init_done = false;

static void idct_init(void) {
    if (idct_init_done) return;
    const double scale = 3.14159265358979323846 / 16.0;
    for (int u = 0; u < 8; u++) {
        for (int x = 0; x < 8; x++) {
            idct_cos[u][x] = cos((2.0 * x + 1.0) * u * scale);
        }
    }
    idct_init_done = true;
}

static const uint8_t zigzag[64] = {
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63
};

static void idct_block(const int16_t *in, uint8_t *out, int stride) {
    idct_init();
    double tmp[8][8];
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            double sum = 0.0;
            for (int v = 0; v < 8; v++) {
                double cv = (v == 0) ? (1.0 / 1.4142135623730951) : 1.0;
                for (int u = 0; u < 8; u++) {
                    double cu = (u == 0) ? (1.0 / 1.4142135623730951) : 1.0;
                    sum += (double)in[v * 8 + u] * cu * cv *
                           idct_cos[u][x] * idct_cos[v][y];
                }
            }
            tmp[y][x] = sum / 4.0;
        }
    }
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int v = (int)(tmp[y][x] + 128.5);
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            out[y * stride + x] = (uint8_t)v;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* JPEG parser / decoder state                                                */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t bits[16];
    uint8_t values[256];
    int num_values;
} jpeg_huff_spec_t;

typedef struct {
    int id;
    int h, v;          /* sampling factors */
    int quant_id;
    int dc_huff_id;
    int ac_huff_id;
    int width, height; /* in pixels (padded to block boundary) */
    uint8_t *pixels;   /* decoded component plane */
} jpeg_comp_t;

typedef struct {
    uint8_t data[64];
} jpeg_quant_table_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;

    int width, height;
    int bits;
    int ncomp;
    jpeg_comp_t comp[4];
    jpeg_quant_table_t quant[4];
    jpeg_huff_spec_t dc_huff[4];
    jpeg_huff_spec_t ac_huff[4];

    int restart_interval; /* MCUs between RST markers, 0 = none */
    bool got_sof;
    bool got_sos;

    /* Scan state */
    int scan_ncomp;
    int scan_comp_id[4];
    int scan_dc_huff[4];
    int scan_ac_huff[4];

    /* Decoded block buffer */
    int16_t block[64];
    int dc_pred[4];
} jpeg_ctx_t;

static uint16_t read_be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

/* -------------------------------------------------------------------------- */
/* Marker parsing                                                             */
/* -------------------------------------------------------------------------- */

static bool jpeg_parse_dqt(jpeg_ctx_t *c) {
    if (c->pos + 2 > c->len) return false;
    uint16_t len = read_be16(c->data + c->pos);
    if (c->pos + len > c->len) return false;
    size_t end = c->pos + len;
    c->pos += 2;
    while (c->pos + 65 <= end) {
        uint8_t pqtq = c->data[c->pos++];
        int precision = (pqtq >> 4) & 0x0F;
        int id = pqtq & 0x0F;
        if (id >= 4) return false;
        if (precision == 0) {
            memcpy(c->quant[id].data, c->data + c->pos, 64);
            c->pos += 64;
        } else {
            /* 16-bit precision not allowed in baseline */
            return false;
        }
    }
    return true;
}

static bool jpeg_parse_sof0(jpeg_ctx_t *c) {
    if (c->pos + 2 > c->len) return false;
    uint16_t len = read_be16(c->data + c->pos);
    if (c->pos + len > c->len) return false;
    size_t end = c->pos + len;
    c->pos += 2;
    if (c->pos + 6 > end) return false;
    c->bits = c->data[c->pos++];
    c->height = (int)read_be16(c->data + c->pos); c->pos += 2;
    c->width = (int)read_be16(c->data + c->pos); c->pos += 2;
    c->ncomp = c->data[c->pos++];
    if (c->bits != 8) return false;
    if (c->ncomp != 1 && c->ncomp != 3) return false;
    if (c->pos + (size_t)c->ncomp * 3 > end) return false;
    int hmax = 0, vmax = 0;
    for (int i = 0; i < c->ncomp; i++) {
        jpeg_comp_t *comp = &c->comp[i];
        comp->id = c->data[c->pos++];
        comp->h = (c->data[c->pos] >> 4) & 0x0F;
        comp->v = c->data[c->pos] & 0x0F;
        c->pos++;
        comp->quant_id = c->data[c->pos++];
        if (comp->h < 1 || comp->h > 4) return false;
        if (comp->v < 1 || comp->v > 4) return false;
        if (comp->h > hmax) hmax = comp->h;
        if (comp->v > vmax) vmax = comp->v;
    }
    /* Compute padded dimensions per component */
    int mcu_w = (c->width + hmax * 8 - 1) / (hmax * 8);
    int mcu_h = (c->height + vmax * 8 - 1) / (vmax * 8);
    for (int i = 0; i < c->ncomp; i++) {
        jpeg_comp_t *comp = &c->comp[i];
        comp->width = mcu_w * comp->h * 8;
        comp->height = mcu_h * comp->v * 8;
        comp->pixels = calloc((size_t)comp->width * (size_t)comp->height, 1);
        if (!comp->pixels) return false;
    }
    c->got_sof = true;
    return true;
}

static bool jpeg_parse_dht(jpeg_ctx_t *c) {
    if (c->pos + 2 > c->len) return false;
    uint16_t len = read_be16(c->data + c->pos);
    if (c->pos + len > c->len) return false;
    size_t end = c->pos + len;
    c->pos += 2;
    while (c->pos < end) {
        if (c->pos >= end) break;
        uint8_t tcth = c->data[c->pos++];
        int tc = (tcth >> 4) & 0x0F;
        int th = tcth & 0x0F;
        if (th >= 4) return false;
        if (c->pos + 16 > end) return false;
        uint8_t bits[16];
        memcpy(bits, c->data + c->pos, 16);
        c->pos += 16;
        int num_values = 0;
        for (int i = 0; i < 16; i++) num_values += bits[i];
        if (c->pos + (size_t)num_values > end) return false;
        jpeg_huff_spec_t *spec = (tc == 0) ? &c->dc_huff[th] : &c->ac_huff[th];
        memcpy(spec->bits, bits, 16);
        memcpy(spec->values, c->data + c->pos, (size_t)num_values);
        spec->num_values = num_values;
        c->pos += (size_t)num_values;
    }
    return true;
}

static bool jpeg_parse_dri(jpeg_ctx_t *c) {
    if (c->pos + 4 > c->len) return false;
    uint16_t len = read_be16(c->data + c->pos);
    if (len != 4) return false;
    c->restart_interval = (int)read_be16(c->data + c->pos + 2);
    c->pos += 4;
    return true;
}

static bool jpeg_parse_sos(jpeg_ctx_t *c) {
    if (c->pos + 2 > c->len) return false;
    uint16_t len = read_be16(c->data + c->pos);
    if (c->pos + len > c->len) return false;
    size_t end = c->pos + len;
    c->pos += 2;
    if (c->pos >= end) return false;
    c->scan_ncomp = c->data[c->pos++];
    if (c->scan_ncomp < 1 || c->scan_ncomp > c->ncomp) return false;
    if (c->pos + (size_t)c->scan_ncomp * 2 > end) return false;
    for (int i = 0; i < c->scan_ncomp; i++) {
        c->scan_comp_id[i] = c->data[c->pos++];
        c->scan_dc_huff[i] = (c->data[c->pos] >> 4) & 0x0F;
        c->scan_ac_huff[i] = c->data[c->pos] & 0x0F;
        c->pos++;
    }
    /* Skip Ss, Se, Ah/Al */
    if (c->pos + 3 > end) return false;
    c->pos += 3;
    c->got_sos = true;
    return true;
}

/* -------------------------------------------------------------------------- */
/* Block decoding                                                             */
/* -------------------------------------------------------------------------- */

static int jpeg_extend(int v, int t) {
    int vt = 1 << (t - 1);
    return (v < vt) ? (v - (1 << t) + 1) : v;
}

static bool jpeg_decode_block(jpeg_ctx_t *c, jpeg_bits_t *b,
                              int comp_idx,
                              const jpeg_huff_table_t *dc_table,
                              const jpeg_huff_table_t *ac_table) {
    memset(c->block, 0, sizeof(c->block));

    /* DC coefficient */
    int s = jpeg_huff_decode(dc_table, b);
    if (s < 0 || s > 15) return false;
    int diff = 0;
    if (s != 0) {
        int bits = (int)bits_read(b, s);
        diff = jpeg_extend(bits, s);
    }
    c->dc_pred[comp_idx] += diff;
    c->block[0] = (int16_t)c->dc_pred[comp_idx];

    /* AC coefficients */
    int k = 1;
    while (k < 64) {
        int rs = jpeg_huff_decode(ac_table, b);
        if (rs < 0) return false;
        if (rs == 0x00) break; /* EOB */
        if (rs == 0xF0) {
            k += 16;
            continue;
        }
        int r = (rs >> 4) & 0x0F;
        int s2 = rs & 0x0F;
        if (s2 == 0) return false;
        k += r;
        if (k >= 64) return false;
        int bits = (int)bits_read(b, s2);
        c->block[k] = (int16_t)jpeg_extend(bits, s2);
        k++;
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* Scan data preprocessing (remove stuffing and RST markers)                  */
/* -------------------------------------------------------------------------- */

static uint8_t *jpeg_clean_scan(const uint8_t *data, size_t start, size_t end, size_t *out_len) {
    uint8_t *buf = (uint8_t *)malloc(end - start);
    if (!buf) return NULL;
    size_t j = 0;
    for (size_t i = start; i < end; i++) {
        if (data[i] == 0xFF) {
            if (i + 1 < end) {
                uint8_t d = data[i + 1];
                if (d == 0x00) {
                    buf[j++] = 0xFF;
                    i++;
                } else if (d >= 0xD0 && d <= 0xD7) {
                    /* RST marker — skip both bytes */
                    i++;
                } else {
                    buf[j++] = data[i];
                }
            } else {
                buf[j++] = data[i];
            }
        } else {
            buf[j++] = data[i];
        }
    }
    *out_len = j;
    return buf;
}

/* -------------------------------------------------------------------------- */
/* Scan decoding                                                              */
/* -------------------------------------------------------------------------- */

static bool jpeg_decode_scan(jpeg_ctx_t *c, size_t scan_start, size_t scan_end) {
    size_t clean_len = 0;
    uint8_t *clean = jpeg_clean_scan(c->data, scan_start, scan_end, &clean_len);
    if (!clean) return false;

    jpeg_bits_t bits = {
        .data = clean,
        .len = clean_len,
        .pos = 0,
        .buf = 0,
        .nbits = 0
    };

    /* Build huffman tables for this scan */
    jpeg_huff_table_t dc_tables[4];
    jpeg_huff_table_t ac_tables[4];
    for (int i = 0; i < c->scan_ncomp; i++) {
        int dcid = c->scan_dc_huff[i];
        int acid = c->scan_ac_huff[i];
        jpeg_huff_build(&dc_tables[i], c->dc_huff[dcid].bits,
                        c->dc_huff[dcid].values, c->dc_huff[dcid].num_values);
        jpeg_huff_build(&ac_tables[i], c->ac_huff[acid].bits,
                        c->ac_huff[acid].values, c->ac_huff[acid].num_values);
    }

    /* Determine MCU grid */
    int hmax = 0, vmax = 0;
    for (int i = 0; i < c->ncomp; i++) {
        if (c->comp[i].h > hmax) hmax = c->comp[i].h;
        if (c->comp[i].v > vmax) vmax = c->comp[i].v;
    }
    int mcu_w = (c->width + hmax * 8 - 1) / (hmax * 8);
    int mcu_h = (c->height + vmax * 8 - 1) / (vmax * 8);

    /* Map scan component index to global component index */
    int comp_map[4];
    for (int i = 0; i < c->scan_ncomp; i++) {
        comp_map[i] = -1;
        for (int j = 0; j < c->ncomp; j++) {
            if (c->comp[j].id == c->scan_comp_id[i]) {
                comp_map[i] = j;
                break;
            }
        }
        if (comp_map[i] < 0) { free(clean); return false; }
    }

    /* Reset DC predictors */
    for (int i = 0; i < 4; i++) c->dc_pred[i] = 0;

    int mcu_count = 0;

    for (int my = 0; my < mcu_h; my++) {
        for (int mx = 0; mx < mcu_w; mx++) {
            for (int ci = 0; ci < c->scan_ncomp; ci++) {
                int gi = comp_map[ci];
                jpeg_comp_t *comp = &c->comp[gi];
                for (int by = 0; by < comp->v; by++) {
                    for (int bx = 0; bx < comp->h; bx++) {
                        if (!jpeg_decode_block(c, &bits, gi,
                                               &dc_tables[ci], &ac_tables[ci])) {
                            free(clean);
                            return false;
                        }
                        /* Dequantize and reorder */
                        jpeg_quant_table_t *q = &c->quant[comp->quant_id];
                        int16_t ordered[64];
                        for (int k = 0; k < 64; k++) {
                            ordered[zigzag[k]] = c->block[k] * (int16_t)q->data[k];
                        }
                        /* IDCT */
                        int px = (mx * comp->h + bx) * 8;
                        int py = (my * comp->v + by) * 8;
                        uint8_t *dst = comp->pixels + py * comp->width + px;
                        idct_block(ordered, dst, comp->width);
                    }
                }
            }
            mcu_count++;
            if (c->restart_interval > 0 && mcu_count >= c->restart_interval) {
                mcu_count = 0;
                for (int i = 0; i < 4; i++) c->dc_pred[i] = 0;
            }
        }
    }

    free(clean);
    return true;
}

/* -------------------------------------------------------------------------- */
/* YCbCr to RGB conversion + upsampling                                       */
/* -------------------------------------------------------------------------- */

static inline uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static ui_image_t *jpeg_to_rgba(jpeg_ctx_t *c) {
    ui_image_t *img = (ui_image_t *)calloc(1, sizeof(ui_image_t));
    if (!img) return NULL;
    img->w = c->width;
    img->h = c->height;
    img->pixels = (uint32_t *)calloc((size_t)c->width * (size_t)c->height, sizeof(uint32_t));
    if (!img->pixels) { free(img); return NULL; }

    if (c->ncomp == 1) {
        uint8_t *yplane = c->comp[0].pixels;
        int ystride = c->comp[0].width;
        for (int y = 0; y < c->height; y++) {
            for (int x = 0; x < c->width; x++) {
                uint8_t v = yplane[y * ystride + x];
                img->pixels[y * c->width + x] =
                    0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
            }
        }
    } else {
        int hmax = 0, vmax = 0;
        for (int i = 0; i < c->ncomp; i++) {
            if (c->comp[i].h > hmax) hmax = c->comp[i].h;
            if (c->comp[i].v > vmax) vmax = c->comp[i].v;
        }
        uint8_t *yplane = c->comp[0].pixels;
        uint8_t *cbplane = c->comp[1].pixels;
        uint8_t *crplane = c->comp[2].pixels;
        int ys = c->comp[0].width;
        int cbs = c->comp[1].width;
        int crs = c->comp[2].width;
        int ch = c->comp[1].h;
        int cv = c->comp[1].v;
        int ch2 = c->comp[2].h;
        int cv2 = c->comp[2].v;

        for (int y = 0; y < c->height; y++) {
            for (int x = 0; x < c->width; x++) {
                int yy = (int)yplane[y * ys + x];
                int cbx = x * ch / hmax;
                int cby = y * cv / vmax;
                int crx = x * ch2 / hmax;
                int cry = y * cv2 / vmax;
                int cb = (int)cbplane[cby * cbs + cbx] - 128;
                int cr = (int)crplane[cry * crs + crx] - 128;
                int r = yy + ((cr * 359) >> 8);
                int g = yy - ((cb * 88) >> 8) - ((cr * 183) >> 8);
                int b = yy + ((cb * 454) >> 8);
                r = clamp_u8(r);
                g = clamp_u8(g);
                b = clamp_u8(b);
                img->pixels[y * c->width + x] =
                    0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
            }
        }
    }
    return img;
}

/* -------------------------------------------------------------------------- */
/* Main entry point                                                           */
/* -------------------------------------------------------------------------- */

ui_image_t *ui_image_load_jpeg(const uint8_t *data, size_t len) {
    if (!data || len < 2) return NULL;
    if (data[0] != 0xFF || data[1] != 0xD8) return NULL; /* SOI */

    jpeg_ctx_t c = {0};
    c.data = data;
    c.len = len;
    c.pos = 2;
    c.restart_interval = 0;

    while (c.pos + 1 < c.len) {
        if (c.data[c.pos] != 0xFF) { c.pos++; continue; }
        uint8_t marker = c.data[c.pos + 1];
        if (marker == 0x00 || marker == 0xFF) { c.pos += 2; continue; }
        c.pos += 2;

        switch (marker) {
        case 0xD9: /* EOI */
            goto done;
        case 0xD8: /* SOI - invalid here */
            return NULL;
        case 0xC0: /* SOF0 */
            if (!jpeg_parse_sof0(&c)) return NULL;
            break;
        case 0xC4: /* DHT */
            if (!jpeg_parse_dht(&c)) return NULL;
            break;
        case 0xDB: /* DQT */
            if (!jpeg_parse_dqt(&c)) return NULL;
            break;
        case 0xDD: /* DRI */
            if (!jpeg_parse_dri(&c)) return NULL;
            break;
        case 0xDA: { /* SOS */
            if (!jpeg_parse_sos(&c)) return NULL;
            size_t scan_start = c.pos;
            /* Find end of scan: next 0xFF marker (excluding 0x00 stuffing and RST) */
            size_t scan_end = c.len;
            for (size_t i = scan_start; i + 1 < c.len; i++) {
                if (c.data[i] == 0xFF) {
                    uint8_t m = c.data[i + 1];
                    if (m != 0x00 && !(m >= 0xD0 && m <= 0xD7) && m != 0xFF) {
                        scan_end = i;
                        break;
                    }
                }
            }
            if (!jpeg_decode_scan(&c, scan_start, scan_end)) {
                for (int i = 0; i < c.ncomp; i++) free(c.comp[i].pixels);
                return NULL;
            }
            c.pos = scan_end;
            break;
        }
        case 0xE0: /* APP0 JFIF */
        case 0xE1: case 0xE2: case 0xE3: case 0xE4:
        case 0xE5: case 0xE6: case 0xE7: case 0xE8:
        case 0xE9: case 0xEA: case 0xEB: case 0xEC:
        case 0xED: case 0xEE: case 0xEF: /* APP1-APP15 */
        case 0xFE: /* COM */
            if (c.pos + 2 > c.len) return NULL;
            {
                uint16_t seglen = read_be16(c.data + c.pos);
                if (seglen < 2) return NULL;
                c.pos += seglen;
            }
            break;
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        case 0xD4: case 0xD5: case 0xD6: case 0xD7: /* RST outside scan */
            break;
        default:
            /* Unknown marker with length */
            if (marker >= 0x02 && marker <= 0xFE) {
                if (c.pos + 2 > c.len) return NULL;
                uint16_t seglen = read_be16(c.data + c.pos);
                if (seglen < 2) return NULL;
                c.pos += seglen;
            }
            break;
        }
    }

done:
    if (!c.got_sof) {
        for (int i = 0; i < c.ncomp; i++) free(c.comp[i].pixels);
        return NULL;
    }
    ui_image_t *img = jpeg_to_rgba(&c);
    for (int i = 0; i < c.ncomp; i++) free(c.comp[i].pixels);
    return img;
}
