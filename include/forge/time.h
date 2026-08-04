/*
 * forge/time.h — High-resolution timing, frame pacing, profiling
 * Pure C23, zero external dependencies.
 */

#ifndef FORGE_TIME_H
#define FORGE_TIME_H

#include "forge/core.h"
#include "forge/log.h"
#include <time.h>

/* -------------------------------------------------------------------------- */
/* Clock                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint64_t freq;
    uint64_t start;
} fge_clock_t;

void fge_clock_init(fge_clock_t *c);
uint64_t fge_clock_now(const fge_clock_t *c);

FGE_INLINE double fge_clock_ticks_to_sec(const fge_clock_t *c, uint64_t ticks) {
    return c ? (double)ticks / (double)c->freq : 0.0;
}
FGE_INLINE double fge_clock_ticks_to_ms(const fge_clock_t *c, uint64_t ticks) {
    return c ? (double)ticks / (double)c->freq * 1000.0 : 0.0;
}
FGE_INLINE double fge_clock_elapsed_sec(const fge_clock_t *c) {
    return fge_clock_ticks_to_sec(c, fge_clock_now(c) - c->start);
}

/* -------------------------------------------------------------------------- */
/* Timer                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    double duration;
    double elapsed;
    bool running;
    bool repeat;
} fge_timer_t;

FGE_INLINE fge_timer_t fge_timer_make(double duration, bool repeat) {
    return (fge_timer_t){ duration, 0.0, false, repeat };
}
FGE_INLINE void fge_timer_start(fge_timer_t *t) { if (t) t->running = true; }
FGE_INLINE void fge_timer_stop(fge_timer_t *t)  { if (t) t->running = false; }
FGE_INLINE void fge_timer_reset(fge_timer_t *t) { if (t) t->elapsed = 0.0; }

bool fge_timer_tick(fge_timer_t *t, double dt);
FGE_INLINE double fge_timer_progress(const fge_timer_t *t) {
    return (t && t->duration > 0.0) ? FGE_CLAMP(t->elapsed / t->duration, 0.0, 1.0) : 0.0;
}

/* -------------------------------------------------------------------------- */
/* Stopwatch                                                                  */
/* -------------------------------------------------------------------------- */

typedef struct {
    const fge_clock_t *clock;
    uint64_t start_ticks;
    uint64_t elapsed_ticks;
    bool running;
} fge_stopwatch_t;

void fge_stopwatch_start(fge_stopwatch_t *sw, const fge_clock_t *clock);
void fge_stopwatch_stop(fge_stopwatch_t *sw);
void fge_stopwatch_reset(fge_stopwatch_t *sw);

FGE_INLINE double fge_stopwatch_elapsed_sec(const fge_stopwatch_t *sw) {
    if (!sw || !sw->clock) return 0.0;
    uint64_t ticks = sw->elapsed_ticks;
    if (sw->running) ticks += fge_clock_now(sw->clock) - sw->start_ticks;
    return fge_clock_ticks_to_sec(sw->clock, ticks);
}
FGE_INLINE double fge_stopwatch_elapsed_ms(const fge_stopwatch_t *sw) {
    return fge_stopwatch_elapsed_sec(sw) * 1000.0;
}

/* -------------------------------------------------------------------------- */
/* Frame timing                                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
    double dt;
    double raw_dt;
    double time;
    double fps;
    uint64_t frame_count;
    uint64_t tick;
    double target_dt;
    double accumulator;
    /* Stats */
    double avg_dt;
    double min_dt;
    double max_dt;
    double fps_history[64];
    int fps_history_idx;
} fge_frame_time_t;

void fge_frame_time_init(fge_frame_time_t *ft, double target_fps);
void fge_frame_time_update(fge_frame_time_t *ft, const fge_clock_t *clock);

