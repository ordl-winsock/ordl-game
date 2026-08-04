/*
 * ORDL UI — Sovereign User Interface Toolkit
 * Pure C23, zero external dependencies, cross-platform.
 *
 * Architecture:
 *   Backend → Canvas → Layout → Widget → Application
 *
 * Backends: terminal (VT100/xterm256), DRM/KMS framebuffer,
 *           Wayland (raw wire), X11 (raw wire), Windows GDI/Direct2D
 */

#ifndef ORDL_UI_H
#define ORDL_UI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------- */
/* Platform abstraction (not Linux-limited)                                   */
/* -------------------------------------------------------------------------- */

#if defined(__linux__)
#  define ORDL_UI_PLATFORM_LINUX   1
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#  define ORDL_UI_PLATFORM_BSD     1
#elif defined(__APPLE__)
#  define ORDL_UI_PLATFORM_MACOS   1
#elif defined(_WIN32)
#  define ORDL_UI_PLATFORM_WINDOWS 1
#else
#  define ORDL_UI_PLATFORM_POSIX   1
#endif

/* -------------------------------------------------------------------------- */
/* Core types                                                                 */
/* -------------------------------------------------------------------------- */

typedef struct ui_rect {
    int x, y, w, h;
} ui_rect_t;

typedef struct ui_point {
    int x, y;
} ui_point_t;

typedef struct ui_size {
    int w, h;
} ui_size_t;

typedef struct ui_color {
    uint8_t r, g, b, a;
} ui_color_t;

static inline ui_color_t ui_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (ui_color_t){r, g, b, 255};
}

static inline ui_color_t ui_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (ui_color_t){r, g, b, a};
}

/* -------------------------------------------------------------------------- */
/* Event system                                                               */
/* -------------------------------------------------------------------------- */

typedef enum {
    UI_EVENT_NONE = 0,
    UI_EVENT_KEY,
    UI_EVENT_MOUSE_MOVE,
    UI_EVENT_MOUSE_PRESS,
    UI_EVENT_MOUSE_RELEASE,
    UI_EVENT_MOUSE_SCROLL,
    UI_EVENT_RESIZE,
    UI_EVENT_FOCUS,
    UI_EVENT_BLUR,
    UI_EVENT_QUIT,
    UI_EVENT_TIMER,      /* 60fps animation tick */
} ui_event_type_t;

typedef enum {
    UI_KEY_NONE = 0,
    UI_KEY_ENTER, UI_KEY_TAB, UI_KEY_BACKSPACE, UI_KEY_DELETE,
    UI_KEY_ESCAPE, UI_KEY_SPACE,
    UI_KEY_UP, UI_KEY_DOWN, UI_KEY_LEFT, UI_KEY_RIGHT,
    UI_KEY_HOME, UI_KEY_END, UI_KEY_PAGE_UP, UI_KEY_PAGE_DOWN,
    UI_KEY_INSERT,
    UI_KEY_F1, UI_KEY_F2, UI_KEY_F3, UI_KEY_F4, UI_KEY_F5,
    UI_KEY_F6, UI_KEY_F7, UI_KEY_F8, UI_KEY_F9, UI_KEY_F10,
    UI_KEY_F11, UI_KEY_F12,
} ui_key_t;

typedef struct {
    ui_event_type_t type;
    uint64_t timestamp_ns;
    union {
        struct {
            ui_key_t key;
            uint32_t codepoint; /* UTF-32 for text input */
            bool ctrl, alt, shift, meta;
        } key;
        struct {
            int x, y;
            int button; /* 0=none, 1=left, 2=right, 3=middle */
            int scroll_dx, scroll_dy;
        } mouse;
        struct {
            int w, h;
        } resize;
    };
} ui_event_t;

/* -------------------------------------------------------------------------- */
/* Canvas — rendering surface                                                 */
/* -------------------------------------------------------------------------- */

typedef enum {
    UI_CANVAS_TERM,   /* Cell grid: char + fg + bg + attrs */
    UI_CANVAS_FB,     /* RGBA8888 pixel buffer */
    UI_CANVAS_GL,     /* GPU texture (future) */
} ui_canvas_type_t;

