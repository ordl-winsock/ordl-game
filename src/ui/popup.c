/*
 * ORDL UI — Popup / menu overlay system
 * Context menus, dropdowns, tooltips as floating overlays.
 * Pure C23, zero external dependencies.
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    ui_widget_t *content;
    int x, y;
    bool visible;
    bool dismiss_on_outside_click;
} popup_state_t;

static popup_state_t g_popup = {0};

void ui_popup_show(ui_app_t *app, ui_widget_t *content, int x, int y) {
    (void)app;
    if (g_popup.content && g_popup.content != content) {
        /* Remove previous content from its parent to avoid double-reference */
        if (g_popup.content->parent) {
            ui_widget_remove_child(g_popup.content->parent, g_popup.content);
        }
    }
    /* Remove new content from its existing parent before showing in popup */
    if (content && content->parent) {
        ui_widget_remove_child(content->parent, content);
    }
    g_popup.content = content;
    g_popup.x = x;
    g_popup.y = y;
    g_popup.visible = true;
    g_popup.dismiss_on_outside_click = true;
    if (content) {
        content->bounds.x = x;
        content->bounds.y = y;
        ui_widget_invalidate(content);
    }
}

void ui_popup_hide(ui_app_t *app) {
    (void)app;
    g_popup.visible = false;
    if (g_popup.content) {
        ui_widget_destroy(g_popup.content);
    }
    g_popup.content = NULL;
}

bool ui_popup_is_visible(void) {
    return g_popup.visible;
}

ui_widget_t *ui_popup_get_content(void) {
    return g_popup.visible ? g_popup.content : NULL;
}

void ui_popup_set_dismiss_on_outside_click(bool dismiss) {
    g_popup.dismiss_on_outside_click = dismiss;
}

/* Called by app step after root render */
void ui_popup_render(ui_app_t *app, ui_canvas_t *c) {
    (void)app;
    if (!g_popup.visible || !g_popup.content) return;
    ui_widget_render(g_popup.content, c);
}