typedef void (*fge_fixed_update_fn)(void *userdata);
void fge_frame_time_fixed_update(fge_frame_time_t *ft, fge_fixed_update_fn fn, void *userdata);
void fge_frame_time_pace(const fge_frame_time_t *ft, const fge_clock_t *clock);

/* -------------------------------------------------------------------------- */
/* Profiler — hierarchical CPU profiling (always compiled, enabled at runtime)*/
/* -------------------------------------------------------------------------- */

#define FGE_PROFILE_MAX_NODES 256
#define FGE_PROFILE_MAX_DEPTH 32

typedef struct {
    const char *name;
    const char *func;
    const char *file;
    int line;
    uint64_t elapsed_ns;
    uint64_t hit_count;
    uint64_t child_elapsed_ns;
    int parent_idx;
    int first_child_idx;
    int next_sibling_idx;
    fge_stopwatch_t watch;
} fge_profile_node_t;

typedef struct {
    fge_profile_node_t nodes[FGE_PROFILE_MAX_NODES];
    int node_count;
    int depth;
    int stack[FGE_PROFILE_MAX_DEPTH];
    fge_clock_t clock;
    bool enabled;
} fge_profiler_t;

extern fge_profiler_t g_fge_profiler;

void fge_profiler_init(void);
void fge_profiler_reset(void);
void fge_profiler_dump(void);

void fge_profile_begin_impl(const char *name, const char *func, const char *file, int line);
void fge_profile_end_impl(void);

#define FGE_PROFILE_BEGIN(name) fge_profile_begin_impl((name), __func__, __FILE__, __LINE__)
#define FGE_PROFILE_END()       fge_profile_end_impl()

/* GCC cleanup attribute for automatic scope profiling */
#if defined(__GNUC__) || defined(__clang__)
typedef struct { const char *name; const char *func; const char *file; int line; } fge_profile_scope_auto_t;
static inline void fge_profile_scope_auto_end(fge_profile_scope_auto_t *s) {
    (void)s; fge_profile_end_impl();
}
#define FGE_CONCAT_(a, b) a##b
#define FGE_CONCAT(a, b) FGE_CONCAT_(a, b)
#define FGE_PROFILE_SCOPE(name) \
    fge_profile_scope_auto_t FGE_CONCAT(_fge_scope_, __LINE__) __attribute__((cleanup(fge_profile_scope_auto_end))) = {(name), __func__, __FILE__, __LINE__}; \
    fge_profile_begin_impl((name), __func__, __FILE__, __LINE__)
#else
#  define FGE_PROFILE_SCOPE(name) FGE_PROFILE_BEGIN(name)
#endif

/* -------------------------------------------------------------------------- */
/* Counter profiler — simple atomic counters                                  */
/* -------------------------------------------------------------------------- */

#define FGE_COUNTER_MAX 64

typedef struct {
    const char *name;
    fge_atomic_u64_t value;
} fge_counter_t;

extern fge_counter_t g_fge_counters[FGE_COUNTER_MAX];
extern fge_atomic_uint_t g_fge_counter_count;

#define FGE_COUNTER_INC(name) do { \
    static int _counter_idx = -1; \
    if (_counter_idx < 0) { \
        unsigned int idx = FGE_ATOMIC_ADD(&g_fge_counter_count, 1); \
        _counter_idx = (int)idx; \
        g_fge_counters[idx].name = (name); \
    } \
    FGE_ATOMIC_ADD(&g_fge_counters[_counter_idx].value, 1); \
} while (0)

#define FGE_COUNTER_ADD(name, val) do { \
    static int _counter_idx = -1; \
    if (_counter_idx < 0) { \
        unsigned int idx = FGE_ATOMIC_ADD(&g_fge_counter_count, 1); \
        _counter_idx = (int)idx; \
        g_fge_counters[idx].name = (name); \
    } \
    FGE_ATOMIC_ADD(&g_fge_counters[_counter_idx].value, (val)); \
} while (0)

void fge_counter_dump_all(void);

#endif /* FORGE_TIME_H */