/* Cell attributes (TUI) */
#define UI_ATTR_BOLD      (1u << 0)
#define UI_ATTR_ITALIC    (1u << 1)
#define UI_ATTR_UNDERLINE (1u << 2)
#define UI_ATTR_STRIKE    (1u << 3)
#define UI_ATTR_LINK      (1u << 4)
#define UI_ATTR_BLINK     (1u << 5)

typedef struct ui_cell {
    uint32_t codepoint;  /* UTF-32 */
    ui_color_t fg, bg;
    uint32_t attrs;
    uint16_t z;          /* Z-index for overlays */
} ui_cell_t;

typedef struct ui_canvas ui_canvas_t;

struct ui_canvas {
    ui_canvas_type_t type;
    int w, h;            /* Width/height in cells (TUI) or logical pixels (FB/GL) */
    float dpi_scale;     /* High-DPI scale factor (1.0 = 96 DPI, 2.0 = 192 DPI) */
    union {
        ui_cell_t *cells;     /* TUI: cell grid */
        uint32_t *pixels;     /* FB: RGBA8888 buffer (physical size = w*h*dpi_scale^2) */
    };
    /* Damage tracking: only redraw changed regions */
    ui_rect_t *damage;
    size_t damage_count, damage_cap;

    /* Clip stack for containment (scroll views, etc) */
    ui_rect_t clip_stack[8];
    int clip_depth;
};

ui_canvas_t *ui_canvas_new_term(int cols, int rows);
ui_canvas_t *ui_canvas_new_fb(int w, int h);
void ui_canvas_free(ui_canvas_t *c);

void ui_canvas_clear(ui_canvas_t *c, ui_color_t bg);
void ui_canvas_damage(ui_canvas_t *c, ui_rect_t r);
void ui_canvas_damage_all(ui_canvas_t *c);

/* Drawing primitives */
void ui_draw_rect(ui_canvas_t *c, ui_rect_t r, ui_color_t fg, ui_color_t bg, uint32_t attrs);
void ui_draw_text(ui_canvas_t *c, int x, int y, const char *utf8, ui_color_t fg, ui_color_t bg, uint32_t attrs);
void ui_draw_text_clipped(ui_canvas_t *c, int x, int y, const char *utf8, ui_color_t fg, ui_color_t bg, uint32_t attrs, int max_w);
void ui_draw_line_h(ui_canvas_t *c, int x, int y, int len, uint32_t codepoint, ui_color_t fg, ui_color_t bg);
void ui_draw_line_v(ui_canvas_t *c, int x, int y, int len, uint32_t codepoint, ui_color_t fg, ui_color_t bg);
void ui_draw_box(ui_canvas_t *c, ui_rect_t r, ui_color_t fg, ui_color_t bg, bool double_line);
void ui_canvas_set_cell(ui_canvas_t *c, int x, int y, uint32_t cp,
                        ui_color_t fg, ui_color_t bg, uint32_t attrs);

/* Clip stack */
void ui_canvas_push_clip(ui_canvas_t *c, ui_rect_t r);
void ui_canvas_pop_clip(ui_canvas_t *c);
bool ui_canvas_clip_test(ui_canvas_t *c, int x, int y);
bool ui_canvas_clip_rect_test(ui_canvas_t *c, ui_rect_t r);

/* -------------------------------------------------------------------------- */
/* Backend — platform-specific I/O                                            */
/* -------------------------------------------------------------------------- */

typedef struct ui_backend ui_backend_t;

struct ui_backend {
    const char *name;
    ui_canvas_t *canvas;

    /* Lifecycle */
    bool (*init)(ui_backend_t *be, int w, int h);
    void (*shutdown)(ui_backend_t *be);

    /* I/O */
    bool (*poll_event)(ui_backend_t *be, ui_event_t *out, int timeout_ms);
    void (*present)(ui_backend_t *be);  /* Flush canvas to screen */

    /* Backend-specific state */
    void *user_data;

    /* Platform capabilities */
    bool supports_mouse;
    bool supports_color;
    bool supports_unicode;
    int  max_colors;
};

