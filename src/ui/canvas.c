/*
 * ORDL UI — Canvas implementation
 * Rendering surface abstraction: TUI cell grid + framebuffer pixel buffer.
 */

#include "forge/ui/ordl_ui.h"
#include "forge/ui/ordl_ui_debug.h"
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Canvas allocation                                                          */
/* -------------------------------------------------------------------------- */

ui_canvas_t *ui_canvas_new_term(int cols, int rows) {
    if (cols <= 0 || rows <= 0) return NULL;
    ui_canvas_t *c = calloc(1, sizeof(ui_canvas_t));
    if (!c) return NULL;
    c->type = UI_CANVAS_TERM;
    c->w = cols;
    c->h = rows;
    c->dpi_scale = 1.0f;
    c->cells = calloc((size_t)cols * (size_t)rows, sizeof(ui_cell_t));
    if (!c->cells) { free(c); return NULL; }
    c->damage_cap = 64;
    c->damage = calloc(c->damage_cap, sizeof(ui_rect_t));
    if (!c->damage) { free(c->cells); free(c); return NULL; }
    c->clip_depth = 0;
    return c;
}

ui_canvas_t *ui_canvas_new_fb(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    ui_canvas_t *c = calloc(1, sizeof(ui_canvas_t));
    if (!c) return NULL;
    c->type = UI_CANVAS_FB;
    c->w = w;
    c->h = h;
    c->dpi_scale = 1.0f;
    c->pixels = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!c->pixels) { free(c); return NULL; }
    c->damage_cap = 64;
    c->damage = calloc(c->damage_cap, sizeof(ui_rect_t));
    if (!c->damage) { free(c->pixels); free(c); return NULL; }
    c->clip_depth = 0;
    return c;
}

void ui_canvas_free(ui_canvas_t *c) {
    if (!c) return;
    if (c->type == UI_CANVAS_TERM) free(c->cells);
    else free(c->pixels);
    free(c->damage);
    free(c);
}

/* -------------------------------------------------------------------------- */
/* Clip stack                                                                 */
/* -------------------------------------------------------------------------- */

void ui_canvas_push_clip(ui_canvas_t *c, ui_rect_t r) {
    if (!c || c->clip_depth >= 8) return;
    /* Intersect with current clip if any */
    if (c->clip_depth > 0) {
        ui_rect_t *cur = &c->clip_stack[c->clip_depth - 1];
        int x1 = r.x > cur->x ? r.x : cur->x;
        int y1 = r.y > cur->y ? r.y : cur->y;
        int x2 = r.x + r.w < cur->x + cur->w ? r.x + r.w : cur->x + cur->w;
        int y2 = r.y + r.h < cur->y + cur->h ? r.y + r.h : cur->y + cur->h;
        r.x = x1; r.y = y1;
        r.w = x2 - x1; r.h = y2 - y1;
    }
    c->clip_stack[c->clip_depth++] = r;
}

void ui_canvas_pop_clip(ui_canvas_t *c) {
    if (!c || c->clip_depth <= 0) return;
    c->clip_depth--;
}

bool ui_canvas_clip_test(ui_canvas_t *c, int x, int y) {
    if (!c || c->clip_depth <= 0) return true;
    ui_rect_t *clip = &c->clip_stack[c->clip_depth - 1];
    return x >= clip->x && x < clip->x + clip->w &&
           y >= clip->y && y < clip->y + clip->h;
}

bool ui_canvas_clip_rect_test(ui_canvas_t *c, ui_rect_t r) {
    if (!c || c->clip_depth <= 0) return true;
    ui_rect_t *clip = &c->clip_stack[c->clip_depth - 1];
    return r.x < clip->x + clip->w && r.x + r.w > clip->x &&
           r.y < clip->y + clip->h && r.y + r.h > clip->y;
}

/* -------------------------------------------------------------------------- */
/* Damage tracking                                                            */
/* -------------------------------------------------------------------------- */

