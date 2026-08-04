/*
 * examples/game_demo/main.c — Full-stack validation demo
 *
 * Demonstrates:
 *   - GPU renderer (OpenGL sprite batching)
 *   - Physics (spatial grid, collision)
 *   - Input timestamping
 *   - Simulation (pixel world)
 *   - Frame pacing
 */

#include "forge/core.h"
#include "forge/math.h"
#include "forge/memory.h"
#include "forge/time.h"
#include "forge/log.h"
#include "forge/physics.h"
#include "forge/renderer_gl.h"
#include "forge/input.h"
#include "forge/simulation.h"
#include "forge/ui/ordl_ui.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#define WINDOW_W 1280
#define WINDOW_H 720
#define WORLD_W 320
#define WORLD_H 180
#define MAX_BODIES 256

typedef struct {
    ui_backend_t *backend;
    ui_app_t *app;
    fge_gl_renderer_t *glr;
    fge_phys_world_t phys;
    fge_sim_grid_t sim;
    fge_input_history_t input;
    fge_clock_t clock;
    fge_frame_time_t ft;

    /* Game state */
    fge_vec2_t player_pos;
    fge_vec2_t player_vel;
    float player_angle;
    uint32_t player_tex;
    uint32_t terrain_tex;

    /* Frame stats */
    uint64_t frame_count;
    double fps;
    double frame_ms;
} game_t;

static game_t g;

static void game_init(void) {
    fge_memzero(&g, sizeof(g));

    /* Backend + GL renderer */
    g.backend = ui_backend_gl_x11_new();
    if (!g.backend) {
        FGE_ERROR(FGE_LOG_CAT_GENERAL, "Failed to create GL X11 backend");
        return;
    }
    g.app = ui_app_new(g.backend, "FORGE Game Demo", WINDOW_W, WINDOW_H);
    if (!g.app) {
        FGE_ERROR(FGE_LOG_CAT_GENERAL, "Failed to create app");
        return;
    }

    /* GL renderer assumes context is current from backend init */
    g.glr = fge_gl_renderer_create(WINDOW_W, WINDOW_H);
    if (!g.glr) {
        FGE_ERROR(FGE_LOG_CAT_GENERAL, "Failed to create GL renderer");
        return;
    }

    /* Physics */
    if (!fge_phys_world_init(&g.phys)) {
        FGE_ERROR(FGE_LOG_CAT_GENERAL, "Failed to init physics");
        return;
    }
    g.phys.gravity = fge_v2(0.0f, 400.0f); /* pixels/s² */
    g.phys.fixed_dt = 1.0f / 120.0f;

    /* Create ground */
    fge_shape_t ground_shape = fge_shape_aabb(0, 0, WORLD_W, 20);
    fge_body_t *ground = fge_phys_create_body(&g.phys, FGE_BODY_STATIC, &ground_shape);
    if (ground) {
        ground->position = fge_v2(0, WORLD_H - 20);
        ground->friction = 0.8f;
    }

    /* Create some dynamic bodies */
    for (int i = 0; i < 32; i++) {
        fge_shape_t shape = fge_shape_circle(fge_v2(0, 0), 4.0f);
        fge_body_t *b = fge_phys_create_body(&g.phys, FGE_BODY_DYNAMIC, &shape);
        if (b) {
            b->position = fge_v2(40.0f + i * 16.0f, 20.0f);
            b->velocity = fge_v2(0, 0);
            b->restitution = 0.6f;
            b->friction = 0.3f;
            b->mass = 1.0f;
        }
    }

    /* Pixel simulation */
    if (!fge_sim_grid_init(&g.sim, WORLD_W, WORLD_H, 2)) {
        FGE_ERROR(FGE_LOG_CAT_GENERAL, "Failed to init sim");
        return;
    }

    /* Create textures */
    uint32_t white = 0xFFFFFFFF;
    g.player_tex = fge_gl_tex_create(g.glr, 1, 1, &white);

    /* Create terrain texture from sim (initially empty) */
    uint32_t *terrain_pixels = FGE_CALLOC(WORLD_W * WORLD_H, sizeof(uint32_t));
    g.terrain_tex = fge_gl_tex_create(g.glr, WORLD_W, WORLD_H, terrain_pixels);
    FGE_FREE(terrain_pixels);

    /* Player state */
    g.player_pos = fge_v2(WORLD_W / 2, 40);
    g.player_vel = fge_v2(0, 0);

    /* Timing */
    fge_clock_init(&g.clock);
    fge_frame_time_init(&g.ft, 144.0); /* 144 Hz target */

    FGE_INFO(FGE_LOG_CAT_GENERAL, "Game demo initialized");
}

