/*
 * src/physics/collision.c — Pixel-perfect 2D collision detection
 *
 * Bitmask-based. 1 bit per pixel, packed into 64-bit words.
 * Row-major: word index = (y * row_words) + (x / 64), bit = x % 64.
 */

#include "forge/collision.h"
#include "forge/memory.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Bitmask                                                                    */
/* -------------------------------------------------------------------------- */

static inline size_t bitmask_row_words(int w) {
    return (size_t)((w + 63) / 64);
}

static inline size_t bitmask_words(int w, int h) {
    return bitmask_row_words(w) * (size_t)h;
}

static inline bool bm_get(const uint64_t *bits, int x, int y, int row_words) {
    size_t wi = (size_t)y * (size_t)row_words + (size_t)(x >> 6);
    uint64_t mask = 1ULL << (x & 63);
    return (bits[wi] & mask) != 0;
}

static inline void bm_set(uint64_t *bits, int x, int y, int row_words) {
    size_t wi = (size_t)y * (size_t)row_words + (size_t)(x >> 6);
    bits[wi] |= 1ULL << (x & 63);
}

static void bitmask_build_mip(fge_bitmask_t *bm) {
    if (!bm || !bm->bits) return;
    int mw = bm->mip_width;
    int mh = bm->mip_height;
    if (mw <= 0 || mh <= 0) return;
    size_t mrw = bitmask_row_words(mw);
    size_t mnw = mrw * (size_t)mh;
    bm->mip_bits = (uint64_t *)FGE_CALLOC(mnw, sizeof(uint64_t));
    if (!bm->mip_bits) return;
    /* Each mip bit covers 4x4 pixels */
    for (int my = 0; my < mh; my++) {
        for (int mx = 0; mx < mw; mx++) {
            bool solid = false;
            for (int dy = 0; dy < 4 && !solid; dy++) {
                for (int dx = 0; dx < 4 && !solid; dx++) {
                    int px = mx * 4 + dx;
                    int py = my * 4 + dy;
                    if (px < bm->width && py < bm->height &&
                        bm_get(bm->bits, px, py, (int)bitmask_row_words(bm->width))) {
                        solid = true;
                    }
                }
            }
            if (solid) bm_set(bm->mip_bits, mx, my, (int)mrw);
        }
    }
}

bool fge_bitmask_from_texture(fge_bitmask_t *bm, const fge_texture_t *tex, uint8_t alpha_threshold) {
    if (!bm || !tex || !tex->pixels || tex->width <= 0 || tex->height <= 0) return false;
    int w = tex->width;
    int h = tex->height;
    size_t rw = bitmask_row_words(w);
    size_t nw = rw * (size_t)h;
    uint64_t *bits = (uint64_t *)FGE_CALLOC(nw, sizeof(uint64_t));
    if (!bits) return false;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint32_t px = tex->pixels[y * w + x];
            uint8_t a = (uint8_t)(px >> 24);
            if (a >= alpha_threshold) {
                bm_set(bits, x, y, (int)rw);
            }
        }
    }
    bm->bits = bits;
    bm->width = w;
    bm->height = h;
    bm->mip_width = (w + 3) / 4;
    bm->mip_height = (h + 3) / 4;
    bm->origin = fge_v2(0.0f, 0.0f);
    bitmask_build_mip(bm);
    return true;
}

bool fge_bitmask_from_alpha(fge_bitmask_t *bm, const uint8_t *alpha, int w, int h) {
    if (!bm || !alpha || w <= 0 || h <= 0) return false;
    size_t rw = bitmask_row_words(w);
    size_t nw = rw * (size_t)h;
    uint64_t *bits = (uint64_t *)FGE_CALLOC(nw, sizeof(uint64_t));
    if (!bits) return false;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (alpha[y * w + x] > 0) {
                bm_set(bits, x, y, (int)rw);
            }
        }
    }
    bm->bits = bits;
    bm->width = w;
    bm->height = h;
    bm->mip_width = (w + 3) / 4;
    bm->mip_height = (h + 3) / 4;
    bm->origin = fge_v2(0.0f, 0.0f);
    bitmask_build_mip(bm);
    return true;
}

