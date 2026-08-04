/*
 * forge/math.h — SIMD-friendly vector / matrix / quaternion math
 * Pure C23, zero external dependencies.
 */

#ifndef FORGE_MATH_H
#define FORGE_MATH_H

#include "forge/core.h"

/* -------------------------------------------------------------------------- */
/* Vec2                                                                       */
/* -------------------------------------------------------------------------- */

typedef struct { float x, y; } fge_vec2_t;
typedef struct { int x, y; } fge_ivec2_t;

FGE_INLINE fge_vec2_t fge_v2(float x, float y)     { return (fge_vec2_t){x, y}; }
FGE_INLINE fge_vec2_t fge_v2_zero(void)            { return (fge_vec2_t){0, 0}; }
FGE_INLINE fge_vec2_t fge_v2_one(void)             { return (fge_vec2_t){1, 1}; }
FGE_INLINE fge_vec2_t fge_v2_add(fge_vec2_t a, fge_vec2_t b)  { return (fge_vec2_t){a.x + b.x, a.y + b.y}; }
FGE_INLINE fge_vec2_t fge_v2_sub(fge_vec2_t a, fge_vec2_t b)  { return (fge_vec2_t){a.x - b.x, a.y - b.y}; }
FGE_INLINE fge_vec2_t fge_v2_mul(fge_vec2_t a, fge_vec2_t b)  { return (fge_vec2_t){a.x * b.x, a.y * b.y}; }
FGE_INLINE fge_vec2_t fge_v2_mulf(fge_vec2_t a, float s)      { return (fge_vec2_t){a.x * s, a.y * s}; }
FGE_INLINE fge_vec2_t fge_v2_divf(fge_vec2_t a, float s)      { return (fge_vec2_t){a.x / s, a.y / s}; }
FGE_INLINE float      fge_v2_dot(fge_vec2_t a, fge_vec2_t b)  { return a.x * b.x + a.y * b.y; }
FGE_INLINE float      fge_v2_len2(fge_vec2_t a)               { return a.x * a.x + a.y * a.y; }
FGE_INLINE float      fge_v2_len(fge_vec2_t a)                { return fge_sqrtf(fge_v2_len2(a)); }
FGE_INLINE fge_vec2_t fge_v2_norm(fge_vec2_t a) {
    float l = fge_v2_len(a);
    return l > FGE_EPSILON_F ? fge_v2_divf(a, l) : fge_v2_zero();
}
FGE_INLINE fge_vec2_t fge_v2_norm_fast(fge_vec2_t a) {
    float il = fge_inv_sqrtf(fge_v2_len2(a));
    return fge_v2_mulf(a, il);
}
FGE_INLINE fge_vec2_t fge_v2_lerp(fge_vec2_t a, fge_vec2_t b, float t) {
    return fge_v2_add(a, fge_v2_mulf(fge_v2_sub(b, a), t));
}
FGE_INLINE fge_vec2_t fge_v2_perp(fge_vec2_t a) { return (fge_vec2_t){-a.y, a.x}; }
FGE_INLINE float      fge_v2_cross(fge_vec2_t a, fge_vec2_t b) { return a.x * b.y - a.y * b.x; }
FGE_INLINE fge_vec2_t fge_v2_min(fge_vec2_t a, fge_vec2_t b) {
    return (fge_vec2_t){FGE_MIN(a.x, b.x), FGE_MIN(a.y, b.y)};
}
FGE_INLINE fge_vec2_t fge_v2_max(fge_vec2_t a, fge_vec2_t b) {
    return (fge_vec2_t){FGE_MAX(a.x, b.x), FGE_MAX(a.y, b.y)};
}
FGE_INLINE fge_vec2_t fge_v2_clamp(fge_vec2_t a, fge_vec2_t lo, fge_vec2_t hi) {
    return fge_v2_min(hi, fge_v2_max(lo, a));
}
FGE_INLINE float      fge_v2_dist(fge_vec2_t a, fge_vec2_t b) {
    return fge_v2_len(fge_v2_sub(a, b));
}
FGE_INLINE fge_vec2_t fge_v2_reflect(fge_vec2_t v, fge_vec2_t n) {
    return fge_v2_sub(v, fge_v2_mulf(n, 2.0f * fge_v2_dot(v, n)));
}
FGE_INLINE fge_vec2_t fge_v2_project(fge_vec2_t v, fge_vec2_t onto) {
    float d = fge_v2_dot(onto, onto);
    return d > FGE_EPSILON_F ? fge_v2_mulf(onto, fge_v2_dot(v, onto) / d) : fge_v2_zero();
}

