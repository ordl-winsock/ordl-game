/*
 * ORDL UI — ORDL-specific widgets
 * Inference panel, mesh viz, crypto status, embedded terminal.
 * Pure C23, zero external dependencies.
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------- */
/* Inference panel — shows model, tokens/sec, status                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    char model[64];
    char status[64];
    float tps;
    int tokens_in, tokens_out;
} inference_panel_data_t;

static ui_size_t inference_panel_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ 30 + s->pad[1] + s->pad[3], 4 + s->pad[0] + s->pad[2] };
}

static void inference_panel_render(ui_widget_t *w, ui_canvas_t *c) {
    inference_panel_data_t *d = (inference_panel_data_t *)w->data;
    ui_style_t *s = &w->style->states[w->state];
    ui_draw_rect(c, w->bounds, s->fg, s->bg, 0);
    int x = w->bounds.x + s->pad[3];
    int y = w->bounds.y + s->pad[0];
    char line[128];
    snprintf(line, sizeof(line), "Model: %s", d ? d->model : "--");
    ui_draw_text(c, x, y, line, s->fg, s->bg, 0);
    snprintf(line, sizeof(line), "Status: %s", d ? d->status : "idle");
    ui_draw_text(c, x, y + 1, line, s->fg, s->bg, 0);
    snprintf(line, sizeof(line), "TPS: %.1f | In: %d Out: %d",
             d ? d->tps : 0.0f, d ? d->tokens_in : 0, d ? d->tokens_out : 0);
    ui_draw_text(c, x, y + 2, line, s->fg, s->bg, 0);
}

static void inference_panel_destroy(ui_widget_t *w) {
    if (w->data) free(w->data);
}

ui_widget_t *ui_inference_panel_new(const char *id) {
    ui_widget_t *w = ui_box_new(id);
    if (!w) return NULL;
    w->type = UI_WIDGET_CUSTOM;
    inference_panel_data_t *d = calloc(1, sizeof(inference_panel_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    w->data = d;
    w->measure = inference_panel_measure;
    w->render = inference_panel_render;
    w->destroy = inference_panel_destroy;
    w->style->states[UI_STATE_NORMAL].bg = ui_rgb(15, 15, 25);
    w->style->states[UI_STATE_NORMAL].fg = ui_rgb(180, 180, 220);
    return w;
}

/* -------------------------------------------------------------------------- */
/* Mesh viz — text display of mesh node count / state                         */
/* -------------------------------------------------------------------------- */

typedef struct {
    int node_count;
    int active_peers;
    char last_event[64];
} mesh_viz_data_t;

static ui_size_t mesh_viz_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ 24 + s->pad[1] + s->pad[3], 3 + s->pad[0] + s->pad[2] };
}

static void mesh_viz_render(ui_widget_t *w, ui_canvas_t *c) {
    mesh_viz_data_t *d = (mesh_viz_data_t *)w->data;
    ui_style_t *s = &w->style->states[w->state];
    ui_draw_rect(c, w->bounds, s->fg, s->bg, 0);
    int x = w->bounds.x + s->pad[3];
    int y = w->bounds.y + s->pad[0];
    char line[128];
    snprintf(line, sizeof(line), "Mesh: %d nodes | %d active",
             d ? d->node_count : 0, d ? d->active_peers : 0);
    ui_draw_text(c, x, y, line, s->fg, s->bg, 0);
    snprintf(line, sizeof(line), "Last: %s", d && d->last_event[0] ? d->last_event : "--");
    ui_draw_text(c, x, y + 1, line, s->fg, s->bg, 0);
}

static void mesh_viz_destroy(ui_widget_t *w) {
    if (w->data) free(w->data);
}

ui_widget_t *ui_mesh_viz_new(const char *id) {
    ui_widget_t *w = ui_box_new(id);
    if (!w) return NULL;
    w->type = UI_WIDGET_CUSTOM;
    mesh_viz_data_t *d = calloc(1, sizeof(mesh_viz_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    w->data = d;
    w->measure = mesh_viz_measure;
    w->render = mesh_viz_render;
    w->destroy = mesh_viz_destroy;
    w->style->states[UI_STATE_NORMAL].bg = ui_rgb(15, 25, 15);
    w->style->states[UI_STATE_NORMAL].fg = ui_rgb(180, 220, 180);
    return w;
}

/* -------------------------------------------------------------------------- */
/* Crypto status — TLS / cipher info                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    char tls_version[16];
    char cipher[32];
    bool secure;
} crypto_status_data_t;

static ui_size_t crypto_status_measure(ui_widget_t *w, ui_canvas_t *c) {
    (void)c;
    ui_style_t *s = &w->style->states[w->state];
    return (ui_size_t){ 28 + s->pad[1] + s->pad[3], 2 + s->pad[0] + s->pad[2] };
}

static void crypto_status_render(ui_widget_t *w, ui_canvas_t *c) {
    crypto_status_data_t *d = (crypto_status_data_t *)w->data;
    ui_style_t *s = &w->style->states[w->state];
    ui_draw_rect(c, w->bounds, s->fg, s->bg, 0);
    int x = w->bounds.x + s->pad[3];
    int y = w->bounds.y + s->pad[0];
    char line[128];
    ui_color_t col = (d && d->secure) ? ui_rgb(100, 255, 100) : ui_rgb(255, 100, 100);
    snprintf(line, sizeof(line), "[%s] %s",
             d && d->tls_version[0] ? d->tls_version : "--",
             d && d->cipher[0] ? d->cipher : "--");
    ui_draw_text(c, x, y, line, col, s->bg, 0);
}

static void crypto_status_destroy(ui_widget_t *w) {
    if (w->data) free(w->data);
}

ui_widget_t *ui_crypto_status_new(const char *id) {
    ui_widget_t *w = ui_box_new(id);
    if (!w) return NULL;
    w->type = UI_WIDGET_CUSTOM;
    crypto_status_data_t *d = calloc(1, sizeof(crypto_status_data_t));
    if (!d) { ui_widget_destroy(w); return NULL; }
    w->data = d;
    w->measure = crypto_status_measure;
    w->render = crypto_status_render;
    w->destroy = crypto_status_destroy;
    w->style->states[UI_STATE_NORMAL].bg = ui_rgb(25, 15, 15);
    w->style->states[UI_STATE_NORMAL].fg = ui_rgb(220, 180, 180);
    return w;
}

/* -------------------------------------------------------------------------- */
/* Embedded terminal — ORDL-styled wrapper around terminal widget             */
/* -------------------------------------------------------------------------- */

ui_widget_t *ui_terminal_embed_new(const char *id) {
    ui_widget_t *w = ui_terminal_new(id);
    if (!w) return NULL;
    w->style->states[UI_STATE_NORMAL].bg = ui_rgb(5, 5, 5);
    w->style->states[UI_STATE_NORMAL].fg = ui_rgb(200, 200, 200);
    return w;
}