/* Built-in backends */
ui_backend_t *ui_backend_term_new(void);   /* TTY / SSH */
ui_backend_t *ui_backend_drm_new(void);    /* Linux DRM/KMS */
ui_backend_t *ui_backend_wayland_new(void); /* Raw Wayland */
ui_backend_t *ui_backend_x11_new(void);     /* Raw X11 */
ui_backend_t *ui_backend_gl_new(void);      /* OpenGL ES via EGL (runtime loaded) */
ui_backend_t *ui_backend_gl_new_with_window(void *native_window); /* Windowed EGL */
ui_backend_t *ui_backend_gl_x11_new(void);     /* OpenGL ES + X11 window */
ui_backend_t *ui_backend_gl_wayland_new(void); /* OpenGL ES + Wayland window */
ui_backend_t *ui_backend_win32_new(void);    /* Windows Win32 + GDI */

/* -------------------------------------------------------------------------- */
/* Layout engine                                                              */
/* -------------------------------------------------------------------------- */

typedef enum {
    UI_DIR_ROW,      /* Horizontal */
    UI_DIR_COLUMN,   /* Vertical */
} ui_direction_t;

typedef enum {
    UI_JUSTIFY_START,
    UI_JUSTIFY_CENTER,
    UI_JUSTIFY_END,
    UI_JUSTIFY_SPACE_BETWEEN,
    UI_JUSTIFY_SPACE_AROUND,
} ui_justify_t;

typedef enum {
    UI_ALIGN_START,
    UI_ALIGN_CENTER,
    UI_ALIGN_END,
    UI_ALIGN_STRETCH,
} ui_align_t;

typedef struct ui_layout ui_layout_t;

struct ui_layout {
    ui_direction_t direction;
    ui_justify_t justify;
    ui_align_t align;
    ui_align_t align_cross;
    int gap;           /* Space between children */
    int pad[4];        /* Top, right, bottom, left padding */
    bool wrap;         /* Wrap to next line/column */
};

static inline ui_layout_t ui_layout_row(void) {
    return (ui_layout_t){ UI_DIR_ROW, UI_JUSTIFY_START, UI_ALIGN_START, UI_ALIGN_START, 0, {0,0,0,0}, false };
}

static inline ui_layout_t ui_layout_col(void) {
    return (ui_layout_t){ UI_DIR_COLUMN, UI_JUSTIFY_START, UI_ALIGN_START, UI_ALIGN_START, 0, {0,0,0,0}, false };
}

/* -------------------------------------------------------------------------- */
/* Accessibility (a11y) — forward declarations                                */
/* -------------------------------------------------------------------------- */

typedef enum {
    UI_ROLE_NONE = 0,
    UI_ROLE_ALERT,
    UI_ROLE_BUTTON,
    UI_ROLE_CHECKBOX,
    UI_ROLE_DIALOG,
    UI_ROLE_GRID,
    UI_ROLE_GRIDCELL,
    UI_ROLE_LINK,
    UI_ROLE_MENU,
    UI_ROLE_MENUBAR,
    UI_ROLE_MENUITEM,
    UI_ROLE_PROGRESSBAR,
    UI_ROLE_SCROLLBAR,
    UI_ROLE_SLIDER,
    UI_ROLE_STATUS,
    UI_ROLE_TAB,
    UI_ROLE_TABPANEL,
    UI_ROLE_TEXTBOX,
    UI_ROLE_TREE,
    UI_ROLE_TREEITEM,
} ui_a11y_role_t;

#define UI_A11Y_FOCUSED   (1u << 0)
#define UI_A11Y_DISABLED  (1u << 1)
#define UI_A11Y_HIDDEN    (1u << 2)
#define UI_A11Y_CHECKED   (1u << 3)
#define UI_A11Y_EXPANDED  (1u << 4)
#define UI_A11Y_SELECTED  (1u << 5)

/* -------------------------------------------------------------------------- */
/* Widget system                                                              */
/* -------------------------------------------------------------------------- */

typedef enum {
    UI_WIDGET_NONE = 0,
    UI_WIDGET_BOX,
    UI_WIDGET_LABEL,
    UI_WIDGET_BUTTON,
    UI_WIDGET_INPUT,
    UI_WIDGET_TEXTAREA,
    UI_WIDGET_CHECKBOX,
    UI_WIDGET_SELECT,
    UI_WIDGET_SLIDER,
    UI_WIDGET_SCROLL,
    UI_WIDGET_SPLIT,
    UI_WIDGET_TABS,
    UI_WIDGET_TABLE,
    UI_WIDGET_TREE,
    UI_WIDGET_DIALOG,
    UI_WIDGET_TOOLTIP,
    UI_WIDGET_PROGRESSBAR,
    UI_WIDGET_SPINNER,
    UI_WIDGET_RADIO,
    UI_WIDGET_MENUBAR,
    UI_WIDGET_STATUSBAR,
    UI_WIDGET_TERMINAL,  /* Embedded terminal emulator */
    UI_WIDGET_CUSTOM,
} ui_widget_type_t;

