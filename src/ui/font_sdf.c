/*
 * ORDL UI — Signed Distance Field rasterizer from scratch
 * Pure C23, zero external dependencies.
 *
 * Converts TrueType glyph outlines into 8-bit grayscale SDF buffers.
 * Quadratic beziers are subdivided into line segments.
 * Inside/outside determined by horizontal ray-cast (even-odd rule).
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* -------------------------------------------------------------------------- */
/* Line segment list (temporary)                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    float x0, y0, x1, y1;
} ui_sdf_seg_t;

static inline float sdf_min(float a, float b) { return a < b ? a : b; }
static inline float sdf_max(float a, float b) { return a > b ? a : b; }
static inline float sdf_clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Distance from point (px,py) to segment (x0,y0)-(x1,y1) */
static float sdf_dist_to_seg(float px, float py, float x0, float y0, float x1, float y1) {
    float abx = x1 - x0;
    float aby = y1 - y0;
    float apx = px - x0;
    float apy = py - y0;
    float ab2 = abx * abx + aby * aby;
    float t = (ab2 > 0.0f) ? (apx * abx + apy * aby) / ab2 : 0.0f;
    t = sdf_clamp(t, 0.0f, 1.0f);
    float cx = x0 + t * abx;
    float cy = y0 + t * aby;
    float dx = px - cx;
    float dy = py - cy;
    return sqrtf(dx * dx + dy * dy);
}

/* Subdivide a quadratic bezier into line segments using De Casteljau.
 * Returns number of segments written (up to max_seg). */
static size_t sdf_subdivide_quad(float x0, float y0, float xc, float yc, float x1, float y1,
                                 ui_sdf_seg_t *segs, size_t max_seg, int depth) {
    if (depth >= 6 || max_seg == 0) {
        segs[0] = (ui_sdf_seg_t){x0, y0, x1, y1};
        return 1;
    }
    /* Flatness test: deviation of control point from chord midpoint */
    float mx = (x0 + x1) * 0.5f;
    float my = (y0 + y1) * 0.5f;
    float dx = xc - mx;
    float dy = yc - my;
    if (dx * dx + dy * dy < 0.25f) {
        segs[0] = (ui_sdf_seg_t){x0, y0, x1, y1};
        return 1;
    }
    /* Split */
    float x01 = (x0 + xc) * 0.5f, y01 = (y0 + yc) * 0.5f;
    float x12 = (xc + x1) * 0.5f, y12 = (yc + y1) * 0.5f;
    float x012 = (x01 + x12) * 0.5f, y012 = (y01 + y12) * 0.5f;
    size_t n1 = sdf_subdivide_quad(x0, y0, x01, y01, x012, y012, segs, max_seg, depth + 1);
    size_t n2 = sdf_subdivide_quad(x012, y012, x12, y12, x1, y1, segs + n1, max_seg - n1, depth + 1);
    return n1 + n2;
}

/* Build segment list from extracted outline.
 * Handles implicit on-curve points between consecutive off-curve points. */
