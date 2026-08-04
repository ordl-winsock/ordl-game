/*
 * forge/collision.h — Pixel-perfect 2D collision detection
 *
 * Bitmask-based, 1 bit per pixel. Broad phase = AABB, narrow phase = bit test.
 * Supports arbitrary transform (position, rotation, uniform scale).
 */

#ifndef FORGE_COLLISION_H
#define FORGE_COLLISION_H

#include "forge/core.h"
#include "forge/math.h"
#include "forge/renderer.h"

/* -------------------------------------------------------------------------- */
/* Bitmask collider                                                           */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint64_t *bits;          /* bit-packed: 1=solid, 0=empty, row-major */
    uint64_t *mip_bits;      /* 1/4 resolution pyramid (each bit = 4x4 px) */
    int width, height;       /* dimensions in pixels */
    int mip_width, mip_height; /* mip dimensions (w/4, h/4) */
    fge_vec2_t origin;       /* local-space origin (e.g. centre) */
} fge_bitmask_t;

/* Build bitmask from texture alpha. Pixels with alpha >= threshold are solid. */
bool fge_bitmask_from_texture(fge_bitmask_t *bm, const fge_texture_t *tex, uint8_t alpha_threshold);

/* Build bitmask from a flat alpha array (0-255). */
bool fge_bitmask_from_alpha(fge_bitmask_t *bm, const uint8_t *alpha, int w, int h);

/* Free bitmask memory */
void fge_bitmask_free(fge_bitmask_t *bm);

/* Query a pixel in local space (no bounds check — caller must clamp) */
bool fge_bitmask_get(const fge_bitmask_t *bm, int x, int y);

/* -------------------------------------------------------------------------- */
/* Collider (transform + bitmask)                                             */
/* -------------------------------------------------------------------------- */

typedef struct {
    fge_vec2_t pos;          /* world position */
    float      rotation;     /* radians */
    float      scale;        /* uniform scale */
    fge_bitmask_t mask;
    /* Cached AABB in world space (updated by fge_collider_update) */
    float aabb_minx, aabb_miny, aabb_maxx, aabb_maxy;
} fge_collider_t;

/* Recompute world-space AABB from transform + local bitmask extents.
 * Call whenever pos/rot/scale changes. */
void fge_collider_update(fge_collider_t *c);

/* Quick AABB overlap test. False = definitely no collision. */
bool fge_collider_aabb_overlap(const fge_collider_t *a, const fge_collider_t *b);

/* Pixel-perfect overlap test. Returns true if any solid pixels overlap.
 * Cost: O(overlap_area) worst case, but bit-packed and cache-friendly. */
bool fge_collider_hit_test(const fge_collider_t *a, const fge_collider_t *b);

/* Pixel-perfect contact generation. Walks the overlap region and computes
 * normal, penetration depth, and contact point from overlapping pixels.
 * Returns true if solid pixels overlap. Fills out_* if non-NULL. */
bool fge_collider_contact(const fge_collider_t *a, const fge_collider_t *b,
                          fge_vec2_t *out_normal, float *out_penetration,
                          fge_vec2_t *out_contact_point);

/* Convenience: point-in-collider (e.g. mouse picking). */
bool fge_collider_contains(const fge_collider_t *c, fge_vec2_t world_pt);

/* -------------------------------------------------------------------------- */
/* Swept test (for fast-moving / small objects)                               */
/* -------------------------------------------------------------------------- */

/* Ray-cast against a bitmask collider. Returns true if the ray from
 * origin in direction dir (normalized) hits within max_t.
 * out_t receives the hit distance if non-NULL. */
bool fge_collider_ray_cast(const fge_collider_t *c,
                           fge_vec2_t origin, fge_vec2_t dir,
                           float max_t, float *out_t);

#endif /* FORGE_COLLISION_H */
