/*
 * src/renderer/renderer_2d.c — FORGE 2D Software Renderer
 *
 * Features:
 *   - RGBA8888 framebuffer
 *   - Sprite batching (4096 sprites, sorted by layer/texture)
 *   - Edge-function triangle rasterizer with flat color and affine texture mapping
 *   - Bresenham line drawing (with thickness)
 *   - Filled circle (midpoint algorithm)
 *   - 8x16 embedded bitmap font text rendering
 *   - Alpha blending (standard and additive)
 *
 * Pure C23, zero external dependencies.
 */

#include "forge/core.h"
#include "forge/math.h"
#include "forge/memory.h"
#include "forge/log.h"
#include "forge/renderer.h"

#include <string.h>

/* -------------------------------------------------------------------------- */
/* Inline helpers                                                             */
/* -------------------------------------------------------------------------- */

FGE_INLINE uint32_t fge_blend_rgba(uint32_t dst, uint32_t src) {
    uint32_t sa = (src >> 24) & 0xFF;
    if (sa == 0) return dst;
    if (sa == 255) return src;
    uint32_t da = (dst >> 24) & 0xFF;
    uint32_t inv_sa = 255 - sa;
    /* (x*sa + y*inv) / 255 == (x*sa + y*inv + 1 + ((x*sa + y*inv) >> 8)) >> 8 */
    uint32_t r = (src & 0xFF) * sa + (dst & 0xFF) * inv_sa;
    uint32_t g = ((src >> 8) & 0xFF) * sa + ((dst >> 8) & 0xFF) * inv_sa;
    uint32_t b = ((src >> 16) & 0xFF) * sa + ((dst >> 16) & 0xFF) * inv_sa;
    uint32_t a = sa * 255 + da * inv_sa;
    r = (r + 1 + (r >> 8)) >> 8;
    g = (g + 1 + (g >> 8)) >> 8;
    b = (b + 1 + (b >> 8)) >> 8;
    a = (a + 1 + (a >> 8)) >> 8;
    return (a << 24) | (b << 16) | (g << 8) | r;
}

