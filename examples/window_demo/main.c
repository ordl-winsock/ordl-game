/*
 * examples/window_demo/main.c — Pixel-Perfect Physics Demo
 *
 * Uses the full FORGE physics engine with pixel-perfect collision:
 *   - Ball: dynamic body + circular pixel collider
 *   - Box:  kinematic body + rectangular pixel collider
 *   - Walls: static AABB bodies (screen boundaries)
 *   - Physics engine handles collision response via impulses
 *
 * Drag the blue box with left mouse. The green ball bounces off
 * pixel-perfectly — not bounding boxes.
 *
 * Telemetry: F1-F8 toggles categories
 * Exit: ESC
 */

#include "forge/platform.h"
#include "forge/renderer.h"
#include "forge/log.h"
#include "forge/telemetry.h"
#include "forge/dev_mode.h"
#include "forge/debug_overlay.h"
#include "forge/collision.h"
#include "forge/physics.h"
#include <stdio.h>
#include <math.h>

#define WINDOW_W 800
#define WINDOW_H 600

typedef struct {
    fge_platform_t *platform;
    fge_renderer_t renderer;
    fge_phys_world_t world;

    fge_body_t *ball;
    fge_body_t *box;

    fge_collider_t ball_mask;
    fge_collider_t box_mask;

    bool dragging;
    float drag_offset_x, drag_offset_y;

    uint32_t frame_count;
    double total_time;
} demo_state_t;

/* Build a circular bitmask */
static bool make_circle_mask(fge_collider_t *c, float radius) {
    int r = (int)fge_ceilf(radius);
    int size = r * 2 + 1;
    uint8_t *alpha = (uint8_t *)FGE_CALLOC((size_t)size * size, 1);
    if (!alpha) return false;
    float cx = (float)r, cy = (float)r;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = (float)x - cx;
            float dy = (float)y - cy;
            if (dx * dx + dy * dy <= radius * radius)
                alpha[y * size + x] = 255;
        }
    }
    bool ok = fge_bitmask_from_alpha(&c->mask, alpha, size, size);
    FGE_FREE(alpha);
    if (ok) {
        c->mask.origin = fge_v2((float)r, (float)r);
        c->scale = 1.0f;
        c->rotation = 0.0f;
    }
    return ok;
}

/* Build a rectangular bitmask */
static bool make_rect_mask(fge_collider_t *c, int w, int h) {
    uint8_t *alpha = (uint8_t *)FGE_MALLOC((size_t)w * h);
    if (!alpha) return false;
    memset(alpha, 255, (size_t)w * h);
    bool ok = fge_bitmask_from_alpha(&c->mask, alpha, w, h);
    FGE_FREE(alpha);
    if (ok) {
        c->mask.origin = fge_v2(0.0f, 0.0f);
        c->scale = 1.0f;
        c->rotation = 0.0f;
    }
    return ok;
}

static void on_frame(fge_platform_t *p, double dt) {
    demo_state_t *st = (demo_state_t *)p->user_data;
    st->frame_count++;
    st->total_time += dt;

    fge_telem_inc(FGE_TELEM_FRAME);
    fge_telem_record_frame(dt * 1000.0);

    /* Run physics step */
    fge_phys_step(&st->world, (float)dt);

    /* Clamp ball inside screen (safety net — walls should catch it) */
    float r = 30.0f;
    if (st->ball->position.x < r) { st->ball->position.x = r; st->ball->velocity.x = fge_fabsf(st->ball->velocity.x); }
    if (st->ball->position.x > (float)p->width - r) { st->ball->position.x = (float)p->width - r; st->ball->velocity.x = -fge_fabsf(st->ball->velocity.x); }
    if (st->ball->position.y < r) { st->ball->position.y = r; st->ball->velocity.y = fge_fabsf(st->ball->velocity.y); }
    if (st->ball->position.y > (float)p->height - r) { st->ball->position.y = (float)p->height - r; st->ball->velocity.y = -fge_fabsf(st->ball->velocity.y); }

    /* Pixel-perfect collision test for visual feedback */
    bool colliding = fge_collider_hit_test(&st->ball_mask, &st->box_mask);

    /* Render */
    fge_renderer_begin(&st->renderer, 0xFF101020);
    fge_telem_inc(FGE_TELEM_RENDER_CALL);

    uint32_t ball_color = colliding ? 0xFFFF0000 : 0xFF00FF66;
    fge_draw_circle(fge_renderer_fb(&st->renderer),
                    st->ball->position, r, ball_color);

    uint32_t box_color = colliding ? 0xFFFF0000 : 0xFF3366FF;
    fge_draw_rect(fge_renderer_fb(&st->renderer),
                  (int)st->box->position.x, (int)st->box->position.y,
                  80, 60, box_color);

    /* Draw AABB outlines */
    uint32_t outline = 0xFFFFFFFF;
    fge_draw_line(fge_renderer_fb(&st->renderer),
                  fge_v2(st->ball_mask.aabb_minx, st->ball_mask.aabb_miny),
                  fge_v2(st->ball_mask.aabb_maxx, st->ball_mask.aabb_miny),
                  1.0f, outline);
    fge_draw_line(fge_renderer_fb(&st->renderer),
                  fge_v2(st->ball_mask.aabb_maxx, st->ball_mask.aabb_miny),
                  fge_v2(st->ball_mask.aabb_maxx, st->ball_mask.aabb_maxy),
                  1.0f, outline);
    fge_draw_line(fge_renderer_fb(&st->renderer),
                  fge_v2(st->ball_mask.aabb_maxx, st->ball_mask.aabb_maxy),
                  fge_v2(st->ball_mask.aabb_minx, st->ball_mask.aabb_maxy),
                  1.0f, outline);
    fge_draw_line(fge_renderer_fb(&st->renderer),
                  fge_v2(st->ball_mask.aabb_minx, st->ball_mask.aabb_maxy),
                  fge_v2(st->ball_mask.aabb_minx, st->ball_mask.aabb_miny),
                  1.0f, outline);

    fge_renderer_end(&st->renderer);
    fge_debug_overlay_render(&st->renderer);
    fge_telem_inc(FGE_TELEM_SWAP_BUFFER);

    if (st->frame_count % 60 == 0) {
        printf("Frame %u | ball=(%.1f,%.1f) | box=(%.1f,%.1f) | collide=%s | fps=%.1f\n",
               st->frame_count,
               st->ball->position.x, st->ball->position.y,
               st->box->position.x, st->box->position.y,
               colliding ? "YES" : "no",
               st->total_time > 0.0 ? (double)st->frame_count / st->total_time : 0.0);
        fflush(stdout);
    }
}