static void game_shutdown(void) {
    if (g.glr) fge_gl_renderer_destroy(g.glr);
    fge_phys_world_free(&g.phys);
    fge_sim_grid_free(&g.sim);
    if (g.app) ui_app_free(g.app);
    if (g.backend) g.backend->shutdown(g.backend);
}

static void game_input(float dt) {
    /* Poll UI events */
    ui_event_t ev;
    float move_x = 0.0f, move_y = 0.0f;
    uint32_t buttons = 0;

    while (g.backend->poll_event(g.backend, &ev, 0)) {
        if (ev.type == UI_EVENT_QUIT) {
            ui_app_quit(g.app);
        }
        if (ev.type == UI_EVENT_KEY) {
            /* Use codepoint for WASD, key enum for special keys */
            uint32_t cp = ev.key.codepoint;
            if (cp >= 'a' && cp <= 'z') cp = cp - 'a' + 'A'; /* normalize */
            switch (cp) {
                case 'A': move_x = -1.0f; break;
                case 'D': move_x =  1.0f; break;
                case 'W': move_y = -1.0f; break;
                case 'S': move_y =  1.0f; break;
                default: break;
            }
            if (ev.key.key == UI_KEY_SPACE) buttons |= FGE_BTN_JUMP;
            if (ev.key.key == UI_KEY_LEFT)  move_x = -1.0f;
            if (ev.key.key == UI_KEY_RIGHT) move_x =  1.0f;
            if (ev.key.key == UI_KEY_UP)    move_y = -1.0f;
            if (ev.key.key == UI_KEY_DOWN)  move_y =  1.0f;
        }
    }

    /* Capture timestamped input */
    fge_input_capture(&g.input, move_x, move_y, 0, 0, buttons, 0, (uint16_t)(dt * 1000.0f));
}

static void game_update(float dt) {
    /* Apply latest input to player */
    const fge_input_frame_t *in = fge_input_latest(&g.input);
    if (in) {
        float accel = 800.0f;
        g.player_vel.x += in->move_x * accel * dt;
        g.player_vel.y += in->move_y * accel * dt;
        if (in->buttons & FGE_BTN_JUMP) {
            /* Jump if on ground */
            fge_vec2_t below = fge_v2_add(g.player_pos, fge_v2(0, 1));
            fge_body_t *hit = NULL;
            if (fge_phys_raycast(&g.phys, g.player_pos, fge_v2(0, 1), 8.0f, &hit, NULL, NULL)) {
                g.player_vel.y = -240.0f;
            }
        }
        /* Paint terrain on click */
        if (in->buttons & FGE_BTN_ATTACK) {
            int wx = (int)(g.player_pos.x + in->aim_x);
            int wy = (int)(g.player_pos.y + in->aim_y);
            fge_sim_grid_paint_circle(&g.sim, wx, wy, 4, FGE_MAT_SAND);
        }
    }

    /* Clamp player speed */
    float max_speed = 160.0f;
    float speed = fge_v2_len(g.player_vel);
    if (speed > max_speed) {
        g.player_vel = fge_v2_mulf(fge_v2_norm(g.player_vel), max_speed);
    }

    /* Move player with simple collision */
    g.player_pos = fge_v2_add(g.player_pos, fge_v2_mulf(g.player_vel, dt));

    /* Keep player in bounds */
    if (g.player_pos.x < 8) { g.player_pos.x = 8; g.player_vel.x = 0; }
    if (g.player_pos.x > WORLD_W - 8) { g.player_pos.x = WORLD_W - 8; g.player_vel.x = 0; }
    if (g.player_pos.y < 8) { g.player_pos.y = 8; g.player_vel.y = 0; }
    if (g.player_pos.y > WORLD_H - 8) { g.player_pos.y = WORLD_H - 8; g.player_vel.y = 0; }

    /* Physics */
    fge_phys_step(&g.phys, dt);

    /* Simulation (throttle to 60 Hz) */
    static double sim_accum = 0.0;
    sim_accum += dt;
    if (sim_accum >= 1.0 / 60.0) {
        fge_sim_step(&g.sim);
        sim_accum = 0.0;
    }

    /* Update terrain texture from sim */
    /* In a real game, we'd track dirty regions. For demo, full update. */
    /* (Omitted for brevity — would use fge_sim_render to a buffer, then fge_gl_tex_update) */
}

