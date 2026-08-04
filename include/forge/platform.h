/*
 * forge/platform.h — Cross-platform window, input, and events
 * Builds on ORDL UI patterns but simplified for game use.
 */

#ifndef FORGE_PLATFORM_H
#define FORGE_PLATFORM_H

#include "forge/core.h"
#include "forge/math.h"
#include "forge/memory.h"
#include "forge/time.h"
#include "forge/log.h"

/* -------------------------------------------------------------------------- */
/* Key codes — unified across platforms                                       */
/* -------------------------------------------------------------------------- */

typedef enum {
    FGE_KEY_NONE = 0,
    FGE_KEY_A, FGE_KEY_B, FGE_KEY_C, FGE_KEY_D, FGE_KEY_E, FGE_KEY_F,
    FGE_KEY_G, FGE_KEY_H, FGE_KEY_I, FGE_KEY_J, FGE_KEY_K, FGE_KEY_L,
    FGE_KEY_M, FGE_KEY_N, FGE_KEY_O, FGE_KEY_P, FGE_KEY_Q, FGE_KEY_R,
    FGE_KEY_S, FGE_KEY_T, FGE_KEY_U, FGE_KEY_V, FGE_KEY_W, FGE_KEY_X,
    FGE_KEY_Y, FGE_KEY_Z,
    FGE_KEY_0, FGE_KEY_1, FGE_KEY_2, FGE_KEY_3, FGE_KEY_4,
    FGE_KEY_5, FGE_KEY_6, FGE_KEY_7, FGE_KEY_8, FGE_KEY_9,
    FGE_KEY_SPACE, FGE_KEY_ENTER, FGE_KEY_TAB, FGE_KEY_BACKSPACE,
    FGE_KEY_ESCAPE, FGE_KEY_DELETE,
    FGE_KEY_UP, FGE_KEY_DOWN, FGE_KEY_LEFT, FGE_KEY_RIGHT,
    FGE_KEY_HOME, FGE_KEY_END, FGE_KEY_PAGE_UP, FGE_KEY_PAGE_DOWN,
    FGE_KEY_INSERT,
    FGE_KEY_F1, FGE_KEY_F2, FGE_KEY_F3, FGE_KEY_F4, FGE_KEY_F5,
    FGE_KEY_F6, FGE_KEY_F7, FGE_KEY_F8, FGE_KEY_F9, FGE_KEY_F10,
    FGE_KEY_F11, FGE_KEY_F12,
    FGE_KEY_LSHIFT, FGE_KEY_RSHIFT, FGE_KEY_LCTRL, FGE_KEY_RCTRL,
    FGE_KEY_LALT, FGE_KEY_RALT, FGE_KEY_LMETA, FGE_KEY_RMETA,
    FGE_KEY_COUNT
} fge_key_t;

/* -------------------------------------------------------------------------- */
/* Mouse buttons                                                              */
/* -------------------------------------------------------------------------- */

typedef enum {
    FGE_MOUSE_NONE = 0,
    FGE_MOUSE_LEFT = 1,
    FGE_MOUSE_RIGHT = 2,
    FGE_MOUSE_MIDDLE = 3,
    FGE_MOUSE_X1 = 4,
    FGE_MOUSE_X2 = 5,
} fge_mouse_button_t;

/* -------------------------------------------------------------------------- */
/* Gamepad                                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    float lx, ly, rx, ry;    /* analog sticks [-1, 1] */
    float lt, rt;            /* triggers [0, 1] */
    bool a, b, x, y;
    bool lb, rb;
    bool back, start, guide;
    bool lthumb, rthumb;
    bool dpad_u, dpad_d, dpad_l, dpad_r;
} fge_gamepad_state_t;

/* -------------------------------------------------------------------------- */
/* Input state                                                                */
/* -------------------------------------------------------------------------- */

typedef struct {
    bool keys[FGE_KEY_COUNT];
    bool keys_prev[FGE_KEY_COUNT];
    bool mouse_buttons[6];
    bool mouse_buttons_prev[6];
    fge_vec2_t mouse_pos;
    fge_vec2_t mouse_delta;
    fge_vec2_t mouse_scroll;
    fge_gamepad_state_t gamepad;
    char text_input[32];     /* UTF-8 text input this frame */
    int text_input_len;
} fge_input_state_t;