/* Called by app event dispatch before root dispatch */
bool ui_popup_handle_event(ui_app_t *app, const ui_event_t *ev) {
    (void)app;
    if (!g_popup.visible) return false;
    if (!g_popup.content) return false;

    if (ev->type == UI_EVENT_MOUSE_PRESS) {
        int mx = ev->mouse.x;
        int my = ev->mouse.y;
        ui_rect_t b = g_popup.content->bounds;
        bool inside = (mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h);
        if (!inside && g_popup.dismiss_on_outside_click) {
            ui_popup_hide(app);
            return true; /* consumed the dismiss click */
        }
        if (inside && g_popup.content->on_event) {
            return g_popup.content->on_event(g_popup.content, ev);
        }
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* Menu widget — vertical list of clickable items                             */
/* -------------------------------------------------------------------------- */

typedef struct {
    char **items;
    size_t count, cap;
    int selected;
    void (*on_select)(const char *item, void *ud);
    void *on_select_ud;
} menu_data_t;

static ui_size_t menu_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    menu_data_t *d = (menu_data_t *)w->data;
    ui_style_t *s = &w->style->states[w->state];
    int max_len = 8;
    for (size_t i = 0; d && i < d->count; i++) {
        int len = d->items[i] ? (int)strlen(d->items[i]) : 0;
        if (len > max_len) max_len = len;
    }
    return (ui_size_t){ max_len + s->pad[1] + s->pad[3],
                        (int)(d ? d->count : 0) + s->pad[0] + s->pad[2] };
}

static void menu_render(ui_widget_t *w, ui_canvas_t *c) {
    menu_data_t *d = (menu_data_t *)w->data;
    ui_style_t *s = &w->style->states[w->state];
    ui_draw_rect(c, w->bounds, s->fg, s->bg, 0);
    if (!d) return;
    int x = w->bounds.x + s->pad[3];
    int y = w->bounds.y + s->pad[0];
    for (size_t i = 0; i < d->count; i++) {
        ui_color_t fg = s->fg;
        ui_color_t bg = s->bg;
        if ((int)i == d->selected) {
            bg = ui_rgb(100, 100, 200);
            fg = ui_rgb(255, 255, 255);
        }
        const char *text = d->items[i] ? d->items[i] : "";
        ui_draw_text(c, x, y + (int)i, text, fg, bg, 0);
    }
}

static bool menu_on_event(ui_widget_t *w, const ui_event_t *ev) {
    menu_data_t *d = (menu_data_t *)w->data;
    if (!d) return false;
    if (ev->type == UI_EVENT_MOUSE_MOVE || ev->type == UI_EVENT_MOUSE_PRESS) {
        int my = ev->mouse.y - w->bounds.y;
        ui_style_t *s = &w->style->states[w->state];
        int idx = my - s->pad[0];
        if (idx >= 0 && idx < (int)d->count) {
            d->selected = idx;
            w->dirty_render = true;
            if (ev->type == UI_EVENT_MOUSE_PRESS && d->on_select) {
                d->on_select(d->items[idx], d->on_select_ud);
                return true;
            }
            return true;
        }
    }
    if (ev->type == UI_EVENT_KEY) {
        if (ev->key.key == UI_KEY_UP) {
            if (d->selected > 0) { d->selected--; w->dirty_render = true; }
            return true;
        }
        if (ev->key.key == UI_KEY_DOWN) {
            if (d->selected < (int)d->count - 1) { d->selected++; w->dirty_render = true; }
            return true;
        }
        if (ev->key.key == UI_KEY_ENTER && d->on_select && d->selected >= 0) {
            d->on_select(d->items[d->selected], d->on_select_ud);
            return true;
        }
    }
    return false;
}

static void menu_destroy(ui_widget_t *w) {
    menu_data_t *d = (menu_data_t *)w->data;
    if (!d) return;
    for (size_t i = 0; i < d->count; i++) free(d->items[i]);
    free(d->items);
    free(d);
}

ui_widget_t *ui_menu_new(const char *id) {
    ui_widget_t *w = ui_box_new(id);
    if (!w) return NULL;
    w->type = UI_WIDGET_CUSTOM;
    menu_data_t *d = calloc(1, sizeof(menu_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    d->selected = -1;
    w->data = d;
    w->measure = menu_measure;
    w->render = menu_render;
    w->on_event = menu_on_event;
    w->destroy = menu_destroy;
    w->style->states[UI_STATE_NORMAL].bg = ui_rgb(30, 30, 30);
    w->style->states[UI_STATE_NORMAL].fg = ui_rgb(200, 200, 200);
    return w;
}

void ui_menu_add_item(ui_widget_t *w, const char *label) {
    if (!w || !label) return;
    menu_data_t *d = (menu_data_t *)w->data;
    if (!d) return;
    if (d->count >= d->cap) {
        size_t nc = d->cap ? d->cap * 2 : 4;
        char **ni = realloc(d->items, nc * sizeof(char *));
        if (!ni) return;
        d->items = ni;
        d->cap = nc;
    }
    d->items[d->count] = strdup(label);
    /* Item strings are owned by the menu widget and freed by menu_destroy().
       GCC analyzer cannot track cross-function ownership; suppress false positive. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
    if (d->items[d->count]) {
        d->count++;
    }
#pragma GCC diagnostic pop
}

void ui_menu_set_callback(ui_widget_t *w, void (*cb)(const char *, void *), void *ud) {
    if (!w) return;
    menu_data_t *d = (menu_data_t *)w->data;
    if (!d) return;
    d->on_select = cb;
    d->on_select_ud = ud;
}

/* -------------------------------------------------------------------------- */
/* Context menu convenience                                                   */
/* -------------------------------------------------------------------------- */

void ui_context_menu_show(ui_app_t *app, int x, int y, const char **items, size_t n,
                          void (*cb)(const char *, void *), void *ud) {
    ui_widget_t *menu = ui_menu_new("ctx_menu");
    if (!menu) return;
    for (size_t i = 0; i < n; i++) ui_menu_add_item(menu, items[i]);
    ui_menu_set_callback(menu, cb, ud);
    ui_popup_show(app, menu, x, y);
}
