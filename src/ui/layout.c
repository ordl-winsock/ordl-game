/*
 * ORDL UI — Layout engine
 * Single-pass flexbox layout: measure bottom-up, arrange top-down.
 * No recursion — iterative with explicit stack.
 */

#include "forge/ui/ordl_ui.h"
#include "forge/ui/ordl_ui_debug.h"
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Measure — bottom-up, iterative                                             */
/* -------------------------------------------------------------------------- */

void ui_layout_measure(ui_widget_t *root, ui_canvas_t *c) {
    if (!root) return;

    /* Post-order traversal stack: push root, then process children first */
    ui_widget_t **stack = NULL;
    size_t stack_cap = 256, stack_len = 0;
    stack = malloc(stack_cap * sizeof(ui_widget_t *));
    if (!stack) return;

    ui_widget_t **post = NULL;
    size_t post_cap = 256, post_len = 0;
    post = malloc(post_cap * sizeof(ui_widget_t *));
    if (!post) { free(stack); return; }

    stack[stack_len++] = root;

    while (stack_len > 0) {
        ui_widget_t *w = stack[--stack_len];
        if (post_len >= post_cap) {
            post_cap *= 2;
            ui_widget_t **n = realloc(post, post_cap * sizeof(ui_widget_t *));
            if (!n) { free(stack); free(post); return; }
            post = n;
        }
        post[post_len++] = w;

        for (size_t i = 0; i < w->child_count; i++) {
            if (stack_len >= stack_cap) {
                stack_cap *= 2;
                ui_widget_t **n = realloc(stack, stack_cap * sizeof(ui_widget_t *));
                if (!n) { free(stack); free(post); return; }
                stack = n;
            }
            stack[stack_len++] = w->children[i];
        }
    }

    /* Process in reverse post-order (children before parents) */
    for (size_t i = post_len; i-- > 0; ) {
        ui_widget_t *w = post[i];
        if (w->measure) {
            w->preferred_size = w->measure(w, c);
        } else {
            /* Default: measure children and sum */
            int pw = 0, ph = 0;
            if (w->layout.direction == UI_DIR_ROW) {
                for (size_t j = 0; j < w->child_count; j++) {
                    ui_widget_t *ch = w->children[j];
                    pw += ch->preferred_size.w;
                    if (j > 0) pw += w->layout.gap;
                    if (ch->preferred_size.h > ph) ph = ch->preferred_size.h;
                    /* Saturate to prevent overflow */
                    if (pw > 32767) pw = 32767;
                    if (ph > 32767) ph = 32767;
                }
            } else {
                for (size_t j = 0; j < w->child_count; j++) {
                    ui_widget_t *ch = w->children[j];
                    ph += ch->preferred_size.h;
                    if (j > 0) ph += w->layout.gap;
                    if (ch->preferred_size.w > pw) pw = ch->preferred_size.w;
                    /* Saturate to prevent overflow */
                    if (pw > 32767) pw = 32767;
                    if (ph > 32767) ph = 32767;
                }
            }
            w->preferred_size.w = pw + w->layout.pad[1] + w->layout.pad[3];
            w->preferred_size.h = ph + w->layout.pad[0] + w->layout.pad[2];
        }
        /* Clamp to min/max */
        if (w->min_size.w > 0 && w->preferred_size.w < w->min_size.w)
            w->preferred_size.w = w->min_size.w;
        if (w->min_size.h > 0 && w->preferred_size.h < w->min_size.h)
            w->preferred_size.h = w->min_size.h;
        if (w->max_size.w > 0 && w->preferred_size.w > w->max_size.w)
            w->preferred_size.w = w->max_size.w;
        if (w->max_size.h > 0 && w->preferred_size.h > w->max_size.h)
            w->preferred_size.h = w->max_size.h;
        /* Account for margin in preferred size */
        if (w->style) {
            ui_style_t *st = &w->style->states[w->state];
            w->preferred_size.w += st->margin[1] + st->margin[3];
            w->preferred_size.h += st->margin[0] + st->margin[2];
        }
    }

    free(stack);
    free(post);
}

/* -------------------------------------------------------------------------- */
/* Arrange — top-down, iterative                                              */
/* -------------------------------------------------------------------------- */