void fge_bitmask_free(fge_bitmask_t *bm) {
    if (!bm) return;
    FGE_FREE(bm->bits);
    FGE_FREE(bm->mip_bits);
    bm->bits = NULL;
    bm->mip_bits = NULL;
    bm->width = bm->height = bm->mip_width = bm->mip_height = 0;
}

bool fge_bitmask_get(const fge_bitmask_t *bm, int x, int y) {
    if (!bm || !bm->bits || x < 0 || x >= bm->width || y < 0 || y >= bm->height) return false;
    return bm_get(bm->bits, x, y, (int)bitmask_row_words(bm->width));
}

/* -------------------------------------------------------------------------- */
/* Collider AABB                                                              */
/* -------------------------------------------------------------------------- */

static void transform_local_to_world(const fge_collider_t *c,
                                     float lx, float ly,
                                     float *wx, float *wy) {
    /* Apply origin offset, scale, rotation, position */
    float sx = (lx - c->mask.origin.x) * c->scale;
    float sy = (ly - c->mask.origin.y) * c->scale;
    if (c->rotation != 0.0f) {
        float co = fge_cosf(c->rotation);
        float sn = fge_sinf(c->rotation);
        float rx = sx * co - sy * sn;
        float ry = sx * sn + sy * co;
        sx = rx; sy = ry;
    }
    *wx = c->pos.x + sx;
    *wy = c->pos.y + sy;
}

static void transform_world_to_local(const fge_collider_t *c,
                                     float wx, float wy,
                                     float *lx, float *ly) {
    float dx = wx - c->pos.x;
    float dy = wy - c->pos.y;
    if (c->rotation != 0.0f) {
        float co = fge_cosf(c->rotation);
        float sn = fge_sinf(c->rotation);
        /* inverse rotation */
        float rx = dx * co + dy * sn;
        float ry = -dx * sn + dy * co;
        dx = rx; dy = ry;
    }
    *lx = dx / c->scale + c->mask.origin.x;
    *ly = dy / c->scale + c->mask.origin.y;
}

void fge_collider_update(fge_collider_t *c) {
    if (!c) return;
    /* Compute AABB by transforming the 4 corners of the local bbox */
    float hw = c->mask.width  * c->scale * 0.5f;
    float hh = c->mask.height * c->scale * 0.5f;
    float ox = c->mask.origin.x * c->scale;
    float oy = c->mask.origin.y * c->scale;

    float corners_x[4] = { -ox,     c->mask.width * c->scale - ox,
                           -ox,     c->mask.width * c->scale - ox };
    float corners_y[4] = { -oy,     -oy,
                           c->mask.height * c->scale - oy,
                           c->mask.height * c->scale - oy };

    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    if (c->rotation == 0.0f) {
        minx = c->pos.x - ox;
        miny = c->pos.y - oy;
        maxx = c->pos.x + c->mask.width  * c->scale - ox;
        maxy = c->pos.y + c->mask.height * c->scale - oy;
    } else {
        float co = fge_cosf(c->rotation);
        float sn = fge_sinf(c->rotation);
        for (int i = 0; i < 4; ++i) {
            float wx = c->pos.x + corners_x[i] * co - corners_y[i] * sn;
            float wy = c->pos.y + corners_x[i] * sn + corners_y[i] * co;
            if (wx < minx) minx = wx;
            if (wy < miny) miny = wy;
            if (wx > maxx) maxx = wx;
            if (wy > maxy) maxy = wy;
        }
    }
    c->aabb_minx = minx; c->aabb_miny = miny;
    c->aabb_maxx = maxx; c->aabb_maxy = maxy;
}

bool fge_collider_aabb_overlap(const fge_collider_t *a, const fge_collider_t *b) {
    if (!a || !b) return false;
    return !(a->aabb_maxx < b->aabb_minx || a->aabb_minx > b->aabb_maxx ||
             a->aabb_maxy < b->aabb_miny || a->aabb_miny > b->aabb_maxy);
}