typedef struct ui_widget ui_widget_t;

/* Style state */
typedef enum {
    UI_STATE_NORMAL = 0,
    UI_STATE_HOVER,
    UI_STATE_ACTIVE,
    UI_STATE_FOCUSED,
    UI_STATE_DISABLED,
    UI_STATE_COUNT,
} ui_widget_state_t;

typedef struct {
    ui_color_t fg, bg, border;
    int border_width;
    int radius;        /* Corner radius (0=sharp) */
    int pad[4];
    int margin[4];
    const char *font_face;
    int font_size;
} ui_style_t;

typedef struct {
    ui_style_t states[UI_STATE_COUNT];
} ui_style_set_t;

/* Widget base */
struct ui_widget {
    ui_widget_type_t type;
    char id[64];

    /* Tree */
    ui_widget_t *parent;
    ui_widget_t **children;
    size_t child_count, child_cap;

    /* Layout */
    ui_layout_t layout;
    ui_rect_t bounds;      /* Absolute position + size */
    ui_size_t min_size, max_size, preferred_size;
    int flex_grow, flex_shrink;
    int flex_basis;

    /* Style */
    ui_style_set_t *style;
    ui_widget_state_t state;

    /* Accessibility */
    ui_a11y_role_t a11y_role;
    char *a11y_label;
    uint32_t a11y_state;

    /* Dirty flags */
    bool dirty_layout;
    bool dirty_render;
    bool visible;
    bool enabled;

    /* Type-specific data (cast by widget type) */
    void *data;

    /* Virtual methods */
    ui_size_t (*measure)(ui_widget_t *w, ui_canvas_t *c);
    void (*render)(ui_widget_t *w, ui_canvas_t *c);
    bool (*on_event)(ui_widget_t *w, const ui_event_t *ev);
    void (*destroy)(ui_widget_t *w);
};

/* Widget constructors */
ui_widget_t *ui_box_new(const char *id);
ui_widget_t *ui_label_new(const char *id, const char *text);
void ui_label_set_text(ui_widget_t *w, const char *text);

ui_widget_t *ui_button_new(const char *id, const char *label);
void ui_button_set_callback(ui_widget_t *w, void (*cb)(ui_widget_t *, void *), void *ud);

ui_widget_t *ui_input_new(const char *id, const char *placeholder);
void ui_input_set_submit_callback(ui_widget_t *w, void (*cb)(ui_widget_t *, void *), void *ud);
const char *ui_input_get_text(ui_widget_t *w);
void ui_input_set_text(ui_widget_t *w, const char *text);

ui_widget_t *ui_textarea_new(const char *id);
ui_widget_t *ui_checkbox_new(const char *id, const char *label, bool checked);
ui_widget_t *ui_scroll_new(const char *id, ui_widget_t *child);
void ui_scroll_set_scroll_y(ui_widget_t *w, int scroll_y);
void ui_scroll_scroll_to_bottom(ui_widget_t *w);
ui_widget_t *ui_split_new(const char *id, ui_direction_t dir, ui_widget_t *a, ui_widget_t *b, float ratio);
ui_widget_t *ui_tabs_new(const char *id);
ui_widget_t *ui_table_new(const char *id, size_t cols);
ui_widget_t *ui_tree_new(const char *id);
ui_widget_t *ui_menubar_new(const char *id);
ui_widget_t *ui_statusbar_new(const char *id);
ui_widget_t *ui_terminal_new(const char *id);

/* Missing widgets */
ui_widget_t *ui_select_new(const char *id);
ui_widget_t *ui_slider_new(const char *id, float min, float max, float value);
ui_widget_t *ui_dialog_new(const char *id, const char *title);
ui_widget_t *ui_tooltip_new(const char *id, const char *text);
ui_widget_t *ui_progressbar_new(const char *id);
ui_widget_t *ui_spinner_new(const char *id);
ui_widget_t *ui_radio_new(const char *id, const char *label);