static void game_render(void) {
    fge_gl_renderer_begin(g.glr, 0.08f, 0.10f, 0.14f, 1.0f);

    /* Scale factor: world (320x180) -> window (1280x720) = 4x */
    float scale = (float)WINDOW_W / (float)WORLD_W;

    /* Draw terrain (sim) as background */
    /* fge_gl_draw_sprite(g.glr, g.terrain_tex, WORLD_W/2*scale, WORLD_H/2*scale,
                          WORLD_W*scale, WORLD_H*scale, 0xFFFFFFFF); */

    /* Draw physics bodies */
    for (uint32_t i = 0; i < g.phys.body_count; i++) {
        fge_body_t *b = &g.phys.bodies[i];
        if (b->type == FGE_BODY_STATIC) {
            fge_aabb_t aabb = fge_body_get_aabb(b);
            float cx = (aabb.min.x + aabb.max.x) * 0.5f * scale;
            float cy = (aabb.min.y + aabb.max.y) * 0.5f * scale;
            float w = (aabb.max.x - aabb.min.x) * scale;
            float h = (aabb.max.y - aabb.min.y) * scale;
            fge_gl_draw_sprite(g.glr, g.player_tex, cx, cy, w, h, 0xFF664422);
        } else {
            float cx = b->position.x * scale;
            float cy = b->position.y * scale;
            float r = 4.0f * scale;
            fge_gl_draw_sprite(g.glr, g.player_tex, cx, cy, r*2, r*2, 0xFF4488FF);
        }
    }

    /* Draw player */
    float px = g.player_pos.x * scale;
    float py = g.player_pos.y * scale;
    fge_gl_draw_sprite(g.glr, g.player_tex, px, py, 8*scale, 8*scale, 0xFF44FF44);

    fge_gl_renderer_end(g.glr);

    /* Swap buffers via backend present */
    if (g.backend && g.backend->present) {
        g.backend->present(g.backend);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    fge_log_init(FGE_LOG_LEVEL_INFO);

    game_init();
    if (!g.glr) {
        FGE_ERROR(FGE_LOG_CAT_GENERAL, "Init failed, exiting");
        game_shutdown();
        return 1;
    }

    FGE_INFO(FGE_LOG_CAT_GENERAL, "Entering main loop");

    while (g.app && g.app->running) {
        uint64_t frame_start = fge_clock_now(&g.clock);

        fge_frame_time_update(&g.ft, &g.clock);
        float dt = (float)g.ft.dt;
        if (dt > 0.1f) dt = 0.1f; /* clamp huge frames */

        game_input(dt);
        game_update(dt);
        game_render();

        /* Frame pacing */
        fge_frame_time_pace(&g.ft, &g.clock);

        g.frame_count++;
        uint64_t frame_end = fge_clock_now(&g.clock);
        g.frame_ms = (double)(frame_end - frame_start) * 1000.0 / (double)g.clock.freq;
        if (g.frame_ms > 0.0) g.fps = 1000.0 / g.frame_ms;

        if ((g.frame_count & 0xFF) == 0) {
            const fge_gl_stats_t *stats = fge_gl_renderer_stats(g.glr);
            FGE_INFO(FGE_LOG_CAT_GENERAL,
                     "FPS: %.1f  Frame: %.2fms  Sprites: %u  DrawCalls: %u  PhysBodies: %u",
                     g.fps, g.frame_ms, stats->sprites_batched, stats->draw_calls,
                     g.phys.body_count);
        }
    }

    game_shutdown();
    return 0;
}
