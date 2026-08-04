/*
 * ORDL UI — TrueType Font parser from scratch
 * Pure C23, zero external dependencies.
 *
 * Parses: head, hhea, maxp, cmap (fmt 4, 12), loca, glyf (simple + composite), hmtx.
 * Safe on malformed input: every offset is bounds-checked against the file size.
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Big-endian helpers                                                         */
/* -------------------------------------------------------------------------- */

static inline uint16_t ttf_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline int16_t ttf_be16i(const uint8_t *p) {
    return (int16_t)ttf_be16(p);
}

static inline uint32_t ttf_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* -------------------------------------------------------------------------- */
/* Internal font structure                                                    */
/* -------------------------------------------------------------------------- */

struct ui_font_ttf {
    const uint8_t *data;
    size_t         len;

    uint32_t off_head;
    uint32_t off_hhea;
    uint32_t off_maxp;
    uint32_t off_cmap;
    uint32_t off_loca;
    uint32_t off_glyf;
    uint32_t off_hmtx;

    uint16_t num_glyphs;
    uint16_t units_per_em;
    int16_t  index_to_loc_format;
    uint16_t num_hmetrics;

    /* CFF/OTF specific */
    uint8_t  *cff_data;       /* owned copy of CFF table */
    size_t   cff_len;
    uint32_t cff_charstrings_offset;
};

/* -------------------------------------------------------------------------- */
/* Bounds-checked data access                                                 */
/* -------------------------------------------------------------------------- */

static inline const uint8_t *ttf_ptr(ui_font_ttf_t *f, uint32_t offset, uint32_t need) {
    if (!f || offset > f->len || need > f->len - offset) return NULL;
    return f->data + offset;
}

static inline bool ttf_has_table(ui_font_ttf_t *f, uint32_t off) {
    return off != 0 && off < f->len;
}

/* -------------------------------------------------------------------------- */
/* Table directory parsing                                                    */
/* -------------------------------------------------------------------------- */