/* Widget tree helpers */
void ui_tabs_add_tab(ui_widget_t *w, const char *label, ui_widget_t *panel);
void ui_table_set_cell(ui_widget_t *w, int row, int col, const char *text);
void ui_tree_add_node(ui_widget_t *w, const char *parent_label, const char *label);
void ui_menubar_add_item(ui_widget_t *w, const char *label);
void ui_statusbar_set_text(ui_widget_t *w, const char *text);
void ui_textarea_set_text(ui_widget_t *w, const char *text);
const char *ui_textarea_get_text(ui_widget_t *w);

/* New widget helpers */
void ui_checkbox_set_checked(ui_widget_t *w, bool checked);
bool ui_checkbox_get_checked(ui_widget_t *w);
void ui_select_add_option(ui_widget_t *w, const char *option);
float ui_slider_get_value(ui_widget_t *w);
void ui_slider_set_value(ui_widget_t *w, float value);
void ui_progressbar_set_progress(ui_widget_t *w, float progress);
void ui_spinner_step(ui_widget_t *w);
void ui_terminal_write(ui_widget_t *w, const char *text);

/* Widget tree */
void ui_widget_add_child(ui_widget_t *parent, ui_widget_t *child);
void ui_widget_remove_child(ui_widget_t *parent, ui_widget_t *child);
void ui_widget_destroy(ui_widget_t *w);
ui_widget_t *ui_widget_find(ui_widget_t *root, const char *id);

/* Widget state */
void ui_widget_set_visible(ui_widget_t *w, bool visible);
void ui_widget_set_enabled(ui_widget_t *w, bool enabled);
void ui_widget_set_focus(ui_widget_t *w, bool focused);
void ui_widget_set_state(ui_widget_t *w, ui_widget_state_t state);

/* Layout */
void ui_layout_measure(ui_widget_t *root, ui_canvas_t *c);
void ui_layout_arrange(ui_widget_t *root, ui_rect_t available);
void ui_layout_run(ui_widget_t *root, ui_canvas_t *c);  /* measure + arrange */

/* Rendering */
void ui_widget_render(ui_widget_t *root, ui_canvas_t *c);
void ui_widget_invalidate(ui_widget_t *w);   /* Mark dirty */
void ui_widget_invalidate_all(ui_widget_t *w); /* Mark entire subtree */

/* Event routing */
bool ui_widget_dispatch_event(ui_widget_t *root, const ui_event_t *ev);
ui_widget_t *ui_widget_hit_test(ui_widget_t *root, int x, int y);

/* -------------------------------------------------------------------------- */
/* Application shell                                                          */
/* -------------------------------------------------------------------------- */

typedef struct ui_app ui_app_t;

typedef void (*ui_app_render_cb)(ui_app_t *app, ui_canvas_t *c);
typedef bool (*ui_app_event_cb)(ui_app_t *app, const ui_event_t *ev);

struct ui_app {
    ui_backend_t *backend;
    ui_widget_t *root;
    ui_widget_t *focused;

    ui_app_render_cb on_render;
    ui_app_event_cb on_event;

    void *user_data;

    bool running;
    int fps_target;
    uint64_t last_frame_ns;
    void (*tick_cb)(ui_app_t *, void *);
    void *tick_userdata;

    /* Animation queue */
    struct ui_anim *anims;
    size_t anim_count, anim_cap;
};

/* -------------------------------------------------------------------------- */
/* Animation                                                                  */
/* -------------------------------------------------------------------------- */

typedef float (*ui_ease_fn)(float t);

typedef struct ui_anim {
    float *value;
    float from, target;
    float duration_ms;
    uint64_t start_ns;
    ui_ease_fn ease;
    bool active;
} ui_anim_t;

float ui_ease_linear(float t);
float ui_ease_in_quad(float t);
float ui_ease_out_quad(float t);
float ui_ease_in_out_quad(float t);
float ui_ease_in_cubic(float t);
float ui_ease_out_cubic(float t);
float ui_ease_in_out_cubic(float t);
float ui_ease_in_quart(float t);
float ui_ease_out_quart(float t);
float ui_ease_in_out_quart(float t);
float ui_ease_in_sine(float t);
float ui_ease_out_sine(float t);
float ui_ease_in_out_sine(float t);
float ui_ease_in_back(float t);
float ui_ease_out_back(float t);
float ui_ease_in_out_back(float t);
float ui_ease_in_bounce(float t);
float ui_ease_out_bounce(float t);
float ui_ease_in_out_bounce(float t);
float ui_ease_in_elastic(float t);
float ui_ease_out_elastic(float t);
float ui_ease_in_out_elastic(float t);

