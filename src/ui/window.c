#include <signal.h>
#include "forge/ui/ordl_ui.h"
#include "forge/ui/ordl_ui_debug.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Signal flag for clean shutdown — provided by FORGE platform layer */
static volatile sig_atomic_t g_running = 1;

void ui_window_mgr_set_running(bool running) {
    g_running = running ? 1 : 0;
}

bool ui_window_mgr_get_running(void) {
    return g_running != 0;
}

/*
 * ORDL UI — Multi-window support
 * Manage multiple top-level windows, each with its own backend.
 * Pure C23, zero external dependencies.
 */

struct ui_window {
    ui_backend_t *backend;
    ui_widget_t *root;
    ui_widget_t *focused;
    char title[128];
    bool visible;
    int w, h;
    ui_window_t *next;
    bool needs_layout;
    bool needs_render;
};

struct ui_window_mgr {
    ui_window_t *windows;
    size_t count;
    bool running;
    uint64_t last_frame_ns;
    int fps_target;
    void (*tick_cb)(ui_window_mgr_t *, void *);
    void *tick_userdata;
};

static uint64_t nanotime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* -------------------------------------------------------------------------- */
/* Window lifecycle                                                           */
/* -------------------------------------------------------------------------- */

ui_window_t *ui_window_new(ui_backend_t *backend, const char *title, int w, int h) {
    if (!backend) return NULL;
    ui_window_t *win = calloc(1, sizeof(ui_window_t));
    if (!win) return NULL;
    win->backend = backend;
    win->w = w;
    win->h = h;
    win->visible = true;
    win->needs_layout = true;
    win->needs_render = true;
    if (title) {
        strncpy(win->title, title, sizeof(win->title) - 1);
        win->title[sizeof(win->title) - 1] = '\0';
    }
    if (!backend->init(backend, w, h)) {
        free(win);
        return NULL;
    }
    return win;
}

void ui_window_destroy(ui_window_t *win) {
    if (!win) return;
    if (win->root) ui_widget_destroy(win->root);
    if (win->backend) {
        win->backend->shutdown(win->backend);
/* shutdown already freed the backend */
    }
    free(win);
}

void ui_window_set_root(ui_window_t *win, ui_widget_t *root) {
    if (!win) return;
    win->root = root;
    win->needs_layout = true;
    win->needs_render = true;
}

void ui_window_set_focus(ui_window_t *win, ui_widget_t *w) {
    if (!win) return;
    if (win->focused && win->focused != w) {
        ui_widget_set_state(win->focused, UI_STATE_NORMAL);
    }
    win->focused = w;
    if (w) {
        ui_widget_set_state(w, UI_STATE_FOCUSED);
    }
}

void ui_window_show(ui_window_t *win) {
    if (win) win->visible = true;
}

void ui_window_hide(ui_window_t *win) {
    if (win) win->visible = false;
}

bool ui_window_visible(ui_window_t *win) {
    return win ? win->visible : false;
}

ui_backend_t *ui_window_backend(ui_window_t *win) {
    return win ? win->backend : NULL;
}

/* -------------------------------------------------------------------------- */
/* Window manager                                                             */
/* -------------------------------------------------------------------------- */

ui_window_mgr_t *ui_window_mgr_new(void) {
    ui_window_mgr_t *mgr = calloc(1, sizeof(ui_window_mgr_t));
    if (!mgr) return NULL;
    mgr->fps_target = 60;
    return mgr;
}

void ui_window_mgr_free(ui_window_mgr_t *mgr) {
    if (!mgr) return;
    ui_window_t *w = mgr->windows;
    while (w) {
        ui_window_t *next = w->next;
        ui_window_destroy(w);
        w = next;
    }
    free(mgr);
}

void ui_window_mgr_add(ui_window_mgr_t *mgr, ui_window_t *win) {
    if (!mgr || !win) return;
    win->next = mgr->windows;
    mgr->windows = win;
    mgr->count++;
}

void ui_window_mgr_remove(ui_window_mgr_t *mgr, ui_window_t *win) {
    if (!mgr || !win) return;
    ui_window_t **pp = &mgr->windows;
    while (*pp) {
        if (*pp == win) {
            *pp = win->next;
            mgr->count--;
            return;
        }
        pp = &(*pp)->next;
    }
}

void ui_window_mgr_quit(ui_window_mgr_t *mgr) {
    if (mgr) mgr->running = false;
}

void ui_window_mgr_set_tick_cb(ui_window_mgr_t *mgr, void (*cb)(ui_window_mgr_t *, void *), void *userdata) {
    if (!mgr) return;
    mgr->tick_cb = cb;
    mgr->tick_userdata = userdata;
}

/* -------------------------------------------------------------------------- */
/* Event loop                                                                 */
/* -------------------------------------------------------------------------- */

