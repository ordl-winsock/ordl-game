/*
 * examples/sandbox_demo/main.c — Falling Sand / Pixel Simulation Demo
 *
 * Interactive cellular automata sandbox inspired by Noita.
 *
 * RESOLUTION: Grid cells are 2×2 display pixels (chunky, visible).
 * Each cell contains 2×2 sub-cells rendered as individual pixels,
 * so fine detail (outlines, edges, text) is visible at the pixel level
 * while physics operates on the larger grid cells.
 *
 * Controls:
 *   Left click  (hold)  — paint material
 *   Right click (hold)  — erase (empty)
 *   1-9,0,P,S,M,C,D,N  — select material
 *   Scroll               — change brush size
 *   Space                — pause/unpause simulation
 *   R                    — reset grid
 *   ESC                  — exit
 */

#include "forge/platform.h"
#include "forge/renderer.h"
#include "forge/log.h"
#include "forge/simulation.h"
#include <stdio.h>
#include <string.h>

/* Display resolution */
#define DISP_W 800
#define DISP_H 600

/* Grid: each cell is 2×2 display pixels */
#define GRID_W 400
#define GRID_H 300
#define SUB_SCALE 2

/* Render buffer at full sub-cell resolution (= display resolution) */
#define RBUF_W (GRID_W * SUB_SCALE)
#define RBUF_H (GRID_H * SUB_SCALE)

typedef struct {
    fge_platform_t *platform;
    fge_renderer_t renderer;
    fge_sim_grid_t grid;

    fge_material_t brush_material;
    int brush_size;
    bool paused;

    uint32_t render_buf[RBUF_W * RBUF_H];
    uint32_t frame_count;
    double total_time;
} demo_state_t;

static const struct {
    fge_material_t mat;
    const char *name;
    fge_key_t key;
} BRUSHES[] = {
    { FGE_MAT_SAND,      "Sand",      FGE_KEY_1 },
    { FGE_MAT_WATER,     "Water",     FGE_KEY_2 },
    { FGE_MAT_STONE,     "Stone",     FGE_KEY_3 },
    { FGE_MAT_WOOD,      "Wood",      FGE_KEY_4 },
    { FGE_MAT_FIRE,      "Fire",      FGE_KEY_5 },
    { FGE_MAT_OIL,       "Oil",       FGE_KEY_6 },
    { FGE_MAT_LAVA,      "Lava",      FGE_KEY_7 },
    { FGE_MAT_ACID,      "Acid",      FGE_KEY_8 },
    { FGE_MAT_GUNPOWDER, "Gunpowder", FGE_KEY_9 },
    { FGE_MAT_ICE,       "Ice",       FGE_KEY_0 },
    { FGE_MAT_PLANT,     "Plant",     FGE_KEY_P },
    { FGE_MAT_SMOKE,     "Smoke",     FGE_KEY_S },
    { FGE_MAT_METAL,     "Metal",     FGE_KEY_M },
    { FGE_MAT_COAL,      "Coal",      FGE_KEY_C },
    { FGE_MAT_DIRT,      "Dirt",      FGE_KEY_D },
    { FGE_MAT_SNOW,      "Snow",      FGE_KEY_N },
};
#define BRUSH_COUNT (sizeof(BRUSHES) / sizeof(BRUSHES[0]))

static void reset_grid(demo_state_t *st) {
    fge_sim_grid_clear(&st->grid);
    for (int x = 0; x < GRID_W; x++) {
        fge_sim_grid_set(&st->grid, x, GRID_H - 1, FGE_MAT_STONE);
        fge_sim_grid_set(&st->grid, x, GRID_H - 2, FGE_MAT_STONE);
    }
    for (int y = 0; y < GRID_H; y++) {
        fge_sim_grid_set(&st->grid, 0, y, FGE_MAT_STONE);
        fge_sim_grid_set(&st->grid, GRID_W - 1, y, FGE_MAT_STONE);
    }
}

static void paint_at(demo_state_t *st, int mx, int my) {
    int cell_x = mx * GRID_W / DISP_W;
    int cell_y = my * GRID_H / DISP_H;
    if (cell_x < 1 || cell_x >= GRID_W - 1 || cell_y < 1 || cell_y >= GRID_H - 1) return;

    bool erasing = fge_input_mouse_down(&st->platform->input, FGE_MOUSE_RIGHT);
    fge_material_t mat = erasing ? FGE_MAT_EMPTY : st->brush_material;
    int r = st->brush_size;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                fge_sim_grid_set(&st->grid, cell_x + dx, cell_y + dy, mat);
            }
        }
    }
}