void ui_canvas_damage(ui_canvas_t *c, ui_rect_t r) {
    if (!c || r.w <= 0 || r.h <= 0) return;
    /* Clamp to canvas bounds (overflow-safe) */
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x >= c->w || r.y >= c->h || r.w <= 0 || r.h <= 0) return;
    if (r.w > c->w - r.x) r.w = c->w - r.x;
    if (r.h > c->h - r.y) r.h = c->h - r.y;
    if (r.w <= 0 || r.h <= 0) return;
    /* Grow array if needed */
    if (c->damage_count >= c->damage_cap) {
        size_t new_cap = c->damage_cap * 2;
        ui_rect_t *new_d = realloc(c->damage, new_cap * sizeof(ui_rect_t));
        if (!new_d) return;
        c->damage = new_d;
        c->damage_cap = new_cap;
    }
    c->damage[c->damage_count++] = r;
}

void ui_canvas_damage_all(ui_canvas_t *c) {
    if (!c || !c->damage) return;
    c->damage_count = 1;
    c->damage[0] = (ui_rect_t){0, 0, c->w, c->h};
}

void ui_canvas_clear(ui_canvas_t *c, ui_color_t bg) {
    ui_debug_log("[canvas_clear] canvas=%dx%d type=%d", c ? c->w : 0, c ? c->h : 0, c ? (int)c->type : -1);
    if (!c) return;
    if (c->type == UI_CANVAS_TERM) {
        ui_cell_t clear_cell = {
            .codepoint = ' ',
            .fg = ui_rgb(192, 192, 192),
            .bg = bg,
            .attrs = 0,
            .z = 0,
        };
        size_t n = (size_t)c->w * (size_t)c->h;
        for (size_t i = 0; i < n; i++) c->cells[i] = clear_cell;
    } else {
        uint32_t rgba = ((uint32_t)bg.r << 0) | ((uint32_t)bg.g << 8) |
                        ((uint32_t)bg.b << 16) | ((uint32_t)bg.a << 24);
        size_t n = (size_t)c->w * (size_t)c->h;
        for (size_t i = 0; i < n; i++) c->pixels[i] = rgba;
    }
    ui_canvas_damage_all(c);
}

/* -------------------------------------------------------------------------- */
/* Drawing primitives                                                         */
/* -------------------------------------------------------------------------- */

static inline bool set_cell(ui_canvas_t *c, int x, int y, uint32_t cp,
                            ui_color_t fg, ui_color_t bg, uint32_t attrs) {
    if (!c || x < 0 || x >= c->w || y < 0 || y >= c->h) return false;
    if (c->clip_depth > 0) {
        ui_rect_t *clip = &c->clip_stack[c->clip_depth - 1];
        if (x < clip->x || x >= clip->x + clip->w ||
            y < clip->y || y >= clip->y + clip->h) return false;
    }
    ui_cell_t *cell = &c->cells[(size_t)y * (size_t)c->w + (size_t)x];
    cell->codepoint = cp;
    cell->fg = fg;
    cell->bg = bg;
    cell->attrs = attrs;
    return true;
}

void ui_canvas_set_cell(ui_canvas_t *c, int x, int y, uint32_t cp,
                        ui_color_t fg, ui_color_t bg, uint32_t attrs) {
    if (!c) return;
    if (set_cell(c, x, y, cp, fg, bg, attrs))
        ui_canvas_damage(c, (ui_rect_t){x, y, 1, 1});
}

void ui_draw_rect(ui_canvas_t *c, ui_rect_t r, ui_color_t fg, ui_color_t bg, uint32_t attrs) {
    if (!c || c->type != UI_CANVAS_TERM) return;
    for (int row = r.y; row < r.y + r.h; row++) {
        for (int col = r.x; col < r.x + r.w; col++) {
            set_cell(c, col, row, ' ', fg, bg, attrs);
        }
    }
    ui_canvas_damage(c, r);
}