/* -------------------------------------------------------------------------- */
/* Pixel-perfect hit test                                                     */
/* -------------------------------------------------------------------------- */

bool fge_collider_hit_test(const fge_collider_t *a, const fge_collider_t *b) {
    if (!a || !b || !a->mask.bits || !b->mask.bits) return false;
    if (!fge_collider_aabb_overlap(a, b)) return false;

    /* Overlap region in world space */
    float ominx = FGE_MAX(a->aabb_minx, b->aabb_minx);
    float ominy = FGE_MAX(a->aabb_miny, b->aabb_miny);
    float omaxx = FGE_MIN(a->aabb_maxx, b->aabb_maxx);
    float omaxy = FGE_MIN(a->aabb_maxy, b->aabb_maxy);

    /* Step size: use the smaller scale for resolution */
    float step = FGE_MIN(a->scale, b->scale);
    if (step <= 0.0f) step = 1.0f;

    int row_words_a = (int)bitmask_row_words(a->mask.width);
    int row_words_b = (int)bitmask_row_words(b->mask.width);

    /* Fast path: if both have mip masks, do coarse 4x4 block test first */
    if (a->mask.mip_bits && b->mask.mip_bits) {
        int mrw_a = (int)bitmask_row_words(a->mask.mip_width);
        int mrw_b = (int)bitmask_row_words(b->mask.mip_width);
        float mip_step = step * 4.0f; /* mip pixels are 4x larger */

        for (float wy = ominy; wy <= omaxy; wy += mip_step) {
            for (float wx = ominx; wx <= omaxx; wx += mip_step) {
                float lax, lay, lbx, lby;
                transform_world_to_local(a, wx, wy, &lax, &lay);
                transform_world_to_local(b, wx, wy, &lbx, &lby);

                /* Mip coordinates (1/4 resolution) */
                int mx_a = (int)fge_floorf(lax * 0.25f);
                int my_a = (int)fge_floorf(lay * 0.25f);
                int mx_b = (int)fge_floorf(lbx * 0.25f);
                int my_b = (int)fge_floorf(lby * 0.25f);

                if (mx_a >= 0 && mx_a < a->mask.mip_width && my_a >= 0 && my_a < a->mask.mip_height &&
                    mx_b >= 0 && mx_b < b->mask.mip_width && my_b >= 0 && my_b < b->mask.mip_height) {
                    bool solid_a = bm_get(a->mask.mip_bits, mx_a, my_a, mrw_a);
                    bool solid_b = bm_get(b->mask.mip_bits, mx_b, my_b, mrw_b);
                    if (solid_a && solid_b) {
                        /* Refine: check 4x4 block at full resolution */
                        float block_minx = wx;
                        float block_miny = wy;
                        float block_maxx = wx + mip_step;
                        float block_maxy = wy + mip_step;
                        if (block_maxx > omaxx) block_maxx = omaxx;
                        if (block_maxy > omaxy) block_maxy = omaxy;
                        for (float fy = block_miny; fy <= block_maxy; fy += step) {
                            for (float fx = block_minx; fx <= block_maxx; fx += step) {
                                float flax, flay, flbx, flby;
                                transform_world_to_local(a, fx, fy, &flax, &flay);
                                transform_world_to_local(b, fx, fy, &flbx, &flby);
                                int ix_a = (int)fge_roundf(flax);
                                int iy_a = (int)fge_roundf(flay);
                                int ix_b = (int)fge_roundf(flbx);
                                int iy_b = (int)fge_roundf(flby);
                                if (ix_a >= 0 && ix_a < a->mask.width && iy_a >= 0 && iy_a < a->mask.height &&
                                    ix_b >= 0 && ix_b < b->mask.width && iy_b >= 0 && iy_b < b->mask.height) {
                                    bool sa = bm_get(a->mask.bits, ix_a, iy_a, row_words_a);
                                    bool sb = bm_get(b->mask.bits, ix_b, iy_b, row_words_b);
                                    if (sa && sb) return true;
                                }
                            }
                        }
                    }
                }
            }
        }
        return false;
    }

    /* Fallback: full-resolution walk */
    for (float wy = ominy; wy <= omaxy; wy += step) {
        for (float wx = ominx; wx <= omaxx; wx += step) {
            float lax, lay, lbx, lby;
            transform_world_to_local(a, wx, wy, &lax, &lay);
            transform_world_to_local(b, wx, wy, &lbx, &lby);

            int ix_a = (int)fge_roundf(lax);
            int iy_a = (int)fge_roundf(lay);
            int ix_b = (int)fge_roundf(lbx);
            int iy_b = (int)fge_roundf(lby);

            if (ix_a >= 0 && ix_a < a->mask.width && iy_a >= 0 && iy_a < a->mask.height &&
                ix_b >= 0 && ix_b < b->mask.width && iy_b >= 0 && iy_b < b->mask.height) {
                bool solid_a = bm_get(a->mask.bits, ix_a, iy_a, row_words_a);
                bool solid_b = bm_get(b->mask.bits, ix_b, iy_b, row_words_b);
                if (solid_a && solid_b) return true;
            }
        }
    }
    return false;
}

