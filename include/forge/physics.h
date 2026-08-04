/*
 * forge/physics.h — 2D Physics Engine
 *
 * Features:
 *   - Rigid bodies: position, velocity, acceleration, mass
 *   - Shapes: AABB, Circle, Capsule
 *   - Collision detection: broad phase (uniform grid), narrow phase (GJK/EPA)
 *   - Ray casting against shapes
 *   - Spatial hash for efficient queries
 *   - Deterministic fixed-timestep simulation
 *
 * Pure C23, zero external dependencies.
 */

#ifndef FORGE_PHYSICS_H
#define FORGE_PHYSICS_H

#include "forge/core.h"
#include "forge/math.h"
#include "forge/memory.h"
#include "forge/log.h"
#include "forge/collision.h"

/* -------------------------------------------------------------------------- */
/* Body types                                                                 */
/* -------------------------------------------------------------------------- */

typedef enum {
    FGE_BODY_STATIC,     /* Infinite mass, doesn't move */
    FGE_BODY_KINEMATIC,  /* Moved by velocity, no forces */
    FGE_BODY_DYNAMIC,    /* Moved by forces and collisions */
} fge_body_type_t;

/* -------------------------------------------------------------------------- */
/* Shapes                                                                     */
/* -------------------------------------------------------------------------- */

typedef enum {
    FGE_SHAPE_AABB,
    FGE_SHAPE_CIRCLE,
    FGE_SHAPE_CAPSULE,
    FGE_SHAPE_COUNT,
} fge_shape_type_t;

typedef struct {
    fge_shape_type_t type;
    union {
        fge_aabb_t aabb;
        struct { fge_vec2_t center; float radius; } circle;
        struct { fge_vec2_t a, b; float radius; } capsule;
    };
} fge_shape_t;

FGE_INLINE fge_shape_t fge_shape_aabb(float x, float y, float w, float h) {
    return (fge_shape_t){ .type = FGE_SHAPE_AABB, .aabb = { {x, y}, {x + w, y + h} } };
}
FGE_INLINE fge_shape_t fge_shape_circle(fge_vec2_t c, float r) {
    return (fge_shape_t){ .type = FGE_SHAPE_CIRCLE, .circle = { c, r } };
}

/* -------------------------------------------------------------------------- */
/* Rigid body                                                                 */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t id;
    fge_body_type_t type;

    /* Transform */
    fge_vec2_t position;
    fge_vec2_t prev_position;  /* for interpolation */
    float angle;
    float prev_angle;

    /* Velocity */
    fge_vec2_t velocity;
    fge_vec2_t force;
    float angular_velocity;
    float torque;

    /* Material */
    float mass;
    float inv_mass;
    float inertia;
    float inv_inertia;
    float restitution;     /* bounce: 0 = none, 1 = perfect */
    float friction;        /* 0 = slippery, 1 = sticky */
    float damping;         /* linear damping per second */

    /* Shape */
    fge_shape_t shape;
    fge_vec2_t shape_offset;  /* local offset from body position */
    fge_collider_t *pixel_collider;  /* optional pixel-perfect collider */

    /* State */
    bool awake;
    bool on_ground;
    uint32_t collision_mask;  /* which layers this body collides with */
    uint32_t collision_layer; /* which layer this body is on */

    /* Grid tracking (for dirty-check rebuild) */
    int32_t grid_min_x, grid_min_y;
    int32_t grid_max_x, grid_max_y;
    bool grid_dirty;

    /* User data */
    void *user_data;
} fge_body_t;

/* Set mass (0 for infinite/static) */
void fge_body_set_mass(fge_body_t *b, float mass);

/* Apply force (accumulated, cleared after integration) */
FGE_INLINE void fge_body_apply_force(fge_body_t *b, fge_vec2_t f) {
    if (b && b->inv_mass > 0.0f) {
        b->force = fge_v2_add(b->force, f);
    }
}

/* Apply impulse (immediate velocity change) */
FGE_INLINE void fge_body_apply_impulse(fge_body_t *b, fge_vec2_t impulse) {
    if (b && b->inv_mass > 0.0f) {
        b->velocity = fge_v2_add(b->velocity, fge_v2_mulf(impulse, b->inv_mass));
    }
}

/* Get world-space AABB */
fge_aabb_t fge_body_get_aabb(const fge_body_t *b);