void ui_draw_text(ui_canvas_t *c, int x, int y, const char *utf8,
                  ui_color_t fg, ui_color_t bg, uint32_t attrs) {
    if (!c || c->type != UI_CANVAS_TERM || !utf8) return;
    int cx = x;
    size_t offset = 0;
    size_t len = strlen(utf8);
    ui_grapheme_t g;
    while (ui_grapheme_next(utf8, len, &offset, &g)) {
        if (g.codepoint == '\n') {
            cx = x;
            y++;
            continue;
        }
        set_cell(c, cx, y, g.codepoint, fg, bg, attrs);
        cx++;
    }
    int drawn = cx - x;
    if (drawn > 0) {
        if (drawn > c->w - x) drawn = c->w - x;
        ui_canvas_damage(c, (ui_rect_t){x, y, drawn, 1});
    }
}

void ui_draw_text_clipped(ui_canvas_t *c, int x, int y, const char *utf8,
                          ui_color_t fg, ui_color_t bg, uint32_t attrs, int max_w) {
    if (!c || c->type != UI_CANVAS_TERM || !utf8 || max_w <= 0) return;
    int cx = x;
    int drawn = 0;
    size_t offset = 0;
    size_t len = strlen(utf8);
    ui_grapheme_t g;
    while (ui_grapheme_next(utf8, len, &offset, &g)) {
        if (g.codepoint == '\n') break;
        if (drawn >= max_w) break;
        set_cell(c, cx, y, g.codepoint, fg, bg, attrs);
        drawn++;
        cx++;
    }
    if (drawn > 0)
        ui_canvas_damage(c, (ui_rect_t){x, y, cx - x, 1});
}

void ui_draw_line_h(ui_canvas_t *c, int x, int y, int len,
                    uint32_t codepoint, ui_color_t fg, ui_color_t bg) {
    if (!c || c->type != UI_CANVAS_TERM) return;
    for (int i = 0; i < len; i++) {
        set_cell(c, x + i, y, codepoint, fg, bg, 0);
    }
    ui_canvas_damage(c, (ui_rect_t){x, y, len, 1});
}

void ui_draw_line_v(ui_canvas_t *c, int x, int y, int len,
                    uint32_t codepoint, ui_color_t fg, ui_color_t bg) {
    if (!c || c->type != UI_CANVAS_TERM) return;
    for (int i = 0; i < len; i++) {
        set_cell(c, x, y + i, codepoint, fg, bg, 0);
    }
    ui_canvas_damage(c, (ui_rect_t){x, y, 1, len});
}

/* Box drawing characters (Unicode) */
#define BOX_H   0x2500  /* ─ */
#define BOX_V   0x2502  /* │ */
#define BOX_TL  0x250C  /* ┌ */
#define BOX_TR  0x2510  /* ┐ */
#define BOX_BL  0x2514  /* └ */
#define BOX_BR  0x2518  /* ┘ */

#define BOX_H2  0x2550  /* ═ */
#define BOX_V2  0x2551  /* ║ */
#define BOX_TL2 0x2554  /* ╔ */
#define BOX_TR2 0x2557  /* ╗ */
#define BOX_BL2 0x255A  /* ╚ */
#define BOX_BR2 0x255D  /* ╝ */

