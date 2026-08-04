/*
 * src/renderer/debug_overlay.c — Render telemetry/debug data onto framebuffer
 *
 * Reads dev_mode flags to decide what to draw.
 * Reads telemetry data for actual numbers.
 * No state of its own.
 */

#include "forge/debug_overlay.h"
#include "forge/dev_mode.h"
#include "forge/telemetry.h"
#include "forge/renderer.h"
#include "forge/log.h"

#include <stdarg.h>
#include <stdio.h>

#define DBG_BG    0xCC000000u   /* semi-transparent black */
#define DBG_GREEN 0xFF00FF00u
#define DBG_YELLOW 0xFFFFFF00u
#define DBG_RED   0xFFFF0000u
#define DBG_WHITE 0xFFFFFFFFu
#define DBG_CYAN  0xFF00FFFFu
#define DBG_LINE_H 18
#define DBG_FONT_SCALE 1.0f

/* Draw a text line with a dark background strip for readability */
static void draw_line(fge_renderer_t *r, int x, int y, uint32_t color, const char *text) {
    int len = 0;
    for (const char *p = text; *p; p++) len++;
    int tw = len * 8;  /* 8px per char at scale 1 */
    fge_draw_rect(&r->fb, x - 2, y - 1, tw + 4, DBG_LINE_H, DBG_BG);
    fge_draw_text(&r->fb, text, x, y, color, DBG_FONT_SCALE);
}

/* Color-code latency: green < 16ms, yellow < 33ms, red otherwise */
static uint32_t latency_color(double ms) {
    if (ms < 16.0) return DBG_GREEN;
    if (ms < 33.0) return DBG_YELLOW;
    return DBG_RED;
}

void fge_debug_overlay_render(fge_renderer_t *r) {
    if (!r) return;
    if (!fge_dev_overlay_visible()) return;

    int x = 10, y = 10;
    char buf[256];

    /* --- Always-visible: last key + dev flags --- */
    {
        const fge_telem_input_t *in = fge_telem_get_input();
        uint32_t flags = 0;
        /* Read flags atomically via enabled checks */
        if (fge_dev_enabled(FGE_DEV_FPS))      flags |= 0x01;
        if (fge_dev_enabled(FGE_DEV_RENDERER)) flags |= 0x02;
        if (fge_dev_enabled(FGE_DEV_INPUT))    flags |= 0x04;
        if (fge_dev_enabled(FGE_DEV_NETWORK))  flags |= 0x08;
        if (fge_dev_enabled(FGE_DEV_PHYSICS))  flags |= 0x10;
        if (fge_dev_enabled(FGE_DEV_ECS))      flags |= 0x20;
        if (fge_dev_enabled(FGE_DEV_PLATFORM)) flags |= 0x40;
        snprintf(buf, sizeof(buf), "[KEY:%3u] dev_flags=0x%02X  (F1-F8 toggles)",
                 in->last_key, (unsigned)flags);
        draw_line(r, x, y, DBG_YELLOW, buf);
        y += DBG_LINE_H;
    }

    /* --- FPS / Frame Timing --- */
    if (fge_dev_enabled(FGE_DEV_FPS)) {
        const fge_telem_histogram_t *h = fge_telem_get_histogram();
        double fps = (h->one_sec_avg > 0.0) ? (1000.0 / h->one_sec_avg) : 0.0;
        uint32_t c = latency_color(h->avg_ms);
        snprintf(buf, sizeof(buf), "FPS: %.1f  %.2f ms  [%.2f .. %.2f]  p95=%.2f p99=%.2f",
                 fps, h->avg_ms, h->min_ms, h->max_ms, h->p95_ms, h->p99_ms);
        draw_line(r, x, y, c, buf);
        y += DBG_LINE_H;
    }

    /* --- Counters --- */
    if (fge_dev_enabled(FGE_DEV_RENDERER)) {
        snprintf(buf, sizeof(buf), "Frames: %llu  DrawCalls: %llu  Triangles: %llu",
                 (unsigned long long)r->frame_count,
                 (unsigned long long)r->draw_calls,
                 (unsigned long long)r->triangles);
        draw_line(r, x, y, DBG_CYAN, buf);
        y += DBG_LINE_H;
    }

    /* --- Input --- */
    if (fge_dev_enabled(FGE_DEV_INPUT)) {
        const fge_telem_input_t *in = fge_telem_get_input();
        snprintf(buf, sizeof(buf), "Input: key=%u mouse=(%.0f,%.0f) d=(%.0f,%.0f) text=[%s]",
                 in->last_key, in->mouse_x, in->mouse_y,
                 in->mouse_dx, in->mouse_dy,
                 in->last_text[0] ? in->last_text : "-");
        draw_line(r, x, y, DBG_WHITE, buf);
        y += DBG_LINE_H;
    }

    /* --- Network --- */
    if (fge_dev_enabled(FGE_DEV_NETWORK)) {
        uint64_t recv = fge_telem_get(FGE_TELEM_NET_RECV);
        uint64_t send = fge_telem_get(FGE_TELEM_NET_SEND);
        snprintf(buf, sizeof(buf), "Net: recv=%llu  send=%llu", (unsigned long long)recv, (unsigned long long)send);
        draw_line(r, x, y, DBG_YELLOW, buf);
        y += DBG_LINE_H;
    }

    /* --- Physics --- */
    if (fge_dev_enabled(FGE_DEV_PHYSICS)) {
        uint64_t steps = fge_telem_get(FGE_TELEM_PHYS_STEP);
        snprintf(buf, sizeof(buf), "Physics: steps=%llu", (unsigned long long)steps);
        draw_line(r, x, y, DBG_GREEN, buf);
        y += DBG_LINE_H;
    }

    /* --- ECS --- */
    if (fge_dev_enabled(FGE_DEV_ECS)) {
        uint64_t queries = fge_telem_get(FGE_TELEM_ECS_QUERY);
        snprintf(buf, sizeof(buf), "ECS: queries=%llu", (unsigned long long)queries);
        draw_line(r, x, y, DBG_CYAN, buf);
        y += DBG_LINE_H;
    }

    /* --- Platform --- */
    if (fge_dev_enabled(FGE_DEV_PLATFORM)) {
        uint64_t frames = fge_telem_get(FGE_TELEM_FRAME);
        uint64_t swaps = fge_telem_get(FGE_TELEM_SWAP_BUFFER);
        snprintf(buf, sizeof(buf), "Platform: frames=%llu  swaps=%llu", (unsigned long long)frames, (unsigned long long)swaps);
        draw_line(r, x, y, DBG_WHITE, buf);
        y += DBG_LINE_H;
    }

    /* --- Help footer --- */
    y += DBG_LINE_H;
    draw_line(r, x, y, DBG_WHITE,
              "F1=overlay F2=FPS F3=renderer F4=input F5=net F6=physics F7=ecs F8=platform");
}

void fge_debug_text(fge_renderer_t *r, int x, int y, uint32_t color, const char *fmt, ...) {
    if (!r) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int len = 0;
    for (const char *p = buf; *p; p++) len++;
    int tw = len * 8;
    fge_draw_rect(&r->fb, x - 2, y - 1, tw + 4, DBG_LINE_H, DBG_BG);
    fge_draw_text(&r->fb, buf, x, y, color, DBG_FONT_SCALE);
}
