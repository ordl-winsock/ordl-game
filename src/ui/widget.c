/*
 * ORDL UI — Widget system
 * Base widget + primitives: box, label, button, input.
 */

#include "forge/ui/ordl_ui.h"
#include "forge/ui/ordl_ui_debug.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------- */
/* Base widget                                                                */
/* -------------------------------------------------------------------------- */

static ui_size_t default_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    return w->preferred_size;
}

static void default_render(ui_widget_t *w, ui_canvas_t *c) {
    /* Default: render background from style, skip fully transparent */
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    if (w->bounds.w > 0 && w->bounds.h > 0 && s->bg.a > 0) {
        ui_draw_rect(c, w->bounds, s->fg, s->bg, 0);
    }
}

static bool default_on_event(ui_widget_t *w, const ui_event_t *ev) {
    (void)w; (void)ev;
    return false;
}

static void default_destroy(ui_widget_t *w) {
    (void)w;
}

static ui_widget_t *widget_alloc(ui_widget_type_t type, const char *id) {
    ui_widget_t *w = calloc(1, sizeof(ui_widget_t));
    if (!w) return NULL;
    w->type = type;
    if (id) {
        strncpy(w->id, id, sizeof(w->id) - 1);
        w->id[sizeof(w->id) - 1] = '\0';
    }
    w->visible = true;
    w->enabled = true;
    w->measure = default_measure;
    w->render = default_render;
    w->on_event = default_on_event;
    w->destroy = default_destroy;
    w->layout = ui_layout_col();
    w->style = calloc(1, sizeof(ui_style_set_t));
    if (!w->style) { free(w); return NULL; }
    /* Default style: transparent bg, white fg */
    for (int i = 0; i < UI_STATE_COUNT; i++) {
        w->style->states[i].fg = ui_rgb(255, 255, 255);
        w->style->states[i].bg = ui_rgba(0, 0, 0, 0);
    }
    return w;
}

/* Forward declaration */
static void ui_widget_dirty_layout(ui_widget_t *w);

static void ui_widget_dirty_layout(ui_widget_t *w) {
    while (w) {
        w->dirty_layout = true;
        w = w->parent;
    }
}

/* -------------------------------------------------------------------------- */
/* Widget tree                                                                */
/* -------------------------------------------------------------------------- */

void ui_widget_add_child(ui_widget_t *parent, ui_widget_t *child) {
    if (!parent || !child) return;
    if (child->parent != NULL) {
        ui_widget_remove_child(child->parent, child);
    }
    if (parent->child_count >= parent->child_cap) {
        size_t new_cap = parent->child_cap ? parent->child_cap * 2 : 4;
        ui_widget_t **n = realloc(parent->children, new_cap * sizeof(ui_widget_t *));
        if (!n) return;
        parent->children = n;
        parent->child_cap = new_cap;
    }
    child->parent = parent;
    parent->children[parent->child_count++] = child;
    ui_widget_dirty_layout(parent);
}

void ui_widget_remove_child(ui_widget_t *parent, ui_widget_t *child) {
    if (!parent || !child) return;
    for (size_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child) {
            memmove(&parent->children[i], &parent->children[i + 1],
                    (parent->child_count - i - 1) * sizeof(ui_widget_t *));
            parent->child_count--;
            child->parent = NULL;
            ui_widget_dirty_layout(parent);
            return;
        }
    }
}

void ui_widget_destroy(ui_widget_t *w) {
    if (!w) return;
    if (w->destroy) w->destroy(w);
    for (size_t i = 0; i < w->child_count; i++) {
        ui_widget_destroy(w->children[i]);
    }
    free(w->children);
    free(w->style);
    /* Note: w->data is freed by the widget-type-specific destroy callback above */
    free(w);
}

ui_widget_t *ui_widget_find(ui_widget_t *root, const char *id) {
    if (!root || !id) return NULL;
    if (strcmp(root->id, id) == 0) return root;

    /* Iterative BFS to avoid stack overflow on deep trees */
    ui_widget_t **queue = malloc(256 * sizeof(ui_widget_t *));
    if (!queue) return NULL;
    size_t qcap = 256, qhead = 0, qtail = 0;
    queue[qtail++] = root;

    while (qhead < qtail) {
        ui_widget_t *w = queue[qhead++];
        for (size_t i = 0; i < w->child_count; i++) {
            ui_widget_t *ch = w->children[i];
            if (strcmp(ch->id, id) == 0) {
                free(queue);
                return ch;
            }
            if (qtail >= qcap) {
                qcap *= 2;
                ui_widget_t **n = realloc(queue, qcap * sizeof(ui_widget_t *));
                if (!n) { free(queue); return NULL; }
                queue = n;
            }
            queue[qtail++] = ch;
        }
    }
    free(queue);
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Widget state                                                               */
/* -------------------------------------------------------------------------- */

void ui_widget_set_visible(ui_widget_t *w, bool visible) {
    if (!w || w->visible == visible) return;
    w->visible = visible;
    ui_widget_invalidate(w);
    if (w->parent) ui_widget_dirty_layout(w->parent);
}

void ui_widget_set_enabled(ui_widget_t *w, bool enabled) {
    if (!w || w->enabled == enabled) return;
    w->enabled = enabled;
    ui_widget_set_state(w, enabled ? UI_STATE_NORMAL : UI_STATE_DISABLED);
}

void ui_widget_set_focus(ui_widget_t *w, bool focused) {
    if (!w) return;
    ui_widget_set_state(w, focused ? UI_STATE_FOCUSED : UI_STATE_NORMAL);
    ui_a11y_set_state(w, UI_A11Y_FOCUSED, focused);
}

void ui_widget_set_state(ui_widget_t *w, ui_widget_state_t state) {
    if (!w || w->state == state) return;
    w->state = state;
    ui_widget_invalidate(w);
}

/* -------------------------------------------------------------------------- */
/* Rendering                                                                  */
/* -------------------------------------------------------------------------- */

static void widget_render_impl(ui_widget_t *w, ui_canvas_t *c, bool parent_dirty) {
    if (!w || !w->visible) return;
    bool render = w->dirty_render || parent_dirty;
    if (render && w->render) w->render(w, c);
    w->dirty_render = false;
    for (size_t i = 0; i < w->child_count; i++) {
        widget_render_impl(w->children[i], c, render);
    }
}

void ui_widget_render(ui_widget_t *root, ui_canvas_t *c) {
    widget_render_impl(root, c, false);
}

void ui_widget_invalidate(ui_widget_t *w) {
    if (!w) return;
    while (w) {
        w->dirty_render = true;
        w = w->parent;
    }
}

void ui_widget_invalidate_all(ui_widget_t *w) {
    if (!w) return;
    w->dirty_render = true;
    for (size_t i = 0; i < w->child_count; i++) {
        ui_widget_invalidate_all(w->children[i]);
    }
}

/* -------------------------------------------------------------------------- */
/* Event routing                                                              */
/* -------------------------------------------------------------------------- */

ui_widget_t *ui_widget_hit_test(ui_widget_t *root, int x, int y) {
    if (!root || !root->visible || !root->enabled) return NULL;
    /* Check children first (topmost) */
    for (size_t i = root->child_count; i-- > 0; ) {
        ui_widget_t *hit = ui_widget_hit_test(root->children[i], x, y);
        if (hit) return hit;
    }
    /* Check self */
    if (x >= root->bounds.x && x < root->bounds.x + root->bounds.w &&
        y >= root->bounds.y && y < root->bounds.y + root->bounds.h) {
        return root;
    }
    return NULL;
}

/* Find first scrollable descendant of a widget */
static ui_widget_t *find_scrollable(ui_widget_t *w) {
    if (!w) return NULL;
    if (w->type == UI_WIDGET_SCROLL) return w;
    for (size_t i = 0; i < w->child_count; i++) {
        ui_widget_t *found = find_scrollable(w->children[i]);
        if (found) return found;
    }
    return NULL;
}

bool ui_widget_dispatch_event(ui_widget_t *root, const ui_event_t *ev) {
    if (!root || !ev) return false;

    /* Mouse events: route to hit widget */
    if (ev->type == UI_EVENT_MOUSE_PRESS || ev->type == UI_EVENT_MOUSE_RELEASE ||
        ev->type == UI_EVENT_MOUSE_MOVE) {
        ui_widget_t *target = ui_widget_hit_test(root, ev->mouse.x, ev->mouse.y);
        if (target && target->on_event) {
            return target->on_event(target, ev);
        }
    }

    /* Scroll events: bubble up from hit widget to ancestors.
       If no ancestor handles it, find a scrollable sibling/cousin. */
    if (ev->type == UI_EVENT_MOUSE_SCROLL) {
        ui_widget_t *target = ui_widget_hit_test(root, ev->mouse.x, ev->mouse.y);
        while (target) {
            if (target->on_event && target->on_event(target, ev)) return true;
            /* If this widget's parent has a scrollable sibling, try it */
            if (target->parent) {
                ui_widget_t *scroll = find_scrollable(target->parent);
                if (scroll && scroll != target && scroll->on_event && scroll->on_event(scroll, ev)) return true;
            }
            target = target->parent;
        }
        /* Final fallback: any scrollable in the entire tree */
        ui_widget_t *scroll = find_scrollable(root);
        if (scroll && scroll->on_event) return scroll->on_event(scroll, ev);
    }

    /* Key events: route to focused widget, bubble up parent chain */
    if (ev->type == UI_EVENT_KEY) {
        ui_widget_t *target = root;
        /* Iteratively find the deepest focused descendant */
        bool changed;
        do {
            changed = false;
            for (size_t i = 0; i < target->child_count; i++) {
                ui_widget_t *ch = target->children[i];
                if (ch->state == UI_STATE_FOCUSED && ch->visible && ch->enabled) {
                    target = ch;
                    changed = true;
                    break;
                }
            }
        } while (changed);
        /* Bubble up: try target, then ancestors until root */
        while (target) {
            if (target->visible && target->enabled && target->on_event && target->on_event(target, ev)) return true;
            if (target == root) break;
            target = target->parent;
        }
    }

    return false;
}

/* -------------------------------------------------------------------------- */
/* Box                                                                        */
/* -------------------------------------------------------------------------- */

ui_widget_t *ui_box_new(const char *id) {
    return widget_alloc(UI_WIDGET_BOX, id);
}

/* -------------------------------------------------------------------------- */
/* Label                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    char *text;
    bool wrap;
} label_data_t;

/* Count how many wrapped lines a text needs given a max width */
static int count_wrapped_lines(const char *text, int max_w) {
    if (!text || max_w <= 0) return 1;
    int lines = 0;
    const char *p = text;
    while (*p) {
        const char *line_end = strchr(p, '\n');
        int line_len = line_end ? (int)(line_end - p) : (int)strlen(p);
        const char *seg = p;
        while (line_len > 0) {
            int seg_len = line_len;
            if (seg_len > max_w) {
                /* Find last space before max_w */
                seg_len = max_w;
                while (seg_len > 0 && seg[seg_len - 1] != ' ') seg_len--;
                if (seg_len == 0) seg_len = max_w; /* break at max_w if no space */
            }
            lines++;
            seg += seg_len;
            line_len -= seg_len;
            /* Skip leading spaces on next segment */
            while (line_len > 0 && *seg == ' ') { seg++; line_len--; }
        }
        if (line_end) p = line_end + 1;
        else break;
    }
    return lines > 0 ? lines : 1;
}

static ui_size_t label_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    label_data_t *d = (label_data_t *)w->data;
    if (!d || !d->text) return (ui_size_t){0, 0};
    if (!w->style) return (ui_size_t){0, 0};
    ui_style_t *s = &w->style->states[w->state];
    int pad_lr = s->pad[1] + s->pad[3];
    int pad_tb = s->pad[0] + s->pad[2];
    /* Use preferred width if set, else text width */
    int avail_w = w->preferred_size.w > pad_lr ? w->preferred_size.w - pad_lr : 80;
    int lines = count_wrapped_lines(d->text, avail_w);
    int tw = ui_text_width(d->text);
    if (tw > avail_w) tw = avail_w;
    return (ui_size_t){
        tw + pad_lr,
        lines + pad_tb
    };
}

