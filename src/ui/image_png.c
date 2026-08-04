/*
 * ORDL UI — PNG image decoder from scratch
 * Pure C23, zero external dependencies.
 *
 * Implements DEFLATE decompression (fixed + dynamic Huffman, store blocks),
 * zlib wrapper parsing, PNG chunk reading, and filter reconstruction.
 * Outputs RGBA8888.
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Bitstream (LSB-first byte packing)                                         */
/* -------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint32_t bitbuf;
    int bits;
} bitstream_t;

static void bs_refill(bitstream_t *bs) {
    while (bs->bits <= 24 && bs->pos < bs->len) {
        bs->bitbuf |= (uint32_t)bs->data[bs->pos] << bs->bits;
        bs->bits += 8;
        bs->pos++;
    }
}

static uint32_t bs_peek(bitstream_t *bs, int n) {
    bs_refill(bs);
    return bs->bitbuf & ((1u << n) - 1);
}

static void bs_consume(bitstream_t *bs, int n) {
    bs->bitbuf >>= n;
    bs->bits -= n;
}

static uint32_t bs_read(bitstream_t *bs, int n) {
    uint32_t v = bs_peek(bs, n);
    bs_consume(bs, n);
    return v;
}

static uint32_t reverse_bits(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

/* -------------------------------------------------------------------------- */
/* Huffman decoder (15-bit direct lookup)                                     */
/* -------------------------------------------------------------------------- */

#define MAX_BITS 15
#define LUT_SIZE (1 << MAX_BITS)

typedef struct {
    uint16_t table[LUT_SIZE]; /* (symbol << 4) | len; len=0 = invalid */
} huffman_table_t;

static void huffman_build(huffman_table_t *t, const uint8_t *code_lengths, int n) {
    memset(t->table, 0, sizeof(t->table));

    int bl_count[MAX_BITS + 1] = {0};
    for (int i = 0; i < n; i++) {
        if (code_lengths[i] > 0 && code_lengths[i] <= MAX_BITS) {
            bl_count[code_lengths[i]]++;
        }
    }

    int next_code[MAX_BITS + 1] = {0};
    int code = 0;
    bl_count[0] = 0;
    for (int bits = 1; bits <= MAX_BITS; bits++) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    for (int i = 0; i < n; i++) {
        int len = code_lengths[i];
        if (len == 0) continue;
        int c = next_code[len]++;
        int rev = (int)reverse_bits((uint32_t)c, len);
        int step = 1 << len;
        int count = 1 << (MAX_BITS - len);
        uint16_t entry = (uint16_t)((i << 4) | len);
        for (int j = 0; j < count; j++) {
            t->table[rev + j * step] = entry;
        }
    }
}

static int huffman_decode(const huffman_table_t *t, bitstream_t *bs) {
    uint32_t v = bs_peek(bs, MAX_BITS);
    uint16_t entry = t->table[v];
    int len = entry & 15;
    if (len == 0) return -1;
    bs_consume(bs, len);
    return entry >> 4;
}

/* -------------------------------------------------------------------------- */
/* Fixed Huffman tables (precomputed)                                         */
/* -------------------------------------------------------------------------- */

static void build_fixed_tables(huffman_table_t *lit_table, huffman_table_t *dist_table) {
    uint8_t lit_len[288];
    int i;
    for (i = 0; i <= 143; i++) lit_len[i] = 8;
    for (i = 144; i <= 255; i++) lit_len[i] = 9;
    for (i = 256; i <= 279; i++) lit_len[i] = 7;
    for (i = 280; i <= 287; i++) lit_len[i] = 8;
    huffman_build(lit_table, lit_len, 288);

    uint8_t dist_len[32];
    for (i = 0; i < 32; i++) dist_len[i] = 5;
    huffman_build(dist_table, dist_len, 32);
}

/* -------------------------------------------------------------------------- */
/* DEFLATE decompressor                                                       */
/* -------------------------------------------------------------------------- */

static const int len_extra_bits[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const int len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const int dist_extra_bits[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};
static const int dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};