void ui_app_animate(ui_app_t *app, float *value, float target, float duration_ms, ui_ease_fn ease);

/* Main loop */
ui_app_t *ui_app_new(ui_backend_t *backend, const char *title, int w, int h);
void ui_app_free(ui_app_t *app);

void ui_app_set_root(ui_app_t *app, ui_widget_t *root);
void ui_app_set_focus(ui_app_t *app, ui_widget_t *w);

void ui_app_set_tick_cb(ui_app_t *app, void (*cb)(ui_app_t *, void *), void *userdata);

void ui_app_run(ui_app_t *app);           /* Blocks until quit */
void ui_app_quit(ui_app_t *app);
void ui_app_step(ui_app_t *app);          /* Single frame for external loops */

/* -------------------------------------------------------------------------- */
/* Image decoders (PNG, BMP from scratch)                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t *pixels;  /* RGBA8888 */
    int w, h;
} ui_image_t;

ui_image_t *ui_image_load_jpeg(const uint8_t *data, size_t len);
ui_image_t *ui_image_load_png(const uint8_t *data, size_t len);
ui_image_t *ui_image_load_bmp(const uint8_t *data, size_t len);
ui_image_t *ui_image_load_gif(const uint8_t *data, size_t len);
void ui_image_free(ui_image_t *img);

/* -------------------------------------------------------------------------- */
/* Universal software renderer (identical pixels on all platforms)            */
/* -------------------------------------------------------------------------- */

void ui_sw_fill_rect(uint32_t *buf, int w, int h, float x, float y, float rw, float rh, ui_color_t color);
void ui_sw_fill_triangle(uint32_t *buf, int w, int h,
                         float x0, float y0, float x1, float y1, float x2, float y2,
                         ui_color_t color);
void ui_sw_line(uint32_t *buf, int w, int h, float x0, float y0, float x1, float y1, float thickness, ui_color_t color);
void ui_sw_fill_circle(uint32_t *buf, int w, int h, float cx, float cy, float r, ui_color_t color);
void ui_sw_fill_rounded_rect(uint32_t *buf, int w, int h,
                             float x, float y, float rw, float rh, float radius,
                             ui_color_t color);
void ui_sw_box_shadow(uint32_t *buf, int w, int h,
                      float x, float y, float rw, float rh, float radius,
                      float blur_radius, ui_color_t shadow_color);
void ui_sw_blit(uint32_t *dst, int dw, int dh, float dx, float dy, float dw_scale, float dh_scale,
                const uint32_t *src, int sw, int sh);
void ui_sw_glyph(uint32_t *buf, int w, int h, int x, int y, uint8_t glyph_row,
                 ui_color_t fg, ui_color_t bg);
void ui_sw_render_rect(ui_canvas_t *c, ui_rect_t r, ui_color_t fg, ui_color_t bg, int radius);

/* -------------------------------------------------------------------------- */
/* Text engine (embedded bitmap + SDF)                                        */
/* -------------------------------------------------------------------------- */

/* Embedded 8x16 monospace font (Unscii, public domain) */
extern const uint8_t ordl_ui_font_unscii_8x16[256 * 16];
extern const uint8_t ordl_ui_font_unscii_16x32[256 * 64];

/* SDF atlas (generated offline, embedded at compile time) */
extern const uint8_t ordl_ui_font_sdf_atlas[256 * 256]; /* 16x16 cells, 16x16 px each */

/* Text measurement */
int ui_text_width(const char *utf8);
int ui_text_height(const char *utf8);

/* Grapheme iteration */
typedef struct {
    const char *start;
    const char *end;
    uint32_t codepoint;
    size_t bytes;
} ui_grapheme_t;

bool ui_grapheme_next(const char *utf8, size_t len, size_t *offset, ui_grapheme_t *out);