static void on_frame(fge_platform_t *p, double dt) {
    demo_state_t *st = (demo_state_t *)p->user_data;
    st->frame_count++;
    st->total_time += dt;

    /* Paint while holding mouse button */
    if (fge_input_mouse_down(&p->input, FGE_MOUSE_LEFT) ||
        fge_input_mouse_down(&p->input, FGE_MOUSE_RIGHT)) {
        paint_at(st, (int)p->input.mouse_pos.x, (int)p->input.mouse_pos.y);
    }

    /* Step simulation */
    if (!st->paused) {
        fge_sim_step(&st->grid);
    }

    /* Render at full sub-cell resolution (800×600) */
    fge_sim_render(&st->grid, st->render_buf, RBUF_W, RBUF_H);

    /* Copy directly to framebuffer (1:1) */
    fge_framebuffer_t *fb = fge_renderer_fb(&st->renderer);
    memcpy(fb->pixels, st->render_buf,
           (size_t)fb->width * fb->height * sizeof(uint32_t));

    if (st->frame_count % 60 == 0) {
        const char *mat_name = fge_sim_material_props(st->brush_material)->name;
        printf("Frame %u | fps=%.1f | mat=%s | brush=%d | %s\n",
               st->frame_count,
               st->total_time > 0 ? (double)st->frame_count / st->total_time : 0.0,
               mat_name, st->brush_size * 2 + 1,
               st->paused ? "PAUSED" : "running");
        fflush(stdout);
    }
}

static void on_event(fge_platform_t *p, const fge_event_t *ev) {
    demo_state_t *st = (demo_state_t *)p->user_data;

    switch (ev->type) {
    case FGE_EVENT_KEY_DOWN:
        if (ev->key.key == FGE_KEY_ESCAPE) p->running = false;
        if (ev->key.key == FGE_KEY_SPACE) st->paused = !st->paused;
        if (ev->key.key == FGE_KEY_R) reset_grid(st);
        for (size_t i = 0; i < BRUSH_COUNT; i++) {
            if (ev->key.key == BRUSHES[i].key) {
                st->brush_material = BRUSHES[i].mat;
                printf("Selected: %s\n", BRUSHES[i].name);
                fflush(stdout);
            }
        }
        break;
    case FGE_EVENT_MOUSE_SCROLL:
        st->brush_size += ev->mouse_scroll.scroll.y > 0.0f ? 1 : -1;
        if (st->brush_size < 0) st->brush_size = 0;
        if (st->brush_size > 15) st->brush_size = 15;
        break;
    case FGE_EVENT_CLOSE:
        p->running = false;
        break;
    default:
        break;
    }
}

int main(void) {
    printf("============================================\n");
    printf("  FORGE Pixel Simulation Sandbox\n");
    printf("  Grid: %dx%d cells (2x2 display pixels each)\n", GRID_W, GRID_H);
    printf("  Sub-cells: %dx%d per cell (1 display pixel each)\n", SUB_SCALE, SUB_SCALE);
    printf("  Effective resolution: %dx%d\n", RBUF_W, RBUF_H);
    printf("  Left click (hold): paint  | Right click (hold): erase\n");
    printf("  1-9,0,P,S,M,C,D,N: materials\n");
    printf("  Scroll: brush size | Space: pause | R: reset | ESC: exit\n");
    printf("============================================\n\n");

    fge_log_init(FGE_LOG_LEVEL_INFO);
    fge_log_add_sink_stdout(FGE_LOG_LEVEL_INFO);

    fge_platform_t *platform = fge_platform_create("FORGE Sandbox", DISP_W, DISP_H, false);
    if (!platform) {
        fprintf(stderr, "ERROR: Failed to create platform window.\n");
        return 1;
    }

    demo_state_t state = {0};
    state.platform = platform;
    state.brush_material = FGE_MAT_SAND;
    state.brush_size = 2;

    if (!fge_renderer_init(&state.renderer, DISP_W, DISP_H)) {
        fprintf(stderr, "ERROR: Failed to initialize renderer.\n");
        fge_platform_destroy(platform);
        return 1;
    }

    if (!fge_sim_grid_init(&state.grid, GRID_W, GRID_H, SUB_SCALE)) {
        fprintf(stderr, "ERROR: Failed to initialize simulation grid.\n");
        return 1;
    }

    reset_grid(&state);

    platform->framebuffer = fge_renderer_fb(&state.renderer);
    platform->user_data = &state;
    platform->on_frame = on_frame;
    platform->on_event = on_event;

    printf("Window: %dx%d | Grid: %dx%d | Sub-scale: %d | Render: %dx%d\n",
           DISP_W, DISP_H, GRID_W, GRID_H, SUB_SCALE, RBUF_W, RBUF_H);
    printf("Starting sandbox...\n\n");

    fge_platform_run(platform);

    printf("\nSandbox finished. Total frames: %u\n", state.frame_count);

    fge_sim_grid_free(&state.grid);
    fge_renderer_shutdown(&state.renderer);
    fge_platform_destroy(platform);
    fge_log_shutdown();

    return 0;
}