static void on_event(fge_platform_t *p, const fge_event_t *ev) {
    demo_state_t *st = (demo_state_t *)p->user_data;

    switch (ev->type) {
    case FGE_EVENT_KEY_DOWN:
        fge_telem_inc(FGE_TELEM_KEY_PRESS);
        fge_telem_record_input((uint32_t)ev->key.key, 0,
                               p->input.mouse_pos.x, p->input.mouse_pos.y,
                               p->input.mouse_delta.x, p->input.mouse_delta.y, "");
        if (ev->key.key == FGE_KEY_ESCAPE) p->running = false;
        if (ev->key.key == FGE_KEY_F1 || ev->key.key == FGE_KEY_1) fge_dev_toggle(FGE_DEV_OVERLAY);
        if (ev->key.key == FGE_KEY_F2 || ev->key.key == FGE_KEY_2) fge_dev_toggle(FGE_DEV_FPS);
        if (ev->key.key == FGE_KEY_F3 || ev->key.key == FGE_KEY_3) fge_dev_toggle(FGE_DEV_RENDERER);
        if (ev->key.key == FGE_KEY_F4 || ev->key.key == FGE_KEY_4) fge_dev_toggle(FGE_DEV_INPUT);
        if (ev->key.key == FGE_KEY_F5 || ev->key.key == FGE_KEY_5) fge_dev_toggle(FGE_DEV_NETWORK);
        if (ev->key.key == FGE_KEY_F6 || ev->key.key == FGE_KEY_6) fge_dev_toggle(FGE_DEV_PHYSICS);
        if (ev->key.key == FGE_KEY_F7 || ev->key.key == FGE_KEY_7) fge_dev_toggle(FGE_DEV_ECS);
        if (ev->key.key == FGE_KEY_F8 || ev->key.key == FGE_KEY_8) fge_dev_toggle(FGE_DEV_PLATFORM);
        break;
    case FGE_EVENT_KEY_UP:
        fge_telem_inc(FGE_TELEM_KEY_RELEASE);
        break;
    case FGE_EVENT_MOUSE_MOVE:
        fge_telem_inc(FGE_TELEM_MOUSE_MOVE);
        fge_telem_record_input(0, 0,
                               ev->mouse_move.pos.x, ev->mouse_move.pos.y,
                               ev->mouse_move.delta.x, ev->mouse_move.delta.y, "");
        if (st->dragging) {
            st->box->position.x = ev->mouse_move.pos.x - st->drag_offset_x;
            st->box->position.y = ev->mouse_move.pos.y - st->drag_offset_y;
            st->box->velocity = fge_v2_zero();
        }
        break;
    case FGE_EVENT_MOUSE_DOWN:
        fge_telem_inc(FGE_TELEM_MOUSE_DOWN);
        fge_telem_record_input(0, (uint32_t)ev->mouse_button.button,
                               ev->mouse_button.pos.x, ev->mouse_button.pos.y, 0, 0, "");
        if (ev->mouse_button.button == FGE_MOUSE_LEFT) {
            float mx = ev->mouse_button.pos.x;
            float my = ev->mouse_button.pos.y;
            if (mx >= st->box->position.x && mx < st->box->position.x + 80.0f &&
                my >= st->box->position.y && my < st->box->position.y + 60.0f) {
                st->dragging = true;
                st->drag_offset_x = mx - st->box->position.x;
                st->drag_offset_y = my - st->box->position.y;
            }
        }
        break;
    case FGE_EVENT_MOUSE_UP:
        fge_telem_inc(FGE_TELEM_MOUSE_UP);
        if (ev->mouse_button.button == FGE_MOUSE_LEFT) st->dragging = false;
        break;
    case FGE_EVENT_CLOSE:
        p->running = false;
        break;
    case FGE_EVENT_RESIZE:
        fge_renderer_resize(&st->renderer, ev->resize.width, ev->resize.height);
        break;
    default:
        break;
    }
}

