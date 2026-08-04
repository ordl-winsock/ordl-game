/*
 * forge/renderer.h — 2D Software Renderer
 *
 * Features:
 *   - Sprite batching: accumulate sprites, sort by texture, draw in batches
 *   - Triangle rasterizer: scanline fill with texture mapping
 *   - Framebuffer target: RGBA8888 pixel buffer
 *   - Integration with platform swap_buffers
 *   - Text rendering via SDF (from ORDL UI font system)
 *   - Post-processing: tint, alpha blend
 *
 * Pure C23, zero external dependencies.
 */

#ifndef FORGE_RENDERER_H
#define FORGE_RENDERER_H

#include "forge/core.h"
#include "forge/math.h"
#include "forge/memory.h"
#include "forge/log.h"

/* -------------------------------------------------------------------------- */
/* Framebuffer                                                                */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t *pixels;    /* RGBA8888, row-major */
    int width, height;
    int stride;          /* bytes per row (may include padding) */
    int stride_pixels;   /* stride / sizeof(uint32_t), cached for indexing */
    bool owned;          /* true if we allocated pixels */
} fge_framebuffer_t;

bool fge_fb_init(fge_framebuffer_t *fb, int w, int h);
bool fge_fb_init_from_buffer(fge_framebuffer_t *fb, uint32_t *pixels, int w, int h, int stride);
void fge_fb_free(fge_framebuffer_t *fb);

FGE_INLINE void fge_fb_clear(fge_framebuffer_t *fb, uint32_t color) {
    if (!fb || !fb->pixels) return;
    size_t n = (size_t)fb->stride_pixels * (size_t)fb->height;
    /* Fill 2 pixels at a time with 64-bit stores when possible */
    if (fb->stride_pixels == fb->width && ((uintptr_t)fb->pixels & 7) == 0) {
        uint64_t c64 = ((uint64_t)color << 32) | color;
        uint64_t *p64 = (uint64_t *)fb->pixels;
        size_t n64 = n / 2;
        for (size_t i = 0; i < n64; i++) p64[i] = c64;
        if (n & 1) fb->pixels[n - 1] = color;
    } else {
        for (size_t i = 0; i < n; i++) fb->pixels[i] = color;
    }
}

FGE_INLINE void fge_fb_pixel(fge_framebuffer_t *fb, int x, int y, uint32_t color) {
    if (!fb || x < 0 || x >= fb->width || y < 0 || y >= fb->height) return;
    fb->pixels[y * fb->stride_pixels + x] = color;
}

/* -------------------------------------------------------------------------- */
/* Texture — CPU-side image data                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t *pixels;
    int width, height;
    uint32_t id;
    bool owned;
} fge_texture_t;

bool fge_tex_init(fge_texture_t *tex, int w, int h);
bool fge_tex_init_from_rgba(fge_texture_t *tex, uint32_t *pixels, int w, int h);
void fge_tex_free(fge_texture_t *tex);

/* Load from raw file data (PNG/JPEG/GIF/BMP decoded by ORDL UI) */
bool fge_tex_load_from_memory(fge_texture_t *tex, const uint8_t *data, size_t len,
                               const char *ext_hint);

/* Sample with bilinear filtering */
uint32_t fge_tex_sample_bilinear(const fge_texture_t *tex, float u, float v);

/* -------------------------------------------------------------------------- */
/* Sprite batch                                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
    fge_vec2_t pos;
    fge_vec2_t size;
    fge_vec2_t uv0, uv1;     /* texture coordinates */
    uint32_t color;          /* tint (RGBA) */
    uint32_t tex_id;         /* texture handle */
    float rotation;          /* radians around center */
    int layer;               /* z-order, higher = on top */
    bool additive;           /* additive blend mode */
} fge_sprite_t;

#define FGE_MAX_BATCH_SPRITES 4096

typedef struct {
    fge_sprite_t sprites[FGE_MAX_BATCH_SPRITES];
    uint32_t count;
    fge_framebuffer_t *target;
} fge_sprite_batch_t;

void fge_batch_init(fge_sprite_batch_t *batch, fge_framebuffer_t *target);
void fge_batch_clear(fge_sprite_batch_t *batch);

/* Add sprite to batch. Flushes automatically if full. */
void fge_batch_add(fge_sprite_batch_t *batch, const fge_sprite_t *sprite);

/* Draw all sprites sorted by layer and texture */
void fge_batch_flush(fge_sprite_batch_t *batch);

/* -------------------------------------------------------------------------- */
/* Low-level rasterizer                                                       */
/* -------------------------------------------------------------------------- */

/* Fill axis-aligned rectangle */
void fge_draw_rect(fge_framebuffer_t *fb, int x, int y, int w, int h, uint32_t color);

/* Fill triangle with flat color */
void fge_draw_triangle(fge_framebuffer_t *fb,
                        fge_vec2_t v0, fge_vec2_t v1, fge_vec2_t v2,
                        uint32_t color);

/* Fill triangle with texture mapping */
void fge_draw_triangle_tex(fge_framebuffer_t *fb,
                            fge_vec2_t v0, fge_vec2_t v1, fge_vec2_t v2,
                            fge_vec2_t uv0, fge_vec2_t uv1, fge_vec2_t uv2,
                            const fge_texture_t *tex, uint32_t tint);

/* Draw line (Wu anti-aliased or Bresenham) */
void fge_draw_line(fge_framebuffer_t *fb, fge_vec2_t a, fge_vec2_t b,
                    float thickness, uint32_t color);

/* Draw circle (filled) */
void fge_draw_circle(fge_framebuffer_t *fb, fge_vec2_t center, float radius, uint32_t color);

/* Draw text using embedded bitmap font (8x16) */
void fge_draw_text(fge_framebuffer_t *fb, const char *text, int x, int y,
                    uint32_t color, float scale);

/* -------------------------------------------------------------------------- */
/* Renderer context                                                           */
/* -------------------------------------------------------------------------- */

typedef struct {
    fge_framebuffer_t fb;
    fge_sprite_batch_t batch;
    uint64_t frame_count;
    uint64_t draw_calls;
    uint64_t triangles;
} fge_renderer_t;

bool fge_renderer_init(fge_renderer_t *r, int w, int h);
void fge_renderer_shutdown(fge_renderer_t *r);
void fge_renderer_resize(fge_renderer_t *r, int w, int h);

/* Begin frame — clears framebuffer */
void fge_renderer_begin(fge_renderer_t *r, uint32_t clear_color);

/* End frame — flushes batch */
void fge_renderer_end(fge_renderer_t *r);

/* Convenience: draw a textured quad */
void fge_renderer_draw_sprite(fge_renderer_t *r, const fge_texture_t *tex,
                               fge_vec2_t pos, fge_vec2_t size, uint32_t tint);

/* Convenience: draw colored quad */
void fge_renderer_draw_quad(fge_renderer_t *r, fge_vec2_t pos, fge_vec2_t size, uint32_t color);

/* Get framebuffer pointer for platform swap */
FGE_INLINE fge_framebuffer_t *fge_renderer_fb(fge_renderer_t *r) {
    return r ? &r->fb : NULL;
}

#endif /* FORGE_RENDERER_H */