bool fge_collider_contact(const fge_collider_t *a, const fge_collider_t *b,
                          fge_vec2_t *out_normal, float *out_penetration,
                          fge_vec2_t *out_contact_point) {
    if (!a || !b || !a->mask.bits || !b->mask.bits) return false;
    if (!fge_collider_aabb_overlap(a, b)) return false;

    float ominx = FGE_MAX(a->aabb_minx, b->aabb_minx);
    float ominy = FGE_MAX(a->aabb_miny, b->aabb_miny);
    float omaxx = FGE_MIN(a->aabb_maxx, b->aabb_maxx);
    float omaxy = FGE_MIN(a->aabb_maxy, b->aabb_maxy);

    float step = FGE_MIN(a->scale, b->scale);
    if (step <= 0.0f) step = 1.0f;

    int row_words_a = (int)bitmask_row_words(a->mask.width);
    int row_words_b = (int)bitmask_row_words(b->mask.width);

    double sum_x = 0.0, sum_y = 0.0;
    uint32_t overlap_count = 0;
    float min_proj = 1e9f, max_proj = -1e9f;

    fge_vec2_t n_dir = fge_v2_sub(b->pos, a->pos);
    if (fge_v2_len2(n_dir) < FGE_EPSILON_F) {
        n_dir = fge_v2(1.0f, 0.0f);
    } else {
        n_dir = fge_v2_norm(n_dir);
    }

    for (float wy = ominy; wy <= omaxy; wy += step) {
        for (float wx = ominx; wx <= omaxx; wx += step) {
            float lax, lay, lbx, lby;
            transform_world_to_local(a, wx, wy, &lax, &lay);
            transform_world_to_local(b, wx, wy, &lbx, &lby);

            int ix_a = (int)fge_roundf(lax);
            int iy_a = (int)fge_roundf(lay);
            int ix_b = (int)fge_roundf(lbx);
            int iy_b = (int)fge_roundf(lby);

            if (ix_a >= 0 && ix_a < a->mask.width && iy_a >= 0 && iy_a < a->mask.height &&
                ix_b >= 0 && ix_b < b->mask.width && iy_b >= 0 && iy_b < b->mask.height) {
                bool solid_a = bm_get(a->mask.bits, ix_a, iy_a, row_words_a);
                bool solid_b = bm_get(b->mask.bits, ix_b, iy_b, row_words_b);
                if (solid_a && solid_b) {
                    sum_x += wx;
                    sum_y += wy;
                    overlap_count++;
                    float proj = wx * n_dir.x + wy * n_dir.y;
                    if (proj < min_proj) min_proj = proj;
                    if (proj > max_proj) max_proj = proj;
                }
            }
        }
    }

    if (overlap_count == 0) return false;

    fge_vec2_t cp = fge_v2((float)(sum_x / (double)overlap_count),
                           (float)(sum_y / (double)overlap_count));

    /* Penetration = projection extent of overlap region onto normal */
    float penetration = (max_proj - min_proj);
    if (penetration < step) penetration = step;

    if (out_normal) *out_normal = n_dir;
    if (out_penetration) *out_penetration = penetration;
    if (out_contact_point) *out_contact_point = cp;
    return true;
}

