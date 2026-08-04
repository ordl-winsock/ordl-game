/*
 * ORDL UI — Application shell
 * Main loop, event dispatch, animation timer.
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* -------------------------------------------------------------------------- */
/* App lifecycle                                                              */
/* -------------------------------------------------------------------------- */

ui_app_t *ui_app_new(ui_backend_t *backend, const char *title, int w, int h) {
    (void)title;
    ui_app_t *app = calloc(1, sizeof(ui_app_t));
    if (!app) return NULL;
    app->backend = backend;
    app->fps_target = 60;
    if (backend && !backend->init(backend, w, h)) {
        free(app);
        return NULL;
    }
    return app;
}

void ui_app_free(ui_app_t *app) {
    if (!app) return;
    if (app->backend && app->backend->shutdown) app->backend->shutdown(app->backend);
    free(app->anims);
    free(app);
}

void ui_app_set_root(ui_app_t *app, ui_widget_t *root) {
    if (!app) return;
    app->root = root;
    if (root && app->backend && app->backend->canvas) {
        ui_layout_run(root, app->backend->canvas);
        ui_widget_invalidate_all(root);
    }
}

void ui_app_set_focus(ui_app_t *app, ui_widget_t *w) {
    if (!app) return;
    ui_widget_t *prev = app->focused;
    if (app->focused) ui_widget_set_state(app->focused, UI_STATE_NORMAL);
    app->focused = w;
    if (w) ui_widget_set_state(w, UI_STATE_FOCUSED);
    ui_a11y_on_focus_changed(prev, w);
}

void ui_app_quit(ui_app_t *app) {
    if (app) app->running = false;
}

void ui_app_set_tick_cb(ui_app_t *app, void (*cb)(ui_app_t *, void *), void *userdata) {
    if (!app) return;
    app->tick_cb = cb;
    app->tick_userdata = userdata;
}

/* -------------------------------------------------------------------------- */
/* Main loop                                                                  */
/* -------------------------------------------------------------------------- */

static uint64_t nanotime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Forward declaration */
static void ui_app_process_anims(ui_app_t *app);

void ui_app_step(ui_app_t *app) {
    if (!app || !app->backend || !app->root) return;

    ui_backend_t *be = app->backend;
    ui_event_t ev;

    /* Tick callback for streaming, animation, etc. */
    if (app->tick_cb) app->tick_cb(app, app->tick_userdata);

    /* Process active animations */
    ui_app_process_anims(app);

    /* Poll events (non-blocking) */
    while (be->poll_event(be, &ev, 0)) {
        /* Handle global events */
        if (ev.type == UI_EVENT_RESIZE) {
            if (be->canvas->type == UI_CANVAS_TERM) {
                /* Terminal resize: recreate canvas */
                ui_canvas_free(be->canvas);
                be->canvas = ui_canvas_new_term(ev.resize.w, ev.resize.h);
                if (!be->canvas) {
                    app->running = false;
                    return;
                }
                if (app->root) {
                    ui_layout_run(app->root, be->canvas);
                    ui_widget_invalidate_all(app->root);
                }
            }
            continue;
        }
        if (ev.type == UI_EVENT_QUIT) {
            app->running = false;
            return;
        }

        /* Dispatch to popup first (overlays) */
        if (ui_popup_handle_event(app, &ev)) continue;

        /* Focus management for click */
        if (ev.type == UI_EVENT_MOUSE_PRESS) {
            ui_widget_t *hit = ui_widget_hit_test(app->root, ev.mouse.x, ev.mouse.y);
            if (hit && hit != app->focused) {
                ui_app_set_focus(app, hit);
            }
        }

        /* Dispatch to focused widget first, then root */
        bool handled = false;
        if (app->focused && app->focused->on_event) {
            handled = app->focused->on_event(app->focused, &ev);
        }
        if (!handled) {
            handled = ui_widget_dispatch_event(app->root, &ev);
        }
        if (!handled && app->on_event) {
            handled = app->on_event(app, &ev);
        }
        (void)handled;
    }

    /* Layout if dirty */
    if (app->root && (app->root->dirty_layout || app->root->dirty_render)) {
        ui_layout_run(app->root, be->canvas);
    }

    /* Render if dirty */
    if (app->root && app->root->dirty_render) {
        ui_canvas_clear(be->canvas, ui_rgb(0, 0, 0));
        ui_widget_render(app->root, be->canvas);
        ui_popup_render(app, be->canvas);
        be->present(be);
    }
}