static void label_render(ui_widget_t *w, ui_canvas_t *c) {
    label_data_t *d = (label_data_t *)w->data;
    if (!d || !d->text || !w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    int x0 = w->bounds.x + s->pad[3];
    int y = w->bounds.y + s->pad[0];
    int max_w = w->bounds.w - s->pad[1] - s->pad[3];
    if (max_w < 1) return;

    const char *p = d->text;
    while (*p && y < w->bounds.y + w->bounds.h) {
        const char *line_end = strchr(p, '\n');
        int line_len = line_end ? (int)(line_end - p) : (int)strlen(p);
        const char *seg = p;
        while (line_len > 0 && y < w->bounds.y + w->bounds.h) {
            int seg_len = line_len;
            if (seg_len > max_w) {
                seg_len = max_w;
                while (seg_len > 0 && seg[seg_len - 1] != ' ') seg_len--;
                if (seg_len == 0) seg_len = max_w;
            }
            /* Draw this segment */
            char buf[256];
            int n = seg_len;
            if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
            memcpy(buf, seg, n);
            buf[n] = '\0';
            ui_draw_text(c, x0, y, buf, s->fg, s->bg, s->border_width);
            y++;
            seg += seg_len;
            line_len -= seg_len;
            while (line_len > 0 && *seg == ' ') { seg++; line_len--; }
        }
        if (line_end) p = line_end + 1;
        else break;
    }
}

static void label_destroy(ui_widget_t *w) {
    label_data_t *d = (label_data_t *)w->data;
    if (d) { free(d->text); free(d); }
}

ui_widget_t *ui_label_new(const char *id, const char *text) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_LABEL, id);
    if (!w) return NULL;
    label_data_t *d = calloc(1, sizeof(label_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    if (text) d->text = strdup(text);
    w->data = d;
    w->measure = label_measure;
    w->render = label_render;
    w->destroy = label_destroy;
    w->preferred_size = label_measure(w, NULL);
    return w;
}

void ui_label_set_text(ui_widget_t *w, const char *text) {
    if (!w || w->type != UI_WIDGET_LABEL) return;
    label_data_t *d = (label_data_t *)w->data;
    if (!d) return;
    free(d->text);
    d->text = text ? strdup(text) : NULL;
    w->dirty_layout = true;
    ui_widget_invalidate(w);
    ui_widget_dirty_layout(w);
}

/* -------------------------------------------------------------------------- */
/* Button                                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    char *label;
    void (*on_click)(ui_widget_t *w, void *user_data);
    void *user_data;
} button_data_t;

static ui_size_t button_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    button_data_t *d = (button_data_t *)w->data;
    if (!d || !d->label) return (ui_size_t){4, 1};
    int tw = ui_text_width(d->label);
    int th = ui_text_height(d->label);
    if (!w->style) return (ui_size_t){0, 0};
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){
        tw + 4 + s->pad[1] + s->pad[3],  /* 2-char padding each side */
        th + s->pad[0] + s->pad[2]
    };
}

static void button_render(ui_widget_t *w, ui_canvas_t *c) {
    button_data_t *d = (button_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    /* Draw box border */
    ui_draw_box(c, w->bounds, s->fg, s->bg, false);
    /* Draw label centered */
    if (d && d->label) {
        int tw = ui_text_width(d->label);
        int x = w->bounds.x + (w->bounds.w - tw) / 2;
        int y = w->bounds.y + (w->bounds.h - 1) / 2;
        ui_draw_text(c, x, y, d->label, s->fg, s->bg, s->border_width);
    }
}

static bool button_on_event(ui_widget_t *w, const ui_event_t *ev) {
    button_data_t *d = (button_data_t *)w->data;
    if (!d || !w->enabled) return false;

    if (ev->type == UI_EVENT_MOUSE_MOVE) {
        if (w->state != UI_STATE_HOVER && w->state != UI_STATE_ACTIVE) {
            ui_widget_set_state(w, UI_STATE_HOVER);
        }
        return true;
    }
    if (ev->type == UI_EVENT_MOUSE_PRESS) {
        ui_widget_set_state(w, UI_STATE_ACTIVE);
        return true;
    }
    if (ev->type == UI_EVENT_MOUSE_RELEASE) {
        ui_widget_set_state(w, UI_STATE_HOVER);
        if (d->on_click) d->on_click(w, d->user_data);
        return true;
    }
    return false;
}

static void button_destroy(ui_widget_t *w) {
    button_data_t *d = (button_data_t *)w->data;
    if (d) { free(d->label); free(d); }
}

ui_widget_t *ui_button_new(const char *id, const char *label) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_BUTTON, id);
    if (!w) return NULL;
    button_data_t *d = calloc(1, sizeof(button_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    if (label) d->label = strdup(label);
    w->data = d;
    w->measure = button_measure;
    w->render = button_render;
    w->on_event = button_on_event;
    w->destroy = button_destroy;
    /* Default button style */
    for (int i = 0; i < UI_STATE_COUNT; i++) {
        w->style->states[i].fg = ui_rgb(255, 255, 255);
        w->style->states[i].bg = ui_rgb(60, 60, 60);
        w->style->states[i].border = ui_rgb(128, 128, 128);
    }
    w->style->states[UI_STATE_HOVER].bg = ui_rgb(80, 80, 80);
    w->style->states[UI_STATE_ACTIVE].bg = ui_rgb(40, 40, 40);
    return w;
}

void ui_button_set_callback(ui_widget_t *w, void (*cb)(ui_widget_t *, void *), void *ud) {
    if (!w || w->type != UI_WIDGET_BUTTON) return;
    button_data_t *d = (button_data_t *)w->data;
    if (d) { d->on_click = cb; d->user_data = ud; }
}

/* -------------------------------------------------------------------------- */
/* Input                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    char *text;
    size_t text_cap;
    size_t text_len;
    size_t cursor;
    char *placeholder;
    bool password;
    int max_len;
    void (*on_submit)(ui_widget_t *, void *);
    void *submit_ud;
} input_data_t;

static ui_size_t input_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    if (!w->style) return (ui_size_t){20, 1};
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ 20 + s->pad[1] + s->pad[3], 1 + s->pad[0] + s->pad[2] };
}

static void input_render(ui_widget_t *w, ui_canvas_t *c) {
    if (!w->style) return;
    input_data_t *d = (input_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    /* Draw box */
    ui_draw_box(c, w->bounds, s->fg, s->bg, false);
    /* Draw text or placeholder */
    int x = w->bounds.x + s->pad[3] + 1;
    int y = w->bounds.y + s->pad[0];
    if (d && d->text_len > 0) {
        if (d->password) {
            char *masked = malloc(d->text_len + 1);
            if (masked) {
                memset(masked, '*', d->text_len);
                masked[d->text_len] = '\0';
                ui_draw_text(c, x, y, masked, s->fg, s->bg, 0);
                free(masked);
            }
        } else {
            /* Clamp display to width */
            int max_chars = w->bounds.w - s->pad[1] - s->pad[3] - 2;
            int start = 0;
            if ((int)d->cursor >= max_chars) start = (int)d->cursor - max_chars + 1;
            char buf[256];
            int n = (int)d->text_len - start;
            if (n > max_chars) n = max_chars;
            if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
            memcpy(buf, d->text + start, n);
            buf[n] = '\0';
            ui_draw_text(c, x, y, buf, s->fg, s->bg, 0);
        }
    } else if (d && d->placeholder && w->state != UI_STATE_FOCUSED) {
        ui_color_t ph_fg = ui_rgba(s->fg.r / 2, s->fg.g / 2, s->fg.b / 2, s->fg.a);
        ui_draw_text(c, x, y, d->placeholder, ph_fg, s->bg, 0);
    }
    /* Draw cursor if focused */
    if (w->state == UI_STATE_FOCUSED) {
        int max_chars = w->bounds.w - s->pad[1] - s->pad[3] - 2;
        int start = 0;
        if ((int)d->cursor >= max_chars) start = (int)d->cursor - max_chars + 1;
        int cursor_x = x + (int)(d->cursor - start);
        if (cursor_x < w->bounds.x + w->bounds.w - s->pad[1]) {
            ui_cell_t cursor_cell = {
                .codepoint = (d->text_len > 0 && d->cursor < d->text_len) ? (uint32_t)d->text[d->cursor] : ' ',
                .fg = s->bg,
                .bg = s->fg,
                .attrs = 0,
            };
            ui_canvas_set_cell(c, cursor_x, y, cursor_cell.codepoint, cursor_cell.fg, cursor_cell.bg, cursor_cell.attrs);
        }
    }
}