static size_t sdf_build_segments(const ui_font_point_t *pts, const uint16_t *contour_ends, int num_contours,
                                 ui_sdf_seg_t *segs, size_t max_seg) {
    size_t seg_count = 0;
    size_t start = 0;
    for (int c = 0; c < num_contours; c++) {
        size_t end = contour_ends[c];
        size_t count = end - start + 1;
        if (count < 2) { start = end + 1; continue; }

        /* Build expanded array with implicit on-curve midpoints inserted */
        float qx[1024], qy[1024];
        bool  qon[1024];
        size_t qn = 0;
        for (size_t i = 0; i < count && qn < 1020; i++) {
            size_t curr = start + i;
            size_t prev = start + (i + count - 1) % count;
            if (!pts[prev].on_curve && !pts[curr].on_curve) {
                qx[qn] = (pts[prev].x + pts[curr].x) * 0.5f;
                qy[qn] = (pts[prev].y + pts[curr].y) * 0.5f;
                qon[qn] = true;
                qn++;
            }
            qx[qn] = pts[curr].x;
            qy[qn] = pts[curr].y;
            qon[qn] = pts[curr].on_curve;
            qn++;
        }
        if (qn < 2) { start = end + 1; continue; }

        /* Walk expanded array, emitting lines and quadratic beziers */
        for (size_t i = 0; i < qn; ) {
            if (!qon[i]) { i++; continue; }
            size_t i0 = i;
            size_t i1 = (i + 1) % qn;
            if (qon[i1]) {
                /* Line segment to next on-curve point */
                if (seg_count < max_seg) {
                    segs[seg_count++] = (ui_sdf_seg_t){qx[i0], qy[i0], qx[i1], qy[i1]};
                }
                i++;
            } else {
                /* Quadratic bezier: on-curve, off-curve, on-curve */
                size_t i2 = (i1 + 1) % qn;
                if (seg_count < max_seg) {
                    size_t added = sdf_subdivide_quad(qx[i0], qy[i0], qx[i1], qy[i1], qx[i2], qy[i2],
                                                      segs + seg_count, max_seg - seg_count, 0);
                    seg_count += added;
                }
                i += 2;
            }
        }
        start = end + 1;
    }
    return seg_count;
}

/* Ray-cast: count horizontal ray crossings to the right of (px,py).
 * Even = outside, odd = inside. */
static bool sdf_point_inside(float px, float py, const ui_sdf_seg_t *segs, size_t n) {
    int crossings = 0;
    for (size_t i = 0; i < n; i++) {
        float x0 = segs[i].x0, y0 = segs[i].y0;
        float x1 = segs[i].x1, y1 = segs[i].y1;
        /* Check if segment straddles horizontal line at py */
        bool cond = (y0 <= py && y1 > py) || (y1 <= py && y0 > py);
        if (!cond) continue;
        /* Compute x intersection */
        float dy = y1 - y0;
        if (dy == 0.0f) continue;
        float ix = x0 + (py - y0) * (x1 - x0) / dy;
        if (ix > px) crossings++;
    }
    return (crossings & 1) != 0;
}

/* -------------------------------------------------------------------------- */
/* Public: render single glyph SDF                                            */
/* -------------------------------------------------------------------------- */