FGE_INLINE uint32_t fge_blend_additive(uint32_t dst, uint32_t src) {
    uint8_t sr = src & 0xFF, sg = (src >> 8) & 0xFF, sb = (src >> 16) & 0xFF, sa = (src >> 24) & 0xFF;
    uint8_t dr = dst & 0xFF, dg = (dst >> 8) & 0xFF, db = (dst >> 16) & 0xFF, da = (dst >> 24) & 0xFF;
    uint16_t r = (uint16_t)sr + dr;
    uint16_t g = (uint16_t)sg + dg;
    uint16_t b = (uint16_t)sb + db;
    uint16_t a = (uint16_t)sa + da;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (a > 255) a = 255;
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

FGE_INLINE uint8_t fge_lerp_u8(uint8_t a, uint8_t b, float t) {
    return (uint8_t)(a + (b - a) * t);
}

static uint32_t fge_tex_modulate(uint32_t texel, uint32_t tint) {
    uint32_t tr = tint & 0xFF, tg = (tint >> 8) & 0xFF, tb = (tint >> 16) & 0xFF, ta = (tint >> 24) & 0xFF;
    uint32_t sr = texel & 0xFF, sg = (texel >> 8) & 0xFF, sb = (texel >> 16) & 0xFF, sa = (texel >> 24) & 0xFF;
    uint32_t r = (sr * tr + 1 + ((sr * tr) >> 8)) >> 8;
    uint32_t g = (sg * tg + 1 + ((sg * tg) >> 8)) >> 8;
    uint32_t b = (sb * tb + 1 + ((sb * tb) >> 8)) >> 8;
    uint32_t a = (sa * ta + 1 + ((sa * ta) >> 8)) >> 8;
    return (a << 24) | (b << 16) | (g << 8) | r;
}

/* -------------------------------------------------------------------------- */
/* Texture registry                                                           */
/* -------------------------------------------------------------------------- */

#define FGE_TEX_REGISTRY_INIT_CAP 64

static fge_texture_t **s_tex_registry = NULL;
static size_t s_tex_registry_cap = 0;
static uint32_t s_next_tex_id = 1;

static void fge_tex_registry_ensure(uint32_t id) {
    if (id < s_tex_registry_cap) return;
    size_t new_cap = s_tex_registry_cap ? s_tex_registry_cap : FGE_TEX_REGISTRY_INIT_CAP;
    while (new_cap <= id) new_cap *= 2;
    fge_texture_t **new_reg = (fge_texture_t **)FGE_REALLOC(s_tex_registry, new_cap * sizeof(fge_texture_t *));
    if (!new_reg) return;
    memset(new_reg + s_tex_registry_cap, 0, (new_cap - s_tex_registry_cap) * sizeof(fge_texture_t *));
    s_tex_registry = new_reg;
    s_tex_registry_cap = new_cap;
}

static void fge_tex_registry_add(fge_texture_t *tex) {
    fge_tex_registry_ensure(tex->id);
    if (tex->id < s_tex_registry_cap) {
        s_tex_registry[tex->id] = tex;
    }
}

static void fge_tex_registry_remove(uint32_t id) {
    if (id < s_tex_registry_cap) {
        s_tex_registry[id] = NULL;
    }
}

static fge_texture_t *fge_tex_registry_get(uint32_t id) {
    if (id < s_tex_registry_cap) {
        return s_tex_registry[id];
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Framebuffer                                                                */
/* -------------------------------------------------------------------------- */

bool fge_fb_init(fge_framebuffer_t *fb, int w, int h) {
    if (!fb || w <= 0 || h <= 0) return false;
    size_t n = (size_t)w * (size_t)h;
    uint32_t *pixels = (uint32_t *)FGE_MALLOC(n * sizeof(uint32_t));
    if (!pixels) return false;
    fb->pixels = pixels;
    fb->width = w;
    fb->height = h;
    fb->stride = w * (int)sizeof(uint32_t);
    fb->stride_pixels = w;
    fb->owned = true;
    return true;
}

bool fge_fb_init_from_buffer(fge_framebuffer_t *fb, uint32_t *pixels, int w, int h, int stride) {
    if (!fb || !pixels || w <= 0 || h <= 0 || stride < w * (int)sizeof(uint32_t)) return false;
    fb->pixels = pixels;
    fb->width = w;
    fb->height = h;
    fb->stride = stride;
    fb->stride_pixels = stride / (int)sizeof(uint32_t);
    fb->owned = false;
    return true;
}

void fge_fb_free(fge_framebuffer_t *fb) {
    if (!fb) return;
    if (fb->owned && fb->pixels) {
        FGE_FREE(fb->pixels);
    }
    fb->pixels = NULL;
    fb->width = fb->height = fb->stride = fb->stride_pixels = 0;
    fb->owned = false;
}

/* -------------------------------------------------------------------------- */
/* Texture                                                                    */
/* -------------------------------------------------------------------------- */

bool fge_tex_init(fge_texture_t *tex, int w, int h) {
    if (!tex || w <= 0 || h <= 0) return false;
    size_t n = (size_t)w * (size_t)h;
    uint32_t *pixels = (uint32_t *)FGE_MALLOC(n * sizeof(uint32_t));
    if (!pixels) return false;
    tex->pixels = pixels;
    tex->width = w;
    tex->height = h;
    tex->id = s_next_tex_id++;
    tex->owned = true;
    fge_tex_registry_add(tex);
    return true;
}

bool fge_tex_init_from_rgba(fge_texture_t *tex, uint32_t *pixels, int w, int h) {
    if (!tex || !pixels || w <= 0 || h <= 0) return false;
    tex->pixels = pixels;
    tex->width = w;
    tex->height = h;
    tex->id = s_next_tex_id++;
    tex->owned = false;
    fge_tex_registry_add(tex);
    return true;
}

void fge_tex_free(fge_texture_t *tex) {
    if (!tex) return;
    fge_tex_registry_remove(tex->id);
    if (tex->owned && tex->pixels) {
        FGE_FREE(tex->pixels);
    }
    tex->pixels = NULL;
    tex->width = tex->height = 0;
    tex->id = 0;
    tex->owned = false;
}

bool fge_tex_load_from_memory(fge_texture_t *tex, const uint8_t *data, size_t len, const char *ext_hint) {
    (void)tex; (void)data; (void)len; (void)ext_hint;
    return false;
}

uint32_t fge_tex_sample_bilinear(const fge_texture_t *tex, float u, float v) {
    if (!tex || !tex->pixels || tex->width <= 0 || tex->height <= 0) return 0;
    if (u < 0.0f) u = 0.0f;
    if (v < 0.0f) v = 0.0f;
    if (u > 1.0f) u = 1.0f;
    if (v > 1.0f) v = 1.0f;
    float x = u * (float)tex->width - 0.5f;
    float y = v * (float)tex->height - 0.5f;
    int x0 = (int)fge_floorf(x);
    int y0 = (int)fge_floorf(y);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    float fx = x - (float)x0;
    float fy = y - (float)y0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= tex->width) x1 = tex->width - 1;
    if (y1 >= tex->height) y1 = tex->height - 1;
    uint32_t c00 = tex->pixels[y0 * tex->width + x0];
    uint32_t c10 = tex->pixels[y0 * tex->width + x1];
    uint32_t c01 = tex->pixels[y1 * tex->width + x0];
    uint32_t c11 = tex->pixels[y1 * tex->width + x1];
    uint8_t c0r = fge_lerp_u8(c00 & 0xFF, c10 & 0xFF, fx);
    uint8_t c0g = fge_lerp_u8((c00 >> 8) & 0xFF, (c10 >> 8) & 0xFF, fx);
    uint8_t c0b = fge_lerp_u8((c00 >> 16) & 0xFF, (c10 >> 16) & 0xFF, fx);
    uint8_t c0a = fge_lerp_u8((c00 >> 24) & 0xFF, (c10 >> 24) & 0xFF, fx);
    uint8_t c1r = fge_lerp_u8(c01 & 0xFF, c11 & 0xFF, fx);
    uint8_t c1g = fge_lerp_u8((c01 >> 8) & 0xFF, (c11 >> 8) & 0xFF, fx);
    uint8_t c1b = fge_lerp_u8((c01 >> 16) & 0xFF, (c11 >> 16) & 0xFF, fx);
    uint8_t c1a = fge_lerp_u8((c01 >> 24) & 0xFF, (c11 >> 24) & 0xFF, fx);
    uint8_t r = fge_lerp_u8(c0r, c1r, fy);
    uint8_t g = fge_lerp_u8(c0g, c1g, fy);
    uint8_t b = fge_lerp_u8(c0b, c1b, fy);
    uint8_t a = fge_lerp_u8(c0a, c1a, fy);
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

/* -------------------------------------------------------------------------- */
/* Low-level rasterizer                                                       */
/* -------------------------------------------------------------------------- */

static void fge_draw_hline_blend(fge_framebuffer_t *fb, int x1, int x2, int y, uint32_t color) {
    if (y < 0 || y >= fb->height) return;
    if (x1 < 0) x1 = 0;
    if (x2 >= fb->width) x2 = fb->width - 1;
    if (x1 > x2) return;
    uint32_t *row = fb->pixels + y * fb->stride_pixels;
    uint8_t ca = (color >> 24) & 0xFF;
    if (ca == 255) {
        for (int x = x1; x <= x2; ++x) row[x] = color;
    } else if (ca > 0) {
        for (int x = x1; x <= x2; ++x) row[x] = fge_blend_rgba(row[x], color);
    }
}

void fge_draw_rect(fge_framebuffer_t *fb, int x, int y, int w, int h, uint32_t color) {
    if (!fb || !fb->pixels || w <= 0 || h <= 0) return;
    int x0 = FGE_MAX(x, 0);
    int y0 = FGE_MAX(y, 0);
    int x1 = FGE_MIN(x + w, fb->width);
    int y1 = FGE_MIN(y + h, fb->height);
    uint8_t ca = (color >> 24) & 0xFF;
    for (int py = y0; py < y1; ++py) {
        uint32_t *row = fb->pixels + py * fb->stride_pixels;
        if (ca == 255) {
            for (int px = x0; px < x1; ++px) row[px] = color;
        } else if (ca > 0) {
            for (int px = x0; px < x1; ++px) row[px] = fge_blend_rgba(row[px], color);
        }
    }
}

void fge_draw_triangle(fge_framebuffer_t *fb,
                        fge_vec2_t v0, fge_vec2_t v1, fge_vec2_t v2,
                        uint32_t color) {
    if (!fb || !fb->pixels) return;
    int minx = (int)fge_floorf(FGE_MIN(v0.x, FGE_MIN(v1.x, v2.x)));
    int miny = (int)fge_floorf(FGE_MIN(v0.y, FGE_MIN(v1.y, v2.y)));
    int maxx = (int)fge_ceilf(FGE_MAX(v0.x, FGE_MAX(v1.x, v2.x)));
    int maxy = (int)fge_ceilf(FGE_MAX(v0.y, FGE_MAX(v1.y, v2.y)));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > fb->width) maxx = fb->width;
    if (maxy > fb->height) maxy = fb->height;
    float area = (v1.x - v0.x) * (v2.y - v0.y) - (v1.y - v0.y) * (v2.x - v0.x);
    if (area == 0.0f) return;
    uint8_t ca = (color >> 24) & 0xFF;
    for (int y = miny; y < maxy; ++y) {
        for (int x = minx; x < maxx; ++x) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;
            float w0 = (v1.x - px) * (v2.y - py) - (v1.y - py) * (v2.x - px);
            float w1 = (v2.x - px) * (v0.y - py) - (v2.y - py) * (v0.x - px);
            float w2 = (v0.x - px) * (v1.y - py) - (v0.y - py) * (v1.x - px);
            bool inside = (area > 0.0f) ? (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f)
                                        : (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f);
            if (inside) {
                uint32_t *p = &fb->pixels[y * fb->stride_pixels + x];
                if (ca == 255) {
                    *p = color;
                } else if (ca > 0) {
                    *p = fge_blend_rgba(*p, color);
                }
            }
        }
    }
}

static void fge_draw_triangle_tex_ex(fge_framebuffer_t *fb,
                                      fge_vec2_t v0, fge_vec2_t v1, fge_vec2_t v2,
                                      fge_vec2_t uv0, fge_vec2_t uv1, fge_vec2_t uv2,
                                      const fge_texture_t *tex, uint32_t tint, bool additive) {
    if (!fb || !fb->pixels || !tex || !tex->pixels) return;
    int minx = (int)fge_floorf(FGE_MIN(v0.x, FGE_MIN(v1.x, v2.x)));
    int miny = (int)fge_floorf(FGE_MIN(v0.y, FGE_MIN(v1.y, v2.y)));
    int maxx = (int)fge_ceilf(FGE_MAX(v0.x, FGE_MAX(v1.x, v2.x)));
    int maxy = (int)fge_ceilf(FGE_MAX(v0.y, FGE_MAX(v1.y, v2.y)));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > fb->width) maxx = fb->width;
    if (maxy > fb->height) maxy = fb->height;
    float area = (v1.x - v0.x) * (v2.y - v0.y) - (v1.y - v0.y) * (v2.x - v0.x);
    if (area == 0.0f) return;
    float inv_area = 1.0f / area;
    for (int y = miny; y < maxy; ++y) {
        for (int x = minx; x < maxx; ++x) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;
            float w0 = (v1.x - px) * (v2.y - py) - (v1.y - py) * (v2.x - px);
            float w1 = (v2.x - px) * (v0.y - py) - (v2.y - py) * (v0.x - px);
            float w2 = (v0.x - px) * (v1.y - py) - (v0.y - py) * (v1.x - px);
            bool inside = (area > 0.0f) ? (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f)
                                        : (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f);
            if (inside) {
                float u = (w0 * uv0.x + w1 * uv1.x + w2 * uv2.x) * inv_area;
                float v = (w0 * uv0.y + w1 * uv1.y + w2 * uv2.y) * inv_area;
                uint32_t texel = fge_tex_sample_bilinear(tex, u, v);
                uint32_t color = fge_tex_modulate(texel, tint);
                uint8_t ca = (color >> 24) & 0xFF;
                uint32_t *p = &fb->pixels[y * fb->stride_pixels + x];
                if (additive) {
                    *p = fge_blend_additive(*p, color);
                } else {
                    if (ca == 255) {
                        *p = color;
                    } else if (ca > 0) {
                        *p = fge_blend_rgba(*p, color);
                    }
                }
            }
        }
    }
}

