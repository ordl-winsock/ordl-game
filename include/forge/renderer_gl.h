/*
 * forge/renderer_gl.h — GPU-accelerated 2D sprite renderer
 *
 * Features:
 *   - Streaming VBO sprite batching (thousands of sprites per draw call)
 *   - Runtime GL function loading (zero dependencies, like existing backends)
 *   - OpenGL ES 2.0 / 3.0 compatible (GLES3 features used when available)
 *   - Texture atlas support with per-sprite UVs
 *   - Layer-based sorting, additive blending, tinting, rotation
 *   - Integrates with existing fge_texture_t registry
 *
 * Pure C23, zero external dependencies.
 */

#ifndef FORGE_RENDERER_GL_H
#define FORGE_RENDERER_GL_H

#include "forge/core.h"
#include "forge/math.h"
#include "forge/renderer.h"  /* for fge_texture_t, fge_sprite_t */

/* -------------------------------------------------------------------------- */
/* GL renderer context                                                        */
/* -------------------------------------------------------------------------- */

typedef struct fge_gl_renderer fge_gl_renderer_t;

/* Create GPU renderer. Does NOT create a GL context — caller must make
 * one current before calling (e.g. via ui_backend_gl_x11_new or custom EGL).
 * Returns NULL on failure. */
[[nodiscard]] fge_gl_renderer_t *fge_gl_renderer_create(int width, int height);

/* Destroy renderer and release GPU resources. GL context must be current. */
void fge_gl_renderer_destroy(fge_gl_renderer_t *r);

/* Notify renderer of framebuffer size change. */
void fge_gl_renderer_resize(fge_gl_renderer_t *r, int width, int height);

/* Begin frame: clears color buffer, resets batch. */
void fge_gl_renderer_begin(fge_gl_renderer_t *r, float clear_r, float clear_g,
                            float clear_b, float clear_a);

/* End frame: flushes all batched sprites to GPU. */
void fge_gl_renderer_end(fge_gl_renderer_t *r);

/* -------------------------------------------------------------------------- */
/* Texture management                                                         */
/* -------------------------------------------------------------------------- */

/* Upload RGBA8 pixel data as a GL texture. Returns texture ID (>0) or 0 on failure.
 * If pixels is NULL, creates an empty texture. */
uint32_t fge_gl_tex_create(fge_gl_renderer_t *r, int width, int height,
                            const uint32_t *pixels);

/* Update a sub-region of an existing texture. */
bool fge_gl_tex_update(fge_gl_renderer_t *r, uint32_t tex_id,
                       int x, int y, int w, int h, const uint32_t *pixels);

/* Delete texture. ID becomes invalid. */
void fge_gl_tex_destroy(fge_gl_renderer_t *r, uint32_t tex_id);

/* -------------------------------------------------------------------------- */
/* Sprite batching                                                            */
/* -------------------------------------------------------------------------- */

/* Draw a sprite. Batches internally; actual GPU submission happens in end().
 *
 * tex_id:     from fge_gl_tex_create, or fge_texture_t->id if using shared registry
 * x, y:       center position in pixels (top-left origin, Y down)
 * w, h:       size in pixels
 * rotation:   radians, clockwise
 * color:      RGBA8 tint (multiplied with texture)
 * u0,v0,u1,v1: UV coordinates (0..1)
 * layer:      z-order (lower = drawn first, higher = on top)
 * additive:   true for additive blending, false for normal alpha
 */
void fge_gl_draw_sprite_ex(fge_gl_renderer_t *r,
                           uint32_t tex_id,
                           float x, float y, float w, float h,
                           float rotation, uint32_t color,
                           float u0, float v0, float u1, float v1,
                           int layer, bool additive);

/* Convenience: full-texture sprite, no rotation, normal blend, layer 0 */
FGE_INLINE void fge_gl_draw_sprite(fge_gl_renderer_t *r, uint32_t tex_id,
                                    float x, float y, float w, float h,
                                    uint32_t color) {
    fge_gl_draw_sprite_ex(r, tex_id, x, y, w, h, 0.0f, color,
                          0.0f, 0.0f, 1.0f, 1.0f, 0, false);
}

/* Convenience: colored quad (uses 1x1 white texture) */
void fge_gl_draw_quad(fge_gl_renderer_t *r, float x, float y, float w, float h,
                       uint32_t color);

/* -------------------------------------------------------------------------- */
/* Stats                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t sprites_batched;
    uint32_t draw_calls;
    uint32_t texture_binds;
    uint32_t vertices_uploaded;
} fge_gl_stats_t;

const fge_gl_stats_t *fge_gl_renderer_stats(const fge_gl_renderer_t *r);

#endif /* FORGE_RENDERER_GL_H */