static bool input_on_event(ui_widget_t *w, const ui_event_t *ev) {
    input_data_t *d = (input_data_t *)w->data;
    if (!d || !w->enabled) return false;
    ui_debug_log("[input_on_event] widget=%s event_type=%d key=%d codepoint=%u text_len=%zu cursor=%zu", 
                 w->id, ev->type, ev->key.key, ev->key.codepoint, d->text_len, d->cursor);

    if (ev->type == UI_EVENT_KEY) {
        if (ev->key.key == UI_KEY_BACKSPACE) {
            if (d->cursor > 0 && d->text_len > 0) {
                memmove(d->text + d->cursor - 1, d->text + d->cursor, d->text_len - d->cursor + 1);
                d->cursor--;
                d->text_len--;
                ui_widget_invalidate(w);
                ui_debug_log("[input_on_event] BACKSPACE cursor=%zu len=%zu text='%s'", d->cursor, d->text_len, d->text);
            }
            return true;
        }
        if (ev->key.key == UI_KEY_LEFT) {
            if (d->cursor > 0) { d->cursor--; ui_widget_invalidate(w); }
            ui_debug_log("[input_on_event] LEFT cursor=%zu", d->cursor);
            return true;
        }
        if (ev->key.key == UI_KEY_RIGHT) {
            if (d->cursor < d->text_len) { d->cursor++; ui_widget_invalidate(w); }
            ui_debug_log("[input_on_event] RIGHT cursor=%zu", d->cursor);
            return true;
        }
        if (ev->key.key == UI_KEY_HOME) {
            d->cursor = 0; ui_widget_invalidate(w); ui_debug_log("[input_on_event] HOME"); return true;
        }
        if (ev->key.key == UI_KEY_END) {
            d->cursor = d->text_len; ui_widget_invalidate(w); ui_debug_log("[input_on_event] END"); return true;
        }
        if (ev->key.key == UI_KEY_ENTER) {
            ui_debug_log("[input_on_event] ENTER text='%s'", d->text);
            if (d->on_submit) d->on_submit(w, d->submit_ud);
            return true;
        }
        if (ev->key.codepoint > 0 && ev->key.codepoint < 0x110000) {
            /* Insert character */
            if (d->max_len > 0 && (int)d->text_len >= d->max_len) {
                ui_debug_log("[input_on_event] INSERT rejected: max_len reached");
                return true;
            }
            if (d->text_len + 2 > d->text_cap) {
                size_t new_cap = d->text_cap ? d->text_cap * 2 : 64;
                char *n = realloc(d->text, new_cap);
                if (!n) return true;
                d->text = n;
                d->text_cap = new_cap;
            }
            memmove(d->text + d->cursor + 1, d->text + d->cursor, d->text_len - d->cursor + 1);
            /* Simple ASCII insert for now */
            if (ev->key.codepoint < 128) {
                d->text[d->cursor] = (char)ev->key.codepoint;
                d->cursor++;
                d->text_len++;
                ui_widget_invalidate(w);
                ui_debug_log("[input_on_event] INSERT '%c' cursor=%zu len=%zu text='%s'", 
                             (char)ev->key.codepoint, d->cursor, d->text_len, d->text);
            } else {
                ui_debug_log("[input_on_event] INSERT non-ascii codepoint=%u rejected", ev->key.codepoint);
            }
            return true;
        }
        ui_debug_log("[input_on_event] unhandled key=%d codepoint=%u", ev->key.key, ev->key.codepoint);
    }
    return false;
}

static void input_destroy(ui_widget_t *w) {
    input_data_t *d = (input_data_t *)w->data;
    if (d) { free(d->text); free(d->placeholder); free(d); }
}

ui_widget_t *ui_input_new(const char *id, const char *placeholder) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_INPUT, id);
    if (!w) return NULL;
    input_data_t *d = calloc(1, sizeof(input_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    d->text_cap = 64;
    d->text = malloc(d->text_cap);
    if (d->text) d->text[0] = '\0';
    if (placeholder) d->placeholder = strdup(placeholder);
    d->max_len = 256;
    w->data = d;
    w->measure = input_measure;
    w->render = input_render;
    w->on_event = input_on_event;
    w->destroy = input_destroy;
    w->a11y_role = UI_ROLE_TEXTBOX;
    for (int i = 0; i < UI_STATE_COUNT; i++) {
        w->style->states[i].fg = ui_rgb(255, 255, 255);
        w->style->states[i].bg = ui_rgb(30, 30, 30);
    }
    w->style->states[UI_STATE_FOCUSED].bg = ui_rgb(40, 40, 40);
    w->style->states[UI_STATE_FOCUSED].border = ui_rgb(100, 150, 255);
    w->preferred_size = input_measure(w, NULL);
    return w;
}

const char *ui_input_get_text(ui_widget_t *w) {
    if (!w || w->type != UI_WIDGET_INPUT) return NULL;
    input_data_t *d = (input_data_t *)w->data;
    return d ? d->text : NULL;
}

void ui_input_set_text(ui_widget_t *w, const char *text) {
    if (!w || w->type != UI_WIDGET_INPUT) return;
    input_data_t *d = (input_data_t *)w->data;
    if (!d) return;
    free(d->text);
    if (text) {
        d->text_len = strlen(text);
        d->text_cap = d->text_len + 1;
        d->text = malloc(d->text_cap);
        if (d->text) memcpy(d->text, text, d->text_len + 1);
    } else {
        d->text = malloc(64);
        d->text_cap = 64;
        d->text_len = 0;
        if (d->text) d->text[0] = '\0';
    }
    d->cursor = d->text_len;
    ui_widget_invalidate(w);
}

void ui_input_set_submit_callback(ui_widget_t *w, void (*cb)(ui_widget_t *, void *), void *ud) {
    if (!w || w->type != UI_WIDGET_INPUT) return;
    input_data_t *d = (input_data_t *)w->data;
    if (d) { d->on_submit = cb; d->submit_ud = ud; }
}

/* -------------------------------------------------------------------------- */
/* Scroll                                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    int scroll_x, scroll_y;
} scroll_data_t;

static ui_size_t scroll_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    if (w->child_count > 0) {
        ui_widget_t *ch = w->children[0];
        return (ui_size_t){ ch->preferred_size.w, ch->preferred_size.h };
    }
    return w->preferred_size;
}

static void scroll_render(ui_widget_t *w, ui_canvas_t *c) {
    if (!w->style) return;
    scroll_data_t *d = (scroll_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    ui_draw_rect(c, w->bounds, s->fg, s->bg, 0);
    if (w->child_count > 0 && d) {
        ui_widget_t *ch = w->children[0];
        /* Calculate viewport and content dimensions */
        int vp_w = w->bounds.w - w->layout.pad[1] - w->layout.pad[3];
        int vp_h = w->bounds.h - w->layout.pad[0] - w->layout.pad[2];
        if (vp_w < 0) vp_w = 0;
        if (vp_h < 0) vp_h = 0;
        /* Estimate content height from children's bounds */
        int content_h = 0;
        for (size_t i = 0; i < ch->child_count; i++) {
            ui_widget_t *child = ch->children[i];
            int bottom = child->bounds.y + child->bounds.h - ch->bounds.y;
            if (bottom > content_h) content_h = bottom;
        }
        if (content_h < vp_h) content_h = vp_h;
        int max_scroll = content_h - vp_h;
        if (max_scroll < 0) max_scroll = 0;
        if (d->scroll_y > max_scroll) d->scroll_y = max_scroll;
        if (d->scroll_y < 0) d->scroll_y = 0;
        /* Set clip to scroll viewport */
        ui_rect_t clip = {
            w->bounds.x + w->layout.pad[3],
            w->bounds.y + w->layout.pad[0],
            vp_w, vp_h
        };
        ui_canvas_push_clip(c, clip);
        ch->bounds.x = w->bounds.x + w->layout.pad[3] - d->scroll_x;
        ch->bounds.y = w->bounds.y + w->layout.pad[0] - d->scroll_y;
        ui_widget_invalidate_all(ch);
        ui_widget_render(ch, c);
        ui_canvas_pop_clip(c);
    }
}

static bool scroll_on_event(ui_widget_t *w, const ui_event_t *ev) {
    scroll_data_t *d = (scroll_data_t *)w->data;
    if (!d) return false;
    if (ev->type == UI_EVENT_MOUSE_SCROLL) {
        d->scroll_y += ev->mouse.scroll_dy * 3;
        /* Clamp will be applied in scroll_render */
        ui_widget_invalidate(w);
        return true;
    }
    return false;
}

static void scroll_destroy(ui_widget_t *w) {
    scroll_data_t *d = (scroll_data_t *)w->data;
    free(d);
}

ui_widget_t *ui_scroll_new(const char *id, ui_widget_t *child) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_SCROLL, id);
    if (!w) return NULL;
    scroll_data_t *d = calloc(1, sizeof(scroll_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    w->data = d;
    w->measure = scroll_measure;
    w->render = scroll_render;
    w->on_event = scroll_on_event;
    w->destroy = scroll_destroy;
    w->style->states[UI_STATE_NORMAL].bg = ui_rgb(20, 20, 20);
    if (child) ui_widget_add_child(w, child);
    return w;
}

void ui_scroll_set_scroll_y(ui_widget_t *w, int scroll_y) {
    if (!w || w->type != UI_WIDGET_SCROLL) return;
    scroll_data_t *d = (scroll_data_t *)w->data;
    if (d) d->scroll_y = scroll_y;
    ui_widget_invalidate(w);
}

void ui_scroll_scroll_to_bottom(ui_widget_t *w) {
    if (!w || w->type != UI_WIDGET_SCROLL) return;
    scroll_data_t *d = (scroll_data_t *)w->data;
    if (d) d->scroll_y = 999999;
    ui_widget_invalidate(w);
}

/* -------------------------------------------------------------------------- */
/* Split pane                                                                 */
/* -------------------------------------------------------------------------- */

typedef struct {
    ui_widget_t *a, *b;
    float ratio;
    int divider;
    bool dragging;
} split_data_t;

static ui_size_t split_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    split_data_t *d = (split_data_t *)w->data;
    int pw = 0, ph = 0;
    if (d && d->a) {
        pw = d->a->preferred_size.w;
        ph = d->a->preferred_size.h;
    }
    if (d && d->b) {
        pw += d->b->preferred_size.w;
        ph += d->b->preferred_size.h;
    }
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ pw + s->pad[1] + s->pad[3] + 1, ph + s->pad[0] + s->pad[2] };
}

