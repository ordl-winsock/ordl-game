/*
 * forge/debug_overlay.h — Render telemetry/debug data onto a framebuffer
 *
 * Reads from:
 *   - dev_mode.h  → which categories are enabled
 *   - telemetry.h → counters, histograms, input state
 *
 * No state of its own. Pure render function.
 */

#ifndef FORGE_DEBUG_OVERLAY_H
#define FORGE_DEBUG_OVERLAY_H

#include "forge/renderer.h"

/* Draw the overlay onto the current framebuffer if overlay is enabled.
 * Safe to call every frame — checks dev_mode internally. */
void fge_debug_overlay_render(fge_renderer_t *r);

/* Draw a specific line of text at (x,y) in debug overlay style
 * (small monospace, semi-transparent background). */
void fge_debug_text(fge_renderer_t *r, int x, int y,
                    uint32_t color, const char *fmt, ...);

#endif /* FORGE_DEBUG_OVERLAY_H */
