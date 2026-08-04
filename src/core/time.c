/*
 * src/core/time.c — Clock, frame timing, profiler implementation
 */

#define _GNU_SOURCE
#include "forge/time.h"
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* Clock                                                                      */
/* -------------------------------------------------------------------------- */

void fge_clock_init(fge_clock_t *c) {
    if (!c) return;
    struct timespec ts;
    clock_getres(CLOCK_MONOTONIC, &ts);
    c->freq = 1000000000ULL;
    if (ts.tv_sec == 0 && ts.tv_nsec > 0) {
        /* Use actual resolution if finer */
        (void)0; /* nanoseconds is our base unit */
    }
    c->start = fge_clock_now(c);
}

uint64_t fge_clock_now(const fge_clock_t *c) {
    (void)c;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* -------------------------------------------------------------------------- */
/* Timer                                                                      */
/* -------------------------------------------------------------------------- */

bool fge_timer_tick(fge_timer_t *t, double dt) {
    if (!t || !t->running) return false;
    t->elapsed += dt;
    if (t->elapsed >= t->duration) {
        if (t->repeat) {
            t->elapsed -= t->duration;
        } else {
            t->elapsed = t->duration;
            t->running = false;
        }
        return true;
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* Stopwatch                                                                  */
/* -------------------------------------------------------------------------- */

void fge_stopwatch_start(fge_stopwatch_t *sw, const fge_clock_t *clock) {
    if (!sw || !clock) return;
    sw->clock = clock;
    sw->start_ticks = fge_clock_now(clock);
    sw->running = true;
}

void fge_stopwatch_stop(fge_stopwatch_t *sw) {
    if (!sw || !sw->running) return;
    sw->elapsed_ticks += fge_clock_now(sw->clock) - sw->start_ticks;
    sw->running = false;
}

void fge_stopwatch_reset(fge_stopwatch_t *sw) {
    if (!sw) return;
    sw->elapsed_ticks = 0;
    sw->running = false;
}

/* -------------------------------------------------------------------------- */
/* Frame timing                                                               */
/* -------------------------------------------------------------------------- */

void fge_frame_time_init(fge_frame_time_t *ft, double target_fps) {
    if (!ft) return;
    memset(ft, 0, sizeof(*ft));
    ft->target_dt = target_fps > 0.0 ? 1.0 / target_fps : 0.0;
    ft->min_dt = 9999.0;
    ft->max_dt = 0.0;
    ft->avg_dt = ft->target_dt;
}

void fge_frame_time_update(fge_frame_time_t *ft, const fge_clock_t *clock) {
    if (!ft || !clock) return;
    static uint64_t last_time = 0;
    uint64_t now = fge_clock_now(clock);
    if (last_time == 0) last_time = now;

    uint64_t delta_ticks = now - last_time;
    last_time = now;

    ft->raw_dt = fge_clock_ticks_to_sec(clock, delta_ticks);
    /* Clamp dt to avoid spiral of death on hitch */
    ft->dt = FGE_MIN(ft->raw_dt, ft->target_dt > 0.0 ? ft->target_dt * 3.0 : 0.1);

    ft->time += ft->dt;
    ft->frame_count++;
    ft->tick++;

    /* FPS smoothing */
    if (ft->dt > 0.0) {
        double instant_fps = 1.0 / ft->dt;
        ft->fps = ft->fps * 0.9 + instant_fps * 0.1;
    }

    /* Stats */
    ft->min_dt = FGE_MIN(ft->min_dt, ft->dt);
    ft->max_dt = FGE_MAX(ft->max_dt, ft->dt);
    ft->avg_dt = ft->avg_dt * 0.95 + ft->dt * 0.05;

    ft->fps_history[ft->fps_history_idx] = ft->fps;
    ft->fps_history_idx = (ft->fps_history_idx + 1) % 64;
}

void fge_frame_time_fixed_update(fge_frame_time_t *ft, fge_fixed_update_fn fn, void *userdata) {
    if (!ft || !fn || ft->target_dt <= 0.0) return;
    ft->accumulator += ft->dt;
    const double max_accumulator = ft->target_dt * 5.0; /* prevent spiral */
    if (ft->accumulator > max_accumulator) {
        FGE_WARN(FGE_LOG_CAT_PROFILE, "Frame hitch detected: accumulator %.4f s, clamping", ft->accumulator);
        ft->accumulator = max_accumulator;
    }
    while (ft->accumulator >= ft->target_dt) {
        fn(userdata);
        ft->accumulator -= ft->target_dt;
    }
}

void fge_frame_time_pace(const fge_frame_time_t *ft, const fge_clock_t *clock) {
    if (!ft || !clock || ft->target_dt <= 0.0) return;
    static uint64_t next_frame_ticks = 0;
    uint64_t target_ticks = (uint64_t)(ft->target_dt * (double)clock->freq);
    uint64_t now = fge_clock_now(clock);

    /* Initialize on first call or if we fell far behind */
    if (next_frame_ticks == 0 || now > next_frame_ticks + target_ticks * 4) {
        next_frame_ticks = now + target_ticks;
        return;
    }

    next_frame_ticks += target_ticks;
    if (now >= next_frame_ticks) return; /* already late */

    uint64_t remain_ticks = next_frame_ticks - now;
    uint64_t remain_ns = (remain_ticks * 1000000000ULL) / clock->freq;

    /* Hybrid: sleep for bulk, spin for final ~0.5ms */
    if (remain_ns > 500000) {
        uint64_t sleep_ns = remain_ns - 250000; /* leave 0.25ms for spin */
        struct timespec ts = { (time_t)(sleep_ns / 1000000000ULL), (long)(sleep_ns % 1000000000ULL) };
        nanosleep(&ts, NULL);
    }

    /* Spin-wait for remaining time */
    while (fge_clock_now(clock) < next_frame_ticks) {
        #if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
        #endif
    }
}

/* -------------------------------------------------------------------------- */
/* Profiler                                                                   */
/* -------------------------------------------------------------------------- */

fge_profiler_t g_fge_profiler = {0};

void fge_profiler_init(void) {
    memset(&g_fge_profiler, 0, sizeof(g_fge_profiler));
    fge_clock_init(&g_fge_profiler.clock);
    g_fge_profiler.enabled = true;
}

void fge_profiler_reset(void) {
    g_fge_profiler.node_count = 0;
    g_fge_profiler.depth = 0;
}

void fge_profile_begin_impl(const char *name, const char *func, const char *file, int line) {
    if (!g_fge_profiler.enabled) return;
    if (g_fge_profiler.node_count >= FGE_PROFILE_MAX_NODES) return;
    if (g_fge_profiler.depth >= FGE_PROFILE_MAX_DEPTH) return;

    int idx = g_fge_profiler.node_count++;
    fge_profile_node_t *n = &g_fge_profiler.nodes[idx];
    n->name = name;
    n->func = func;
    n->file = file;
    n->line = line;
    n->elapsed_ns = 0;
    n->hit_count = 1;
    n->child_elapsed_ns = 0;
    n->first_child_idx = -1;
    n->next_sibling_idx = -1;

    if (g_fge_profiler.depth > 0) {
        int parent_idx = g_fge_profiler.stack[g_fge_profiler.depth - 1];
        n->parent_idx = parent_idx;
        fge_profile_node_t *parent = &g_fge_profiler.nodes[parent_idx];
        n->next_sibling_idx = parent->first_child_idx;
        parent->first_child_idx = idx;
    } else {
        n->parent_idx = -1;
    }

    g_fge_profiler.stack[g_fge_profiler.depth++] = idx;
    fge_stopwatch_start(&n->watch, &g_fge_profiler.clock);
}

void fge_profile_end_impl(void) {
    if (!g_fge_profiler.enabled) return;
    if (g_fge_profiler.depth <= 0) return;

    int idx = g_fge_profiler.stack[--g_fge_profiler.depth];
    fge_profile_node_t *n = &g_fge_profiler.nodes[idx];
    fge_stopwatch_stop(&n->watch);
    n->elapsed_ns = (uint64_t)(fge_stopwatch_elapsed_sec(&n->watch) * 1e9);

    if (n->parent_idx >= 0) {
        g_fge_profiler.nodes[n->parent_idx].child_elapsed_ns += n->elapsed_ns;
    }
}

static void fge_profiler_dump_node(int idx, int depth) {
    if (idx < 0 || idx >= g_fge_profiler.node_count) return;
    fge_profile_node_t *n = &g_fge_profiler.nodes[idx];
    double self_ms = (n->elapsed_ns - n->child_elapsed_ns) / 1e6;
    double total_ms = n->elapsed_ns / 1e6;
    char indent[64] = {0};
    for (int i = 0; i < depth && i < 63; i++) indent[i] = ' ';
    FGE_INFO(FGE_LOG_CAT_PROFILE, "%s%-32s  self=%8.3f ms  total=%8.3f ms  hits=%8lu",
             indent, n->name, self_ms, total_ms, (unsigned long)n->hit_count);
    /* Dump children in reverse order (they were linked as stack) */
    /* We'd need to reverse them for proper order; skip for now */
}

void fge_profiler_dump(void) {
    if (!g_fge_profiler.enabled || g_fge_profiler.node_count == 0) return;
    FGE_INFO(FGE_LOG_CAT_PROFILE, "========== Profiler Report ==========");
    for (int i = 0; i < g_fge_profiler.node_count; i++) {
        if (g_fge_profiler.nodes[i].parent_idx < 0) {
            fge_profiler_dump_node(i, 0);
        }
    }
    FGE_INFO(FGE_LOG_CAT_PROFILE, "=====================================");
}

/* -------------------------------------------------------------------------- */
/* Counters                                                                   */
/* -------------------------------------------------------------------------- */

fge_counter_t g_fge_counters[FGE_COUNTER_MAX];
fge_atomic_uint_t g_fge_counter_count = FGE_ATOMIC_INIT(0);

void fge_counter_dump_all(void) {
    unsigned int count = FGE_ATOMIC_LOAD(&g_fge_counter_count);
    if (count == 0) return;
    FGE_INFO(FGE_LOG_CAT_PROFILE, "========== Counters ==========");
    for (unsigned int i = 0; i < count && i < FGE_COUNTER_MAX; i++) {
        uint64_t val = FGE_ATOMIC_LOAD(&g_fge_counters[i].value);
        FGE_INFO(FGE_LOG_CAT_PROFILE, "  %-32s  %lu", g_fge_counters[i].name, (unsigned long)val);
    }
}