static void split_arrange(ui_widget_t *w) {
    split_data_t *d = (split_data_t *)w->data;
    if (!d) return;
    ui_style_t *s = &w->style->states[w->state];
    int pad_l = s->pad[3], pad_t = s->pad[0];
    int inner_w = w->bounds.w - s->pad[1] - s->pad[3];
    int inner_h = w->bounds.h - s->pad[0] - s->pad[2];
    bool row = w->layout.direction == UI_DIR_ROW;
    if (row) {
        int split_pos = (int)(inner_w * d->ratio);
        if (d->a) {
            d->a->bounds = (ui_rect_t){ w->bounds.x + pad_l, w->bounds.y + pad_t, split_pos, inner_h };
        }
        if (d->b) {
            d->b->bounds = (ui_rect_t){ w->bounds.x + pad_l + split_pos + 1, w->bounds.y + pad_t, inner_w - split_pos - 1, inner_h };
        }
        d->divider = split_pos;
    } else {
        int split_pos = (int)(inner_h * d->ratio);
        if (d->a) {
            d->a->bounds = (ui_rect_t){ w->bounds.x + pad_l, w->bounds.y + pad_t, inner_w, split_pos };
        }
        if (d->b) {
            d->b->bounds = (ui_rect_t){ w->bounds.x + pad_l, w->bounds.y + pad_t + split_pos + 1, inner_w, inner_h - split_pos - 1 };
        }
        d->divider = split_pos;
    }
}