FGE_INLINE bool fge_input_key_down(const fge_input_state_t *in, fge_key_t key) {
    return (size_t)key < FGE_KEY_COUNT && in->keys[key];
}
FGE_INLINE bool fge_input_key_pressed(const fge_input_state_t *in, fge_key_t key) {
    return (size_t)key < FGE_KEY_COUNT && in->keys[key] && !in->keys_prev[key];
}
FGE_INLINE bool fge_input_key_released(const fge_input_state_t *in, fge_key_t key) {
    return (size_t)key < FGE_KEY_COUNT && !in->keys[key] && in->keys_prev[key];
}
FGE_INLINE bool fge_input_mouse_down(const fge_input_state_t *in, fge_mouse_button_t btn) {
    return (size_t)btn < 6 && in->mouse_buttons[btn];
}
FGE_INLINE bool fge_input_mouse_pressed(const fge_input_state_t *in, fge_mouse_button_t btn) {
    return (size_t)btn < 6 && in->mouse_buttons[btn] && !in->mouse_buttons_prev[btn];
}

/* -------------------------------------------------------------------------- */
/* Events                                                                     */
/* -------------------------------------------------------------------------- */

typedef enum {
    FGE_EVENT_NONE = 0,
    FGE_EVENT_KEY_DOWN,
    FGE_EVENT_KEY_UP,
    FGE_EVENT_TEXT_INPUT,
    FGE_EVENT_MOUSE_MOVE,
    FGE_EVENT_MOUSE_DOWN,
    FGE_EVENT_MOUSE_UP,
    FGE_EVENT_MOUSE_SCROLL,
    FGE_EVENT_RESIZE,
    FGE_EVENT_FOCUS_GAIN,
    FGE_EVENT_FOCUS_LOST,
    FGE_EVENT_CLOSE,
    FGE_EVENT_GAMEPAD_CONNECT,
    FGE_EVENT_GAMEPAD_DISCONNECT,
    FGE_EVENT_GAMEPAD_AXIS,
    FGE_EVENT_GAMEPAD_BUTTON,
} fge_event_type_t;

typedef struct {
    fge_event_type_t type;
    union {
        struct { fge_key_t key; bool repeat; } key;
        struct { char text[32]; int len; } text;
        struct { fge_vec2_t pos; fge_vec2_t delta; } mouse_move;
        struct { fge_mouse_button_t button; fge_vec2_t pos; } mouse_button;
        struct { fge_vec2_t scroll; } mouse_scroll;
        struct { int width; int height; } resize;
        struct { int index; } gamepad;
        struct { int index; int axis; float value; } gamepad_axis;
        struct { int index; int button; bool pressed; } gamepad_button;
    };
} fge_event_t;


/* -------------------------------------------------------------------------- */
/* Window / Platform                                                          */
/* -------------------------------------------------------------------------- */

typedef struct fge_platform fge_platform_t;

struct fge_platform {
    void *native_window;     /* backend-specific handle */
    void *native_display;
    int width, height;
    bool fullscreen;
    bool focused;
    bool running;

    /* Input state */
    fge_input_state_t input;

    /* Event queue (ring buffer) */
    fge_event_t *events;
    uint32_t event_capacity;
    fge_atomic_uint_t event_head;
    fge_atomic_uint_t event_tail;

    /* Callbacks */
    void (*on_event)(fge_platform_t *p, const fge_event_t *event);
    void (*on_frame)(fge_platform_t *p, double dt);

    /* Backend function pointers */
    bool (*init)(fge_platform_t *p, const char *title, int w, int h, bool fullscreen);
    void (*shutdown)(fge_platform_t *p);
    bool (*poll_event)(fge_platform_t *p, fge_event_t *out);
    void (*swap_buffers)(fge_platform_t *p);
    void (*set_title)(fge_platform_t *p, const char *title);
    void (*set_vsync)(fge_platform_t *p, bool enabled);
    void (*show_cursor)(fge_platform_t *p, bool show);
    void (*grab_input)(fge_platform_t *p, bool grab);
    const char *(*get_clipboard)(fge_platform_t *p);
    void (*set_clipboard)(fge_platform_t *p, const char *text);

    /* User data */
    void *user_data;
    void *framebuffer;   /* fge_framebuffer_t* for swap_buffers presentation */
};

/* Create platform window. Backend chosen automatically based on platform. */
[[nodiscard]]
fge_platform_t *fge_platform_create(const char *title, int w, int h, bool fullscreen);
void fge_platform_destroy(fge_platform_t *p);

/* Event pumping */
bool fge_platform_poll_event(fge_platform_t *p, fge_event_t *out);
void fge_platform_push_event(fge_platform_t *p, const fge_event_t *event);

/* Main loop (blocks until window closed) */
void fge_platform_run(fge_platform_t *p);

/* Single frame step (for external loops) */
void fge_platform_step(fge_platform_t *p, double dt);

/* Utility */
FGE_INLINE void fge_platform_get_size(const fge_platform_t *p, int *w, int *h) {
    if (w) *w = p ? p->width : 0;
    if (h) *h = p ? p->height : 0;
}
FGE_INLINE float fge_platform_aspect(const fge_platform_t *p) {
    return (p && p->height > 0) ? (float)p->width / (float)p->height : 1.0f;
}

#endif /* FORGE_PLATFORM_H */
