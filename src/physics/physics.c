/*
 * src/physics/physics.c — FORGE 2D Physics Engine
 *
 * Features:
 *   - Rigid body dynamics with semi-implicit Euler integration
 *   - Shapes: AABB, Circle, Capsule
 *   - Broad phase: uniform spatial hash grid
 *   - Narrow phase: AABB-AABB, Circle-Circle, AABB-Circle
 *   - Contact manifold generation
 *   - Impulse-based collision response with friction
 *   - Ray casting against all shapes
 *   - Fixed timestep with substeps
 *
 * Pure C23, zero external dependencies.
 */

#include "forge/physics.h"
#include <math.h>

/* -------------------------------------------------------------------------- */
/* Internal helpers                                                           */
/* -------------------------------------------------------------------------- */

static uint32_t grid_hash(int32_t gx, int32_t gy) {
    return ((uint32_t)gx * 73856093u) ^ ((uint32_t)gy * 19349663u);
}

/* -------------------------------------------------------------------------- */
/* Body                                                                       */
/* -------------------------------------------------------------------------- */

void fge_body_set_mass(fge_body_t *b, float mass) {
    if (!b) return;
    if (mass <= 0.0f || b->type == FGE_BODY_STATIC) {
        b->mass = 0.0f;
        b->inv_mass = 0.0f;
    } else {
        b->mass = mass;
        b->inv_mass = 1.0f / mass;
    }
}

fge_aabb_t fge_body_get_aabb(const fge_body_t *b) {
    fge_vec2_t world = fge_v2_add(b->position, b->shape_offset);

    /* Pixel collider takes precedence for AABB */
    if (b->pixel_collider) {
        fge_collider_t *pc = b->pixel_collider;
        pc->pos = world;
        pc->rotation = b->angle;
        fge_collider_update(pc);
        return fge_aabb2(
            fge_v2(pc->aabb_minx, pc->aabb_miny),
            fge_v2(pc->aabb_maxx, pc->aabb_maxy)
        );
    }

    switch (b->shape.type) {
        case FGE_SHAPE_AABB: {
            fge_aabb_t local = b->shape.aabb;
            return fge_aabb2(fge_v2_add(local.min, world), fge_v2_add(local.max, world));
        }
        case FGE_SHAPE_CIRCLE: {
            fge_vec2_t c = fge_v2_add(b->shape.circle.center, world);
            float r = b->shape.circle.radius;
            return fge_aabb2(fge_v2(c.x - r, c.y - r), fge_v2(c.x + r, c.y + r));
        }
        case FGE_SHAPE_CAPSULE: {
            fge_vec2_t a = fge_v2_add(b->shape.capsule.a, world);
            fge_vec2_t bp = fge_v2_add(b->shape.capsule.b, world);
            float r = b->shape.capsule.radius;
            fge_vec2_t min = fge_v2_min(a, bp);
            fge_vec2_t max = fge_v2_max(a, bp);
            min = fge_v2_sub(min, fge_v2(r, r));
            max = fge_v2_add(max, fge_v2(r, r));
            return fge_aabb2(min, max);
        }
        default:
            return fge_aabb2(world, world);
    }
}

/* -------------------------------------------------------------------------- */
/* Spatial Grid                                                               */
/* -------------------------------------------------------------------------- */

bool fge_phys_grid_init(fge_phys_grid_t *g, uint32_t initial_cells) {
    if (!g) return false;
    fge_memzero(g, sizeof(*g));
    if (initial_cells == 0) initial_cells = 256;
    g->cells = (fge_phys_grid_cell_t *)FGE_CALLOC(initial_cells, sizeof(fge_phys_grid_cell_t));
    if (!g->cells) return false;
    g->cell_capacity = initial_cells;
    g->cell_size = FGE_PHYS_GRID_CELL_SIZE;
    return true;
}

void fge_phys_grid_free(fge_phys_grid_t *g) {
    if (!g) return;
    FGE_FREE(g->cells);
    fge_memzero(g, sizeof(*g));
}

void fge_phys_grid_clear(fge_phys_grid_t *g) {
    if (!g) return;
    for (uint32_t i = 0; i < g->cell_capacity; i++) {
        g->cells[i].count = 0;
        g->cells[i].next = 0;
    }
}

void fge_phys_grid_insert(fge_phys_grid_t *g, fge_body_t *body) {
    if (!g || !body) return;
    fge_aabb_t aabb = fge_body_get_aabb(body);
    float inv_cs = 1.0f / g->cell_size;
    int32_t min_x = (int32_t)fge_floorf(aabb.min.x * inv_cs);
    int32_t min_y = (int32_t)fge_floorf(aabb.min.y * inv_cs);
    int32_t max_x = (int32_t)fge_floorf(aabb.max.x * inv_cs);
    int32_t max_y = (int32_t)fge_floorf(aabb.max.y * inv_cs);

    for (int32_t y = min_y; y <= max_y; y++) {
        for (int32_t x = min_x; x <= max_x; x++) {
            uint32_t h = grid_hash(x, y);
            uint32_t idx = h % g->cell_capacity;
            fge_phys_grid_cell_t *cell = &g->cells[idx];
            if (cell->count < FGE_PHYS_CELL_BODY_CAP) {
                cell->bodies[cell->count++] = body;
            }
        }
    }
}