static void window_process_events(ui_window_t *win) {
    if (!win->visible || !win->backend) return;
    ui_debug_log("[window_process_events] win=%s visible=%d focused=%p", win->title, win->visible, (void*)win->focused);
    ui_backend_t *be = win->backend;
    ui_event_t ev = {0};
    int event_count = 0;
    while (be->poll_event(be, &ev, 0)) {
        event_count++;
        ui_debug_log("[window_process_events] event #%d type=%d", event_count, ev.type);
        if (ev.type == UI_EVENT_QUIT) {
            win->visible = false;
            continue;
        }
        /* Ctrl+C -> quit */
        if (ev.type == UI_EVENT_KEY && ev.key.ctrl && ev.key.codepoint == 'C') {
            win->visible = false;
            continue;
        }
        if (ev.type == UI_EVENT_RESIZE) {
            ui_debug_log("[window_process_events] RESIZE w=%d h=%d", ev.resize.w, ev.resize.h);
            int new_w = ev.resize.w > 0 ? ev.resize.w : 80;
            int new_h = ev.resize.h > 0 ? ev.resize.h : 24;
            win->w = new_w;
            win->h = new_h;
            if (be->canvas->type == UI_CANVAS_TERM) {
                ui_canvas_free(be->canvas);
                be->canvas = ui_canvas_new_term(new_w, new_h);
                if (!be->canvas) {
                    ui_debug_log("[window_process_events] canvas allocation failed on resize");
                    win->visible = false;
                    continue;
                }
            }
            win->needs_layout = true;
            continue;
        }
        if (ev.type == UI_EVENT_MOUSE_PRESS) {
            ui_debug_log("[window_process_events] MOUSE_PRESS x=%d y=%d", ev.mouse.x, ev.mouse.y);
            ui_widget_t *hit = ui_widget_hit_test(win->root, ev.mouse.x, ev.mouse.y);
            ui_debug_log("[window_process_events] hit_test=%p id=%s", (void*)hit, hit ? hit->id : "null");
            if (hit && hit != win->focused) {
                ui_window_set_focus(win, hit);
            }
        }
        if (win->root) {
            if (ev.type == UI_EVENT_KEY && win->focused) {
                ui_debug_log("[window_process_events] dispatch KEY to focused=%p id=%s", (void*)win->focused, win->focused->id);
                /* Direct dispatch to focused widget, bubble up to root */
                ui_widget_t *target = win->focused;
                while (target) {
                    if (target->on_event && target->on_event(target, &ev)) {
                        ui_debug_log("[window_process_events] KEY handled by %p id=%s", (void*)target, target->id);
                        break;
                    }
                    target = target->parent;
                }
                if (!target) ui_debug_log("[window_process_events] KEY not handled");
            } else {
                bool handled = ui_widget_dispatch_event(win->root, &ev);
                ui_debug_log("[window_process_events] dispatch_event handled=%d", handled);
            }
        }
    }
    if (event_count > 0) ui_debug_log("[window_process_events] processed %d events", event_count);
}

static void window_render(ui_window_t *win) {
    if (!win->visible || !win->backend || !win->root) return;
    ui_backend_t *be = win->backend;
    if (win->needs_layout) {
        ui_layout_run(win->root, be->canvas);
        ui_widget_invalidate_all(win->root);
        win->needs_layout = false;
        win->needs_render = true;
    }
    if (win->needs_render) {
        ui_widget_render(win->root, be->canvas);
        be->present(be);
        win->needs_render = false;
    }
}

void ui_window_mgr_step(ui_window_mgr_t *mgr) {
    if (!mgr) return;

    /* Tick callback for streaming, animation, etc. */
    if (mgr->tick_cb) mgr->tick_cb(mgr, mgr->tick_userdata);

    /* Poll events for all windows */
    ui_window_t *w = mgr->windows;
    while (w) {
        window_process_events(w);
        w = w->next;
    }
    /* Render all visible windows */
    w = mgr->windows;
    while (w) {
        if (w->visible && w->root) {
            /* Mark dirty if root is dirty */
            if (w->root->dirty_layout) {
                w->needs_layout = true;
                w->needs_render = true;
            }
            if (w->root->dirty_render) {
                w->needs_render = true;
            }
            window_render(w);
        }
        w = w->next;
    }
}

void ui_window_mgr_run(ui_window_mgr_t *mgr) {
    if (!mgr) return;
    mgr->running = true;
    while (mgr->running && g_running) {
        uint64_t frame_start = nanotime();
        /* Check if any windows still visible */
        bool any_visible = false;
        ui_window_t *w = mgr->windows;
        while (w) {
            if (w->visible) { any_visible = true; break; }
            w = w->next;
        }
        if (!any_visible) {
            mgr->running = false;
            break;
        }
        ui_window_mgr_step(mgr);
        if (mgr->fps_target > 0) {
            uint64_t frame_ns = 1000000000ULL / (uint64_t)mgr->fps_target;
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