static void split_render(ui_widget_t *w, ui_canvas_t *c) {
    split_data_t *d = (split_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    ui_draw_rect(c, w->bounds, s->fg, s->bg, 0);
    split_arrange(w);
    if (d && d->a) { ui_widget_invalidate_all(d->a); ui_widget_render(d->a, c); }
    if (d && d->b) { ui_widget_invalidate_all(d->b); ui_widget_render(d->b, c); }
    /* Draw divider */
    bool row = w->layout.direction == UI_DIR_ROW;
    ui_color_t div_fg = ui_rgb(128, 128, 128);
    if (row) {
        int x = w->bounds.x + s->pad[3] + d->divider;
        for (int y = w->bounds.y + s->pad[0]; y < w->bounds.y + w->bounds.h - s->pad[2]; y++) {
            ui_canvas_set_cell(c, x, y, 0x2502, div_fg, s->bg, 0);
        }
    } else {
        int y = w->bounds.y + s->pad[0] + d->divider;
        for (int x = w->bounds.x + s->pad[3]; x < w->bounds.x + w->bounds.w - s->pad[1]; x++) {
            ui_canvas_set_cell(c, x, y, 0x2500, div_fg, s->bg, 0);
        }
    }
    ui_canvas_damage(c, w->bounds);
}

static bool split_on_event(ui_widget_t *w, const ui_event_t *ev) {
    split_data_t *d = (split_data_t *)w->data;
    if (!d) return false;
    bool row = w->layout.direction == UI_DIR_ROW;
    if (ev->type == UI_EVENT_MOUSE_PRESS) {
        int pos = row ? ev->mouse.x - w->bounds.x : ev->mouse.y - w->bounds.y;
        int div_pos = row ? d->divider + w->style->states[w->state].pad[3] : d->divider + w->style->states[w->state].pad[0];
        if (pos >= div_pos - 1 && pos <= div_pos + 1) {
            d->dragging = true;
            return true;
        }
    }
    if (ev->type == UI_EVENT_MOUSE_RELEASE) {
        d->dragging = false;
    }
    if (ev->type == UI_EVENT_MOUSE_MOVE && d->dragging) {
        int inner = row ? w->bounds.w - w->style->states[w->state].pad[1] - w->style->states[w->state].pad[3] : w->bounds.h - w->style->states[w->state].pad[0] - w->style->states[w->state].pad[2];
        int pos = row ? ev->mouse.x - w->bounds.x - w->style->states[w->state].pad[3] : ev->mouse.y - w->bounds.y - w->style->states[w->state].pad[0];
        if (inner > 0) {
            d->ratio = (float)pos / (float)inner;
            if (d->ratio < 0.1f) d->ratio = 0.1f;
            if (d->ratio > 0.9f) d->ratio = 0.9f;
        }
        w->dirty_layout = true;
        ui_widget_invalidate(w);
        return true;
    }
    return false;
}

static void split_destroy(ui_widget_t *w) {
    split_data_t *d = (split_data_t *)w->data;
    free(d);
}

ui_widget_t *ui_split_new(const char *id, ui_direction_t dir, ui_widget_t *a, ui_widget_t *b, float ratio) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_SPLIT, id);
    if (!w) return NULL;
    split_data_t *d = calloc(1, sizeof(split_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    d->a = a; d->b = b; d->ratio = ratio;
    w->layout.direction = dir;
    w->data = d;
    w->measure = split_measure;
    w->render = split_render;
    w->on_event = split_on_event;
    w->destroy = split_destroy;
    if (a) ui_widget_add_child(w, a);
    if (b) ui_widget_add_child(w, b);
    return w;
}

/* -------------------------------------------------------------------------- */
/* Tabs                                                                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    char **labels;
    ui_widget_t **panels;
    int count, cap;
    int active;
} tabs_data_t;

static ui_size_t tabs_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    tabs_data_t *d = (tabs_data_t *)w->data;
    int tw = 0, th = 1;
    if (d) {
        for (int i = 0; i < d->count; i++) {
            tw += ui_text_width(d->labels[i]) + 4;
        }
        if (d->active >= 0 && d->active < d->count && d->panels[d->active]) {
            ui_widget_t *p = d->panels[d->active];
            if (p->preferred_size.w > tw) tw = p->preferred_size.w;
            th += p->preferred_size.h;
        }
    }
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ tw + s->pad[1] + s->pad[3], th + s->pad[0] + s->pad[2] };
}

static void tabs_render(ui_widget_t *w, ui_canvas_t *c) {
    tabs_data_t *d = (tabs_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    ui_draw_rect(c, w->bounds, s->fg, s->bg, 0);
    if (!d) return;
    int x = w->bounds.x + s->pad[3];
    int y = w->bounds.y + s->pad[0];
    for (int i = 0; i < d->count; i++) {
        int lw = ui_text_width(d->labels[i]) + 4;
        ui_color_t fg = (i == d->active) ? ui_rgb(0, 0, 0) : s->fg;
        ui_color_t bg = (i == d->active) ? ui_rgb(200, 200, 200) : ui_rgb(60, 60, 60);
        ui_draw_rect(c, (ui_rect_t){ x, y, lw, 1 }, fg, bg, 0);
        ui_draw_text(c, x + 2, y, d->labels[i], fg, bg, 0);
        x += lw;
    }
    if (d->active >= 0 && d->active < d->count && d->panels[d->active]) {
        ui_widget_t *p = d->panels[d->active];
        p->bounds = (ui_rect_t){ w->bounds.x + s->pad[3], y + 1, w->bounds.w - s->pad[1] - s->pad[3], w->bounds.h - s->pad[0] - s->pad[2] - 1 };
        ui_widget_invalidate_all(p);
        ui_widget_render(p, c);
    }
}

static bool tabs_on_event(ui_widget_t *w, const ui_event_t *ev) {
    tabs_data_t *d = (tabs_data_t *)w->data;
    if (!d || ev->type != UI_EVENT_MOUSE_PRESS) return false;
    int x = w->bounds.x + w->style->states[w->state].pad[3];
    int mx = ev->mouse.x;
    for (int i = 0; i < d->count; i++) {
        int lw = ui_text_width(d->labels[i]) + 4;
        if (mx >= x && mx < x + lw) {
            d->active = i;
            ui_widget_invalidate(w);
            return true;
        }
        x += lw;
    }
    return false;
}

static void tabs_destroy(ui_widget_t *w) {
    tabs_data_t *d = (tabs_data_t *)w->data;
    if (d) {
        for (int i = 0; i < d->count; i++) free(d->labels[i]);
        free(d->labels);
        free(d->panels);
        free(d);
    }
}

ui_widget_t *ui_tabs_new(const char *id) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_TABS, id);
    if (!w) return NULL;
    tabs_data_t *d = calloc(1, sizeof(tabs_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    d->cap = 4;
    d->labels = calloc(d->cap, sizeof(char *));
    d->panels = calloc(d->cap, sizeof(ui_widget_t *));
    if (!d->labels || !d->panels) {
        free(d->labels); free(d->panels); free(d); ui_widget_destroy(w); return NULL;
    }
    w->data = d;
    w->measure = tabs_measure;
    w->render = tabs_render;
    w->on_event = tabs_on_event;
    w->destroy = tabs_destroy;
    return w;
}

void ui_tabs_add_tab(ui_widget_t *w, const char *label, ui_widget_t *panel) {
    if (!w || w->type != UI_WIDGET_TABS) return;
    tabs_data_t *d = (tabs_data_t *)w->data;
    if (!d) return;
    if (d->count >= d->cap) {
        d->cap *= 2;
        d->labels = realloc(d->labels, d->cap * sizeof(char *));
        d->panels = realloc(d->panels, d->cap * sizeof(ui_widget_t *));
    }
    d->labels[d->count] = strdup(label);
    /* Label strings are owned by the tabs widget and freed by tabs_destroy().
       GCC analyzer cannot track cross-function ownership; suppress false positive. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
    if (d->labels[d->count]) {
        d->panels[d->count] = panel;
        if (panel) ui_widget_add_child(w, panel);
        d->count++;
    }
#pragma GCC diagnostic pop
    if (d->active < 0) d->active = 0;
    w->dirty_layout = true;
}

/* -------------------------------------------------------------------------- */
/* Table                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    char **cells;
    int rows, cols;
    int *col_widths;
} table_data_t;

static ui_size_t table_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    table_data_t *d = (table_data_t *)w->data;
    int tw = 0, th = 0;
    if (d && d->col_widths) {
        for (int i = 0; i < d->cols; i++) tw += d->col_widths[i] + 1;
        th = d->rows;
    }
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ tw + s->pad[1] + s->pad[3], th + s->pad[0] + s->pad[2] };
}

static void table_render(ui_widget_t *w, ui_canvas_t *c) {
    table_data_t *d = (table_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    if (!d) return;
    int x0 = w->bounds.x + s->pad[3];
    int y0 = w->bounds.y + s->pad[0];
    for (int row = 0; row < d->rows && row < w->bounds.h - s->pad[0] - s->pad[2]; row++) {
        int x = x0;
        for (int col = 0; col < d->cols; col++) {
            char *cell = d->cells[row * d->cols + col];
            int cw = d->col_widths[col];
            ui_draw_rect(c, (ui_rect_t){ x, y0 + row, cw, 1 }, s->fg, s->bg, 0);
            if (cell) ui_draw_text(c, x, y0 + row, cell, s->fg, s->bg, 0);
            x += cw + 1;
            if (col < d->cols - 1) {
                ui_canvas_set_cell(c, x - 1, y0 + row, 0x2502, s->fg, s->bg, 0);
            }
        }
    }
    ui_canvas_damage(c, w->bounds);
}

static void table_destroy(ui_widget_t *w) {
    table_data_t *d = (table_data_t *)w->data;
    if (d) {
        for (int i = 0; i < d->rows * d->cols; i++) free(d->cells[i]);
        free(d->cells);
        free(d->col_widths);
        free(d);
    }
}

ui_widget_t *ui_table_new(const char *id, size_t cols) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_TABLE, id);
    if (!w) return NULL;
    table_data_t *d = calloc(1, sizeof(table_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    d->cols = (int)cols;
    d->col_widths = calloc(cols, sizeof(int));
    if (!d->col_widths) { free(d); ui_widget_destroy(w); return NULL; }
    for (size_t i = 0; i < cols; i++) d->col_widths[i] = 10;
    w->data = d;
    w->measure = table_measure;
    w->render = table_render;
    w->destroy = table_destroy;
    return w;
}

void ui_table_set_cell(ui_widget_t *w, int row, int col, const char *text) {
    if (!w || w->type != UI_WIDGET_TABLE || col < 0) return;
    table_data_t *d = (table_data_t *)w->data;
    if (!d || col >= d->cols) return;
    if (row >= d->rows) {
        int old_rows = d->rows;
        d->rows = row + 1;
        d->cells = realloc(d->cells, d->rows * d->cols * sizeof(char *));
        memset(d->cells + old_rows * d->cols, 0, (d->rows - old_rows) * d->cols * sizeof(char *));
    }
    int idx = row * d->cols + col;
    free(d->cells[idx]);
    /* Cell strings are owned by the table and freed by table_destroy().
       GCC analyzer cannot track cross-function ownership; suppress false positive. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
    d->cells[idx] = text ? strdup(text) : NULL;
#pragma GCC diagnostic pop
    ui_widget_invalidate(w);
}

/* -------------------------------------------------------------------------- */
/* Tree                                                                       */
/* -------------------------------------------------------------------------- */

typedef struct tree_node {
    char *label;
    bool expanded;
    struct tree_node **children;
    int child_count, child_cap;
} tree_node_t;

typedef struct {
    tree_node_t *root;
} tree_data_t;

static tree_node_t *tree_node_new(const char *label) __attribute__((noinline));
static tree_node_t *tree_node_new(const char *label) {
    tree_node_t *n = calloc(1, sizeof(tree_node_t));
    if (n && label) {
        /* Node label is owned by the tree and freed by tree_node_free().
           GCC analyzer cannot track cross-function ownership; suppress false positive. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
        n->label = strdup(label);
#pragma GCC diagnostic pop
    }
    return n;
}

static void tree_node_free(tree_node_t *n) {
    if (!n) return;
    for (int i = 0; i < n->child_count; i++) tree_node_free(n->children[i]);
    free(n->children);
    free(n->label);
    free(n);
}

static int tree_count_visible(tree_node_t *n, int depth) {
    if (!n) return 0;
    int count = 1;
    if (n->expanded) {
        for (int i = 0; i < n->child_count; i++) {
            count += tree_count_visible(n->children[i], depth + 1);
        }
    }
    return count;
}

static void tree_render_node(ui_canvas_t *c, tree_node_t *n, int x, int *y, int max_y, ui_color_t fg, ui_color_t bg, int depth) {
    if (!n || *y >= max_y) return;
    char buf[256];
    const char *prefix = n->child_count > 0 ? (n->expanded ? "[-] " : "[+] ") : "    ";
    int indent = depth * 2;
    snprintf(buf, sizeof(buf), "%*s%s%s", indent, "", prefix, n->label ? n->label : "");
    ui_draw_text(c, x, *y, buf, fg, bg, 0);
    (*y)++;
    if (n->expanded) {
        for (int i = 0; i < n->child_count; i++) {
            tree_render_node(c, n->children[i], x, y, max_y, fg, bg, depth + 1);
        }
    }
}

static ui_size_t tree_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    tree_data_t *d = (tree_data_t *)w->data;
    int rows = d && d->root ? tree_count_visible(d->root, 0) : 0;
    if (!w->style) return (ui_size_t){0, 0};
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ 20 + s->pad[1] + s->pad[3], rows + s->pad[0] + s->pad[2] };
}

static void tree_render(ui_widget_t *w, ui_canvas_t *c) {
    tree_data_t *d = (tree_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    ui_draw_rect(c, w->bounds, s->fg, s->bg, 0);
    if (!d || !d->root) return;
    int y = w->bounds.y + s->pad[0];
    tree_render_node(c, d->root, w->bounds.x + s->pad[3], &y, w->bounds.y + w->bounds.h - s->pad[2], s->fg, s->bg, 0);
}

static tree_node_t *tree_hit_node(tree_node_t *n, int *y, int target_y) {
    if (!n) return NULL;
    if (*y == target_y) return n;
    (*y)++;
    if (n->expanded) {
        for (int i = 0; i < n->child_count; i++) {
            tree_node_t *hit = tree_hit_node(n->children[i], y, target_y);
            if (hit) return hit;
        }
    }
    return NULL;
}

static bool tree_on_event(ui_widget_t *w, const ui_event_t *ev) {
    tree_data_t *d = (tree_data_t *)w->data;
    if (!d || !d->root || ev->type != UI_EVENT_MOUSE_PRESS) return false;
    int target_y = ev->mouse.y - w->bounds.y - w->style->states[w->state].pad[0];
    if (target_y < 0) return false;
    int y = 0;
    tree_node_t *hit = tree_hit_node(d->root, &y, target_y);
    if (hit && hit->child_count > 0) {
        hit->expanded = !hit->expanded;
        w->dirty_layout = true;
        ui_widget_invalidate(w);
        return true;
    }
    return false;
}

static void tree_destroy(ui_widget_t *w) {
    tree_data_t *d = (tree_data_t *)w->data;
    if (d) { tree_node_free(d->root); free(d); }
}

ui_widget_t *ui_tree_new(const char *id) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_TREE, id);
    if (!w) return NULL;
    tree_data_t *d = calloc(1, sizeof(tree_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    w->data = d;
    w->measure = tree_measure;
    w->render = tree_render;
    w->on_event = tree_on_event;
    w->destroy = tree_destroy;
    return w;
}

void ui_tree_add_node(ui_widget_t *w, const char *parent_label, const char *label) {
    if (!w || w->type != UI_WIDGET_TREE || !label) return;
    tree_data_t *d = (tree_data_t *)w->data;
    if (!d) return;
    tree_node_t *parent = NULL;
    if (!parent_label || !d->root) {
        if (!d->root) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
            d->root = tree_node_new(label);
            w->dirty_layout = true;
            return;
#pragma GCC diagnostic pop
        }
        parent = d->root;
    }
    if (!parent && parent_label) {
        /* Simple search: find node by label (first match) */
        /* BFS queue */
        tree_node_t **queue = malloc(256 * sizeof(tree_node_t *));
        if (!queue) return;
        int qhead = 0, qtail = 0;
        queue[qtail++] = d->root;
        while (qhead < qtail && !parent) {
            tree_node_t *n = queue[qhead++];
            if (n->label && strcmp(n->label, parent_label) == 0) { parent = n; break; }
            for (int i = 0; i < n->child_count; i++) {
                if (qtail < 256) queue[qtail++] = n->children[i];
            }
        }
        free(queue);
    }
    if (parent) {
        if (parent->child_count >= parent->child_cap) {
            parent->child_cap = parent->child_cap ? parent->child_cap * 2 : 4;
            parent->children = realloc(parent->children, parent->child_cap * sizeof(tree_node_t *));
        }
        /* Tree node labels are owned by the node and freed by tree_node_free().
           GCC analyzer cannot track cross-function ownership; suppress false positive. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
        tree_node_t *node = tree_node_new(label);
        if (node) parent->children[parent->child_count++] = node;
#pragma GCC diagnostic pop
        w->dirty_layout = true;
    }
}

/* -------------------------------------------------------------------------- */
/* Menubar                                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    char **labels;
    int count, cap;
    int active;
} menubar_data_t;

static ui_size_t menubar_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    menubar_data_t *d = (menubar_data_t *)w->data;
    int tw = 0;
    if (d) {
        for (int i = 0; i < d->count; i++) tw += ui_text_width(d->labels[i]) + 4;
    }
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ tw + s->pad[1] + s->pad[3], 1 + s->pad[0] + s->pad[2] };
}

static void menubar_render(ui_widget_t *w, ui_canvas_t *c) {
    menubar_data_t *d = (menubar_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    ui_draw_rect(c, w->bounds, s->fg, s->bg, 0);
    if (!d) return;
    int x = w->bounds.x + s->pad[3];
    int y = w->bounds.y + s->pad[0];
    for (int i = 0; i < d->count; i++) {
        int lw = ui_text_width(d->labels[i]) + 4;
        ui_color_t fg = (i == d->active) ? ui_rgb(0, 0, 0) : s->fg;
        ui_color_t bg = (i == d->active) ? ui_rgb(200, 200, 200) : s->bg;
        ui_draw_rect(c, (ui_rect_t){ x, y, lw, 1 }, fg, bg, 0);
        ui_draw_text(c, x + 2, y, d->labels[i], fg, bg, 0);
        x += lw;
    }
}

static bool menubar_on_event(ui_widget_t *w, const ui_event_t *ev) {
    menubar_data_t *d = (menubar_data_t *)w->data;
    if (!d) return false;
    if (ev->type == UI_EVENT_MOUSE_MOVE || ev->type == UI_EVENT_MOUSE_PRESS) {
        int mx = ev->mouse.x - w->bounds.x - w->style->states[w->state].pad[3];
        int x = 0;
        for (int i = 0; i < d->count; i++) {
            int lw = ui_text_width(d->labels[i]) + 4;
            if (mx >= x && mx < x + lw) {
                if (d->active != i) {
                    d->active = i;
                    ui_widget_invalidate(w);
                }
                return true;
            }
            x += lw;
        }
        if (d->active >= 0) {
            d->active = -1;
            ui_widget_invalidate(w);
        }
    }
    return false;
}

static void menubar_destroy(ui_widget_t *w) {
    menubar_data_t *d = (menubar_data_t *)w->data;
    if (d) {
        for (int i = 0; i < d->count; i++) free(d->labels[i]);
        free(d->labels);
        free(d);
    }
}

ui_widget_t *ui_menubar_new(const char *id) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_MENUBAR, id);
    if (!w) return NULL;
    menubar_data_t *d = calloc(1, sizeof(menubar_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    d->cap = 8;
    d->labels = calloc(d->cap, sizeof(char *));
    d->active = -1;
    w->data = d;
    w->measure = menubar_measure;
    w->render = menubar_render;
    w->on_event = menubar_on_event;
    w->destroy = menubar_destroy;
    w->layout = ui_layout_row();
    return w;
}

void ui_menubar_add_item(ui_widget_t *w, const char *label) {
    if (!w || w->type != UI_WIDGET_MENUBAR || !label) return;
    menubar_data_t *d = (menubar_data_t *)w->data;
    if (!d) return;
    if (d->count >= d->cap) {
        d->cap *= 2;
        d->labels = realloc(d->labels, d->cap * sizeof(char *));
    }
    d->labels[d->count++] = strdup(label);
    /* Label strings are owned by the menubar and freed by menubar_destroy().
       GCC analyzer cannot track cross-function ownership; suppress false positive. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
    w->dirty_layout = true;
#pragma GCC diagnostic pop
}

/* -------------------------------------------------------------------------- */
/* Statusbar                                                                  */
/* -------------------------------------------------------------------------- */

typedef struct {
    char *text;
} statusbar_data_t;

static ui_size_t statusbar_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    if (!w->style) return (ui_size_t){10, 1};
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ 10 + s->pad[1] + s->pad[3], 1 + s->pad[0] + s->pad[2] };
}

static void statusbar_render(ui_widget_t *w, ui_canvas_t *c) {
    if (!w->style) return;
    statusbar_data_t *d = (statusbar_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    ui_draw_rect(c, w->bounds, s->fg, s->bg, 0);
    if (d && d->text) {
        int x = w->bounds.x + s->pad[3];
        int y = w->bounds.y + s->pad[0];
        int max_w = w->bounds.w - s->pad[1] - s->pad[3];
        if (max_w < 1) return;
        ui_draw_text_clipped(c, x, y, d->text, s->fg, s->bg, 0, max_w);
    }
}

static void statusbar_destroy(ui_widget_t *w) {
    statusbar_data_t *d = (statusbar_data_t *)w->data;
    if (d) { free(d->text); free(d); }
}

ui_widget_t *ui_statusbar_new(const char *id) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_STATUSBAR, id);
    if (!w) return NULL;
    statusbar_data_t *d = calloc(1, sizeof(statusbar_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    w->data = d;
    w->measure = statusbar_measure;
    w->render = statusbar_render;
    w->destroy = statusbar_destroy;
    w->a11y_role = UI_ROLE_STATUS;
    for (int i = 0; i < UI_STATE_COUNT; i++) {
        w->style->states[i].bg = ui_rgb(40, 40, 40);
        w->style->states[i].fg = ui_rgb(200, 200, 200);
    }
    w->preferred_size = statusbar_measure(w, NULL);
    return w;
}

void ui_statusbar_set_text(ui_widget_t *w, const char *text) {
    if (!w || w->type != UI_WIDGET_STATUSBAR) return;
    statusbar_data_t *d = (statusbar_data_t *)w->data;
    if (!d) return;
    free(d->text);
    d->text = text ? strdup(text) : NULL;
    ui_widget_invalidate(w);
}

/* -------------------------------------------------------------------------- */
/* Textarea (multi-line input)                                                */
/* -------------------------------------------------------------------------- */

typedef struct {
    char *text;
    size_t text_cap;
    size_t text_len;
    int scroll_y;
} textarea_data_t;

static ui_size_t textarea_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    if (!w->style) return (ui_size_t){0, 0};
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ 20 + s->pad[1] + s->pad[3], 5 + s->pad[0] + s->pad[2] };
}