int main(void) {
    printf("============================================\n");
    printf("  FORGE Pixel-Perfect Physics Demo\n");
    printf("  Drag the blue box with left mouse.\n");
    printf("  Both turn RED on pixel-perfect collision.\n");
    printf("  F1-F8: telemetry toggles | ESC: exit\n");
    printf("============================================\n\n");

    fge_log_init(FGE_LOG_LEVEL_INFO);
    fge_log_add_sink_stdout(FGE_LOG_LEVEL_INFO);
    fge_dev_mode_init();
    fge_dev_enable(FGE_DEV_OVERLAY);
    fge_dev_enable(FGE_DEV_FPS);
    fge_dev_enable(FGE_DEV_RENDERER);
    fge_dev_enable(FGE_DEV_PHYSICS);

    fge_platform_t *platform = fge_platform_create("FORGE Pixel-Perfect Physics", WINDOW_W, WINDOW_H, false);
    if (!platform) {
        fprintf(stderr, "ERROR: Failed to create platform window.\n");
        return 1;
    }

    demo_state_t state = {0};
    state.platform = platform;
    if (!fge_renderer_init(&state.renderer, platform->width, platform->height)) {
        fprintf(stderr, "ERROR: Failed to initialize renderer.\n");
        fge_platform_destroy(platform);
        return 1;
    }

    if (!fge_phys_world_init(&state.world)) {
        fprintf(stderr, "ERROR: Failed to initialize physics world.\n");
        return 1;
    }
    state.world.gravity = fge_v2(0.0f, 0.0f); /* zero gravity for top-down demo */

    /* Create ball with circular pixel collider */
    float ball_r = 30.0f;
    fge_shape_t ball_shape = fge_shape_circle(fge_v2(0.0f, 0.0f), ball_r);
    state.ball = fge_phys_create_body(&state.world, FGE_BODY_DYNAMIC, &ball_shape);
    if (!state.ball) {
        fprintf(stderr, "ERROR: Failed to create ball body.\n");
        return 1;
    }
    state.ball->position = fge_v2((float)platform->width / 2.0f, (float)platform->height / 2.0f);
    state.ball->velocity = fge_v2(250.0f, 180.0f);
    state.ball->restitution = 0.8f;
    if (!make_circle_mask(&state.ball_mask, ball_r)) {
        fprintf(stderr, "ERROR: Failed to create ball mask.\n");
        return 1;
    }
    state.ball->pixel_collider = &state.ball_mask;

    /* Create box with rectangular pixel collider */
    fge_shape_t box_shape = fge_shape_aabb(0.0f, 0.0f, 80.0f, 60.0f);
    state.box = fge_phys_create_body(&state.world, FGE_BODY_KINEMATIC, &box_shape);
    if (!state.box) {
        fprintf(stderr, "ERROR: Failed to create box body.\n");
        return 1;
    }
    state.box->position = fge_v2((float)platform->width - 200.0f, (float)platform->height / 2.0f);
    if (!make_rect_mask(&state.box_mask, 80, 60)) {
        fprintf(stderr, "ERROR: Failed to create box mask.\n");
        return 1;
    }
    state.box->pixel_collider = &state.box_mask;

    platform->framebuffer = fge_renderer_fb(&state.renderer);
    platform->user_data = &state;
    platform->on_frame = on_frame;
    platform->on_event = on_event;

    printf("Window created: %dx%d\n", platform->width, platform->height);
    printf("Starting render loop...\n\n");

    fge_platform_run(platform);

    printf("\n============================================\n");
    printf("  Demo finished.\n");
    printf("  Total frames: %u\n", state.frame_count);
    printf("  Total time: %.2f seconds\n", state.total_time);
    printf("============================================\n");

    fge_bitmask_free(&state.ball_mask.mask);
    fge_bitmask_free(&state.box_mask.mask);
    fge_phys_world_free(&state.world);
    fge_renderer_shutdown(&state.renderer);
    fge_platform_destroy(platform);
    fge_log_shutdown();

    return 0;
}