void fge_draw_triangle_tex(fge_framebuffer_t *fb,
                            fge_vec2_t v0, fge_vec2_t v1, fge_vec2_t v2,
                            fge_vec2_t uv0, fge_vec2_t uv1, fge_vec2_t uv2,
                            const fge_texture_t *tex, uint32_t tint) {
    fge_draw_triangle_tex_ex(fb, v0, v1, v2, uv0, uv1, uv2, tex, tint, false);
}

void fge_draw_line(fge_framebuffer_t *fb, fge_vec2_t a, fge_vec2_t b,
                    float thickness, uint32_t color) {
    if (!fb || !fb->pixels) return;
    int x0 = (int)fge_roundf(a.x);
    int y0 = (int)fge_roundf(a.y);
    int x1 = (int)fge_roundf(b.x);
    int y1 = (int)fge_roundf(b.y);
    int t = (int)(thickness + 0.5f);
    if (t <= 1) {
        int dx = FGE_ABS(x1 - x0);
        int dy = FGE_ABS(y1 - y0);
        int sx = (x0 < x1) ? 1 : -1;
        int sy = (y0 < y1) ? 1 : -1;
        int err = dx - dy;
        uint8_t ca = (color >> 24) & 0xFF;
        while (1) {
            if (x0 >= 0 && x0 < fb->width && y0 >= 0 && y0 < fb->height) {
                uint32_t *p = &fb->pixels[y0 * fb->stride_pixels + x0];
                if (ca == 255) *p = color;
                else if (ca > 0) *p = fge_blend_rgba(*p, color);
            }
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 < dx) { err += dx; y0 += sy; }
        }
    } else {
        int half = t / 2;
        int dx = FGE_ABS(x1 - x0);
        int dy = FGE_ABS(y1 - y0);
        int sx = (x0 < x1) ? 1 : -1;
        int sy = (y0 < y1) ? 1 : -1;
        int err = dx - dy;
        while (1) {
            fge_draw_rect(fb, x0 - half, y0 - half, t, t, color);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 < dx) { err += dx; y0 += sy; }
        }
    }
}