static void textarea_render(ui_widget_t *w, ui_canvas_t *c) {
    textarea_data_t *d = (textarea_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    ui_draw_box(c, w->bounds, s->fg, s->bg, false);
    int x = w->bounds.x + s->pad[3] + 1;
    int y = w->bounds.y + s->pad[0];
    int max_rows = w->bounds.h - s->pad[0] - s->pad[2];
    if (d && d->text) {
        const char *p = d->text;
        int row = 0;
        int skipped = 0;
        while (*p && row < max_rows) {
            if (skipped < d->scroll_y) {
                if (*p == '\n') { skipped++; }
                p++;
                continue;
            }
            const char *line_end = p;
            while (*line_end && *line_end != '\n' && (line_end - p) < (w->bounds.w - s->pad[1] - s->pad[3] - 2)) line_end++;
            int n = (int)(line_end - p);
            char buf[256];
            if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
            memcpy(buf, p, n);
            buf[n] = '\0';
            ui_draw_text(c, x, y + row, buf, s->fg, s->bg, 0);
            row++;
            p = line_end;
            if (*p == '\n') p++;
        }
    }
}

static bool textarea_on_event(ui_widget_t *w, const ui_event_t *ev) {
    textarea_data_t *d = (textarea_data_t *)w->data;
    if (!d || !w->enabled) return false;
    if (ev->type == UI_EVENT_KEY) {
        if (ev->key.key == UI_KEY_BACKSPACE) {
            if (d->text_len > 0) {
                d->text_len--;
                d->text[d->text_len] = '\0';
                ui_widget_invalidate(w);
            }
            return true;
        }
        if (ev->key.key == UI_KEY_ENTER) {
            if (d->text_len + 1 < d->text_cap) {
                d->text[d->text_len++] = '\n';
                d->text[d->text_len] = '\0';
                ui_widget_invalidate(w);
            }
            return true;
        }
        if (ev->key.codepoint > 0 && ev->key.codepoint < 0x110000 && ev->key.codepoint < 128) {
            if (d->text_len + 1 >= d->text_cap) {
                size_t new_cap = d->text_cap ? d->text_cap * 2 : 256;
                char *n = realloc(d->text, new_cap);
                if (!n) return true;
                d->text = n;
                d->text_cap = new_cap;
            }
            d->text[d->text_len++] = (char)ev->key.codepoint;
            d->text[d->text_len] = '\0';
            ui_widget_invalidate(w);
            return true;
        }
    }
    if (ev->type == UI_EVENT_MOUSE_SCROLL) {
        d->scroll_y += ev->mouse.scroll_dy;
        if (d->scroll_y < 0) d->scroll_y = 0;
        ui_widget_invalidate(w);
        return true;
    }
    return false;
}

static void textarea_destroy(ui_widget_t *w) {
    textarea_data_t *d = (textarea_data_t *)w->data;
    if (d) { free(d->text); free(d); }
}

ui_widget_t *ui_textarea_new(const char *id) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_TEXTAREA, id);
    if (!w) return NULL;
    textarea_data_t *d = calloc(1, sizeof(textarea_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    d->text_cap = 1024;
    d->text = malloc(d->text_cap);
    if (d->text) d->text[0] = '\0';
    w->data = d;
    w->measure = textarea_measure;
    w->render = textarea_render;
    w->on_event = textarea_on_event;
    w->destroy = textarea_destroy;
    w->style->states[UI_STATE_NORMAL].bg = ui_rgb(30, 30, 30);
    w->style->states[UI_STATE_FOCUSED].bg = ui_rgb(40, 40, 40);
    w->style->states[UI_STATE_FOCUSED].border = ui_rgb(100, 150, 255);
    return w;
}

void ui_textarea_set_text(ui_widget_t *w, const char *text) {
    if (!w || w->type != UI_WIDGET_TEXTAREA) return;
    textarea_data_t *d = (textarea_data_t *)w->data;
    if (!d) return;
    free(d->text);
    if (text) {
        d->text_len = strlen(text);
        d->text_cap = d->text_len + 1;
        d->text = malloc(d->text_cap);
        if (d->text) memcpy(d->text, text, d->text_len + 1);
    } else {
        d->text = malloc(1024);
        d->text_cap = 1024;
        d->text_len = 0;
        if (d->text) d->text[0] = '\0';
    }
    ui_widget_invalidate(w);
}

const char *ui_textarea_get_text(ui_widget_t *w) {
    if (!w || w->type != UI_WIDGET_TEXTAREA) return NULL;
    textarea_data_t *d = (textarea_data_t *)w->data;
    return d ? d->text : NULL;
}

/* -------------------------------------------------------------------------- */
/* Checkbox                                                                   */
/* -------------------------------------------------------------------------- */

typedef struct {
    char *label;
    bool checked;
    void (*on_change)(ui_widget_t *w, bool checked, void *user_data);
    void *user_data;
} checkbox_data_t;

static ui_size_t checkbox_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    checkbox_data_t *d = (checkbox_data_t *)w->data;
    int lw = d && d->label ? ui_text_width(d->label) : 0;
    int lh = d && d->label ? ui_text_height(d->label) : 1;
    if (!w->style) return (ui_size_t){0, 0};
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ lw + 4 + s->pad[1] + s->pad[3], lh + s->pad[0] + s->pad[2] };
}