void fge_phys_grid_query(const fge_phys_grid_t *g, fge_aabb_t aabb,
                          fge_phys_query_cb_t cb, void *userdata) {
    if (!g || !cb) return;
    float inv_cs = 1.0f / g->cell_size;
    int32_t min_x = (int32_t)fge_floorf(aabb.min.x * inv_cs);
    int32_t min_y = (int32_t)fge_floorf(aabb.min.y * inv_cs);
    int32_t max_x = (int32_t)fge_floorf(aabb.max.x * inv_cs);
    int32_t max_y = (int32_t)fge_floorf(aabb.max.y * inv_cs);

    for (int32_t y = min_y; y <= max_y; y++) {
        for (int32_t x = min_x; x <= max_x; x++) {
            uint32_t h = grid_hash(x, y);
            uint32_t idx = h % g->cell_capacity;
            const fge_phys_grid_cell_t *cell = &g->cells[idx];
            for (uint32_t i = 0; i < cell->count; i++) {
                cb(cell->bodies[i], userdata);
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Narrow Phase                                                               */
/* -------------------------------------------------------------------------- */

static bool collide_aabb_aabb(const fge_body_t *a, const fge_body_t *b, fge_contact_t *contact) {
    fge_aabb_t aa = fge_body_get_aabb(a);
    fge_aabb_t bb = fge_body_get_aabb(b);

    float overlap_x = FGE_MIN(aa.max.x, bb.max.x) - FGE_MAX(aa.min.x, bb.min.x);
    float overlap_y = FGE_MIN(aa.max.y, bb.max.y) - FGE_MAX(aa.min.y, bb.min.y);
    if (overlap_x <= 0.0f || overlap_y <= 0.0f) return false;

    fge_vec2_t normal;
    float penetration;
    if (overlap_x < overlap_y) {
        penetration = overlap_x;
        normal = fge_v2(a->position.x < b->position.x ? -1.0f : 1.0f, 0.0f);
    } else {
        penetration = overlap_y;
        normal = fge_v2(0.0f, a->position.y < b->position.y ? -1.0f : 1.0f);
    }

    fge_vec2_t cp = fge_v2(
        (FGE_MAX(aa.min.x, bb.min.x) + FGE_MIN(aa.max.x, bb.max.x)) * 0.5f,
        (FGE_MAX(aa.min.y, bb.min.y) + FGE_MIN(aa.max.y, bb.max.y)) * 0.5f
    );

    if (contact) {
        contact->normal = normal;
        contact->penetration = penetration;
        contact->contact_point = cp;
        contact->body_a = (fge_body_t *)a;
        contact->body_b = (fge_body_t *)b;
    }
    return true;
}

static bool collide_circle_circle(const fge_body_t *a, const fge_body_t *b, fge_contact_t *contact) {
    fge_vec2_t ca = fge_v2_add(fge_v2_add(a->position, a->shape_offset), a->shape.circle.center);
    fge_vec2_t cb = fge_v2_add(fge_v2_add(b->position, b->shape_offset), b->shape.circle.center);
    float ra = a->shape.circle.radius;
    float rb = b->shape.circle.radius;

    fge_vec2_t n = fge_v2_sub(cb, ca);
    float dist2 = fge_v2_len2(n);
    float r = ra + rb;
    if (dist2 >= r * r) return false;

    float dist = fge_sqrtf(dist2);
    fge_vec2_t normal;
    float penetration;
    fge_vec2_t cp;

    if (dist < FGE_EPSILON_F) {
        normal = fge_v2(1.0f, 0.0f);
        penetration = r;
        cp = ca;
    } else {
        normal = fge_v2_divf(n, dist);
        penetration = r - dist;
        cp = fge_v2_add(ca, fge_v2_mulf(normal, ra));
    }

    if (contact) {
        contact->normal = normal;
        contact->penetration = penetration;
        contact->contact_point = cp;
        contact->body_a = (fge_body_t *)a;
        contact->body_b = (fge_body_t *)b;
    }
    return true;
}

static bool collide_aabb_circle(const fge_body_t *aabb_body, const fge_body_t *circle_body,
                                 fge_contact_t *contact) {
    fge_aabb_t aabb = fge_body_get_aabb(aabb_body);
    fge_vec2_t c = fge_v2_add(fge_v2_add(circle_body->position, circle_body->shape_offset),
                               circle_body->shape.circle.center);
    float r = circle_body->shape.circle.radius;

    fge_vec2_t closest;
    closest.x = FGE_CLAMP(c.x, aabb.min.x, aabb.max.x);
    closest.y = FGE_CLAMP(c.y, aabb.min.y, aabb.max.y);

    fge_vec2_t diff = fge_v2_sub(c, closest);
    float dist2 = fge_v2_len2(diff);
    if (dist2 > r * r) return false;

    float dist = fge_sqrtf(dist2);
    fge_vec2_t normal;
    float penetration;
    fge_vec2_t cp;

    if (dist < FGE_EPSILON_F) {
        float dx1 = c.x - aabb.min.x;
        float dx2 = aabb.max.x - c.x;
        float dy1 = c.y - aabb.min.y;
        float dy2 = aabb.max.y - c.y;
        float min_d = dx1;
        normal = fge_v2(1.0f, 0.0f);
        if (dx2 < min_d) { min_d = dx2; normal = fge_v2(-1.0f, 0.0f); }
        if (dy1 < min_d) { min_d = dy1; normal = fge_v2(0.0f, 1.0f); }
        if (dy2 < min_d) { min_d = dy2; normal = fge_v2(0.0f, -1.0f); }
        penetration = FGE_MAX(0.0f, r - min_d);
        cp = fge_v2_add(c, fge_v2_mulf(normal, -min_d));
        if (penetration < FGE_EPSILON_F) penetration = r * 0.5f;
    } else {
        normal = fge_v2_divf(diff, dist);
        penetration = r - dist;
        cp = closest;
    }

    if (contact) {
        contact->normal = normal;
        contact->penetration = penetration;
        contact->contact_point = cp;
        contact->body_a = (fge_body_t *)aabb_body;
        contact->body_b = (fge_body_t *)circle_body;
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* Exact capsule collision & raycast helpers                                  */
/* -------------------------------------------------------------------------- */

/* 2D cross product of vectors (a x b) */
static float vec2_cross(fge_vec2_t a, fge_vec2_t b) {
    return a.x * b.y - a.y * b.x;
}

/* Check if point c lies on segment ab (assuming collinear) */
static bool on_segment(fge_vec2_t a, fge_vec2_t b, fge_vec2_t c) {
    return c.x >= FGE_MIN(a.x, b.x) - FGE_EPSILON_F &&
           c.x <= FGE_MAX(a.x, b.x) + FGE_EPSILON_F &&
           c.y >= FGE_MIN(a.y, b.y) - FGE_EPSILON_F &&
           c.y <= FGE_MAX(a.y, b.y) + FGE_EPSILON_F;
}

/* Check if two 2D segments intersect (proper or improper) */
static bool segments_intersect(fge_vec2_t a1, fge_vec2_t a2,
                                fge_vec2_t b1, fge_vec2_t b2) {
    float d1 = vec2_cross(fge_v2_sub(b2, b1), fge_v2_sub(a1, b1));
    float d2 = vec2_cross(fge_v2_sub(b2, b1), fge_v2_sub(a2, b1));
    float d3 = vec2_cross(fge_v2_sub(a2, a1), fge_v2_sub(b1, a1));
    float d4 = vec2_cross(fge_v2_sub(a2, a1), fge_v2_sub(b2, a1));

    if (((d1 > FGE_EPSILON_F && d2 < -FGE_EPSILON_F) ||
         (d1 < -FGE_EPSILON_F && d2 > FGE_EPSILON_F)) &&
        ((d3 > FGE_EPSILON_F && d4 < -FGE_EPSILON_F) ||
         (d3 < -FGE_EPSILON_F && d4 > FGE_EPSILON_F))) {
        return true;
    }

    if (fge_fabsf(d1) < FGE_EPSILON_F && on_segment(b1, b2, a1)) return true;
    if (fge_fabsf(d2) < FGE_EPSILON_F && on_segment(b1, b2, a2)) return true;
    if (fge_fabsf(d3) < FGE_EPSILON_F && on_segment(a1, a2, b1)) return true;
    if (fge_fabsf(d4) < FGE_EPSILON_F && on_segment(a1, a2, b2)) return true;

    return false;
}

/* Squared distance from point p to segment ab, with optional parametric coord */
static float dist_sq_point_segment_t(fge_vec2_t p, fge_vec2_t a, fge_vec2_t b,
                                      float *out_t) {
    fge_vec2_t ab = fge_v2_sub(b, a);
    float ab_dot_ab = fge_v2_dot(ab, ab);
    float t = 0.0f;
    if (ab_dot_ab > FGE_EPSILON_F) {
        t = fge_v2_dot(fge_v2_sub(p, a), ab) / ab_dot_ab;
        t = FGE_CLAMP(t, 0.0f, 1.0f);
    }
    if (out_t) *out_t = t;
    fge_vec2_t closest = fge_v2_add(a, fge_v2_mulf(ab, t));
    fge_vec2_t diff = fge_v2_sub(p, closest);
    return fge_v2_dot(diff, diff);
}

/* Shortest squared distance between two segments, with closest points */
static float dist_sq_segment_segment(fge_vec2_t a1, fge_vec2_t a2,
                                      fge_vec2_t b1, fge_vec2_t b2,
                                      fge_vec2_t *out_ca, fge_vec2_t *out_cb) {
    if (segments_intersect(a1, a2, b1, b2)) {
        if (out_ca) *out_ca = a1;
        if (out_cb) *out_cb = a1;
        return 0.0f;
    }

    float t;
    float min_d = dist_sq_point_segment_t(a1, b1, b2, &t);
    fge_vec2_t ca = a1;
    fge_vec2_t cb = fge_v2_add(b1, fge_v2_mulf(fge_v2_sub(b2, b1), t));

    float d = dist_sq_point_segment_t(a2, b1, b2, &t);
    if (d < min_d) {
        min_d = d;
        ca = a2;
        cb = fge_v2_add(b1, fge_v2_mulf(fge_v2_sub(b2, b1), t));
    }

    d = dist_sq_point_segment_t(b1, a1, a2, &t);
    if (d < min_d) {
        min_d = d;
        cb = b1;
        ca = fge_v2_add(a1, fge_v2_mulf(fge_v2_sub(a2, a1), t));
    }

    d = dist_sq_point_segment_t(b2, a1, a2, &t);
    if (d < min_d) {
        min_d = d;
        cb = b2;
        ca = fge_v2_add(a1, fge_v2_mulf(fge_v2_sub(a2, a1), t));
    }

    if (out_ca) *out_ca = ca;
    if (out_cb) *out_cb = cb;
    return min_d;
}

/* Exact segment-AABB intersection (Liang-Barsky) */
static bool segment_intersects_aabb(fge_vec2_t a, fge_vec2_t b, fge_aabb_t box) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float p[4] = {-dx, dx, -dy, dy};
    float q[4] = {a.x - box.min.x, box.max.x - a.x,
                  a.y - box.min.y, box.max.y - a.y};
    float u1 = 0.0f, u2 = 1.0f;
    for (int i = 0; i < 4; i++) {
        if (fge_fabsf(p[i]) < FGE_EPSILON_F) {
            if (q[i] < 0.0f) return false;
        } else {
            float t = q[i] / p[i];
            if (p[i] < 0.0f) {
                if (t > u2) return false;
                if (t > u1) u1 = t;
            } else {
                if (t < u1) return false;
                if (t < u2) u2 = t;
            }
        }
    }
    return u1 <= u2;
}

/* Shortest squared distance from segment to AABB, with closest points */
static float dist_sq_segment_aabb(fge_vec2_t a, fge_vec2_t b,
                                   fge_aabb_t box,
                                   fge_vec2_t *out_s, fge_vec2_t *out_b) {
    if (segment_intersects_aabb(a, b, box)) {
        if (out_s) *out_s = a;
        if (out_b) *out_b = a;
        return 0.0f;
    }

    fge_vec2_t corners[4] = {
        fge_v2(box.min.x, box.min.y),
        fge_v2(box.max.x, box.min.y),
        fge_v2(box.max.x, box.max.y),
        fge_v2(box.min.x, box.max.y)
    };

    float min_d = INFINITY;
    fge_vec2_t best_s = a, best_b = corners[0];

    /* Distance from segment to each AABB edge */
    for (int i = 0; i < 4; i++) {
        fge_vec2_t ca, cb;
        float d = dist_sq_segment_segment(a, b, corners[i], corners[(i + 1) & 3], &ca, &cb);
        if (d < min_d) {
            min_d = d;
            best_s = ca;
            best_b = cb;
        }
    }

    /* Distance from segment endpoints to AABB */
    for (int i = 0; i < 2; i++) {
        fge_vec2_t p = (i == 0) ? a : b;
        fge_vec2_t clamped = fge_v2(
            FGE_CLAMP(p.x, box.min.x, box.max.x),
            FGE_CLAMP(p.y, box.min.y, box.max.y)
        );
        fge_vec2_t diff = fge_v2_sub(p, clamped);
        float d = fge_v2_dot(diff, diff);
        if (d < min_d) {
            min_d = d;
            best_s = p;
            best_b = clamped;
        }
    }

    if (out_s) *out_s = best_s;
    if (out_b) *out_b = best_b;
    return min_d;
}

FGE_INLINE fge_vec2_t closest_point_on_segment(fge_vec2_t p, fge_vec2_t a, fge_vec2_t b) {
    fge_vec2_t ab = fge_v2_sub(b, a);
    float t = fge_v2_dot(fge_v2_sub(p, a), ab) / fge_v2_dot(ab, ab);
    t = FGE_CLAMP(t, 0.0f, 1.0f);
    return fge_v2_add(a, fge_v2_mulf(ab, t));
}

/* -------------------------------------------------------------------------- */
/* Exact capsule narrow phase                                                 */
/* -------------------------------------------------------------------------- */

static bool collide_capsule_circle(const fge_body_t *cap, const fge_body_t *circ,
                                    fge_contact_t *contact) {
    fge_vec2_t world = fge_v2_add(cap->position, cap->shape_offset);
    fge_vec2_t a = fge_v2_add(cap->shape.capsule.a, world);
    fge_vec2_t b = fge_v2_add(cap->shape.capsule.b, world);
    float cr = cap->shape.capsule.radius;

    fge_vec2_t cc = fge_v2_add(fge_v2_add(circ->position, circ->shape_offset),
                                circ->shape.circle.center);
    float ccr = circ->shape.circle.radius;

    float t;
    float dist2 = dist_sq_point_segment_t(cc, a, b, &t);
    float rsum = cr + ccr;
    if (dist2 >= rsum * rsum) return false;

    float dist = fge_sqrtf(dist2);
    fge_vec2_t closest = fge_v2_add(a, fge_v2_mulf(fge_v2_sub(b, a), t));
    fge_vec2_t normal;
    fge_vec2_t cp;
    float penetration;

    if (dist < FGE_EPSILON_F) {
        normal = fge_v2_norm(fge_v2_sub(b, a));
        if (fge_v2_len2(normal) < FGE_EPSILON_F) normal = fge_v2(1.0f, 0.0f);
        penetration = rsum;
        cp = closest;
    } else {
        normal = fge_v2_divf(fge_v2_sub(cc, closest), dist);
        penetration = rsum - dist;
        cp = fge_v2_add(closest, fge_v2_mulf(normal, cr));
    }

    if (contact) {
        contact->normal = normal;
        contact->penetration = penetration;
        contact->contact_point = cp;
        contact->body_a = (fge_body_t *)cap;
        contact->body_b = (fge_body_t *)circ;
    }
    return true;
}

static bool collide_capsule_aabb(const fge_body_t *cap, const fge_body_t *aabb_body,
                                  fge_contact_t *contact) {
    fge_vec2_t world = fge_v2_add(cap->position, cap->shape_offset);
    fge_vec2_t a = fge_v2_add(cap->shape.capsule.a, world);
    fge_vec2_t b = fge_v2_add(cap->shape.capsule.b, world);
    float cr = cap->shape.capsule.radius;

    fge_aabb_t box = fge_body_get_aabb(aabb_body);

    fge_vec2_t cs, cb;
    float dist2 = dist_sq_segment_aabb(a, b, box, &cs, &cb);
    if (dist2 >= cr * cr) return false;

    float dist = fge_sqrtf(dist2);
    fge_vec2_t normal;
    fge_vec2_t cp;
    float penetration;

    if (dist < FGE_EPSILON_F) {
        /* Segment intersects or is inside AABB -- use perpendicular normal */
        fge_vec2_t seg_dir = fge_v2_norm(fge_v2_sub(b, a));
        if (fge_v2_len2(seg_dir) < FGE_EPSILON_F) seg_dir = fge_v2(1.0f, 0.0f);
        normal = fge_v2(-seg_dir.y, seg_dir.x);
        /* Ensure normal points from capsule (A) toward AABB (B) */
        fge_vec2_t mid = fge_v2_mulf(fge_v2_add(a, b), 0.5f);
        if (fge_v2_dot(fge_v2_sub(mid, cb), normal) > 0.0f)
            normal = fge_v2_mulf(normal, -1.0f);
        penetration = cr;
        cp = cs;
    } else {
        normal = fge_v2_divf(fge_v2_sub(cb, cs), dist);
        penetration = cr - dist;
        cp = fge_v2_add(cs, fge_v2_mulf(normal, cr));
    }

    if (contact) {
        contact->normal = normal;
        contact->penetration = penetration;
        contact->contact_point = cp;
        contact->body_a = (fge_body_t *)cap;
        contact->body_b = (fge_body_t *)aabb_body;
    }
    return true;
}

static bool collide_capsule_capsule(const fge_body_t *a, const fge_body_t *b,
                                     fge_contact_t *contact) {
    fge_vec2_t wa = fge_v2_add(a->position, a->shape_offset);
    fge_vec2_t a1 = fge_v2_add(a->shape.capsule.a, wa);
    fge_vec2_t a2 = fge_v2_add(a->shape.capsule.b, wa);
    float ra = a->shape.capsule.radius;

    fge_vec2_t wb = fge_v2_add(b->position, b->shape_offset);
    fge_vec2_t b1 = fge_v2_add(b->shape.capsule.a, wb);
    fge_vec2_t b2 = fge_v2_add(b->shape.capsule.b, wb);
    float rb = b->shape.capsule.radius;

    fge_vec2_t ca, cb;
    float dist2 = dist_sq_segment_segment(a1, a2, b1, b2, &ca, &cb);
    float rsum = ra + rb;
    if (dist2 >= rsum * rsum) return false;

    float dist = fge_sqrtf(dist2);
    fge_vec2_t normal;
    fge_vec2_t cp;
    float penetration;

    if (dist < FGE_EPSILON_F) {
        normal = fge_v2_norm(fge_v2_sub(a2, a1));
        if (fge_v2_len2(normal) < FGE_EPSILON_F) normal = fge_v2(1.0f, 0.0f);
        penetration = rsum;
        cp = ca;
    } else {
        normal = fge_v2_divf(fge_v2_sub(cb, ca), dist);
        penetration = rsum - dist;
        cp = fge_v2_add(ca, fge_v2_mulf(normal, ra));
    }

    if (contact) {
        contact->normal = normal;
        contact->penetration = penetration;
        contact->contact_point = cp;
        contact->body_a = (fge_body_t *)a;
        contact->body_b = (fge_body_t *)b;
    }
    return true;
}

static bool collide_capsule_generic(const fge_body_t *cap, const fge_body_t *other,
                                     fge_contact_t *contact) {
    switch (other->shape.type) {
        case FGE_SHAPE_CIRCLE:
            return collide_capsule_circle(cap, other, contact);
        case FGE_SHAPE_AABB:
            return collide_capsule_aabb(cap, other, contact);
        case FGE_SHAPE_CAPSULE:
            return collide_capsule_capsule(cap, other, contact);
        default:
            return false;
    }
}

bool fge_physics_collide(const fge_body_t *a, const fge_body_t *b, fge_contact_t *contact) {
    if (!a || !b) return false;
    if (a == b) return false;
    if ((a->collision_layer & b->collision_mask) == 0 &&
        (b->collision_layer & a->collision_mask) == 0) {
        return false;
    }

    /* Pixel-perfect collision: both bodies have pixel colliders */
    if (a->pixel_collider && b->pixel_collider) {
        fge_vec2_t normal;
        float penetration;
        fge_vec2_t cp;
        if (!fge_collider_contact(a->pixel_collider, b->pixel_collider,
                                   &normal, &penetration, &cp)) {
            return false;
        }
        if (contact) {
            contact->normal = normal;
            contact->penetration = penetration;
            contact->contact_point = cp;
            contact->body_a = (fge_body_t *)a;
            contact->body_b = (fge_body_t *)b;
        }
        return true;
    }

    fge_shape_type_t ta = a->shape.type;
    fge_shape_type_t tb = b->shape.type;

    /* AABB-AABB */
    if (ta == FGE_SHAPE_AABB && tb == FGE_SHAPE_AABB)
        return collide_aabb_aabb(a, b, contact);
    /* Circle-Circle */
    if (ta == FGE_SHAPE_CIRCLE && tb == FGE_SHAPE_CIRCLE)
        return collide_circle_circle(a, b, contact);
    /* AABB-Circle (both orders) */
    if (ta == FGE_SHAPE_AABB && tb == FGE_SHAPE_CIRCLE)
        return collide_aabb_circle(a, b, contact);
    if (ta == FGE_SHAPE_CIRCLE && tb == FGE_SHAPE_AABB) {
        bool hit = collide_aabb_circle(b, a, contact);
        if (hit && contact) {
            contact->normal = fge_v2_mulf(contact->normal, -1.0f);
            FGE_SWAP(fge_body_t *, contact->body_a, contact->body_b);
        }
        return hit;
    }
    /* Capsule vs anything -- exact segment-based distance */
    if (ta == FGE_SHAPE_CAPSULE)
        return collide_capsule_generic(a, b, contact);
    if (tb == FGE_SHAPE_CAPSULE) {
        bool hit = collide_capsule_generic(b, a, contact);
        if (hit && contact) {
            contact->normal = fge_v2_mulf(contact->normal, -1.0f);
            FGE_SWAP(fge_body_t *, contact->body_a, contact->body_b);
        }
        return hit;
    }

    return false;
}

/* -------------------------------------------------------------------------- */
/* Ray Casting                                                                */
/* -------------------------------------------------------------------------- */

bool fge_physics_raycast_aabb(fge_aabb_t aabb, fge_vec2_t origin, fge_vec2_t dir,
                               float max_dist, float *out_dist) {
    float tmin = 0.0f;
    float tmax = max_dist;

    for (int axis = 0; axis < 2; axis++) {
        float o = (axis == 0) ? origin.x : origin.y;
        float d = (axis == 0) ? dir.x : dir.y;
        float min_v = (axis == 0) ? aabb.min.x : aabb.min.y;
        float max_v = (axis == 0) ? aabb.max.x : aabb.max.y;

        if (fge_fabsf(d) < FGE_EPSILON_F) {
            if (o < min_v || o > max_v) return false;
        } else {
            float inv_d = 1.0f / d;
            float t1 = (min_v - o) * inv_d;
            float t2 = (max_v - o) * inv_d;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            tmin = FGE_MAX(tmin, t1);
            tmax = FGE_MIN(tmax, t2);
            if (tmin > tmax) return false;
        }
    }

    if (tmin < 0.0f) tmin = tmax;
    if (tmin < 0.0f || tmin > max_dist) return false;

    if (out_dist) *out_dist = tmin;
    return true;
}

static bool raycast_circle(fge_vec2_t center, float radius, fge_vec2_t origin,
                            fge_vec2_t dir, float max_dist, float *out_dist,
                            fge_vec2_t *out_normal, fge_vec2_t *out_point) {
    fge_vec2_t m = fge_v2_sub(origin, center);
    float b = fge_v2_dot(m, dir);
    float c = fge_v2_dot(m, m) - radius * radius;
    if (c > 0.0f && b > 0.0f) return false;
    float discr = b * b - c;
    if (discr < 0.0f) return false;
    float t = -b - fge_sqrtf(discr);
    if (t < 0.0f) t = 0.0f;
    if (t > max_dist) return false;

    fge_vec2_t p = fge_v2_add(origin, fge_v2_mulf(dir, t));
    fge_vec2_t n = fge_v2_norm(fge_v2_sub(p, center));
    if (out_dist) *out_dist = t;
    if (out_normal) *out_normal = n;
    if (out_point) *out_point = p;
    return true;
}

static bool raycast_capsule(fge_vec2_t a, fge_vec2_t b, float radius,
                             fge_vec2_t origin, fge_vec2_t dir, float max_dist,
                             float *out_dist, fge_vec2_t *out_normal, fge_vec2_t *out_point) {
    /* Early-out: origin inside capsule */
    if (dist_sq_point_segment_t(origin, a, b, NULL) <= radius * radius) {
        if (out_dist) *out_dist = 0.0f;
        if (out_point) *out_point = origin;
        if (out_normal) {
            float t;
            dist_sq_point_segment_t(origin, a, b, &t);
            fge_vec2_t closest = fge_v2_add(a, fge_v2_mulf(fge_v2_sub(b, a), t));
            *out_normal = fge_v2_norm(fge_v2_sub(origin, closest));
            if (fge_v2_len2(*out_normal) < FGE_EPSILON_F) *out_normal = fge_v2(1.0f, 0.0f);
        }
        return true;
    }

    float closest_t = max_dist;
    fge_vec2_t closest_n = fge_v2(1.0f, 0.0f);
    fge_vec2_t closest_p = origin;
    bool hit = false;

    /* 1. Raycast against endpoint circles */
    float t;
    fge_vec2_t n, p;
    if (raycast_circle(a, radius, origin, dir, max_dist, &t, &n, &p)) {
        if (t < closest_t) { closest_t = t; closest_n = n; closest_p = p; hit = true; }
    }
    if (raycast_circle(b, radius, origin, dir, max_dist, &t, &n, &p)) {
        if (t < closest_t) { closest_t = t; closest_n = n; closest_p = p; hit = true; }
    }

    /* 2. Raycast against capsule sides (two parallel offset segments) */
    fge_vec2_t seg = fge_v2_sub(b, a);
    float seg_len2 = fge_v2_dot(seg, seg);
    if (seg_len2 > FGE_EPSILON_F) {
        float seg_len = fge_sqrtf(seg_len2);
        fge_vec2_t d = fge_v2_divf(seg, seg_len);
        fge_vec2_t perp = fge_v2(-d.y, d.x);

        for (int side = 0; side < 2; side++) {
            fge_vec2_t offset = (side == 0)
                ? fge_v2_mulf(perp, radius)
                : fge_v2_mulf(perp, -radius);
            fge_vec2_t line_a = fge_v2_add(a, offset);
            fge_vec2_t pvec = fge_v2_sub(line_a, origin);

            /* Solve: origin + u*dir = line_a + t*d  =>  u*dir - t*d = pvec */
            float det = dir.y * d.x - dir.x * d.y;
            if (fge_fabsf(det) < FGE_EPSILON_F) continue;

            float u = (pvec.y * d.x - pvec.x * d.y) / det;
            float line_t = (dir.x * pvec.y - dir.y * pvec.x) / det;

            if (u >= 0.0f && u <= max_dist && line_t >= 0.0f && line_t <= seg_len) {
                if (u < closest_t) {
                    closest_t = u;
                    closest_n = (side == 0) ? perp : fge_v2_mulf(perp, -1.0f);
                    closest_p = fge_v2_add(origin, fge_v2_mulf(dir, u));
                    hit = true;
                }
            }
        }
    }

    if (hit) {
        if (out_dist) *out_dist = closest_t;
        if (out_normal) *out_normal = closest_n;
        if (out_point) *out_point = closest_p;
    }
    return hit;
}

bool fge_physics_raycast_body(const fge_body_t *body, fge_vec2_t origin, fge_vec2_t dir,
                               float max_dist, float *out_dist,
                               fge_vec2_t *out_normal, fge_vec2_t *out_point) {
    if (!body) return false;
    fge_vec2_t world = fge_v2_add(body->position, body->shape_offset);

    switch (body->shape.type) {
        case FGE_SHAPE_AABB: {
            fge_aabb_t aabb = fge_body_get_aabb(body);
            float t;
            if (!fge_physics_raycast_aabb(aabb, origin, dir, max_dist, &t)) return false;
            if (out_dist) *out_dist = t;
            if (out_point) *out_point = fge_v2_add(origin, fge_v2_mulf(dir, t));
            /* Compute normal from hit face */
            if (out_normal) {
                fge_vec2_t pt = fge_v2_add(origin, fge_v2_mulf(dir, t));
                float dx1 = fge_fabsf(pt.x - aabb.min.x);
                float dx2 = fge_fabsf(pt.x - aabb.max.x);
                float dy1 = fge_fabsf(pt.y - aabb.min.y);
                float dy2 = fge_fabsf(pt.y - aabb.max.y);
                float min_d = dx1;
                *out_normal = fge_v2(-1.0f, 0.0f);
                if (dx2 < min_d) { min_d = dx2; *out_normal = fge_v2(1.0f, 0.0f); }
                if (dy1 < min_d) { min_d = dy1; *out_normal = fge_v2(0.0f, -1.0f); }
                if (dy2 < min_d) { *out_normal = fge_v2(0.0f, 1.0f); }
            }
            return true;
        }
        case FGE_SHAPE_CIRCLE: {
            fge_vec2_t c = fge_v2_add(body->shape.circle.center, world);
            return raycast_circle(c, body->shape.circle.radius, origin, dir, max_dist,
                                   out_dist, out_normal, out_point);
        }
        case FGE_SHAPE_CAPSULE: {
            fge_vec2_t ca = fge_v2_add(body->shape.capsule.a, world);
            fge_vec2_t cb = fge_v2_add(body->shape.capsule.b, world);
            return raycast_capsule(ca, cb, body->shape.capsule.radius, origin, dir, max_dist,
                                    out_dist, out_normal, out_point);
        }
        default:
            return false;
    }
}

/* -------------------------------------------------------------------------- */
/* World                                                                      */
/* -------------------------------------------------------------------------- */

bool fge_phys_world_init(fge_phys_world_t *w) {
    if (!w) return false;
    fge_memzero(w, sizeof(*w));
    w->gravity = fge_v2(0.0f, -9.8f);
    w->fixed_dt = 1.0f / 60.0f;
    w->velocity_iterations = 6;
    w->position_iterations = 2;
    if (!fge_phys_grid_init(&w->grid, 1024)) return false;
    return true;
}

void fge_phys_world_free(fge_phys_world_t *w) {
    if (!w) return;
    fge_phys_grid_free(&w->grid);
    fge_memzero(w, sizeof(*w));
}

fge_body_t *fge_phys_create_body(fge_phys_world_t *w, fge_body_type_t type,
                                  const fge_shape_t *shape) {
    if (!w || !shape) return NULL;
    if (w->body_count >= FGE_PHYS_MAX_BODIES) return NULL;

    uint32_t idx = w->body_count++;
    fge_body_t *b = &w->bodies[idx];
    fge_memzero(b, sizeof(*b));
    b->id = idx;
    b->type = type;
    b->shape = *shape;
    b->awake = true;
    b->restitution = 0.2f;
    b->friction = 0.3f;
    b->damping = 0.01f;
    b->collision_mask = 0xFFFFFFFFu;
    b->collision_layer = 1u;
    b->grid_dirty = true;

    if (type == FGE_BODY_STATIC) {
        b->mass = 0.0f;
        b->inv_mass = 0.0f;
    } else {
        fge_body_set_mass(b, 1.0f);
    }
    return b;
}

void fge_phys_destroy_body(fge_phys_world_t *w, fge_body_t *body) {
    if (!w || !body) return;
    uint32_t idx = body->id;
    if (idx >= w->body_count) return;

    /* Swap with last and decrement count */
    uint32_t last = w->body_count - 1;
    if (idx != last) {
        w->bodies[idx] = w->bodies[last];
        w->bodies[idx].id = idx;
    }
    w->body_count--;
}

/* -------------------------------------------------------------------------- */
/* Collision Solver                                                           */
/* -------------------------------------------------------------------------- */

static void solve_collision(fge_contact_t *c) {
    fge_body_t *a = c->body_a;
    fge_body_t *b = c->body_b;
    if (!a || !b) return;

    float inv_mass_a = a->inv_mass;
    float inv_mass_b = b->inv_mass;
    if (inv_mass_a == 0.0f && inv_mass_b == 0.0f) return;

    fge_vec2_t rel_vel = fge_v2_sub(b->velocity, a->velocity);
    float vel_along_normal = fge_v2_dot(rel_vel, c->normal);
    if (vel_along_normal > 0.0f) return; /* separating */

    float e = FGE_MIN(a->restitution, b->restitution);
    float j = -(1.0f + e) * vel_along_normal;
    j /= (inv_mass_a + inv_mass_b);

    fge_vec2_t impulse = fge_v2_mulf(c->normal, j);
    a->velocity = fge_v2_sub(a->velocity, fge_v2_mulf(impulse, inv_mass_a));
    b->velocity = fge_v2_add(b->velocity, fge_v2_mulf(impulse, inv_mass_b));

    /* Friction */
    rel_vel = fge_v2_sub(b->velocity, a->velocity);
    fge_vec2_t tangent = fge_v2_sub(rel_vel, fge_v2_mulf(c->normal, fge_v2_dot(rel_vel, c->normal)));
    float tl = fge_v2_len(tangent);
    if (tl > FGE_EPSILON_F) {
        tangent = fge_v2_divf(tangent, tl);
        float jt = -fge_v2_dot(rel_vel, tangent);
        jt /= (inv_mass_a + inv_mass_b);
        float mu = fge_sqrtf(a->friction * a->friction + b->friction * b->friction);
        if (fge_fabsf(jt) > j * mu) {
            jt = (jt > 0.0f ? 1.0f : -1.0f) * j * mu;
        }
        fge_vec2_t friction_impulse = fge_v2_mulf(tangent, jt);
        a->velocity = fge_v2_sub(a->velocity, fge_v2_mulf(friction_impulse, inv_mass_a));
        b->velocity = fge_v2_add(b->velocity, fge_v2_mulf(friction_impulse, inv_mass_b));
    }
}

static void apply_pos_correction(fge_contact_t *c) {
    fge_body_t *a = c->body_a;
    fge_body_t *b = c->body_b;
    if (!a || !b) return;

    float inv_mass_a = a->inv_mass;
    float inv_mass_b = b->inv_mass;
    if (inv_mass_a == 0.0f && inv_mass_b == 0.0f) return;

    const float percent = 0.4f;
    const float slop = 0.01f;
    float pen = c->penetration - slop;
    if (pen <= 0.0f) return;

    fge_vec2_t correction = fge_v2_mulf(c->normal, pen / (inv_mass_a + inv_mass_b) * percent);
    a->position = fge_v2_sub(a->position, fge_v2_mulf(correction, inv_mass_a));
    b->position = fge_v2_add(b->position, fge_v2_mulf(correction, inv_mass_b));
}

/* -------------------------------------------------------------------------- */
/* Step                                                                       */
/* -------------------------------------------------------------------------- */

void fge_phys_step(fge_phys_world_t *w, float dt) {
    if (!w || dt <= 0.0f) return;

    int steps = (int)fge_ceilf(dt / w->fixed_dt);
    float sub_dt = dt / (float)steps;
    if (sub_dt > w->fixed_dt) sub_dt = w->fixed_dt;

    for (int step = 0; step < steps; step++) {
        /* a. Apply forces (gravity) */
        for (uint32_t i = 0; i < w->body_count; i++) {
            fge_body_t *b = &w->bodies[i];
            if (b->type != FGE_BODY_DYNAMIC) continue;
            if (b->inv_mass > 0.0f) {
                b->force = fge_v2_add(b->force, fge_v2_mulf(w->gravity, b->mass));
            }
        }

        /* b. Integrate velocities */
        for (uint32_t i = 0; i < w->body_count; i++) {
            fge_body_t *b = &w->bodies[i];
            if (b->type != FGE_BODY_DYNAMIC) continue;
            if (b->inv_mass > 0.0f) {
                fge_vec2_t accel = fge_v2_mulf(b->force, b->inv_mass);
                b->velocity = fge_v2_add(b->velocity, fge_v2_mulf(accel, sub_dt));
            }
            b->force = fge_v2_zero();
        }

        /* c. Integrate positions and check grid dirty */
        bool grid_needs_rebuild = false;
        float inv_cs = 1.0f / w->grid.cell_size;
        for (uint32_t i = 0; i < w->body_count; i++) {
            fge_body_t *b = &w->bodies[i];
            if (b->type == FGE_BODY_STATIC) continue;
            b->prev_position = b->position;
            b->position = fge_v2_add(b->position, fge_v2_mulf(b->velocity, sub_dt));

            /* Check if grid cell range changed */
            fge_aabb_t aabb = fge_body_get_aabb(b);
            int32_t min_x = (int32_t)fge_floorf(aabb.min.x * inv_cs);
            int32_t min_y = (int32_t)fge_floorf(aabb.min.y * inv_cs);
            int32_t max_x = (int32_t)fge_floorf(aabb.max.x * inv_cs);
            int32_t max_y = (int32_t)fge_floorf(aabb.max.y * inv_cs);
            if (b->grid_dirty || min_x != b->grid_min_x || min_y != b->grid_min_y ||
                max_x != b->grid_max_x || max_y != b->grid_max_y) {
                b->grid_min_x = min_x; b->grid_min_y = min_y;
                b->grid_max_x = max_x; b->grid_max_y = max_y;
                b->grid_dirty = false;
                grid_needs_rebuild = true;
            }
        }

        /* d. Rebuild spatial grid only if needed */
        if (grid_needs_rebuild || w->step_count == 0) {
            fge_phys_grid_clear(&w->grid);
            for (uint32_t i = 0; i < w->body_count; i++) {
                fge_phys_grid_insert(&w->grid, &w->bodies[i]);
            }
        }

        /* e/f/g. Broad phase + narrow phase + solve + position correction */
        for (uint32_t i = 0; i < w->body_count; i++) {
            fge_body_t *a = &w->bodies[i];
            if (a->type == FGE_BODY_STATIC) continue;
            fge_aabb_t aabb = fge_body_get_aabb(a);

            float inv_cs = 1.0f / w->grid.cell_size;
            int32_t min_x = (int32_t)fge_floorf(aabb.min.x * inv_cs);
            int32_t min_y = (int32_t)fge_floorf(aabb.min.y * inv_cs);
            int32_t max_x = (int32_t)fge_floorf(aabb.max.x * inv_cs);
            int32_t max_y = (int32_t)fge_floorf(aabb.max.y * inv_cs);

            for (int32_t y = min_y; y <= max_y; y++) {
                for (int32_t x = min_x; x <= max_x; x++) {
                    uint32_t h = grid_hash(x, y);
                    uint32_t idx = h % w->grid.cell_capacity;
                    fge_phys_grid_cell_t *cell = &w->grid.cells[idx];

                    for (uint32_t k = 0; k < cell->count; k++) {
                        fge_body_t *b = cell->bodies[k];
                        /* Skip self and duplicate pairs (b->id <= a->id covers both) */
                        if (b->id <= a->id) continue;

                        fge_contact_t contact;
                        if (fge_physics_collide(a, b, &contact)) {
                            for (int iter = 0; iter < w->velocity_iterations; iter++) {
                                solve_collision(&contact);
                            }
                            for (int iter = 0; iter < w->position_iterations; iter++) {
                                apply_pos_correction(&contact);
                            }
                        }
                    }
                }
            }
        }

        /* h. Apply damping */
        for (uint32_t i = 0; i < w->body_count; i++) {
            fge_body_t *b = &w->bodies[i];
            if (b->type != FGE_BODY_DYNAMIC) continue;
            float damp = 1.0f - b->damping * sub_dt;
            if (damp < 0.0f) damp = 0.0f;
            b->velocity = fge_v2_mulf(b->velocity, damp);
        }

        w->step_count++;
    }
}

/* -------------------------------------------------------------------------- */
/* Queries                                                                    */
/* -------------------------------------------------------------------------- */

void fge_phys_query_aabb(fge_phys_world_t *w, fge_aabb_t aabb,
                          fge_phys_query_cb_t cb, void *userdata) {
    if (!w || !cb) return;
    fge_phys_grid_query(&w->grid, aabb, cb, userdata);
}

bool fge_phys_raycast(fge_phys_world_t *w, fge_vec2_t origin, fge_vec2_t dir,
                       float max_dist, fge_body_t **out_body,
                       fge_vec2_t *out_point, fge_vec2_t *out_normal) {
    if (!w) return false;
    fge_vec2_t d = fge_v2_norm(dir);
    float closest_t = max_dist;
    fge_body_t *hit_body = NULL;
    fge_vec2_t hit_point = fge_v2_zero();
    fge_vec2_t hit_normal = fge_v2_zero();

    for (uint32_t i = 0; i < w->body_count; i++) {
        float t;
        fge_vec2_t n, p;
        if (fge_physics_raycast_body(&w->bodies[i], origin, d, closest_t, &t, &n, &p)) {
            closest_t = t;
            hit_body = &w->bodies[i];
            hit_point = p;
            hit_normal = n;
        }
    }

    if (!hit_body) return false;
    if (out_body) *out_body = hit_body;
    if (out_point) *out_point = hit_point;
    if (out_normal) *out_normal = hit_normal;
    return true;
}