void fge_draw_circle(fge_framebuffer_t *fb, fge_vec2_t center, float radius, uint32_t color) {
    if (!fb || !fb->pixels || radius <= 0.0f) return;
    int cx = (int)fge_roundf(center.x);
    int cy = (int)fge_roundf(center.y);
    int r = (int)fge_roundf(radius);
    if (r <= 0) return;
    int x = r;
    int y = 0;
    int d = 1 - r;
    while (x >= y) {
        fge_draw_hline_blend(fb, cx - x, cx + x, cy + y, color);
        fge_draw_hline_blend(fb, cx - x, cx + x, cy - y, color);
        fge_draw_hline_blend(fb, cx - y, cx + y, cy + x, color);
        fge_draw_hline_blend(fb, cx - y, cx + y, cy - x, color);
        y++;
        if (d <= 0) {
            d = d + 2 * y + 1;
        } else {
            x--;
            d = d + 2 * (y - x) + 1;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Font data                                                                  */
/* -------------------------------------------------------------------------- */

#include "font_builtin_data.inc"

void fge_draw_text(fge_framebuffer_t *fb, const char *text, int x, int y,
                    uint32_t color, float scale) {
    if (!fb || !fb->pixels || !text || scale <= 0.0f) return;
    int s = (int)(scale + 0.5f);
    if (s < 1) s = 1;
    int advance = 8 * s;
    int pen_x = x;
    int pen_y = y;
    uint8_t ca = (color >> 24) & 0xFF;
    for (const char *p = text; *p; ++p) {
        unsigned char ch = (unsigned char)*p;
        if (ch >= FONT_FIRST_CHAR && ch <= FONT_LAST_CHAR) {
            const uint8_t *glyph = s_font_8x16_data[ch - FONT_FIRST_CHAR];
            for (int row = 0; row < 16; ++row) {
                uint8_t bits = glyph[row];
                if (bits == 0) continue;
                for (int col = 0; col < 8; ++col) {
                    if (bits & (1 << (7 - col))) {
                        int px = pen_x + col * s;
                        int py = pen_y + row * s;
                        if (s == 1) {
                            if (px >= 0 && px < fb->width && py >= 0 && py < fb->height) {
                                uint32_t *pix = &fb->pixels[py * fb->stride_pixels + px];
                                if (ca == 255) *pix = color;
                                else if (ca > 0) *pix = fge_blend_rgba(*pix, color);
                            }
                        } else {
                            fge_draw_rect(fb, px, py, s, s, color);
                        }
                    }
                }
            }
        }
        pen_x += advance;
    }
}

/* -------------------------------------------------------------------------- */
/* Sprite batch                                                               */
/* -------------------------------------------------------------------------- */

void fge_batch_init(fge_sprite_batch_t *batch, fge_framebuffer_t *target) {
    if (!batch) return;
    batch->count = 0;
    batch->target = target;
}

void fge_batch_clear(fge_sprite_batch_t *batch) {
    if (!batch) return;
    batch->count = 0;
}

void fge_batch_add(fge_sprite_batch_t *batch, const fge_sprite_t *sprite) {
    if (!batch || !sprite) return;
    if (batch->count >= FGE_MAX_BATCH_SPRITES) {
        fge_batch_flush(batch);
    }
    batch->sprites[batch->count++] = *sprite;
}

static int sprite_cmp(const void *a, const void *b) {
    const fge_sprite_t *sa = (const fge_sprite_t *)a;
    const fge_sprite_t *sb = (const fge_sprite_t *)b;
    if (sa->layer != sb->layer) return (sa->layer < sb->layer) ? -1 : 1;
    if (sa->tex_id != sb->tex_id) return (sa->tex_id < sb->tex_id) ? -1 : 1;
    return 0;
}

static void fge_batch_sort(fge_sprite_batch_t *batch) {
    if (batch->count < 2) return;
    qsort(batch->sprites, batch->count, sizeof(fge_sprite_t), sprite_cmp);
}

void fge_batch_flush(fge_sprite_batch_t *batch) {
    if (!batch || !batch->target || batch->count == 0) return;
    fge_batch_sort(batch);
    fge_framebuffer_t *fb = batch->target;
    for (uint32_t i = 0; i < batch->count; ++i) {
        const fge_sprite_t *s = &batch->sprites[i];
        fge_texture_t *tex = fge_tex_registry_get(s->tex_id);
        if (!tex) continue;
        float hw = s->size.x * 0.5f;
        float hh = s->size.y * 0.5f;
        fge_vec2_t p0 = fge_v2(-hw, -hh);
        fge_vec2_t p1 = fge_v2( hw, -hh);
        fge_vec2_t p2 = fge_v2( hw,  hh);
        fge_vec2_t p3 = fge_v2(-hw,  hh);
        if (s->rotation != 0.0f) {
            float c = fge_cosf(s->rotation);
            float sn = fge_sinf(s->rotation);
            p0 = fge_v2(p0.x * c - p0.y * sn, p0.x * sn + p0.y * c);
            p1 = fge_v2(p1.x * c - p1.y * sn, p1.x * sn + p1.y * c);
            p2 = fge_v2(p2.x * c - p2.y * sn, p2.x * sn + p2.y * c);
            p3 = fge_v2(p3.x * c - p3.y * sn, p3.x * sn + p3.y * c);
        }
        p0 = fge_v2_add(p0, s->pos);
        p1 = fge_v2_add(p1, s->pos);
        p2 = fge_v2_add(p2, s->pos);
        p3 = fge_v2_add(p3, s->pos);
        fge_vec2_t uv0 = s->uv0;
        fge_vec2_t uv1 = fge_v2(s->uv1.x, s->uv0.y);
        fge_vec2_t uv2 = s->uv1;
        fge_vec2_t uv3 = fge_v2(s->uv0.x, s->uv1.y);
        fge_draw_triangle_tex_ex(fb, p0, p1, p2, uv0, uv1, uv2, tex, s->color, s->additive);
        fge_draw_triangle_tex_ex(fb, p0, p2, p3, uv0, uv2, uv3, tex, s->color, s->additive);
    }
    batch->count = 0;
}

/* -------------------------------------------------------------------------- */
/* Renderer context                                                           */
/* -------------------------------------------------------------------------- */

bool fge_renderer_init(fge_renderer_t *r, int w, int h) {
    if (!r) return false;
    memset(r, 0, sizeof(*r));
    if (!fge_fb_init(&r->fb, w, h)) {
        FGE_ERROR(FGE_LOG_CAT_RENDERER, "Failed to create framebuffer %dx%d", w, h);
        return false;
    }
    fge_batch_init(&r->batch, &r->fb);
    FGE_INFO(FGE_LOG_CAT_RENDERER, "Renderer initialized %dx%d", w, h);
    return true;
}

void fge_renderer_shutdown(fge_renderer_t *r) {
    if (!r) return;
    fge_renderer_end(r);
    fge_fb_free(&r->fb);
    memset(r, 0, sizeof(*r));
    FGE_INFO(FGE_LOG_CAT_RENDERER, "Renderer shutdown");
}

void fge_renderer_resize(fge_renderer_t *r, int w, int h) {
    if (!r) return;
    fge_renderer_end(r);
    fge_fb_free(&r->fb);
    if (!fge_fb_init(&r->fb, w, h)) {
        FGE_ERROR(FGE_LOG_CAT_RENDERER, "Failed to resize framebuffer to %dx%d", w, h);
        return;
    }
    r->batch.target = &r->fb;
}

void fge_renderer_begin(fge_renderer_t *r, uint32_t clear_color) {
    if (!r) return;
    fge_fb_clear(&r->fb, clear_color);
    r->draw_calls = 0;
    r->triangles = 0;
}

void fge_renderer_end(fge_renderer_t *r) {
    if (!r) return;
    fge_batch_flush(&r->batch);
    r->frame_count++;
}

void fge_renderer_draw_sprite(fge_renderer_t *r, const fge_texture_t *tex,
                               fge_vec2_t pos, fge_vec2_t size, uint32_t tint) {
    if (!r || !tex) return;
    fge_sprite_t s = {0};
    s.pos = pos;
    s.size = size;
    s.uv0 = fge_v2(0.0f, 0.0f);
    s.uv1 = fge_v2(1.0f, 1.0f);
    s.color = tint;
    s.tex_id = tex->id;
    s.rotation = 0.0f;
    s.layer = 0;
    s.additive = false;
    fge_batch_add(&r->batch, &s);
    r->draw_calls++;
    r->triangles += 2;
}

void fge_renderer_draw_quad(fge_renderer_t *r, fge_vec2_t pos, fge_vec2_t size, uint32_t color) {
    if (!r) return;
    int x = (int)fge_roundf(pos.x);
    int y = (int)fge_roundf(pos.y);
    int w = (int)fge_roundf(size.x);
    int h = (int)fge_roundf(size.y);
    fge_draw_rect(&r->fb, x, y, w, h, color);
    r->draw_calls++;
    r->triangles += 2;
}