static void checkbox_render(ui_widget_t *w, ui_canvas_t *c) {
    checkbox_data_t *d = (checkbox_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    int x = w->bounds.x + s->pad[3];
    int y = w->bounds.y + s->pad[0];
    char box[4] = { '[', d && d->checked ? 'x' : ' ', ']', '\0' };
    ui_draw_text(c, x, y, box, s->fg, s->bg, 0);
    if (d && d->label) {
        ui_draw_text(c, x + 4, y, d->label, s->fg, s->bg, 0);
    }
}

static bool checkbox_on_event(ui_widget_t *w, const ui_event_t *ev) {
    checkbox_data_t *d = (checkbox_data_t *)w->data;
    if (!d || !w->enabled) return false;
    if (ev->type == UI_EVENT_MOUSE_PRESS) {
        d->checked = !d->checked;
        ui_widget_invalidate(w);
        if (d->on_change) d->on_change(w, d->checked, d->user_data);
        return true;
    }
    if (ev->type == UI_EVENT_KEY && ev->key.key == UI_KEY_ENTER) {
        d->checked = !d->checked;
        ui_widget_invalidate(w);
        if (d->on_change) d->on_change(w, d->checked, d->user_data);
        return true;
    }
    return false;
}

static void checkbox_destroy(ui_widget_t *w) {
    checkbox_data_t *d = (checkbox_data_t *)w->data;
    if (d) { free(d->label); free(d); }
}

ui_widget_t *ui_checkbox_new(const char *id, const char *label, bool checked) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_CHECKBOX, id);
    if (!w) return NULL;
    checkbox_data_t *d = calloc(1, sizeof(checkbox_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    if (label) d->label = strdup(label);
    d->checked = checked;
    w->data = d;
    w->measure = checkbox_measure;
    w->render = checkbox_render;
    w->on_event = checkbox_on_event;
    w->destroy = checkbox_destroy;
    w->a11y_role = UI_ROLE_CHECKBOX;
    return w;
}

void ui_checkbox_set_checked(ui_widget_t *w, bool checked) {
    if (!w || w->type != UI_WIDGET_CHECKBOX) return;
    checkbox_data_t *d = (checkbox_data_t *)w->data;
    if (d && d->checked != checked) { d->checked = checked; ui_widget_invalidate(w); }
}

bool ui_checkbox_get_checked(ui_widget_t *w) {
    if (!w || w->type != UI_WIDGET_CHECKBOX) return false;
    checkbox_data_t *d = (checkbox_data_t *)w->data;
    return d ? d->checked : false;
}

/* -------------------------------------------------------------------------- */
/* Radio button                                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
    char *label;
    bool selected;
    void (*on_select)(ui_widget_t *w, void *user_data);
    void *user_data;
} radio_data_t;

static ui_size_t radio_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    radio_data_t *d = (radio_data_t *)w->data;
    int lw = d && d->label ? ui_text_width(d->label) : 0;
    int lh = d && d->label ? ui_text_height(d->label) : 1;
    if (!w->style) return (ui_size_t){0, 0};
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ lw + 4 + s->pad[1] + s->pad[3], lh + s->pad[0] + s->pad[2] };
}

static void radio_render(ui_widget_t *w, ui_canvas_t *c) {
    radio_data_t *d = (radio_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    int x = w->bounds.x + s->pad[3];
    int y = w->bounds.y + s->pad[0];
    char box[4] = { '(', d && d->selected ? '*' : ' ', ')', '\0' };
    ui_draw_text(c, x, y, box, s->fg, s->bg, 0);
    if (d && d->label) {
        ui_draw_text(c, x + 4, y, d->label, s->fg, s->bg, 0);
    }
}

static bool radio_on_event(ui_widget_t *w, const ui_event_t *ev) {
    radio_data_t *d = (radio_data_t *)w->data;
    if (!d || !w->enabled) return false;
    if (ev->type == UI_EVENT_MOUSE_PRESS && !d->selected) {
        d->selected = true;
        ui_widget_invalidate(w);
        if (d->on_select) d->on_select(w, d->user_data);
        return true;
    }
    return false;
}

static void radio_destroy(ui_widget_t *w) {
    radio_data_t *d = (radio_data_t *)w->data;
    if (d) { free(d->label); free(d); }
}

ui_widget_t *ui_radio_new(const char *id, const char *label) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_RADIO, id);
    if (!w) return NULL;
    radio_data_t *d = calloc(1, sizeof(radio_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    if (label) d->label = strdup(label);
    w->data = d;
    w->measure = radio_measure;
    w->render = radio_render;
    w->on_event = radio_on_event;
    w->destroy = radio_destroy;
    return w;
}

/* -------------------------------------------------------------------------- */
/* Select (dropdown)                                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    char **options;
    size_t option_count, option_cap;
    size_t selected_index;
    bool expanded;
} select_data_t;

static ui_size_t select_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    select_data_t *d = (select_data_t *)w->data;
    int max_w = 10;
    for (size_t i = 0; d && i < d->option_count; i++) {
        int ow = d->options[i] ? ui_text_width(d->options[i]) : 0;
        if (ow > max_w) max_w = ow;
    }
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ max_w + 4 + s->pad[1] + s->pad[3], 1 + s->pad[0] + s->pad[2] };
}

static void select_render(ui_widget_t *w, ui_canvas_t *c) {
    select_data_t *d = (select_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    const char *text = (d && d->selected_index < d->option_count && d->options[d->selected_index])
                       ? d->options[d->selected_index] : "---";
    int x = w->bounds.x + s->pad[3];
    int y = w->bounds.y + s->pad[0];
    ui_draw_rect(c, w->bounds, s->fg, s->bg, 0);
    ui_draw_text(c, x, y, text, s->fg, s->bg, 0);
    ui_draw_text(c, x + w->bounds.w - 2 - s->pad[1], y, d && d->expanded ? "^" : "v", s->fg, s->bg, 0);
}

static bool select_on_event(ui_widget_t *w, const ui_event_t *ev) {
    select_data_t *d = (select_data_t *)w->data;
    if (!d || !w->enabled) return false;
    if (ev->type == UI_EVENT_MOUSE_PRESS) {
        d->expanded = !d->expanded;
        ui_widget_invalidate(w);
        return true;
    }
    return false;
}

static void select_destroy(ui_widget_t *w) {
    select_data_t *d = (select_data_t *)w->data;
    if (d) {
        for (size_t i = 0; i < d->option_count; i++) free(d->options[i]);
        free(d->options);
        free(d);
    }
}

ui_widget_t *ui_select_new(const char *id) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_SELECT, id);
    if (!w) return NULL;
    select_data_t *d = calloc(1, sizeof(select_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    w->data = d;
    w->measure = select_measure;
    w->render = select_render;
    w->on_event = select_on_event;
    w->destroy = select_destroy;
    return w;
}

void ui_select_add_option(ui_widget_t *w, const char *option) {
    if (!w || w->type != UI_WIDGET_SELECT || !option) return;
    select_data_t *d = (select_data_t *)w->data;
    if (!d) return;
    if (d->option_count >= d->option_cap) {
        size_t nc = d->option_cap ? d->option_cap * 2 : 4;
        char **no = realloc(d->options, nc * sizeof(char *));
        if (!no) return;
        d->options = no;
        d->option_cap = nc;
    }
    d->options[d->option_count++] = strdup(option);
    /* Option strings are owned by the select widget and freed by select_destroy().
       GCC analyzer cannot track cross-function ownership; suppress false positive. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
    w->dirty_layout = true;
#pragma GCC diagnostic pop
}

/* -------------------------------------------------------------------------- */
/* Slider                                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    float min, max, value;
    void (*on_change)(ui_widget_t *w, float value, void *user_data);
    void *user_data;
} slider_data_t;

static ui_size_t slider_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    if (!w->style) return (ui_size_t){0, 0};
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ 20 + s->pad[1] + s->pad[3], 1 + s->pad[0] + s->pad[2] };
}

static void slider_render(ui_widget_t *w, ui_canvas_t *c) {
    slider_data_t *d = (slider_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    int x = w->bounds.x + s->pad[3];
    int y = w->bounds.y + s->pad[0];
    int bw = w->bounds.w - s->pad[1] - s->pad[3];
    ui_draw_line_h(c, x, y, bw, '-', s->fg, s->bg);
    if (d && bw > 0) {
        float t = (d->value - d->min) / (d->max - d->min);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        int tx = x + (int)(t * (bw - 1));
        ui_canvas_set_cell(c, tx, y, 'O', s->fg, s->bg, 0);
    }
}

static bool slider_on_event(ui_widget_t *w, const ui_event_t *ev) {
    slider_data_t *d = (slider_data_t *)w->data;
    if (!d || !w->enabled) return false;
    if (ev->type == UI_EVENT_MOUSE_PRESS || ev->type == UI_EVENT_MOUSE_MOVE) {
        ui_style_t *s = &w->style->states[w->state];
        int bw = w->bounds.w - s->pad[1] - s->pad[3];
        int x = w->bounds.x + s->pad[3];
        if (bw > 0) {
            float t;
            if (bw > 1) {
                t = (float)(ev->mouse.x - x) / (float)(bw - 1);
            } else {
                t = 0.5f;
            }
            if (t < 0) t = 0;
        if (t > 1) t = 1;
            float nv = d->min + t * (d->max - d->min);
            if (nv != d->value) {
                d->value = nv;
                ui_widget_invalidate(w);
                if (d->on_change) d->on_change(w, d->value, d->user_data);
            }
        }
        return true;
    }
    return false;
}

static void slider_destroy(ui_widget_t *w) {
    slider_data_t *d = (slider_data_t *)w->data;
    if (d) free(d);
}

ui_widget_t *ui_slider_new(const char *id, float min, float max, float value) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_SLIDER, id);
    if (!w) return NULL;
    slider_data_t *d = calloc(1, sizeof(slider_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    d->min = min; d->max = max; d->value = value;
    w->data = d;
    w->measure = slider_measure;
    w->render = slider_render;
    w->on_event = slider_on_event;
    w->destroy = slider_destroy;
    return w;
}

float ui_slider_get_value(ui_widget_t *w) {
    if (!w || w->type != UI_WIDGET_SLIDER) return 0.0f;
    slider_data_t *d = (slider_data_t *)w->data;
    return d ? d->value : 0.0f;
}

void ui_slider_set_value(ui_widget_t *w, float value) {
    if (!w || w->type != UI_WIDGET_SLIDER) return;
    slider_data_t *d = (slider_data_t *)w->data;
    if (d) { d->value = value; ui_widget_invalidate(w); }
}

/* -------------------------------------------------------------------------- */
/* Dialog                                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    char *title;
    ui_widget_t *content;
    ui_widget_t *buttons;
    bool modal;
} dialog_data_t;

static ui_size_t dialog_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    dialog_data_t *d = (dialog_data_t *)w->data;
    int tw = d && d->title ? ui_text_width(d->title) : 0;
    int th = d && d->title ? 1 : 0;
    if (!w->style) return (ui_size_t){0, 0};
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ tw + 4 + s->pad[1] + s->pad[3], th + 4 + s->pad[0] + s->pad[2] };
}

static void dialog_render(ui_widget_t *w, ui_canvas_t *c) {
    dialog_data_t *d = (dialog_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    ui_draw_box(c, w->bounds, s->fg, s->bg, true);
    if (d && d->title) {
        int x = w->bounds.x + s->pad[3] + 1;
        int y = w->bounds.y + s->pad[0];
        ui_draw_text(c, x, y, d->title, s->fg, s->bg, UI_ATTR_BOLD);
    }
}

static void dialog_destroy(ui_widget_t *w) {
    dialog_data_t *d = (dialog_data_t *)w->data;
    if (d) { free(d->title); free(d); }
}

ui_widget_t *ui_dialog_new(const char *id, const char *title) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_DIALOG, id);
    if (!w) return NULL;
    dialog_data_t *d = calloc(1, sizeof(dialog_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    if (title) d->title = strdup(title);
    d->modal = true;
    w->data = d;
    w->measure = dialog_measure;
    w->render = dialog_render;
    w->destroy = dialog_destroy;
    w->a11y_role = UI_ROLE_DIALOG;
    return w;
}

/* -------------------------------------------------------------------------- */
/* Tooltip                                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    char *text;
    uint64_t show_until_ns;
} tooltip_data_t;

static ui_size_t tooltip_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    tooltip_data_t *d = (tooltip_data_t *)w->data;
    int tw = d && d->text ? ui_text_width(d->text) : 0;
    int th = d && d->text ? 1 : 0;
    if (!w->style) return (ui_size_t){0, 0};
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ tw + 2 + s->pad[1] + s->pad[3], th + s->pad[0] + s->pad[2] };
}

static void tooltip_render(ui_widget_t *w, ui_canvas_t *c) {
    tooltip_data_t *d = (tooltip_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    ui_draw_rect(c, w->bounds, s->fg, s->bg, 0);
    if (d && d->text) {
        int x = w->bounds.x + s->pad[3];
        int y = w->bounds.y + s->pad[0];
        ui_draw_text(c, x, y, d->text, s->fg, s->bg, 0);
    }
}

static void tooltip_destroy(ui_widget_t *w) {
    tooltip_data_t *d = (tooltip_data_t *)w->data;
    if (d) { free(d->text); free(d); }
}

ui_widget_t *ui_tooltip_new(const char *id, const char *text) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_TOOLTIP, id);
    if (!w) return NULL;
    tooltip_data_t *d = calloc(1, sizeof(tooltip_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    if (text) d->text = strdup(text);
    w->data = d;
    w->measure = tooltip_measure;
    w->render = tooltip_render;
    w->destroy = tooltip_destroy;
    w->style->states[UI_STATE_NORMAL].bg = ui_rgb(50, 50, 50);
    w->style->states[UI_STATE_NORMAL].fg = ui_rgb(255, 255, 255);
    return w;
}

/* -------------------------------------------------------------------------- */
/* Progressbar                                                                */
/* -------------------------------------------------------------------------- */

typedef struct {
    float progress;
} progressbar_data_t;

static ui_size_t progressbar_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    if (!w->style) return (ui_size_t){0, 0};
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ 20 + s->pad[1] + s->pad[3], 1 + s->pad[0] + s->pad[2] };
}

