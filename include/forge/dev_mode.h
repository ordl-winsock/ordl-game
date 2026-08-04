/*
 * forge/dev_mode.h — Developer mode toggle & category flags
 *
 * Simple bitmask of which telemetry/debug features are enabled.
 * Used by debug_overlay to decide what to draw, and by the
 * engine to decide what to log/collect.
 */

#ifndef FORGE_DEV_MODE_H
#define FORGE_DEV_MODE_H

#include "forge/core.h"

typedef enum {
    FGE_DEV_FPS      = (1u << 0),
    FGE_DEV_MEMORY   = (1u << 1),
    FGE_DEV_NETWORK  = (1u << 2),
    FGE_DEV_ECS      = (1u << 3),
    FGE_DEV_PHYSICS  = (1u << 4),
    FGE_DEV_INPUT    = (1u << 5),
    FGE_DEV_RENDERER = (1u << 6),
    FGE_DEV_PLATFORM = (1u << 7),
    FGE_DEV_OVERLAY  = (1u << 8),   /* master overlay on/off */
    FGE_DEV_ALL      = 0xFFFFFFFFu,
} fge_dev_category_t;

void fge_dev_mode_init(void);
void fge_dev_toggle(fge_dev_category_t cat);
bool fge_dev_enabled(fge_dev_category_t cat);
void fge_dev_enable(fge_dev_category_t cat);
void fge_dev_disable(fge_dev_category_t cat);
void fge_dev_set_all(bool on);

/* Convenience */
static inline bool fge_dev_overlay_visible(void) {
    return fge_dev_enabled(FGE_DEV_OVERLAY);
}

#endif /* FORGE_DEV_MODE_H */