/* UTF-8 decode helper */
bool ui_utf8_decode(const char *s, size_t len, size_t *offset, uint32_t *out);

/* -------------------------------------------------------------------------- */
/* TTF Font parser (zero dependencies)                                        */
/* -------------------------------------------------------------------------- */

typedef struct ui_font_ttf ui_font_ttf_t;

ui_font_ttf_t *ui_font_ttf_load(const uint8_t *data, size_t len);
bool ui_font_load_otf(const uint8_t *data, size_t len, ui_font_ttf_t *out);
void           ui_font_ttf_free(ui_font_ttf_t *f);

/* Glyph lookup via cmap */
uint16_t ui_font_ttf_glyph_index(ui_font_ttf_t *f, uint32_t codepoint);

/* Glyph metrics (in font units) */
typedef struct {
    int16_t x_min, y_min, x_max, y_max; /* bounding box */
    int16_t advance_width;
    int16_t left_side_bearing;
} ui_font_glyph_metrics_t;

bool ui_font_ttf_glyph_metrics(ui_font_ttf_t *f, uint16_t gid,
                               ui_font_glyph_metrics_t *out);

/* Outline extraction */
typedef struct {
    float x, y;
    bool on_curve;
} ui_font_point_t;

/* Returns number of contours, or -1 on error. Caller provides point/contour buffers. */
int ui_font_ttf_glyph_outline(ui_font_ttf_t *f, uint16_t gid,
                              ui_font_point_t *pts, size_t pt_cap,
                              uint16_t *contour_ends, size_t contour_cap);

int ui_font_ttf_units_per_em(ui_font_ttf_t *f);
int ui_font_ttf_num_glyphs(ui_font_ttf_t *f);

/* -------------------------------------------------------------------------- */
/* SDF rasterizer (signed distance field)                                     */
/* -------------------------------------------------------------------------- */

/* Render a single glyph into an 8-bit grayscale SDF buffer.
 * px_size: target pixel size (e.g. 32)
 * out:     grayscale buffer of size out_w * out_h
 * pad:     padding pixels around glyph (for mipmapping / filtering)
 * Returns false if glyph has no outline. */
bool ui_font_sdf_render(ui_font_ttf_t *f, uint16_t gid, int px_size,
                        uint8_t *out, int out_w, int out_h, int pad);

/* Build a packed SDF atlas for a set of codepoints.
 * Returns allocated atlas buffer (RGBA8888 for GL upload) or NULL.
 * out_cols/rows receive the grid dimensions.
 * glyph_positions: caller-allocated array of n ui_rect_t to receive UVs. */
uint32_t *ui_font_sdf_atlas_build(ui_font_ttf_t *f,
                                  const uint32_t *codepoints, size_t n,
                                  int px_size, int pad,
                                  int *out_cols, int *out_rows,
                                  ui_rect_t *glyph_positions);

/* -------------------------------------------------------------------------- */
/* Theme / style system                                                       */
/* -------------------------------------------------------------------------- */

typedef struct ui_theme ui_theme_t;

ui_theme_t *ui_theme_load_default(void);
ui_theme_t *ui_theme_load_toml(const char *path);
void ui_theme_free(ui_theme_t *t);

ui_style_set_t *ui_theme_get_style(ui_theme_t *t, const char *widget_class);
void ui_widget_apply_theme(ui_widget_t *w, ui_theme_t *t);

/* Predefined themes */
ui_theme_t *ui_theme_dark(void);
ui_theme_t *ui_theme_light(void);
ui_theme_t *ui_theme_grok(void);
ui_theme_t *ui_theme_tokyo(void);
ui_theme_t *ui_theme_rosepine(void);

/* -------------------------------------------------------------------------- */
/* Accessibility (a11y)                                                       */
/* -------------------------------------------------------------------------- */

void ui_a11y_set_role(ui_widget_t *w, ui_a11y_role_t role);
void ui_a11y_set_label(ui_widget_t *w, const char *label);
void ui_a11y_set_state(ui_widget_t *w, uint32_t flags, bool on);
void ui_a11y_announce(const char *text);   /* Screen reader announcement */
void ui_a11y_describe_widget(ui_widget_t *w, char *out, size_t out_len);
void ui_a11y_on_focus_changed(ui_widget_t *prev, ui_widget_t *curr);

