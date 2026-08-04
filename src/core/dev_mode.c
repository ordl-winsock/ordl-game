/*
 * src/core/dev_mode.c — Developer mode category flags
 *
 * Simple global bitmask. Lock-free via atomics.
 * Production builds can stub these out for zero overhead.
 */

#include "forge/dev_mode.h"

static _Atomic uint32_t g_dev_flags = 0;

void fge_dev_mode_init(void) {
    /* Default: all off. App or command line can enable. */
    atomic_store_explicit(&g_dev_flags, 0, memory_order_relaxed);
}

void fge_dev_toggle(fge_dev_category_t cat) {
    uint32_t old = atomic_load_explicit(&g_dev_flags, memory_order_relaxed);
    uint32_t neu = old ^ (uint32_t)cat;
    atomic_store_explicit(&g_dev_flags, neu, memory_order_relaxed);
}

bool fge_dev_enabled(fge_dev_category_t cat) {
    uint32_t flags = atomic_load_explicit(&g_dev_flags, memory_order_relaxed);
    return (flags & (uint32_t)cat) != 0;
}

void fge_dev_enable(fge_dev_category_t cat) {
    uint32_t old = atomic_load_explicit(&g_dev_flags, memory_order_relaxed);
    uint32_t neu = old | (uint32_t)cat;
    atomic_store_explicit(&g_dev_flags, neu, memory_order_relaxed);
}

void fge_dev_disable(fge_dev_category_t cat) {
    uint32_t old = atomic_load_explicit(&g_dev_flags, memory_order_relaxed);
    uint32_t neu = old & ~(uint32_t)cat;
    atomic_store_explicit(&g_dev_flags, neu, memory_order_relaxed);
}

void fge_dev_set_all(bool on) {
    atomic_store_explicit(&g_dev_flags, on ? 0xFFFFFFFFu : 0, memory_order_relaxed);
}