/* -------------------------------------------------------------------------- */
/* Contact / manifold                                                         */
/* -------------------------------------------------------------------------- */

typedef struct {
    fge_vec2_t normal;       /* points from A to B */
    float penetration;       /* overlap depth */
    fge_vec2_t contact_point;
    fge_body_t *body_a;
    fge_body_t *body_b;
} fge_contact_t;

/* -------------------------------------------------------------------------- */
/* Spatial hash (uniform grid) — broad phase                                  */
/* -------------------------------------------------------------------------- */

#define FGE_PHYS_GRID_CELL_SIZE 128.0f
#define FGE_PHYS_GRID_MASK 0xFFFFu
#define FGE_PHYS_CELL_BODY_CAP 64

typedef struct fge_phys_grid_cell fge_phys_grid_cell_t;

struct fge_phys_grid_cell {
    fge_body_t *bodies[FGE_PHYS_CELL_BODY_CAP];
    uint32_t count;
    uint32_t next;  /* index into cell array for hash collision */
};

typedef struct {
    fge_phys_grid_cell_t *cells;
    uint32_t cell_count;
    uint32_t cell_capacity;
    float cell_size;
} fge_phys_grid_t;

bool fge_phys_grid_init(fge_phys_grid_t *g, uint32_t initial_cells);
void fge_phys_grid_free(fge_phys_grid_t *g);
void fge_phys_grid_clear(fge_phys_grid_t *g);
void fge_phys_grid_insert(fge_phys_grid_t *g, fge_body_t *body);

/* Query all bodies potentially colliding with AABB */
typedef void (*fge_phys_query_cb_t)(fge_body_t *body, void *userdata);
void fge_phys_grid_query(const fge_phys_grid_t *g, fge_aabb_t aabb,
                          fge_phys_query_cb_t cb, void *userdata);

/* -------------------------------------------------------------------------- */
/* Collision detection — narrow phase                                         */
/* -------------------------------------------------------------------------- */

/* Test two shapes for overlap. If contact is non-null, fills contact info. */
bool fge_physics_collide(const fge_body_t *a, const fge_body_t *b, fge_contact_t *contact);

/* Ray cast against a body. Returns hit distance, fills normal and point. */
bool fge_physics_raycast_body(const fge_body_t *body, fge_vec2_t origin, fge_vec2_t dir,
                               float max_dist, float *out_dist,
                               fge_vec2_t *out_normal, fge_vec2_t *out_point);

/* Ray cast against AABB */
bool fge_physics_raycast_aabb(fge_aabb_t aabb, fge_vec2_t origin, fge_vec2_t dir,
                               float max_dist, float *out_dist);

/* -------------------------------------------------------------------------- */
/* Physics world                                                              */
/* -------------------------------------------------------------------------- */

#define FGE_PHYS_MAX_BODIES 10000

typedef struct {
    fge_body_t bodies[FGE_PHYS_MAX_BODIES];
    uint32_t body_count;

    fge_phys_grid_t grid;

    /* Simulation params */
    fge_vec2_t gravity;
    float fixed_dt;          /* seconds per physics step (e.g. 1/60) */
    int velocity_iterations;
    int position_iterations;

    uint32_t step_count;
} fge_phys_world_t;

bool fge_phys_world_init(fge_phys_world_t *w);
void fge_phys_world_free(fge_phys_world_t *w);

/* Create body, returns body pointer or nullptr if full */
fge_body_t *fge_phys_create_body(fge_phys_world_t *w, fge_body_type_t type,
                                  const fge_shape_t *shape);
void fge_phys_destroy_body(fge_phys_world_t *w, fge_body_t *body);

/* Step simulation by dt seconds (substeps for fixed timestep) */
void fge_phys_step(fge_phys_world_t *w, float dt);

/* Query bodies in AABB */
void fge_phys_query_aabb(fge_phys_world_t *w, fge_aabb_t aabb,
                          fge_phys_query_cb_t cb, void *userdata);

/* Ray cast against all bodies */
bool fge_phys_raycast(fge_phys_world_t *w, fge_vec2_t origin, fge_vec2_t dir,
                       float max_dist, fge_body_t **out_body,
                       fge_vec2_t *out_point, fge_vec2_t *out_normal);

#endif /* FORGE_PHYSICS_H */
