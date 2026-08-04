/*
 * FORGE Game Engine — UI Integration Layer
 * Bridges the ORDL UI toolkit into FORGE's module system.
 *
 * The ORDL UI toolkit provides:
 *   - 7 backends: Terminal, DRM/KMS, Wayland (raw), X11 (raw), GL/ES, Win32
 *   - Canvas: TUI cell grid + RGBA8888 framebuffer
 *   - Widget system: 20+ widget types with flexbox layout
 *   - Software renderer: shapes, text, images, SDF fonts
 *   - Image decoders: PNG, BMP, JPEG, GIF (from scratch)
 *   - Font system: TTF/OTF parser, SDF rasterizer, embedded bitmap fonts
 *   - Animation: 18 easing functions
 *   - Accessibility: ARIA roles, screen reader support
 *   - Gamepad input
 *   - Multi-window manager
 *
 * All ORDL UI APIs are available via forge/ui/ordl_ui.h.
 * This header provides FORGE-branded aliases and bridge helpers.
 */

#ifndef FORGE_UI_H
#define FORGE_UI_H

#include "forge/ui/ordl_ui.h"
#include "forge/ui/ordl_ui_debug.h"
#include "forge/core.h"
#include "forge/math.h"

/* -------------------------------------------------------------------------- */
/* FORGE-branded type aliases                                                 */
/* -------------------------------------------------------------------------- */

typedef ui_backend_t       fge_ui_backend_t;
typedef ui_canvas_t        fge_ui_canvas_t;
typedef ui_widget_t        fge_ui_widget_t;
typedef ui_app_t           fge_ui_app_t;
typedef ui_window_t        fge_ui_window_t;
typedef ui_window_mgr_t    fge_ui_window_mgr_t;
typedef ui_event_t         fge_ui_event_t;
typedef ui_rect_t          fge_ui_rect_t;
typedef ui_color_t         fge_ui_color_t;
typedef ui_layout_t        fge_ui_layout_t;
typedef ui_theme_t         fge_ui_theme_t;
typedef ui_image_t         fge_ui_image_t;
typedef ui_anim_t          fge_ui_anim_t;
typedef ui_gamepad_state_t fge_ui_gamepad_state_t;

typedef ui_event_type_t    fge_ui_event_type_t;
typedef ui_key_t           fge_ui_key_t;
typedef ui_widget_type_t   fge_ui_widget_type_t;
typedef ui_widget_state_t  fge_ui_widget_state_t;
typedef ui_canvas_type_t   fge_ui_canvas_type_t;
typedef ui_direction_t     fge_ui_direction_t;
typedef ui_justify_t       fge_ui_justify_t;
typedef ui_align_t         fge_ui_align_t;
typedef ui_a11y_role_t     fge_ui_a11y_role_t;
typedef ui_ease_fn         fge_ui_ease_fn_t;

/* -------------------------------------------------------------------------- */
/* Color bridge: FORGE math → UI color                                        */
/* -------------------------------------------------------------------------- */

static inline float fge_ui_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline fge_ui_color_t fge_ui_color_from_vec4(fge_vec4_t v) {
    return (fge_ui_color_t){
        (uint8_t)(fge_ui_clampf(v.x, 0.0f, 1.0f) * 255.0f),
        (uint8_t)(fge_ui_clampf(v.y, 0.0f, 1.0f) * 255.0f),
        (uint8_t)(fge_ui_clampf(v.z, 0.0f, 1.0f) * 255.0f),
        (uint8_t)(fge_ui_clampf(v.w, 0.0f, 1.0f) * 255.0f),
    };
}

static inline fge_vec4_t fge_ui_color_to_vec4(fge_ui_color_t c) {
    return (fge_vec4_t){
        c.r / 255.0f,
        c.g / 255.0f,
        c.b / 255.0f,
        c.a / 255.0f,
    };
}

/* -------------------------------------------------------------------------- */
/* Rect bridge: FORGE math → UI rect                                          */
/* -------------------------------------------------------------------------- */

static inline fge_ui_rect_t fge_ui_rect(int x, int y, int w, int h) {
    return (fge_ui_rect_t){x, y, w, h};
}