bool fge_collider_contains(const fge_collider_t *c, fge_vec2_t world_pt) {
    if (!c || !c->mask.bits) return false;
    float lx, ly;
    transform_world_to_local(c, world_pt.x, world_pt.y, &lx, &ly);
    int ix = (int)fge_roundf(lx);
    int iy = (int)fge_roundf(ly);
    if (ix < 0 || ix >= c->mask.width || iy < 0 || iy >= c->mask.height) return false;
    return bm_get(c->mask.bits, ix, iy, (int)bitmask_row_words(c->mask.width));
}

/* -------------------------------------------------------------------------- */
/* Ray cast                                                                   */
/* -------------------------------------------------------------------------- */

bool fge_collider_ray_cast(const fge_collider_t *c,
                           fge_vec2_t origin, fge_vec2_t dir,
                           float max_t, float *out_t) {
    if (!c || !c->mask.bits || max_t <= 0.0f) return false;
    /* DDA through local-space mask */
    float lx0, ly0;
    transform_world_to_local(c, origin.x, origin.y, &lx0, &ly0);
    /* Transform dir to local space (rotation only, scale inverse) */
    float ldx = dir.x, ldy = dir.y;
    if (c->rotation != 0.0f) {
        float co = fge_cosf(c->rotation);
        float sn = fge_sinf(c->rotation);
        float rx = ldx * co + ldy * sn;
        float ry = -ldx * sn + ldy * co;
        ldx = rx; ldy = ry;
    }
    ldx /= c->scale; ldy /= c->scale;
    float len = fge_sqrtf(ldx * ldx + ldy * ldy);
    if (len <= 0.0f) return false;
    ldx /= len; ldy /= len;

    /* DDA grid walk */
    int ix = (int)fge_floorf(lx0);
    int iy = (int)fge_floorf(ly0);
    int step_x = (ldx >= 0.0f) ? 1 : -1;
    int step_y = (ldy >= 0.0f) ? 1 : -1;
    float t_max_x = (ldx != 0.0f) ? ((step_x > 0 ? (ix + 1) : ix) - lx0) / ldx : 1e9f;
    float t_max_y = (ldy != 0.0f) ? ((step_y > 0 ? (iy + 1) : iy) - ly0) / ldy : 1e9f;
    float t_delta_x = (ldx != 0.0f) ? (float)step_x / ldx : 1e9f;
    float t_delta_y = (ldy != 0.0f) ? (float)step_y / ldy : 1e9f;
    if (t_delta_x < 0) t_delta_x = -t_delta_x;
    if (t_delta_y < 0) t_delta_y = -t_delta_y;

    int row_words = (int)bitmask_row_words(c->mask.width);
    float max_local_t = max_t / c->scale;

    for (int step = 0; step < 4096; ++step) {
        if (ix >= 0 && ix < c->mask.width && iy >= 0 && iy < c->mask.height) {
            if (bm_get(c->mask.bits, ix, iy, row_words)) {
                float t = (t_max_x < t_max_y) ? t_max_x : t_max_y;
                if (t < 0.0f) t = 0.0f;
                if (out_t) *out_t = t * c->scale;
                return true;
            }
        }
        if (t_max_x < t_max_y) {
            ix += step_x;
            if (t_max_x > max_local_t) break;
            t_max_x += t_delta_x;
        } else {
            iy += step_y;
            if (t_max_y > max_local_t) break;
            t_max_y += t_delta_y;
        }
    }
    return false;
}