void ui_app_run(ui_app_t *app) {
    if (!app) return;
    app->running = true;

    while (app->running) {
        uint64_t frame_start = nanotime();

        ui_app_step(app);

        /* Frame rate limiting */
        if (app->fps_target > 0) {
            uint64_t frame_ns = 1000000000ULL / (uint64_t)app->fps_target;
            uint64_t elapsed = nanotime() - frame_start;
            if (elapsed < frame_ns) {
                struct timespec ts = {
                    (time_t)((frame_ns - elapsed) / 1000000000ULL),
                    (long)((frame_ns - elapsed) % 1000000000ULL)
                };
                nanosleep(&ts, NULL);
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Animation                                                                  */
/* -------------------------------------------------------------------------- */

float ui_ease_linear(float t) { return t; }
float ui_ease_in_quad(float t) { return t * t; }
float ui_ease_out_quad(float t) { return 1.0f - (1.0f - t) * (1.0f - t); }
float ui_ease_in_out_quad(float t) {
    return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
}

/* Cubic */
float ui_ease_in_cubic(float t) { return t * t * t; }
float ui_ease_out_cubic(float t) { float u = 1.0f - t; return 1.0f - u * u * u; }
float ui_ease_in_out_cubic(float t) {
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - 4.0f * (1.0f - t) * (1.0f - t) * (1.0f - t);
}

/* Quart */
float ui_ease_in_quart(float t) { return t * t * t * t; }
float ui_ease_out_quart(float t) { float u = 1.0f - t; return 1.0f - u * u * u * u; }
float ui_ease_in_out_quart(float t) {
    return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - 8.0f * (1.0f - t) * (1.0f - t) * (1.0f - t) * (1.0f - t);
}

/* Sine */
float ui_ease_in_sine(float t) { return 1.0f - cosf((float)(t * 3.14159265359 / 2.0)); }
float ui_ease_out_sine(float t) { return sinf((float)(t * 3.14159265359 / 2.0)); }
float ui_ease_in_out_sine(float t) { return 0.5f * (1.0f - cosf((float)(t * 3.14159265359))); }

/* Back */
float ui_ease_in_back(float t) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    return c3 * t * t * t - c1 * t * t;
}
float ui_ease_out_back(float t) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float u = 1.0f - t;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}
float ui_ease_in_out_back(float t) {
    const float c1 = 1.70158f, c2 = c1 * 1.525f;
    return t < 0.5f
        ? 4.0f * t * t * ((c2 + 1.0f) * 2.0f * t - c2)
        : 0.5f * ((2.0f * t - 2.0f) * (2.0f * t - 2.0f) * ((c2 + 1.0f) * (2.0f * t - 2.0f) + c2) + 2.0f);
}

/* Bounce */
static float bounce_out(float t) {
    const float n1 = 7.5625f, d1 = 2.75f;
    if (t < 1.0f / d1) return n1 * t * t;
    if (t < 2.0f / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
    if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
    t -= 2.625f / d1; return n1 * t * t + 0.984375f;
}
float ui_ease_in_bounce(float t) { return 1.0f - bounce_out(1.0f - t); }
float ui_ease_out_bounce(float t) { return bounce_out(t); }
float ui_ease_in_out_bounce(float t) {
    return t < 0.5f ? 0.5f * (1.0f - bounce_out(1.0f - 2.0f * t)) : 0.5f * bounce_out(2.0f * t - 1.0f) + 0.5f;
}

/* Elastic */
static float elastic_out(float t) {
    if (t == 0.0f || t == 1.0f) return t;
    const float c4 = (float)(2.0 * 3.14159265359 / 3.0);
    return powf(2.0f, -10.0f * t) * sinf((t * 10.0f - 0.75f) * c4) + 1.0f;
}
float ui_ease_in_elastic(float t) {
    if (t == 0.0f || t == 1.0f) return t;
    const float c4 = (float)(2.0 * 3.14159265359 / 3.0);
    return -powf(2.0f, 10.0f * t - 10.0f) * sinf((t * 10.0f - 10.75f) * c4);
}
float ui_ease_out_elastic(float t) { return elastic_out(t); }
float ui_ease_in_out_elastic(float t) {
    if (t == 0.0f || t == 1.0f) return t;
    const float c5 = (float)(2.0 * 3.14159265359 / 4.5);
    return t < 0.5f
        ? -0.5f * powf(2.0f, 20.0f * t - 10.0f) * sinf((20.0f * t - 11.125f) * c5)
        : 0.5f * powf(2.0f, -20.0f * t + 10.0f) * sinf((20.0f * t - 11.125f) * c5) + 1.0f;
}

void ui_app_animate(ui_app_t *app, float *value, float target, float duration_ms, ui_ease_fn ease) {
    if (!app || !value || duration_ms <= 0.0f) {
        if (value) *value = target;
        return;
    }
    /* Find existing animation for this value and update it, or add new */
    for (size_t i = 0; i < app->anim_count; i++) {
        if (app->anims[i].value == value) {
            app->anims[i].from = *value;
            app->anims[i].target = target;
            app->anims[i].duration_ms = duration_ms;
            app->anims[i].start_ns = nanotime();
            app->anims[i].ease = ease ? ease : ui_ease_linear;
            app->anims[i].active = true;
            return;
        }
    }
    /* Grow array if needed */
    if (app->anim_count >= app->anim_cap) {
        size_t new_cap = app->anim_cap ? app->anim_cap * 2 : 4;
        ui_anim_t *n = realloc(app->anims, new_cap * sizeof(ui_anim_t));
        if (!n) { *value = target; return; }
        app->anims = n;
        app->anim_cap = new_cap;
    }
    ui_anim_t *a = &app->anims[app->anim_count++];
    a->value = value;
    a->from = *value;
    a->target = target;
    a->duration_ms = duration_ms;
    a->start_ns = nanotime();
    a->ease = ease ? ease : ui_ease_linear;
    a->active = true;
}

static void ui_app_process_anims(ui_app_t *app) {
    if (!app || !app->anims) return;
    uint64_t now = nanotime();
    for (size_t i = 0; i < app->anim_count; i++) {
        ui_anim_t *a = &app->anims[i];
        if (!a->active) continue;
        float elapsed_ms = (float)((now - a->start_ns) / 1000000.0);
        if (elapsed_ms >= a->duration_ms) {
            *a->value = a->target;
            a->active = false;
            continue;
        }
        float t = elapsed_ms / a->duration_ms;
        float e = a->ease(t);
        *a->value = a->from + (a->target - a->from) * e;
    }
    /* Compact: remove inactive anims */
    size_t j = 0;
    for (size_t i = 0; i < app->anim_count; i++) {
        if (app->anims[i].active) {
            if (i != j) app->anims[j] = app->anims[i];
            j++;
        }
    }
    app->anim_count = j;
}