static uint32_t ttf_find_table(ui_font_ttf_t *f, const char tag[4]) {
    if (!f || f->len < 12) return 0;
    uint16_t num_tables = ttf_be16(f->data + 4);
    if (f->len < 12 + (size_t)num_tables * 16) return 0;
    for (uint16_t i = 0; i < num_tables; i++) {
        const uint8_t *rec = f->data + 12 + (size_t)i * 16;
        if (memcmp(rec, tag, 4) == 0) {
            uint32_t off = ttf_be32(rec + 8);
            uint32_t len = ttf_be32(rec + 12);
            if (off + len <= f->len) return off;
            return 0;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Load / free                                                                */
/* -------------------------------------------------------------------------- */

ui_font_ttf_t *ui_font_ttf_load(const uint8_t *data, size_t len) {
    if (!data || len < 12) return NULL;
    uint32_t sfnt = ttf_be32(data);
    if (sfnt != 0x00010000 && sfnt != 0x4F54544F) return NULL;

    ui_font_ttf_t *f = calloc(1, sizeof(ui_font_ttf_t));
    if (!f) return NULL;
    f->data = data;
    f->len  = len;

    f->off_head = ttf_find_table(f, "head");
    f->off_hhea = ttf_find_table(f, "hhea");
    f->off_maxp = ttf_find_table(f, "maxp");
    f->off_cmap = ttf_find_table(f, "cmap");
    f->off_loca = ttf_find_table(f, "loca");
    f->off_glyf = ttf_find_table(f, "glyf");
    f->off_hmtx = ttf_find_table(f, "hmtx");

    if (!ttf_has_table(f, f->off_head) ||
        !ttf_has_table(f, f->off_loca) ||
        !ttf_has_table(f, f->off_glyf)) {
        free(f);
        return NULL;
    }

    const uint8_t *head = ttf_ptr(f, f->off_head, 54);
    if (head) {
        f->units_per_em = ttf_be16(head + 18);
        f->index_to_loc_format = ttf_be16i(head + 50);
    }

    const uint8_t *maxp = ttf_ptr(f, f->off_maxp, 6);
    if (maxp) {
        f->num_glyphs = ttf_be16(maxp + 4);
    }

    const uint8_t *hhea = ttf_ptr(f, f->off_hhea, 36);
    if (hhea) {
        f->num_hmetrics = ttf_be16(hhea + 34);
    }

    return f;
}

void ui_font_ttf_free(ui_font_ttf_t *f) {
    if (f) {
        free(f->cff_data);
        free(f);
    }
}

int ui_font_ttf_units_per_em(ui_font_ttf_t *f) {
    return f ? f->units_per_em : 0;
}

int ui_font_ttf_num_glyphs(ui_font_ttf_t *f) {
    return f ? f->num_glyphs : 0;
}

/* -------------------------------------------------------------------------- */
/* cmap parsing (format 4 and 12)                                             */
/* -------------------------------------------------------------------------- */

static uint16_t ttf_cmap_lookup(ui_font_ttf_t *f, uint32_t codepoint) {
    if (!ttf_has_table(f, f->off_cmap)) return 0;
    const uint8_t *cmap = ttf_ptr(f, f->off_cmap, 4);
    if (!cmap) return 0;
    uint16_t num_tables = ttf_be16(cmap + 2);
    if (f->off_cmap + 4 + (size_t)num_tables * 8 > f->len) return 0;

    for (uint16_t i = 0; i < num_tables; i++) {
        const uint8_t *rec = f->data + f->off_cmap + 4 + (size_t)i * 8;
        uint32_t off = ttf_be32(rec + 4);
        uint32_t fmt_off = f->off_cmap + off;
        const uint8_t *fmt = ttf_ptr(f, fmt_off, 6);
        if (!fmt) continue;
        uint16_t format = ttf_be16(fmt);

        if (format == 4) {
            uint16_t seg_count_x2 = ttf_be16(fmt + 6);
            uint16_t seg_count = seg_count_x2 / 2;
            if (seg_count == 0) continue;
            const uint8_t *end_codes   = fmt + 14;
            const uint8_t *start_codes = fmt + 16 + seg_count_x2;
            const uint8_t *id_delta    = fmt + 16 + seg_count_x2 * 2;
            const uint8_t *id_range_off= fmt + 16 + seg_count_x2 * 3;
            for (uint16_t s = 0; s < seg_count; s++) {
                uint16_t end   = ttf_be16(end_codes   + (size_t)s * 2);
                uint16_t start = ttf_be16(start_codes + (size_t)s * 2);
                if (codepoint < start || codepoint > end) continue;
                int16_t  delta = ttf_be16i(id_delta    + (size_t)s * 2);
                uint16_t ro    = ttf_be16(id_range_off+ (size_t)s * 2);
                if (ro == 0) {
                    return (uint16_t)((int32_t)codepoint + delta);
                } else {
                    const uint8_t *glyph_ptr = id_range_off + (size_t)s * 2 + ro + ((codepoint - start) * 2);
                    if (glyph_ptr >= f->data && glyph_ptr + 2 <= f->data + f->len) {
                        uint16_t gid = ttf_be16(glyph_ptr);
                        if (gid != 0) return (uint16_t)(gid + delta);
                    }
                }
            }
        } else if (format == 12) {
            const uint8_t *hdr = ttf_ptr(f, fmt_off, 16);
            if (!hdr) continue;
            uint32_t ngroups = ttf_be32(hdr + 12);
            if (fmt_off + 16 + (size_t)ngroups * 12 > f->len) continue;
            for (uint32_t g = 0; g < ngroups; g++) {
                const uint8_t *grp = f->data + fmt_off + 16 + (size_t)g * 12;
                uint32_t start = ttf_be32(grp);
                uint32_t end   = ttf_be32(grp + 4);
                uint32_t start_glyph = ttf_be32(grp + 8);
                if (codepoint >= start && codepoint <= end) {
                    return (uint16_t)(start_glyph + (codepoint - start));
                }
            }
        }
    }
    return 0;
}

uint16_t ui_font_ttf_glyph_index(ui_font_ttf_t *f, uint32_t codepoint) {
    if (!f) return 0;
    return ttf_cmap_lookup(f, codepoint);
}

/* -------------------------------------------------------------------------- */
/* Glyph metrics                                                              */
/* -------------------------------------------------------------------------- */

bool ui_font_ttf_glyph_metrics(ui_font_ttf_t *f, uint16_t gid,
                               ui_font_glyph_metrics_t *out) {
    if (!f || !out || gid >= f->num_glyphs) return false;
    memset(out, 0, sizeof(*out));

    if (ttf_has_table(f, f->off_hmtx)) {
        if (gid < f->num_hmetrics) {
            const uint8_t *hm = ttf_ptr(f, f->off_hmtx + (size_t)gid * 4, 4);
            if (hm) {
                out->advance_width = (int16_t)ttf_be16(hm);
                out->left_side_bearing = ttf_be16i(hm + 2);
            }
        } else {
            const uint8_t *hm = ttf_ptr(f, f->off_hmtx + (size_t)(f->num_hmetrics - 1) * 4, 4);
            if (hm) {
                out->advance_width = (int16_t)ttf_be16(hm);
            }
            const uint8_t *lsb = ttf_ptr(f,
                f->off_hmtx + (size_t)f->num_hmetrics * 4 +
                (size_t)(gid - f->num_hmetrics) * 2, 2);
            if (lsb) out->left_side_bearing = ttf_be16i(lsb);
        }
    }

    if (ttf_has_table(f, f->off_glyf) && ttf_has_table(f, f->off_loca)) {
        uint32_t g_off, g_next;
        if (f->index_to_loc_format == 0) {
            const uint8_t *loca = ttf_ptr(f, f->off_loca + (size_t)gid * 2, 4);
            if (!loca) return true;
            g_off  = (uint32_t)ttf_be16(loca) * 2;
            g_next = (uint32_t)ttf_be16(loca + 2) * 2;
        } else {
            const uint8_t *loca = ttf_ptr(f, f->off_loca + (size_t)gid * 4, 8);
            if (!loca) return true;
            g_off  = ttf_be32(loca);
            g_next = ttf_be32(loca + 4);
        }
        if (g_off == g_next) return true;
        const uint8_t *gh = ttf_ptr(f, f->off_glyf + g_off, 10);
        if (gh) {
            out->x_min = ttf_be16i(gh + 2);
            out->y_min = ttf_be16i(gh + 4);
            out->x_max = ttf_be16i(gh + 6);
            out->y_max = ttf_be16i(gh + 8);
        }
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* Simple glyf outline extraction                                             */
/* -------------------------------------------------------------------------- */

static int ttf_glyf_simple_outline(const uint8_t *data, size_t avail,
                                   ui_font_point_t *pts, size_t pt_cap,
                                   uint16_t *contour_ends, size_t contour_cap) {
    if (avail < 10) return -1;
    int16_t num_contours = ttf_be16i(data);
    if (num_contours <= 0) return -1;
    if ((size_t)num_contours > contour_cap) return -1;

    uint16_t total_points = 0;
    for (int i = 0; i < num_contours; i++) {
        uint16_t ep = ttf_be16(data + 10 + (size_t)i * 2);
        contour_ends[i] = ep;
        total_points = ep + 1;
    }
    if (total_points == 0 || (size_t)total_points > pt_cap) return -1;

    uint16_t instruction_len = ttf_be16(data + 10 + (size_t)num_contours * 2);
    size_t flags_off = 10 + (size_t)num_contours * 2 + 2 + instruction_len;
    if (flags_off > avail) return -1;

    uint8_t *flags = calloc((size_t)total_points, 1);
    if (!flags) return -1;

    size_t p = flags_off;
    size_t flag_i = 0;
    while (flag_i < total_points && p < avail) {
        uint8_t f = data[p++];
        flags[flag_i++] = f;
        if ((f & 0x08) && p < avail && flag_i < total_points) {
            uint8_t repeat = data[p++];
            for (uint8_t r = 0; r < repeat && flag_i < total_points; r++) {
                flags[flag_i++] = f;
            }
        }
    }
    if (flag_i != total_points) { free(flags); return -1; }

    /* Decode x coordinates */
    int16_t prev_x = 0;
    size_t xp = p;
    for (size_t i = 0; i < total_points; i++) {
        int16_t dx = 0;
        if (flags[i] & 0x02) {
            /* x-short: 1 byte, sign from bit 4 */
            if (xp >= avail) { free(flags); return -1; }
            dx = (int16_t)data[xp++];
            if ((flags[i] & 0x10) == 0) dx = (int16_t)-dx;
        } else {
            if (flags[i] & 0x10) {
                dx = 0;
            } else {
                if (xp + 2 > avail) { free(flags); return -1; }
                dx = ttf_be16i(data + xp); xp += 2;
            }
        }
        pts[i].x = (float)(prev_x + dx);
        pts[i].on_curve = (flags[i] & 0x01) != 0;
        prev_x = (int16_t)(prev_x + dx);
    }

    /* Decode y coordinates */
    int16_t prev_y = 0;
    size_t yp = xp;
    for (size_t i = 0; i < total_points; i++) {
        int16_t dy = 0;
        if (flags[i] & 0x04) {
            /* y-short: 1 byte, sign from bit 5 */
            if (yp >= avail) { free(flags); return -1; }
            dy = (int16_t)data[yp++];
            if ((flags[i] & 0x20) == 0) dy = (int16_t)-dy;
        } else {
            if (flags[i] & 0x20) {
                dy = 0;
            } else {
                if (yp + 2 > avail) { free(flags); return -1; }
                dy = ttf_be16i(data + yp); yp += 2;
            }
        }
        pts[i].y = (float)(prev_y + dy);
        prev_y = (int16_t)(prev_y + dy);
    }

    free(flags);
    return num_contours;
}

/* -------------------------------------------------------------------------- */
/* Composite glyph decomposition (depth-limited)                              */
/* -------------------------------------------------------------------------- */

static int ttf_glyf_composite_outline(ui_font_ttf_t *f, uint32_t glyf_off,
                                      const uint8_t *data, size_t avail,
                                      ui_font_point_t *pts, size_t pt_cap,
                                      uint16_t *contour_ends, size_t contour_cap,
                                      int depth);

static int ttf_glyf_outline_internal(ui_font_ttf_t *f, uint32_t glyf_off,
                                     const uint8_t *data, size_t avail,
                                     ui_font_point_t *pts, size_t pt_cap,
                                     uint16_t *contour_ends, size_t contour_cap,
                                     int depth);

static int ttf_glyf_composite_outline(ui_font_ttf_t *f, uint32_t glyf_off,
                                      const uint8_t *data, size_t avail,
                                      ui_font_point_t *pts, size_t pt_cap,
                                      uint16_t *contour_ends, size_t contour_cap,
                                      int depth) {
    (void)glyf_off;
    if (depth > 4) return -1;
    size_t off = 10;
    size_t pt_idx = 0;
    int contour_idx = 0;

    while (off + 4 <= avail) {
        uint16_t flags = ttf_be16(data + off);
        uint16_t gidx  = ttf_be16(data + off + 2);
        off += 4;

        bool args_are_words = (flags & 0x0001) != 0;
        bool args_are_xy    = (flags & 0x0002) != 0;
        bool have_scale     = (flags & 0x0008) != 0;
        bool have_xy_scale  = (flags & 0x0040) != 0;
        bool have_2x2       = (flags & 0x0080) != 0;

        int16_t arg1 = 0, arg2 = 0;
        if (args_are_words) {
            if (off + 4 > avail) return -1;
            arg1 = ttf_be16i(data + off);
            arg2 = ttf_be16i(data + off + 2);
            off += 4;
        } else {
            if (off + 2 > avail) return -1;
            arg1 = (int8_t)data[off];
            arg2 = (int8_t)data[off + 1];
            off += 2;
        }

        float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f_ = 0.0f;

        if (args_are_xy) {
            e = (float)arg1;
            f_ = (float)arg2;
        }

        if (have_scale) {
            if (off + 2 > avail) return -1;
            float s = (float)ttf_be16i(data + off) / 16384.0f;
            off += 2;
            a = d = s;
        } else if (have_xy_scale) {
            if (off + 4 > avail) return -1;
            a = (float)ttf_be16i(data + off) / 16384.0f;
            d = (float)ttf_be16i(data + off + 2) / 16384.0f;
            off += 4;
        } else if (have_2x2) {
            if (off + 8 > avail) return -1;
            a = (float)ttf_be16i(data + off) / 16384.0f;
            b = (float)ttf_be16i(data + off + 2) / 16384.0f;
            c = (float)ttf_be16i(data + off + 4) / 16384.0f;
            d = (float)ttf_be16i(data + off + 6) / 16384.0f;
            off += 8;
        }

        /* Fetch sub-glyph */
        uint32_t sub_off, sub_next;
        if (f->index_to_loc_format == 0) {
            const uint8_t *loca = ttf_ptr(f, f->off_loca + (size_t)gidx * 2, 4);
            if (!loca) return -1;
            sub_off  = (uint32_t)ttf_be16(loca) * 2;
            sub_next = (uint32_t)ttf_be16(loca + 2) * 2;
        } else {
            const uint8_t *loca = ttf_ptr(f, f->off_loca + (size_t)gidx * 4, 8);
            if (!loca) return -1;
            sub_off  = ttf_be32(loca);
            sub_next = ttf_be32(loca + 4);
        }
        if (sub_off == sub_next) continue;

        ui_font_point_t *sub_pts = malloc(512 * sizeof(ui_font_point_t));
        uint16_t *sub_ends = malloc(32 * sizeof(uint16_t));
        if (!sub_pts || !sub_ends) { free(sub_pts); free(sub_ends); return -1; }
        int sub_nc = ttf_glyf_outline_internal(f, f->off_glyf + sub_off,
            ttf_ptr(f, f->off_glyf + sub_off, sub_next - sub_off),
            sub_next - sub_off, sub_pts, 512, sub_ends, 32, depth + 1);
        if (sub_nc < 0) { free(sub_pts); free(sub_ends); return -1; }

        uint16_t sub_total = sub_nc > 0 ? (uint16_t)(sub_ends[sub_nc - 1] + 1) : 0;
        if (pt_idx + sub_total > pt_cap) { free(sub_pts); free(sub_ends); return -1; }
        if (contour_idx + sub_nc > (int)contour_cap) { free(sub_pts); free(sub_ends); return -1; }

        for (uint16_t i = 0; i < sub_total; i++) {
            float x = sub_pts[i].x;
            float y = sub_pts[i].y;
            pts[pt_idx + i].x = a * x + c * y + e;
            pts[pt_idx + i].y = b * x + d * y + f_;
            pts[pt_idx + i].on_curve = sub_pts[i].on_curve;
        }
        for (int i = 0; i < sub_nc; i++) {
            contour_ends[contour_idx + i] = (uint16_t)(pt_idx + sub_ends[i]);
        }
        pt_idx += sub_total;
        contour_idx += sub_nc;
        free(sub_pts);
        free(sub_ends);

        if ((flags & 0x0020) == 0) break; /* MORE_COMPONENTS */
    }
    return contour_idx;
}

static int ttf_glyf_outline_internal(ui_font_ttf_t *f, uint32_t glyf_off,
                                     const uint8_t *data, size_t avail,
                                     ui_font_point_t *pts, size_t pt_cap,
                                     uint16_t *contour_ends, size_t contour_cap,
                                     int depth) {
    if (!data || avail < 10) return -1;
    int16_t num_contours = ttf_be16i(data);
    if (num_contours >= 0) {
        return ttf_glyf_simple_outline(data, avail, pts, pt_cap, contour_ends, contour_cap);
    } else {
        return ttf_glyf_composite_outline(f, glyf_off, data, avail,
                                          pts, pt_cap, contour_ends, contour_cap, depth);
    }
}

/* -------------------------------------------------------------------------- */
/* Public outline API                                                         */
/* -------------------------------------------------------------------------- */

int ui_font_ttf_glyph_outline(ui_font_ttf_t *f, uint16_t gid,
                              ui_font_point_t *pts, size_t pt_cap,
                              uint16_t *contour_ends, size_t contour_cap) {
    if (!f || gid >= f->num_glyphs) return -1;
    if (!ttf_has_table(f, f->off_glyf) || !ttf_has_table(f, f->off_loca)) return -1;

    uint32_t g_off, g_next;
    if (f->index_to_loc_format == 0) {
        const uint8_t *loca = ttf_ptr(f, f->off_loca + (size_t)gid * 2, 4);
        if (!loca) return -1;
        g_off  = (uint32_t)ttf_be16(loca) * 2;
        g_next = (uint32_t)ttf_be16(loca + 2) * 2;
    } else {
        const uint8_t *loca = ttf_ptr(f, f->off_loca + (size_t)gid * 4, 8);
        if (!loca) return -1;
        g_off  = ttf_be32(loca);
        g_next = ttf_be32(loca + 4);
    }
    if (g_off == g_next) return 0; /* empty glyph */
    const uint8_t *gd = ttf_ptr(f, f->off_glyf + g_off, g_next - g_off);
    if (!gd) return -1;
    return ttf_glyf_outline_internal(f, f->off_glyf + g_off, gd, g_next - g_off,
                                     pts, pt_cap, contour_ends, contour_cap, 0);
}

/* -------------------------------------------------------------------------- */
/* OTF/CFF loader (appended)                                                  */
/* -------------------------------------------------------------------------- */

#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} cff_buf_t;

static uint8_t cff_r8(cff_buf_t *b) {
    return (b->pos < b->len) ? b->data[b->pos++] : 0;
}

static uint16_t cff_r16(cff_buf_t *b) {
    uint8_t a = cff_r8(b);
    uint8_t c = cff_r8(b);
    return (uint16_t)((a << 8) | c);
}

typedef struct {
    uint16_t count;
    uint8_t offsize;
    const uint8_t *offsets;
    const uint8_t *data;
    size_t data_len;
} cff_index_t;

static bool cff_parse_index(cff_buf_t *b, cff_index_t *idx) {
    if (b->pos + 2 > b->len) return false;
    idx->count = cff_r16(b);
    if (idx->count == 0) {
        idx->offsets = NULL; idx->data = NULL; idx->data_len = 0;
        return true;
    }
    if (b->pos + 1 > b->len) return false;
    idx->offsize = cff_r8(b);
    size_t off_bytes = (size_t)(idx->count + 1) * idx->offsize;
    if (b->pos + off_bytes > b->len) return false;
    idx->offsets = b->data + b->pos;
    b->pos += off_bytes;
    uint32_t last_off = 0;
    switch (idx->offsize) {
    case 1: last_off = idx->offsets[idx->count]; break;
    case 2: last_off = (idx->offsets[idx->count * 2 - 2] << 8) |
                        idx->offsets[idx->count * 2 - 1]; break;
    case 4: last_off = ((uint32_t)idx->offsets[idx->count * 4 - 4] << 24) |
                       ((uint32_t)idx->offsets[idx->count * 4 - 3] << 16) |
                       ((uint32_t)idx->offsets[idx->count * 4 - 2] << 8) |
                       idx->offsets[idx->count * 4 - 1]; break;
    default: return false;
    }
    if (b->pos + last_off - 1 > b->len) return false;
    idx->data = b->data + b->pos;
    idx->data_len = last_off - 1;
    b->pos += last_off - 1;
    return true;
}

static uint32_t cff_index_offset(const cff_index_t *idx, uint16_t i) {
    if (i > idx->count) return 0;
    switch (idx->offsize) {
    case 1: return idx->offsets[i];
    case 2: return (idx->offsets[i * 2] << 8) | idx->offsets[i * 2 + 1];
    case 4: return ((uint32_t)idx->offsets[i * 4] << 24) |
                   ((uint32_t)idx->offsets[i * 4 + 1] << 16) |
                   ((uint32_t)idx->offsets[i * 4 + 2] << 8) |
                   idx->offsets[i * 4 + 3];
    }
    return 0;
}

static cff_buf_t cff_index_get(const cff_index_t *idx, uint16_t i) {
    cff_buf_t r = {0};
    if (i >= idx->count) return r;
    uint32_t start = cff_index_offset(idx, i);
    uint32_t end = cff_index_offset(idx, i + 1);
    r.data = idx->data + start - 1;
    r.len = end - start;
    return r;
}

static bool cff_read_operand(cff_buf_t *b, float *out) {
    if (b->pos >= b->len) return false;
    uint8_t b0 = b->data[b->pos];
    if (b0 >= 32 && b0 <= 246) { *out = (float)(b0 - 139); b->pos++; return true; }
    if (b0 >= 247 && b0 <= 250) {
        if (b->pos + 1 >= b->len) return false;
        *out = (float)((b0 - 247) * 256 + b->data[b->pos + 1] + 108);
        b->pos += 2; return true;
    }
    if (b0 >= 251 && b0 <= 254) {
        if (b->pos + 1 >= b->len) return false;
        *out = (float)(-(b0 - 251) * 256 - b->data[b->pos + 1] - 108);
        b->pos += 2; return true;
    }
    if (b0 == 28) {
        if (b->pos + 2 >= b->len) return false;
        int16_t v = (int16_t)((b->data[b->pos + 1] << 8) | b->data[b->pos + 2]);
        *out = (float)v; b->pos += 3; return true;
    }
    if (b0 == 29) {
        if (b->pos + 4 >= b->len) return false;
        int32_t v = ((int32_t)b->data[b->pos + 1] << 24) |
                    ((int32_t)b->data[b->pos + 2] << 16) |
                    ((int32_t)b->data[b->pos + 3] << 8) |
                    (int32_t)b->data[b->pos + 4];
        *out = (float)v; b->pos += 5; return true;
    }
    if (b0 == 30) {
        b->pos++;
        while (b->pos < b->len) {
            uint8_t nib = b->data[b->pos];
            if ((nib & 0xF0) == 0xF0 || (nib & 0x0F) == 0x0F) { b->pos++; break; }
            b->pos++;
        }
        *out = 0.0f; return true;
    }
    return false;
}

typedef struct {
    uint32_t charstrings_offset;
    uint32_t charset_offset;
    uint32_t private_size;
    uint32_t private_offset;
} cff_top_dict_t;

static void cff_parse_top_dict(const uint8_t *data, size_t len, cff_top_dict_t *dict) {
    memset(dict, 0, sizeof(*dict));
    cff_buf_t b = { data, len, 0 };
    float stack[48];
    int sp = 0;
    while (b.pos < b.len) {
        uint8_t op = cff_r8(&b);
        if (op <= 21) {
            if (op == 12) { if (b.pos >= b.len) break; op = 1200 + cff_r8(&b); }
            switch (op) {
            case 17: if (sp > 0) dict->charstrings_offset = (uint32_t)stack[sp - 1]; break;
            case 15: if (sp > 0) dict->charset_offset = (uint32_t)stack[sp - 1]; break;
            case 18: if (sp >= 2) { dict->private_size = (uint32_t)stack[sp - 2]; dict->private_offset = (uint32_t)stack[sp - 1]; } break;
            }
            sp = 0;
        } else if (op >= 22 && op <= 27) {
            sp = 0;
        } else if (op == 28 || op == 29 || op == 30 || (op >= 32 && op <= 254)) {
            b.pos--;
            float v;
            if (cff_read_operand(&b, &v) && sp < 48) stack[sp++] = v;
        }
    }
}

static uint32_t sfnt_tag(const char *s) {
    return ((uint32_t)(unsigned char)s[0] << 24) |
           ((uint32_t)(unsigned char)s[1] << 16) |
           ((uint32_t)(unsigned char)s[2] << 8) |
           (uint32_t)(unsigned char)s[3];
}

bool ui_font_load_otf(const uint8_t *data, size_t len, ui_font_ttf_t *out) {
    if (!data || len < 4 || !out) return false;
    uint32_t sfnt_version = ((uint32_t)data[0] << 24) |
                            ((uint32_t)data[1] << 16) |
                            ((uint32_t)data[2] << 8) |
                            (uint32_t)data[3];
    bool is_cff = (sfnt_version == 0x4F54544F);
    bool is_ttf = (sfnt_version == 0x00010000);
    if (!is_cff && !is_ttf) return false;

    uint16_t num_tables = (uint16_t)((data[4] << 8) | data[5]);
    if (len < 12u + (size_t)num_tables * 16u) return false;

    const uint8_t *table_dir = data + 12;
    const uint8_t *cff_data = NULL;
    size_t cff_len = 0;
    const uint8_t *head_data = NULL;
    const uint8_t *hhea_data = NULL;
    const uint8_t *hmtx_data = NULL;
    size_t hmtx_len = 0;
    const uint8_t *maxp_data = NULL;

    for (uint16_t i = 0; i < num_tables; i++) {
        const uint8_t *entry = table_dir + i * 16;
        uint32_t tag = sfnt_tag((const char *)entry);
        uint32_t off = ((uint32_t)entry[8] << 24) | ((uint32_t)entry[9] << 16) |
                       ((uint32_t)entry[10] << 8) | (uint32_t)entry[11];
        uint32_t tlen = ((uint32_t)entry[12] << 24) | ((uint32_t)entry[13] << 16) |
                        ((uint32_t)entry[14] << 8) | (uint32_t)entry[15];
        if (off + tlen > len) continue;
        if (tag == sfnt_tag("CFF ")) { cff_data = data + off; cff_len = tlen; }
        else if (tag == sfnt_tag("head")) { head_data = data + off; }
        else if (tag == sfnt_tag("hhea")) { hhea_data = data + off; }
        else if (tag == sfnt_tag("hmtx")) { hmtx_data = data + off; hmtx_len = tlen; }
        else if (tag == sfnt_tag("maxp")) { maxp_data = data + off; }
    }

    memset(out, 0, sizeof(*out));
    out->data = data;
    out->len = len;

    if (!head_data) return false;
    out->off_head = (uint32_t)(head_data - data);
    out->units_per_em = (uint16_t)((head_data[18] << 8) | head_data[19]);

    if (!hhea_data) return false;
    out->off_hhea = (uint32_t)(hhea_data - data);
    out->num_hmetrics = (uint16_t)((hhea_data[34] << 8) | hhea_data[35]);

    if (!maxp_data) return false;
    out->off_maxp = (uint32_t)(maxp_data - data);
    out->num_glyphs = (uint16_t)((maxp_data[4] << 8) | maxp_data[5]);

    if (hmtx_data && hmtx_len >= (size_t)out->num_hmetrics * 4) {
        out->off_hmtx = (uint32_t)(hmtx_data - data);
    }

    if (cff_data && cff_len > 0) {
        cff_buf_t cb = { cff_data, cff_len, 0 };
        uint8_t major = cff_r8(&cb);
        uint8_t minor = cff_r8(&cb);
        uint8_t hdr_size = cff_r8(&cb);
        uint8_t offsize = cff_r8(&cb);
        (void)major; (void)minor; (void)offsize;
        cb.pos = hdr_size;

        cff_index_t name_index, top_dict_index, string_index, global_subr_index;
        if (!cff_parse_index(&cb, &name_index)) return false;
        if (!cff_parse_index(&cb, &top_dict_index)) return false;
        if (!cff_parse_index(&cb, &string_index)) return false;
        if (!cff_parse_index(&cb, &global_subr_index)) return false;

        cff_buf_t top_dict_buf = cff_index_get(&top_dict_index, 0);
        cff_top_dict_t top_dict;
        cff_parse_top_dict(top_dict_buf.data, top_dict_buf.len, &top_dict);

        if (top_dict.charstrings_offset == 0 ||
            top_dict.charstrings_offset >= cff_len) return false;
        cff_buf_t charstrings_buf = { cff_data + top_dict.charstrings_offset,
                                      cff_len - top_dict.charstrings_offset, 0 };
        cff_index_t charstrings_index;
        if (!cff_parse_index(&charstrings_buf, &charstrings_index)) return false;

        out->num_glyphs = charstrings_index.count;
        out->cff_data = (uint8_t *)malloc(cff_len);
        if (out->cff_data) {
            memcpy(out->cff_data, cff_data, cff_len);
            out->cff_len = cff_len;
            out->cff_charstrings_offset = top_dict.charstrings_offset;
        }
    }

    return true;
}