/* -------------------------------------------------------------------------- */
/* Clipboard                                                                  */
/* -------------------------------------------------------------------------- */

char *ui_clipboard_read(void);
bool ui_clipboard_write(const char *text);

/* -------------------------------------------------------------------------- */
/* ORDL-specific widgets                                                      */
/* -------------------------------------------------------------------------- */

ui_widget_t *ui_inference_panel_new(const char *id);
ui_widget_t *ui_mesh_viz_new(const char *id);
ui_widget_t *ui_crypto_status_new(const char *id);
ui_widget_t *ui_terminal_embed_new(const char *id);

/* -------------------------------------------------------------------------- */
/* Popup / menu overlay system                                                */
/* -------------------------------------------------------------------------- */

void ui_popup_show(ui_app_t *app, ui_widget_t *content, int x, int y);
void ui_popup_hide(ui_app_t *app);
bool ui_popup_is_visible(void);
ui_widget_t *ui_popup_get_content(void);
void ui_popup_set_dismiss_on_outside_click(bool dismiss);
void ui_popup_render(ui_app_t *app, ui_canvas_t *c);
bool ui_popup_handle_event(ui_app_t *app, const ui_event_t *ev);

ui_widget_t *ui_menu_new(const char *id);
void ui_menu_add_item(ui_widget_t *w, const char *label);
void ui_menu_set_callback(ui_widget_t *w, void (*cb)(const char *, void *), void *ud);
void ui_context_menu_show(ui_app_t *app, int x, int y, const char **items, size_t n,
                          void (*cb)(const char *, void *), void *ud);

/* -------------------------------------------------------------------------- */
/* Multi-window support                                                       */
/* -------------------------------------------------------------------------- */

typedef struct ui_window ui_window_t;
typedef struct ui_window_mgr ui_window_mgr_t;

ui_window_t *ui_window_new(ui_backend_t *backend, const char *title, int w, int h);
void ui_window_destroy(ui_window_t *win);
void ui_window_set_root(ui_window_t *win, ui_widget_t *root);
void ui_window_set_focus(ui_window_t *win, ui_widget_t *w);
void ui_window_show(ui_window_t *win);
void ui_window_hide(ui_window_t *win);
bool ui_window_visible(ui_window_t *win);
ui_backend_t *ui_window_backend(ui_window_t *win);

ui_window_mgr_t *ui_window_mgr_new(void);
void ui_window_mgr_free(ui_window_mgr_t *mgr);
void ui_window_mgr_add(ui_window_mgr_t *mgr, ui_window_t *win);
void ui_window_mgr_remove(ui_window_mgr_t *mgr, ui_window_t *win);
void ui_window_mgr_run(ui_window_mgr_t *mgr);
void ui_window_mgr_step(ui_window_mgr_t *mgr);
void ui_window_mgr_quit(ui_window_mgr_t *mgr);
void ui_window_mgr_set_running(bool running);
bool ui_window_mgr_get_running(void);
void ui_window_mgr_set_tick_cb(ui_window_mgr_t *mgr, void (*cb)(ui_window_mgr_t *, void *), void *userdata);

/* -------------------------------------------------------------------------- */
/* Input Method Editor (IME) — CJK text input                                 */
/* -------------------------------------------------------------------------- */

void ui_ime_init(void);
void ui_ime_shutdown(void);
bool ui_ime_process_key(ui_key_t key, uint32_t codepoint, bool ctrl, bool alt, bool shift, bool meta);
const char *ui_ime_get_preedit(void);
const char *ui_ime_commit(void);
int ui_ime_candidate_count(void);
const char *ui_ime_candidate(int index);
void ui_ime_select_candidate(int index);
void ui_ime_reset(void);

/* -------------------------------------------------------------------------- */
/* Gamepad / joystick                                                         */
/* -------------------------------------------------------------------------- */

typedef struct {
    float lx, ly, rx, ry, lt, rt;
    bool a, b, x, y, lb, rb, back, start, guide, lthumb, rthumb;
    bool dpad_u, dpad_d, dpad_l, dpad_r;
} ui_gamepad_state_t;

bool ui_gamepad_open(int device_index);
void ui_gamepad_close(void);
bool ui_gamepad_poll(ui_gamepad_state_t *out);
const char *ui_gamepad_name(void);

#endif /* ORDL_UI_H */