void ui_layout_arrange(ui_widget_t *root, ui_rect_t available) {
    if (!root) return;

    typedef struct { ui_widget_t *w; ui_rect_t area; } item_t;

    item_t *stack = malloc(256 * sizeof(item_t));
    if (!stack) return;
    size_t stack_cap = 256, stack_len = 0;

    stack[stack_len++] = (item_t){ root, available };

    while (stack_len > 0) {
        item_t it = stack[--stack_len];
        ui_widget_t *w = it.w;
        ui_rect_t area = it.area;

        /* Apply widget's own margin */
        if (w->style) {
            area.x += w->style->states[w->state].margin[3];
            area.y += w->style->states[w->state].margin[0];
            area.w -= w->style->states[w->state].margin[1] + w->style->states[w->state].margin[3];
            area.h -= w->style->states[w->state].margin[0] + w->style->states[w->state].margin[2];
        }
        if (area.w < 0) area.w = 0;
        if (area.h < 0) area.h = 0;

        w->bounds = area;
        w->dirty_render = true;
        w->dirty_layout = false;

        /* Layout children */
        if (w->child_count == 0) continue;

        ui_layout_t *lo = &w->layout;
        int pad_w = lo->pad[1] + lo->pad[3];
        int pad_h = lo->pad[0] + lo->pad[2];
        int inner_w = area.w - pad_w;
        int inner_h = area.h - pad_h;

        /* Calculate total flex and space */
        int total_main = 0;
        int total_grow = 0;
        int total_shrink = 0;
        for (size_t i = 0; i < w->child_count; i++) {
            ui_widget_t *ch = w->children[i];
            int main = (lo->direction == UI_DIR_ROW) ? ch->preferred_size.w : ch->preferred_size.h;
            total_main += main;
            total_grow += ch->flex_grow;
            total_shrink += ch->flex_shrink;
        }
        int gap_total = (w->child_count > 1) ? (int)(w->child_count - 1) * lo->gap : 0;
        total_main += gap_total;

        int free_space = (lo->direction == UI_DIR_ROW) ? inner_w - total_main : inner_h - total_main;

        /* Distribute space */
        int *sizes = calloc(w->child_count, sizeof(int));
        if (!sizes) continue;

        for (size_t i = 0; i < w->child_count; i++) {
            ui_widget_t *ch = w->children[i];
            sizes[i] = (lo->direction == UI_DIR_ROW) ? ch->preferred_size.w : ch->preferred_size.h;
        }

        if (free_space > 0 && total_grow > 0) {
            int remainder = 0;
            for (size_t i = 0; i < w->child_count; i++) {
                ui_widget_t *ch = w->children[i];
                if (ch->flex_grow > 0) {
                    remainder += free_space * ch->flex_grow;
                    int add = remainder / total_grow;
                    remainder %= total_grow;
                    sizes[i] += add;
                }
            }
        } else if (free_space < 0 && total_shrink > 0) {
            int remainder = 0;
            for (size_t i = 0; i < w->child_count; i++) {
                ui_widget_t *ch = w->children[i];
                if (ch->flex_shrink > 0) {
                    remainder += free_space * ch->flex_shrink;
                    int sub = remainder / total_shrink;
                    remainder %= total_shrink;
                    sizes[i] += sub;
                }
            }
        }

        /* Ensure no negative sizes after distribution */
        for (size_t i = 0; i < w->child_count; i++) {
            if (sizes[i] < 0) sizes[i] = 0;
        }

        /* Position children */
        int pos = (lo->direction == UI_DIR_ROW) ? lo->pad[3] : lo->pad[0];
        int cross_size = (lo->direction == UI_DIR_ROW) ? inner_h : inner_w;
        if (cross_size < 0) cross_size = 0;

        for (size_t i = 0; i < w->child_count; i++) {
            ui_widget_t *ch = w->children[i];
            int cross = (lo->direction == UI_DIR_ROW) ? ch->preferred_size.h : ch->preferred_size.w;

            /* Cross-axis alignment */
            int cross_pos = 0;
            switch (lo->align_cross) {
                case UI_ALIGN_CENTER: cross_pos = (cross_size - cross) / 2; break;
                case UI_ALIGN_END:    cross_pos = cross_size - cross; break;
                case UI_ALIGN_STRETCH: cross = cross_size; break;
                default: break;
            }

            /* Clamp main-axis size to remaining space */
            int remaining = (lo->direction == UI_DIR_ROW)
                ? inner_w - (pos - lo->pad[3])
                : inner_h - (pos - lo->pad[0]);
            if (remaining < 0) remaining = 0;
            if (sizes[i] < 0) sizes[i] = 0;
            if (sizes[i] > remaining) sizes[i] = remaining;

            /* Clamp cross-axis size to parent's inner bounds */
            if (lo->direction == UI_DIR_ROW) {
                if (cross > inner_h) cross = inner_h;
                if (cross < 0) cross = 0;
            } else {
                if (cross > inner_w) cross = inner_w;
                if (cross < 0) cross = 0;
            }

            ui_rect_t child_area;
            if (lo->direction == UI_DIR_ROW) {
                child_area = (ui_rect_t){
                    area.x + pos, area.y + lo->pad[0] + cross_pos,
                    sizes[i], cross
                };
                pos += sizes[i] + lo->gap;
            } else {
                child_area = (ui_rect_t){
                    area.x + lo->pad[3] + cross_pos, area.y + pos,
                    cross, sizes[i]
                };
                pos += sizes[i] + lo->gap;
            }

            /* Clamp child area to parent's available space */
            if (child_area.x < area.x) child_area.x = area.x;
            if (child_area.y < area.y) child_area.y = area.y;
            if (child_area.x + child_area.w > area.x + area.w)
                child_area.w = area.x + area.w - child_area.x;
            if (child_area.y + child_area.h > area.y + area.h)
                child_area.h = area.y + area.h - child_area.y;
            if (child_area.w < 0) child_area.w = 0;
            if (child_area.h < 0) child_area.h = 0;

            if (stack_len >= stack_cap) {
                stack_cap *= 2;
                item_t *n = realloc(stack, stack_cap * sizeof(item_t));
                if (!n) { free(sizes); free(stack); return; }
                stack = n;
            }
            stack[stack_len++] = (item_t){ ch, child_area };
        }

        free(sizes);
    }

    free(stack);
}

void ui_layout_run(ui_widget_t *root, ui_canvas_t *c) {
    ui_debug_log("[layout_run] root=%s canvas=%dx%d", root ? root->id : "null", c ? c->w : 0, c ? c->h : 0);
    ui_layout_measure(root, c);
    if (root)
        ui_layout_arrange(root, (ui_rect_t){0, 0, c->w, c->h});
    ui_debug_log("[layout_run] done root_bounds=%d,%d %dx%d", root ? root->bounds.x : 0, root ? root->bounds.y : 0, root ? root->bounds.w : 0, root ? root->bounds.h : 0);
}