/* -------------------------------------------------------------------------- */
/* Vec3                                                                       */
/* -------------------------------------------------------------------------- */

typedef struct { float x, y, z; } fge_vec3_t;

FGE_INLINE fge_vec3_t fge_v3(float x, float y, float z)       { return (fge_vec3_t){x, y, z}; }
FGE_INLINE fge_vec3_t fge_v3_zero(void)                       { return (fge_vec3_t){0, 0, 0}; }
FGE_INLINE fge_vec3_t fge_v3_one(void)                        { return (fge_vec3_t){1, 1, 1}; }
FGE_INLINE fge_vec3_t fge_v3_up(void)                         { return (fge_vec3_t){0, 1, 0}; }
FGE_INLINE fge_vec3_t fge_v3_right(void)                      { return (fge_vec3_t){1, 0, 0}; }
FGE_INLINE fge_vec3_t fge_v3_forward(void)                    { return (fge_vec3_t){0, 0, -1}; }
FGE_INLINE fge_vec3_t fge_v3_add(fge_vec3_t a, fge_vec3_t b)  { return (fge_vec3_t){a.x + b.x, a.y + b.y, a.z + b.z}; }
FGE_INLINE fge_vec3_t fge_v3_sub(fge_vec3_t a, fge_vec3_t b)  { return (fge_vec3_t){a.x - b.x, a.y - b.y, a.z - b.z}; }
FGE_INLINE fge_vec3_t fge_v3_mul(fge_vec3_t a, fge_vec3_t b)  { return (fge_vec3_t){a.x * b.x, a.y * b.y, a.z * b.z}; }
FGE_INLINE fge_vec3_t fge_v3_mulf(fge_vec3_t a, float s)      { return (fge_vec3_t){a.x * s, a.y * s, a.z * s}; }
FGE_INLINE fge_vec3_t fge_v3_divf(fge_vec3_t a, float s)      { return (fge_vec3_t){a.x / s, a.y / s, a.z / s}; }
FGE_INLINE float      fge_v3_dot(fge_vec3_t a, fge_vec3_t b)  { return a.x * b.x + a.y * b.y + a.z * b.z; }
FGE_INLINE fge_vec3_t fge_v3_cross(fge_vec3_t a, fge_vec3_t b) {
    return (fge_vec3_t){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}
FGE_INLINE float      fge_v3_len2(fge_vec3_t a)               { return fge_v3_dot(a, a); }
FGE_INLINE float      fge_v3_len(fge_vec3_t a)                { return fge_sqrtf(fge_v3_len2(a)); }
FGE_INLINE fge_vec3_t fge_v3_norm(fge_vec3_t a) {
    float l = fge_v3_len(a);
    return l > FGE_EPSILON_F ? fge_v3_divf(a, l) : fge_v3_zero();
}
FGE_INLINE fge_vec3_t fge_v3_norm_fast(fge_vec3_t a) {
    return fge_v3_mulf(a, fge_inv_sqrtf(fge_v3_len2(a)));
}
FGE_INLINE fge_vec3_t fge_v3_lerp(fge_vec3_t a, fge_vec3_t b, float t) {
    return fge_v3_add(a, fge_v3_mulf(fge_v3_sub(b, a), t));
}
FGE_INLINE fge_vec3_t fge_v3_min(fge_vec3_t a, fge_vec3_t b) {
    return (fge_vec3_t){FGE_MIN(a.x, b.x), FGE_MIN(a.y, b.y), FGE_MIN(a.z, b.z)};
}
FGE_INLINE fge_vec3_t fge_v3_max(fge_vec3_t a, fge_vec3_t b) {
    return (fge_vec3_t){FGE_MAX(a.x, b.x), FGE_MAX(a.y, b.y), FGE_MAX(a.z, b.z)};
}
FGE_INLINE float      fge_v3_dist(fge_vec3_t a, fge_vec3_t b) {
    return fge_v3_len(fge_v3_sub(a, b));
}
FGE_INLINE fge_vec3_t fge_v3_reflect(fge_vec3_t v, fge_vec3_t n) {
    return fge_v3_sub(v, fge_v3_mulf(n, 2.0f * fge_v3_dot(v, n)));
}

/* -------------------------------------------------------------------------- */
/* Vec4                                                                       */
/* -------------------------------------------------------------------------- */

typedef struct { float x, y, z, w; } fge_vec4_t;

FGE_INLINE fge_vec4_t fge_v4(float x, float y, float z, float w) { return (fge_vec4_t){x, y, z, w}; }
FGE_INLINE fge_vec4_t fge_v4_from_v3(fge_vec3_t v, float w)      { return (fge_vec4_t){v.x, v.y, v.z, w}; }
FGE_INLINE fge_vec3_t fge_v4_to_v3(fge_vec4_t v)                 { return (fge_vec3_t){v.x, v.y, v.z}; }
FGE_INLINE fge_vec4_t fge_v4_add(fge_vec4_t a, fge_vec4_t b)     { return (fge_vec4_t){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
FGE_INLINE fge_vec4_t fge_v4_mulf(fge_vec4_t a, float s)         { return (fge_vec4_t){a.x * s, a.y * s, a.z * s, a.w * s}; }
FGE_INLINE float      fge_v4_dot(fge_vec4_t a, fge_vec4_t b)     { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

/* -------------------------------------------------------------------------- */
/* Mat4 — column-major, m[col*4 + row]                                        */
/* -------------------------------------------------------------------------- */

typedef struct { float m[16]; } fge_mat4_t;

FGE_INLINE fge_mat4_t fge_m4_identity(void) {
    fge_mat4_t r = {0};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

FGE_INLINE fge_mat4_t fge_m4_translate(fge_vec3_t t) {
    fge_mat4_t r = fge_m4_identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

FGE_INLINE fge_mat4_t fge_m4_scale(fge_vec3_t s) {
    fge_mat4_t r = {0};
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z; r.m[15] = 1.0f;
    return r;
}

FGE_INLINE fge_vec4_t fge_m4_mulv(fge_mat4_t m, fge_vec4_t v) {
    return fge_v4(
        m.m[0]*v.x + m.m[4]*v.y + m.m[8]*v.z  + m.m[12]*v.w,
        m.m[1]*v.x + m.m[5]*v.y + m.m[9]*v.z  + m.m[13]*v.w,
        m.m[2]*v.x + m.m[6]*v.y + m.m[10]*v.z + m.m[14]*v.w,
        m.m[3]*v.x + m.m[7]*v.y + m.m[11]*v.z + m.m[15]*v.w
    );
}

FGE_INLINE fge_vec3_t fge_m4_mulv3(fge_mat4_t m, fge_vec3_t v, float w) {
    fge_vec4_t r = fge_m4_mulv(m, fge_v4(v.x, v.y, v.z, w));
    return fge_v3(r.x, r.y, r.z);
}

fge_mat4_t fge_m4_rotate_x(float angle);
fge_mat4_t fge_m4_rotate_y(float angle);
fge_mat4_t fge_m4_rotate_z(float angle);
fge_mat4_t fge_m4_rotate_axis(fge_vec3_t axis, float angle);
fge_mat4_t fge_m4_mul(fge_mat4_t a, fge_mat4_t b);
fge_mat4_t fge_m4_inverse(fge_mat4_t m);
fge_mat4_t fge_m4_transpose(fge_mat4_t m);
fge_mat4_t fge_m4_perspective(float fovy, float aspect, float near, float far);
fge_mat4_t fge_m4_ortho(float left, float right, float bottom, float top, float near, float far);
fge_mat4_t fge_m4_look_at(fge_vec3_t eye, fge_vec3_t center, fge_vec3_t up);

FGE_INLINE fge_mat4_t fge_m4_ortho_2d(float w, float h, bool y_down) {
    return y_down ? fge_m4_ortho(0, w, h, 0, -1, 1) : fge_m4_ortho(0, w, 0, h, -1, 1);
}

/* -------------------------------------------------------------------------- */
/* Quat                                                                       */
/* -------------------------------------------------------------------------- */

typedef struct { float x, y, z, w; } fge_quat_t;

FGE_INLINE fge_quat_t fge_quat(float x, float y, float z, float w) { return (fge_quat_t){x, y, z, w}; }
FGE_INLINE fge_quat_t fge_quat_identity(void)                      { return (fge_quat_t){0, 0, 0, 1}; }
FGE_INLINE float      fge_quat_len2(fge_quat_t q)                  { return q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w; }
FGE_INLINE float      fge_quat_len(fge_quat_t q)                   { return fge_sqrtf(fge_quat_len2(q)); }
FGE_INLINE fge_quat_t fge_quat_norm(fge_quat_t q) {
    float l = fge_quat_len(q);
    return l > FGE_EPSILON_F ? fge_quat(q.x/l, q.y/l, q.z/l, q.w/l) : fge_quat_identity();
}
FGE_INLINE fge_quat_t fge_quat_mul(fge_quat_t a, fge_quat_t b) {
    return fge_quat(
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    );
}
FGE_INLINE fge_quat_t fge_quat_conj(fge_quat_t q) { return fge_quat(-q.x, -q.y, -q.z, q.w); }
FGE_INLINE fge_vec3_t fge_quat_rotate_vec3(fge_quat_t q, fge_vec3_t v) {
    fge_quat_t p = fge_quat(v.x, v.y, v.z, 0);
    fge_quat_t r = fge_quat_mul(fge_quat_mul(q, p), fge_quat_conj(q));
    return fge_v3(r.x, r.y, r.z);
}

fge_quat_t fge_quat_from_axis_angle(fge_vec3_t axis, float angle);
fge_quat_t fge_quat_slerp(fge_quat_t a, fge_quat_t b, float t);
fge_mat4_t fge_quat_to_mat4(fge_quat_t q);

/* -------------------------------------------------------------------------- */
/* AABB                                                                       */
/* -------------------------------------------------------------------------- */

typedef struct { fge_vec2_t min, max; } fge_aabb2_t;
typedef struct { fge_vec3_t min, max; } fge_aabb3_t;

typedef fge_aabb2_t fge_aabb_t;

FGE_INLINE fge_aabb2_t fge_aabb2(fge_vec2_t min, fge_vec2_t max) { return (fge_aabb2_t){min, max}; }
FGE_INLINE fge_aabb3_t fge_aabb3(fge_vec3_t min, fge_vec3_t max) { return (fge_aabb3_t){min, max}; }

FGE_INLINE bool fge_aabb2_contains(fge_aabb2_t b, fge_vec2_t p) {
    return p.x >= b.min.x && p.x <= b.max.x && p.y >= b.min.y && p.y <= b.max.y;
}
FGE_INLINE bool fge_aabb2_overlaps(fge_aabb2_t a, fge_aabb2_t b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y;
}
FGE_INLINE fge_aabb2_t fge_aabb2_union(fge_aabb2_t a, fge_aabb2_t b) {
    return fge_aabb2(fge_v2_min(a.min, b.min), fge_v2_max(a.max, b.max));
}
FGE_INLINE fge_vec2_t fge_aabb2_size(fge_aabb2_t b)   { return fge_v2_sub(b.max, b.min); }
FGE_INLINE fge_vec2_t fge_aabb2_center(fge_aabb2_t b) { return fge_v2_mulf(fge_v2_add(b.min, b.max), 0.5f); }

FGE_INLINE bool fge_aabb3_contains(fge_aabb3_t b, fge_vec3_t p) {
    return p.x >= b.min.x && p.x <= b.max.x && p.y >= b.min.y && p.y <= b.max.y && p.z >= b.min.z && p.z <= b.max.z;
}
FGE_INLINE bool fge_aabb3_overlaps(fge_aabb3_t a, fge_aabb3_t b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x
        && a.min.y <= b.max.y && a.max.y >= b.min.y
        && a.min.z <= b.max.z && a.max.z >= b.min.z;
}

/* -------------------------------------------------------------------------- */
/* Color                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct { float r, g, b, a; } fge_color_t;
typedef struct { uint8_t r, g, b, a; } fge_color8_t;

FGE_INLINE fge_color_t  fge_color(float r, float g, float b, float a) { return (fge_color_t){r, g, b, a}; }
FGE_INLINE fge_color8_t fge_color8(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return (fge_color8_t){r, g, b, a}; }
FGE_INLINE fge_color_t  fge_color_white(void)  { return fge_color(1, 1, 1, 1); }
FGE_INLINE fge_color_t  fge_color_black(void)  { return fge_color(0, 0, 0, 1); }
FGE_INLINE fge_color_t  fge_color_red(void)    { return fge_color(1, 0, 0, 1); }
FGE_INLINE fge_color_t  fge_color_green(void)  { return fge_color(0, 1, 0, 1); }
FGE_INLINE fge_color_t  fge_color_blue(void)   { return fge_color(0, 0, 1, 1); }

FGE_INLINE fge_color_t fge_color_from_hex(uint32_t hex) {
    return fge_color(
        ((hex >> 16) & 0xFF) / 255.0f,
        ((hex >> 8)  & 0xFF) / 255.0f,
        ((hex >> 0)  & 0xFF) / 255.0f,
        ((hex >> 24) & 0xFF) / 255.0f
    );
}
FGE_INLINE fge_color_t fge_color_lerp(fge_color_t a, fge_color_t b, float t) {
    return fge_color(FGE_LERP(a.r,b.r,t), FGE_LERP(a.g,b.g,t), FGE_LERP(a.b,b.b,t), FGE_LERP(a.a,b.a,t));
}
FGE_INLINE uint32_t fge_color_to_rgba8(fge_color_t c) {
    return ((uint32_t)(uint8_t)(FGE_CLAMP(c.a,0,1)*255) << 24) |
           ((uint32_t)(uint8_t)(FGE_CLAMP(c.b,0,1)*255) << 16) |
           ((uint32_t)(uint8_t)(FGE_CLAMP(c.g,0,1)*255) << 8)  |
           ((uint32_t)(uint8_t)(FGE_CLAMP(c.r,0,1)*255));
}
FGE_INLINE fge_color_t fge_color_premultiply(fge_color_t c) {
    return fge_color(c.r * c.a, c.g * c.a, c.b * c.a, c.a);
}

/* -------------------------------------------------------------------------- */
/* Transform                                                                  */
/* -------------------------------------------------------------------------- */

typedef struct {
    fge_vec3_t position;
    fge_quat_t rotation;
    fge_vec3_t scale;
} fge_transform_t;

FGE_INLINE fge_transform_t fge_transform_default(void) {
    return (fge_transform_t){ fge_v3_zero(), fge_quat_identity(), fge_v3_one() };
}

fge_mat4_t fge_transform_to_mat4(fge_transform_t t);

/* -------------------------------------------------------------------------- */
/* Circle                                                                     */
/* -------------------------------------------------------------------------- */

/* (moved above circle) */

FGE_INLINE float fge_v2_dist2(fge_vec2_t a, fge_vec2_t b);

typedef struct { fge_vec2_t center; float radius; } fge_circle_t;

FGE_INLINE bool fge_circle_contains(fge_circle_t c, fge_vec2_t p) {
    return fge_v2_dist2(c.center, p) <= c.radius * c.radius;
}
FGE_INLINE bool fge_circle_overlaps(fge_circle_t a, fge_circle_t b) {
    float r = a.radius + b.radius;
    return fge_v2_dist2(a.center, b.center) <= r * r;
}

FGE_INLINE float fge_v2_dist2(fge_vec2_t a, fge_vec2_t b) {
    float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

#endif /* FORGE_MATH_H */