bool ui_font_sdf_render(ui_font_ttf_t *f, uint16_t gid, int px_size,
                        uint8_t *out, int out_w, int out_h, int pad) {
    if (!f || !out || out_w <= 0 || out_h <= 0 || px_size <= 0) return false;

    ui_font_glyph_metrics_t m;
    if (!ui_font_ttf_glyph_metrics(f, gid, &m)) return false;
    if (m.x_max <= m.x_min || m.y_max <= m.y_min) {
        memset(out, 0, (size_t)out_w * (size_t)out_h);
        return true; /* empty glyph */
    }

    /* Extract outline */
    ui_font_point_t *pts = malloc(1024 * sizeof(ui_font_point_t));
    uint16_t *contour_ends = malloc(64 * sizeof(uint16_t));
    if (!pts || !contour_ends) { free(pts); free(contour_ends); return false; }
    int nc = ui_font_ttf_glyph_outline(f, gid, pts, 1024, contour_ends, 64);
    if (nc < 0) { free(pts); free(contour_ends); return false; }
    if (nc == 0) {
        free(pts); free(contour_ends);
        memset(out, 0, (size_t)out_w * (size_t)out_h);
        return true;
    }

    /* Build segments */
    ui_sdf_seg_t *segs = malloc(2048 * sizeof(ui_sdf_seg_t));
    if (!segs) { free(pts); free(contour_ends); return false; }
    size_t nseg = sdf_build_segments(pts, contour_ends, nc, segs, 2048);
    if (nseg == 0) {
        free(pts); free(contour_ends); free(segs);
        memset(out, 0, (size_t)out_w * (size_t)out_h);
        return true;
    }

    /* Scale from font units to pixels */
    float upem = (float)ui_font_ttf_units_per_em(f);
    if (upem <= 0.0f) return false;
    float scale = (float)px_size / upem;

    /* Compute bbox in pixels */
    float bx0 = (float)m.x_min * scale;
    float by0 = (float)m.y_min * scale;
    float bx1 = (float)m.x_max * scale;
    float by1 = (float)m.y_max * scale;
    float gw = bx1 - bx0;
    float gh = by1 - by0;

    /* Center glyph in output buffer with padding */
    float offset_x = ((float)out_w - gw) * 0.5f - bx0 + (float)pad;
    float offset_y = ((float)out_h - gh) * 0.5f - by0 + (float)pad;

    /* Normalize distance: one pixel in SDF space corresponds to this many font units */
    float dist_scale = 4.0f; /* tune: higher = sharper transitions */

    for (int py = 0; py < out_h; py++) {
        for (int px = 0; px < out_w; px++) {
            float fx = (float)px - offset_x;
            float fy = (float)py - offset_y;
            /* Map back to font-unit space for distance computation */
            float fux = fx / scale;
            float fuy = fy / scale;

            float min_dist = 1e9f;
            for (size_t i = 0; i < nseg; i++) {
                float d = sdf_dist_to_seg(fux, fuy, segs[i].x0, segs[i].y0, segs[i].x1, segs[i].y1);
                if (d < min_dist) min_dist = d;
            }

            bool inside = sdf_point_inside(fux, fuy, segs, nseg);
            float signed_dist = inside ? -min_dist : min_dist;

            /* Map to 8-bit: 128 = zero distance (on curve), <128 = outside, >128 = inside */
            float v = 128.0f + signed_dist * scale * dist_scale;
            int iv = (int)(v + 0.5f);
            if (iv < 0) iv = 0;
            if (iv > 255) iv = 255;
            out[py * out_w + px] = (uint8_t)iv;
        }
    }
    free(pts); free(contour_ends); free(segs);
    return true;
}

/* -------------------------------------------------------------------------- */
/* Public: build packed SDF atlas (RGBA8888)                                  */
/* -------------------------------------------------------------------------- */

uint32_t *ui_font_sdf_atlas_build(ui_font_ttf_t *f,
                                  const uint32_t *codepoints, size_t n,
                                  int px_size, int pad,
                                  int *out_cols, int *out_rows,
                                  ui_rect_t *glyph_positions) {
    if (!f || !codepoints || n == 0 || !out_cols || !out_rows || !glyph_positions) return NULL;

    /* Determine atlas grid size */
    int cell = px_size + pad * 2;
    int cols = 1;
    while ((size_t)cols * (size_t)cols < n) cols++;
    int rows = (int)((n + (size_t)cols - 1) / (size_t)cols);

    int atlas_w = cols * cell;
    int atlas_h = rows * cell;
    uint32_t *atlas = calloc((size_t)atlas_w * (size_t)atlas_h, sizeof(uint32_t));
    if (!atlas) return NULL;

    uint8_t *sdf = calloc((size_t)cell * (size_t)cell, 1);
    if (!sdf) { free(atlas); return NULL; }

    for (size_t i = 0; i < n; i++) {
        int col = (int)(i % (size_t)cols);
        int row = (int)(i / (size_t)cols);
        int x0 = col * cell;
        int y0 = row * cell;

        uint16_t gid = ui_font_ttf_glyph_index(f, codepoints[i]);
        bool has_glyph = ui_font_sdf_render(f, gid, px_size, sdf, cell, cell, pad);

        if (has_glyph) {
            for (int y = 0; y < cell; y++) {
                for (int x = 0; x < cell; x++) {
                    uint8_t v = sdf[y * cell + x];
                    atlas[(y0 + y) * atlas_w + (x0 + x)] =
                        ((uint32_t)0xFF << 24) | ((uint32_t)v << 16) | ((uint32_t)v << 8) | (uint32_t)v;
                }
            }
        }

        glyph_positions[i] = (ui_rect_t){ x0, y0, cell, cell };
    }

    free(sdf);
    *out_cols = cols;
    *out_rows = rows;
    return atlas;
}