static inline bool fge_ui_rect_contains(fge_ui_rect_t r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* -------------------------------------------------------------------------- */
/* Backend constructors (FORGE naming)                                        */
/* -------------------------------------------------------------------------- */

static inline fge_ui_backend_t *fge_ui_backend_term(void) {
    return ui_backend_term_new();
}

static inline fge_ui_backend_t *fge_ui_backend_drm(void) {
    return ui_backend_drm_new();
}

static inline fge_ui_backend_t *fge_ui_backend_wayland(void) {
    return ui_backend_wayland_new();
}

static inline fge_ui_backend_t *fge_ui_backend_x11(void) {
    return ui_backend_x11_new();
}

static inline fge_ui_backend_t *fge_ui_backend_gl(void) {
    return ui_backend_gl_new();
}

/* -------------------------------------------------------------------------- */
/* Canvas helpers                                                             */
/* -------------------------------------------------------------------------- */

static inline fge_ui_canvas_t *fge_ui_canvas_term(int cols, int rows) {
    return ui_canvas_new_term(cols, rows);
}

static inline fge_ui_canvas_t *fge_ui_canvas_fb(int w, int h) {
    return ui_canvas_new_fb(w, h);
}

static inline void fge_ui_canvas_free(fge_ui_canvas_t *c) {
    ui_canvas_free(c);
}

/* -------------------------------------------------------------------------- */
/* Widget constructors (FORGE naming)                                         */
/* -------------------------------------------------------------------------- */

static inline fge_ui_widget_t *fge_ui_box(const char *id) {
    return ui_box_new(id);
}

static inline fge_ui_widget_t *fge_ui_label(const char *id, const char *text) {
    return ui_label_new(id, text);
}

static inline fge_ui_widget_t *fge_ui_button(const char *id, const char *label) {
    return ui_button_new(id, label);
}

static inline fge_ui_widget_t *fge_ui_input(const char *id, const char *placeholder) {
    return ui_input_new(id, placeholder);
}

static inline fge_ui_widget_t *fge_ui_scroll(const char *id, fge_ui_widget_t *child) {
    return ui_scroll_new(id, child);
}

static inline fge_ui_widget_t *fge_ui_split(const char *id, fge_ui_direction_t dir,
                                              fge_ui_widget_t *a, fge_ui_widget_t *b, float ratio) {
    return ui_split_new(id, dir, a, b, ratio);
}

static inline void fge_ui_add_child(fge_ui_widget_t *parent, fge_ui_widget_t *child) {
    ui_widget_add_child(parent, child);
}

/* -------------------------------------------------------------------------- */
/* Layout helpers                                                             */
/* -------------------------------------------------------------------------- */

static inline fge_ui_layout_t fge_ui_layout_row(void) {
    return ui_layout_row();
}

static inline fge_ui_layout_t fge_ui_layout_col(void) {
    return ui_layout_col();
}

static inline void fge_ui_layout_run(fge_ui_widget_t *root, fge_ui_canvas_t *canvas) {
    ui_layout_run(root, canvas);
}

/* -------------------------------------------------------------------------- */
/* Application / Window Manager helpers                                       */
/* -------------------------------------------------------------------------- */

static inline fge_ui_app_t *fge_ui_app_new(fge_ui_backend_t *be, const char *title, int w, int h) {
    return ui_app_new(be, title, w, h);
}

static inline void fge_ui_app_run(fge_ui_app_t *app) {
    ui_app_run(app);
}

static inline void fge_ui_app_step(fge_ui_app_t *app) {
    ui_app_step(app);
}

static inline void fge_ui_app_quit(fge_ui_app_t *app) {
    ui_app_quit(app);
}

static inline fge_ui_window_mgr_t *fge_ui_wm_new(void) {
    return ui_window_mgr_new();
}

static inline void fge_ui_wm_run(fge_ui_window_mgr_t *mgr) {
    ui_window_mgr_run(mgr);
}

static inline void fge_ui_wm_step(fge_ui_window_mgr_t *mgr) {
    ui_window_mgr_step(mgr);
}

/* -------------------------------------------------------------------------- */
/* Image loading                                                              */
/* -------------------------------------------------------------------------- */

static inline fge_ui_image_t *fge_ui_image_load_jpeg(const uint8_t *data, size_t len) {
    return ui_image_load_jpeg(data, len);
}

static inline fge_ui_image_t *fge_ui_image_load_png(const uint8_t *data, size_t len) {
    return ui_image_load_png(data, len);
}

static inline void fge_ui_image_free(fge_ui_image_t *img) {
    ui_image_free(img);
}

/* -------------------------------------------------------------------------- */
/* Gamepad                                                                    */
/* -------------------------------------------------------------------------- */

static inline bool fge_ui_gamepad_poll(fge_ui_gamepad_state_t *out) {
    return ui_gamepad_poll(out);
}

/* -------------------------------------------------------------------------- */
/* Debug / Diagnostics                                                        */
/* -------------------------------------------------------------------------- */

static inline void fge_ui_debug_clear(void) {
    ui_debug_clear();
}

#endif /* FORGE_UI_H */