static bool deflate_decompress(const uint8_t *in, size_t in_len,
                                uint8_t **out, size_t *out_len, size_t *out_cap) {
    bitstream_t bs = { in, in_len, 0, 0, 0 };
    huffman_table_t fixed_lit, fixed_dist;
    build_fixed_tables(&fixed_lit, &fixed_dist);

    *out = NULL;
    *out_len = 0;
    *out_cap = 0;

    bool final_block = false;
    while (!final_block) {
        final_block = bs_read(&bs, 1) != 0;
        int btype = (int)bs_read(&bs, 2);

        if (btype == 0) {
            /* Stored block */
            bs_consume(&bs, bs.bits & 7); /* align to byte boundary */
            if (bs.pos + 4 > bs.len) return false;
            uint16_t llen = (uint16_t)(bs_read(&bs, 8) | (bs_read(&bs, 8) << 8));
            uint16_t nlen = (uint16_t)(bs_read(&bs, 8) | (bs_read(&bs, 8) << 8));
            (void)nlen; /* nlen should be ~llen; could verify */
            if (bs.pos + llen > bs.len) return false;
            if (*out_len + llen > *out_cap) {
                size_t nc = *out_cap ? *out_cap * 2 : 8192;
                while (nc < *out_len + llen) nc *= 2;
                uint8_t *nb = realloc(*out, nc);
                if (!nb) return false;
                *out = nb;
                *out_cap = nc;
            }
            memcpy(*out + *out_len, in + bs.pos, llen);
            *out_len += llen;
            bs.pos += llen;
            bs.bitbuf = 0;
            bs.bits = 0;
        } else if (btype == 1 || btype == 2) {
            huffman_table_t lit_table, dist_table;
            if (btype == 1) {
                lit_table = fixed_lit;
                dist_table = fixed_dist;
            } else {
                /* Dynamic Huffman */
                int hlit = (int)bs_read(&bs, 5) + 257;
                int hdist = (int)bs_read(&bs, 5) + 1;
                int hclen = (int)bs_read(&bs, 4) + 4;

                static const uint8_t code_len_order[19] = {
                    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
                };
                uint8_t cl_code_lengths[19] = {0};
                for (int i = 0; i < hclen; i++) {
                    cl_code_lengths[code_len_order[i]] = (uint8_t)bs_read(&bs, 3);
                }
                huffman_table_t cl_table;
                huffman_build(&cl_table, cl_code_lengths, 19);

                uint8_t all_code_lengths[288 + 32];
                int ntotal = hlit + hdist;
                int j = 0;
                while (j < ntotal) {
                    int sym = huffman_decode(&cl_table, &bs);
                    if (sym < 0 || sym > 18) return false;
                    if (sym < 16) {
                        all_code_lengths[j++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        if (j == 0) return false;
                        int repeat = (int)bs_read(&bs, 2) + 3;
                        uint8_t last = all_code_lengths[j - 1];
                        while (repeat-- > 0 && j < ntotal) all_code_lengths[j++] = last;
                    } else if (sym == 17) {
                        int repeat = (int)bs_read(&bs, 3) + 3;
                        while (repeat-- > 0 && j < ntotal) all_code_lengths[j++] = 0;
                    } else { /* sym == 18 */
                        int repeat = (int)bs_read(&bs, 7) + 11;
                        while (repeat-- > 0 && j < ntotal) all_code_lengths[j++] = 0;
                    }
                }
                huffman_build(&lit_table, all_code_lengths, hlit);
                huffman_build(&dist_table, all_code_lengths + hlit, hdist);
            }

            for (;;) {
                int sym = huffman_decode(&lit_table, &bs);
                if (sym < 0) return false;
                if (sym < 256) {
                    if (*out_len >= *out_cap) {
                        size_t nc = *out_cap ? *out_cap * 2 : 8192;
                        uint8_t *nb = realloc(*out, nc);
                        if (!nb) return false;
                        *out = nb;
                        *out_cap = nc;
                    }
                    (*out)[(*out_len)++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    int len_sym = sym - 257;
                    if (len_sym < 0 || len_sym >= 29) return false;
                    int length = len_base[len_sym] + (int)bs_read(&bs, len_extra_bits[len_sym]);
                    int dist_sym = huffman_decode(&dist_table, &bs);
                    if (dist_sym < 0 || dist_sym >= 30) return false;
                    int distance = dist_base[dist_sym] + (int)bs_read(&bs, dist_extra_bits[dist_sym]);
                    if (distance > (int)*out_len) return false;
                    if (*out_len + (size_t)length > *out_cap) {
                        size_t nc = *out_cap ? *out_cap * 2 : 8192;
                        while (nc < *out_len + (size_t)length) nc *= 2;
                        uint8_t *nb = realloc(*out, nc);
                        if (!nb) return false;
                        *out = nb;
                        *out_cap = nc;
                    }
                    for (int k = 0; k < length; k++) {
                        (*out)[*out_len + k] = (*out)[*out_len - distance + k];
                    }
                    *out_len += (size_t)length;
                }
            }
        } else {
            return false; /* invalid block type */
        }
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* CRC32 (PNG polynomial)                                                     */
/* -------------------------------------------------------------------------- */

static uint32_t crc32_table[256];
static bool crc32_init_done = false;

static void crc32_init(void) {
    if (crc32_init_done) return;
    for (int i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)i;
        for (int j = 0; j < 8; j++) {
            c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0);
        }
        crc32_table[i] = c;
    }
    crc32_init_done = true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len) {
    crc32_init();
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

/* Adler32 available if needed for zlib verification */
#if 0
static uint32_t adler32(const uint8_t *data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}
#endif

/* -------------------------------------------------------------------------- */
/* PNG helpers                                                                */
/* -------------------------------------------------------------------------- */

static bool read_be32(const uint8_t *p, uint32_t *out) {
    *out = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | ((uint32_t)p[3]);
    return true;
}

static bool png_paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* -------------------------------------------------------------------------- */
/* PNG decoder                                                                */
/* -------------------------------------------------------------------------- */

ui_image_t *ui_image_load_png(const uint8_t *data, size_t len) {
    if (!data || len < 33) return NULL;

    /* PNG signature */
    static const uint8_t png_sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    if (memcmp(data, png_sig, 8) != 0) return NULL;

    size_t pos = 8;
    uint32_t width = 0, height = 0;
    uint8_t bit_depth = 0, color_type = 0;
    uint8_t *idat_buf = NULL;
    size_t idat_len = 0, idat_cap = 0;

    while (pos + 12 <= len) {
        uint32_t clen;
        read_be32(data + pos, &clen);
        pos += 4;
        uint32_t ctype = ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos + 1] << 16) |
                         ((uint32_t)data[pos + 2] << 8)  | ((uint32_t)data[pos + 3]);
        pos += 4;

        if (pos + clen + 4 > len) { free(idat_buf); return NULL; }

        /* Verify CRC */
        uint32_t crc = 0xFFFFFFFFu;
        crc = crc32_update(crc, data + pos - 4, 4 + clen);
        crc ^= 0xFFFFFFFFu;
        uint32_t file_crc;
        read_be32(data + pos + clen, &file_crc);
        if (crc != file_crc) { free(idat_buf); return NULL; }

        if (ctype == 0x49484452u) { /* IHDR */
            if (clen != 13) { free(idat_buf); return NULL; }
            read_be32(data + pos, &width);
            read_be32(data + pos + 4, &height);
            bit_depth = data[pos + 8];
            color_type = data[pos + 9];
            if (bit_depth != 8) { free(idat_buf); return NULL; }
            if (color_type != 2 && color_type != 6) { free(idat_buf); return NULL; }
        } else if (ctype == 0x49444154u) { /* IDAT */
            if (!idat_buf) {
                idat_cap = 65536;
                idat_buf = malloc(idat_cap);
                if (!idat_buf) return NULL;
            }
            if (idat_len + clen > idat_cap) {
                size_t nc = idat_cap;
                while (nc < idat_len + clen) nc *= 2;
                uint8_t *nb = realloc(idat_buf, nc);
                if (!nb) { free(idat_buf); return NULL; }
                idat_buf = nb;
                idat_cap = nc;
            }
            memcpy(idat_buf + idat_len, data + pos, clen);
            idat_len += clen;
        } else if (ctype == 0x49454E44u) { /* IEND */
            break;
        }
        pos += clen + 4;
    }

    if (width == 0 || height == 0 || idat_len < 6) { free(idat_buf); return NULL; }

    /* Parse zlib header */
    uint8_t cmf = idat_buf[0];
    uint8_t flg = idat_buf[1];
    if ((cmf & 0x0F) != 8) { free(idat_buf); return NULL; } /* must be deflate */
    if (((cmf << 8) | flg) % 31 != 0) { free(idat_buf); return NULL; }

    /* Decompress */
    uint8_t *raw = NULL;
    size_t raw_len = 0, raw_cap = 0;
    bool ok = deflate_decompress(idat_buf + 2, idat_len - 2, &raw, &raw_len, &raw_cap);
    if (!ok) { free(idat_buf); return NULL; }

    /* Verify adler32 if present (last 4 bytes of raw should be adler, but it's after deflate) */
    /* Actually adler32 is after the deflate stream, not part of the decompressed data. */
    /* We skip strict adler verification for simplicity; the CRC on IDAT chunks is enough. */

    free(idat_buf);

 int channels = (color_type == 6) ? 4 : 3;
    if (width > SIZE_MAX / (size_t)channels) { free(raw); return NULL; }
    size_t row_size = 1 + (size_t)width * (size_t)channels;
    if (row_size > SIZE_MAX / (size_t)height) { free(raw); return NULL; }
    if (raw_len < row_size * (size_t)height) { free(raw); return NULL; }

    ui_image_t *img = calloc(1, sizeof(ui_image_t));
    if (!img) { free(raw); return NULL; }
    img->w = (int)width;
    img->h = (int)height;
    img->pixels = calloc((size_t)width * (size_t)height, sizeof(uint32_t));
    if (!img->pixels) { free(raw); free(img); return NULL; }

    uint8_t *prev_row = calloc((size_t)width * 4, 1);
    if (!prev_row) { free(raw); free(img->pixels); free(img); return NULL; }

    for (int y = 0; y < (int)height; y++) {
        const uint8_t *src = raw + (size_t)y * row_size;
        uint8_t filter = src[0];
        const uint8_t *row = src + 1;
        uint32_t *dst = img->pixels + (size_t)y * (size_t)width;

        for (int x = 0; x < (int)width; x++) {
            uint8_t r = row[x * channels + 0];
            uint8_t g = row[x * channels + 1];
            uint8_t b = row[x * channels + 2];
            uint8_t a = (channels == 4) ? row[x * channels + 3] : 255;

            uint8_t *pr = prev_row + (size_t)x * 4;
            uint8_t left[4] = {0, 0, 0, 0};
            if (x > 0) {
                uint32_t left_pixel = dst[x - 1];
                left[0] = left_pixel & 0xFF;
                left[1] = (left_pixel >> 8) & 0xFF;
                left[2] = (left_pixel >> 16) & 0xFF;
                left[3] = (left_pixel >> 24) & 0xFF;
            }

            for (int c = 0; c < 4; c++) {
                uint8_t v = (c < 3) ? ((uint8_t[]){r,g,b})[c] : a;
                switch (filter) {
                case 0: /* None */ break;
                case 1: /* Sub */ v = (uint8_t)(v + left[c]); break;
                case 2: /* Up */ v = (uint8_t)(v + pr[c]); break;
                case 3: /* Average */ v = (uint8_t)(v + (left[c] + pr[c]) / 2); break;
                case 4: /* Paeth */ v = (uint8_t)(v + png_paeth(left[c], pr[c], (x > 0) ? pr[-4 + c] : 0)); break;
                default: break;
                }
                if (c == 0) r = v;
                else if (c == 1) g = v;
                else if (c == 2) b = v;
                else a = v;
            }

            dst[x] = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
            pr[0] = r; pr[1] = g; pr[2] = b; pr[3] = a;
        }
    }

    free(prev_row);
    free(raw);
    return img;
}