void ui_draw_box(ui_canvas_t *c, ui_rect_t r, ui_color_t fg, ui_color_t bg, bool double_line) {
    if (!c || c->type != UI_CANVAS_TERM) return;
    int x1 = r.x, y1 = r.y;
    int x2 = (r.w > 0) ? r.x + r.w - 1 : r.x;
    int y2 = (r.h > 0) ? r.y + r.h - 1 : r.y;
    if (x1 > x2) x2 = x1;
    if (y1 > y2) y2 = y1;
    uint32_t h = double_line ? BOX_H2 : BOX_H;
    uint32_t v = double_line ? BOX_V2 : BOX_V;
    uint32_t tl = double_line ? BOX_TL2 : BOX_TL;
    uint32_t tr = double_line ? BOX_TR2 : BOX_TR;
    uint32_t bl = double_line ? BOX_BL2 : BOX_BL;
    uint32_t br = double_line ? BOX_BR2 : BOX_BR;
    /* Corners */
    set_cell(c, x1, y1, tl, fg, bg, 0);
    set_cell(c, x2, y1, tr, fg, bg, 0);
    set_cell(c, x1, y2, bl, fg, bg, 0);
    set_cell(c, x2, y2, br, fg, bg, 0);
    /* Edges */
    for (int x = x1 + 1; x < x2; x++) {
        set_cell(c, x, y1, h, fg, bg, 0);
        set_cell(c, x, y2, h, fg, bg, 0);
    }
    for (int y = y1 + 1; y < y2; y++) {
        set_cell(c, x1, y, v, fg, bg, 0);
        set_cell(c, x2, y, v, fg, bg, 0);
    }
    /* Fill */
    for (int y = y1 + 1; y < y2; y++) {
        for (int x = x1 + 1; x < x2; x++) {
            set_cell(c, x, y, ' ', fg, bg, 0);
        }
    }
    ui_canvas_damage(c, r);
}

/* -------------------------------------------------------------------------- */
/* Grapheme iteration (simplified, no full Unicode segmentation)              */
/* -------------------------------------------------------------------------- */

bool ui_utf8_decode(const char *s, size_t len, size_t *offset, uint32_t *out) {
    if (*offset >= len) return false;
    unsigned char b0 = (unsigned char)s[*offset];
    if (b0 < 0x80) {
        *out = b0;
        *offset += 1;
        return true;
    }
    if ((b0 & 0xE0) == 0xC0 && *offset + 1 < len) {
        *out = ((uint32_t)(b0 & 0x1F) << 6) | ((uint32_t)(s[*offset + 1] & 0x3F));
        *offset += 2;
        return true;
    }
    if ((b0 & 0xF0) == 0xE0 && *offset + 2 < len) {
        *out = ((uint32_t)(b0 & 0x0F) << 12) |
               ((uint32_t)(s[*offset + 1] & 0x3F) << 6) |
               ((uint32_t)(s[*offset + 2] & 0x3F));
        *offset += 3;
        return true;
    }
    if ((b0 & 0xF8) == 0xF0 && *offset + 3 < len) {
        *out = ((uint32_t)(b0 & 0x07) << 18) |
               ((uint32_t)(s[*offset + 1] & 0x3F) << 12) |
               ((uint32_t)(s[*offset + 2] & 0x3F) << 6) |
               ((uint32_t)(s[*offset + 3] & 0x3F));
        *offset += 4;
        return true;
    }
    /* Invalid sequence: skip one byte */
    *out = 0xFFFD;
    *offset += 1;
    return true;
}

bool ui_grapheme_next(const char *utf8, size_t len, size_t *offset, ui_grapheme_t *out) {
    if (!utf8 || !offset || !out || *offset >= len) return false;
    out->start = utf8 + *offset;
    if (!ui_utf8_decode(utf8, len, offset, &out->codepoint)) return false;
    out->end = utf8 + *offset;
    out->bytes = (size_t)(out->end - out->start);
    return true;
}

int ui_text_width(const char *utf8) {
    if (!utf8) return 0;
    int w = 0;
    size_t offset = 0;
    size_t len = strlen(utf8);
    ui_grapheme_t g;
    while (ui_grapheme_next(utf8, len, &offset, &g)) {
        if (g.codepoint == '\n') continue;
        w++;
    }
    return w;
}

int ui_text_height(const char *utf8) {
    if (!utf8) return 0;
    int h = 1;
    for (const char *p = utf8; *p; p++) {
        if (*p == '\n') h++;
    }
    return h;
}