static void progressbar_render(ui_widget_t *w, ui_canvas_t *c) {
    progressbar_data_t *d = (progressbar_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    int x = w->bounds.x + s->pad[3];
    int y = w->bounds.y + s->pad[0];
    int bw = w->bounds.w - s->pad[1] - s->pad[3];
    int filled = d ? (int)(d->progress * bw) : 0;
    if (filled < 0) filled = 0;
    if (filled > bw) filled = bw;
    for (int i = 0; i < bw; i++) {
        ui_color_t col = i < filled ? ui_rgb(100, 200, 100) : s->bg;
        ui_canvas_set_cell(c, x + i, y, i < filled ? '#' : '-', s->fg, col, 0);
    }
}

static void progressbar_destroy(ui_widget_t *w) {
    progressbar_data_t *d = (progressbar_data_t *)w->data;
    if (d) free(d);
}

ui_widget_t *ui_progressbar_new(const char *id) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_PROGRESSBAR, id);
    if (!w) return NULL;
    progressbar_data_t *d = calloc(1, sizeof(progressbar_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    w->data = d;
    w->measure = progressbar_measure;
    w->render = progressbar_render;
    w->destroy = progressbar_destroy;
    w->a11y_role = UI_ROLE_PROGRESSBAR;
    return w;
}

void ui_progressbar_set_progress(ui_widget_t *w, float progress) {
    if (!w || w->type != UI_WIDGET_PROGRESSBAR) return;
    progressbar_data_t *d = (progressbar_data_t *)w->data;
    if (d) { d->progress = progress; ui_widget_invalidate(w); }
}

/* -------------------------------------------------------------------------- */
/* Spinner                                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint64_t last_frame_ns;
    int frame;
} spinner_data_t;

static const char spinner_chars[] = "|/-\\";

static ui_size_t spinner_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    if (!w->style) return (ui_size_t){0, 0};
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ 1 + s->pad[1] + s->pad[3], 1 + s->pad[0] + s->pad[2] };
}

static void spinner_render(ui_widget_t *w, ui_canvas_t *c) {
    spinner_data_t *d = (spinner_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    int x = w->bounds.x + s->pad[3];
    int y = w->bounds.y + s->pad[0];
    char ch = d ? spinner_chars[d->frame % 4] : '|';
    ui_canvas_set_cell(c, x, y, (uint32_t)ch, s->fg, s->bg, 0);
}

static bool spinner_on_event(ui_widget_t *w, const ui_event_t *ev) {
    (void)w; (void)ev;
    return false;
}

static void spinner_destroy(ui_widget_t *w) {
    spinner_data_t *d = (spinner_data_t *)w->data;
    if (d) free(d);
}

ui_widget_t *ui_spinner_new(const char *id) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_SPINNER, id);
    if (!w) return NULL;
    spinner_data_t *d = calloc(1, sizeof(spinner_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    w->data = d;
    w->measure = spinner_measure;
    w->render = spinner_render;
    w->on_event = spinner_on_event;
    w->destroy = spinner_destroy;
    return w;
}

void ui_spinner_step(ui_widget_t *w) {
    if (!w || w->type != UI_WIDGET_SPINNER) return;
    spinner_data_t *d = (spinner_data_t *)w->data;
    if (d) { d->frame++; ui_widget_invalidate(w); }
}

/* -------------------------------------------------------------------------- */
/* Terminal (embedded terminal emulator — simplified)                         */
/* -------------------------------------------------------------------------- */

typedef struct {
    char *buffer;
    size_t buf_cap, buf_len;
    int scroll_y;
    int cols, rows;
} terminal_data_t;

static ui_size_t terminal_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    terminal_data_t *d = (terminal_data_t *)w->data;
    int cols = d ? d->cols : 80;
    int rows = d ? d->rows : 24;
    if (!w->style) return (ui_size_t){0, 0};
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ cols + s->pad[1] + s->pad[3], rows + s->pad[0] + s->pad[2] };
}

static void terminal_render(ui_widget_t *w, ui_canvas_t *c) {
    terminal_data_t *d = (terminal_data_t *)w->data;
    if (!w->style) return;
    ui_style_t *s = &w->style->states[w->state];
    ui_draw_rect(c, w->bounds, s->fg, s->bg, 0);
    if (!d || !d->buffer) return;
    int x = w->bounds.x + s->pad[3];
    int y = w->bounds.y + s->pad[0];
    int row = 0;
    const char *p = d->buffer;
    const char *end = d->buffer + d->buf_len;
    while (p < end && row < d->rows) {
        const char *line_end = p;
        while (line_end < end && *line_end != '\n') line_end++;
        size_t line_len = (size_t)(line_end - p);
        if (line_len > (size_t)d->cols) line_len = (size_t)d->cols;
        char buf[256];
        if (line_len >= sizeof(buf)) line_len = sizeof(buf) - 1;
        memcpy(buf, p, line_len);
        buf[line_len] = '\0';
        ui_draw_text(c, x, y + row, buf, s->fg, s->bg, 0);
        row++;
        p = line_end;
        if (p < end && *p == '\n') p++;
    }
}

static bool terminal_on_event(ui_widget_t *w, const ui_event_t *ev) {
    terminal_data_t *d = (terminal_data_t *)w->data;
    if (!d) return false;
    if (ev->type == UI_EVENT_MOUSE_SCROLL) {
        d->scroll_y += ev->mouse.scroll_dy;
        if (d->scroll_y < 0) d->scroll_y = 0;
        ui_widget_invalidate(w);
        return true;
    }
    return false;
}

static void terminal_destroy(ui_widget_t *w) {
    terminal_data_t *d = (terminal_data_t *)w->data;
    if (d) { free(d->buffer); free(d); }
}

ui_widget_t *ui_terminal_new(const char *id) {
    ui_widget_t *w = widget_alloc(UI_WIDGET_TERMINAL, id);
    if (!w) return NULL;
    terminal_data_t *d = calloc(1, sizeof(terminal_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    d->cols = 80;
    d->rows = 24;
    d->buf_cap = 4096;
    d->buffer = malloc(d->buf_cap);
    if (d->buffer) d->buffer[0] = '\0';
    w->data = d;
    w->measure = terminal_measure;
    w->render = terminal_render;
    w->on_event = terminal_on_event;
    w->destroy = terminal_destroy;
    w->style->states[UI_STATE_NORMAL].bg = ui_rgb(10, 10, 10);
    w->style->states[UI_STATE_NORMAL].fg = ui_rgb(200, 200, 200);
    return w;
}

void ui_terminal_write(ui_widget_t *w, const char *text) {
    if (!w || w->type != UI_WIDGET_TERMINAL || !text) return;
    terminal_data_t *d = (terminal_data_t *)w->data;
    if (!d) return;
    size_t len = strlen(text);
    if (d->buf_len + len + 1 > d->buf_cap) {
        size_t nc = d->buf_cap * 2;
        while (nc < d->buf_len + len + 1) nc *= 2;
        char *nb = realloc(d->buffer, nc);
        if (!nb) return;
        d->buffer = nb;
        d->buf_cap = nc;
    }
    memcpy(d->buffer + d->buf_len, text, len);
    d->buf_len += len;
    d->buffer[d->buf_len] = '\0';
    ui_widget_invalidate(w);
}

