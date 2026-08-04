/*
 * src/core/log.c — Comprehensive logging system implementation
 */

#define _GNU_SOURCE
#include "forge/log.h"
#include "forge/memory.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>

/* -------------------------------------------------------------------------- */
/* Global logger state                                                        */
/* -------------------------------------------------------------------------- */

fge_logger_t g_fge_logger = {0};

/* ANSI color codes */
static const char *s_level_colors[] = {
    "\033[90m",  /* TRACE - bright black */
    "\033[36m",  /* DEBUG - cyan */
    "\033[32m",  /* INFO - green */
    "\033[33m",  /* WARN - yellow */
    "\033[31m",  /* ERROR - red */
    "\033[35m",  /* FATAL - magenta */
    "\033[0m",
};
static const char *s_color_reset = "\033[0m";

/* -------------------------------------------------------------------------- */
/* Ring buffer                                                                */
/* -------------------------------------------------------------------------- */

bool fge_log_ring_init(fge_log_ring_t *ring, uint32_t capacity) {
    if (!ring || capacity == 0) return false;
    if (!fge_ispow2_u32(capacity)) capacity = fge_next_pow2_u32(capacity);
    ring->entries = FGE_CALLOC(capacity, sizeof(fge_log_entry_t));
    if (!ring->entries) return false;
    ring->capacity = capacity;
    FGE_ATOMIC_STORE(&ring->head, 0);
    FGE_ATOMIC_STORE(&ring->tail, 0);
    return true;
}

void fge_log_ring_free(fge_log_ring_t *ring) {
    if (!ring) return;
    FGE_FREE(ring->entries);
    ring->entries = nullptr;
    ring->capacity = 0;
}

bool fge_log_ring_push(fge_log_ring_t *ring, const fge_log_entry_t *entry) {
    if (!ring || !entry) return false;
    uint32_t head = FGE_ATOMIC_LOAD(&ring->head);
    uint32_t tail = FGE_ATOMIC_LOAD_ACQ(&ring->tail);
    uint32_t cap = ring->capacity;
    if (((head + 1) & (cap - 1)) == tail) {
        return false; /* full */
    }
    ring->entries[head & (cap - 1)] = *entry;
    FGE_ATOMIC_STORE_REL(&ring->head, (head + 1) & (cap - 1));
    return true;
}

bool fge_log_ring_pop(fge_log_ring_t *ring, fge_log_entry_t *out) {
    if (!ring || !out) return false;
    uint32_t tail = FGE_ATOMIC_LOAD(&ring->tail);
    uint32_t head = FGE_ATOMIC_LOAD_ACQ(&ring->head);
    if (tail == head) return false; /* empty */
    *out = ring->entries[tail & (ring->capacity - 1)];
    FGE_ATOMIC_STORE_REL(&ring->tail, (tail + 1) & (ring->capacity - 1));
    return true;
}

uint32_t fge_log_ring_count(const fge_log_ring_t *ring) {
    if (!ring) return 0;
    uint32_t h = FGE_ATOMIC_LOAD(&ring->head);
    uint32_t t = FGE_ATOMIC_LOAD(&ring->tail);
    return (h - t) & (ring->capacity - 1);
}

/* -------------------------------------------------------------------------- */
/* Internal helpers                                                           */
/* -------------------------------------------------------------------------- */

static uint64_t fge_log_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint32_t fge_log_thread_id(void) {
    return (uint32_t)syscall(SYS_gettid);
}

static void fge_log_format_entry(char *buf, size_t buf_size, const fge_log_entry_t *e, bool use_color) {
    /* timestamp */
    double sec = e->timestamp_ns / 1e9;
    int n = snprintf(buf, buf_size, "[%10.6f] ", sec);
    if (n < 0 || (size_t)n >= buf_size) return;
    size_t pos = (size_t)n;

    /* thread */
    n = snprintf(buf + pos, buf_size - pos, "[%5u] ", e->thread_id);
    if (n < 0 || (size_t)n >= buf_size - pos) return;
    pos += (size_t)n;

    /* level + category */
    const char *color = use_color ? s_level_colors[e->level] : "";
    const char *reset = use_color ? s_color_reset : "";
    n = snprintf(buf + pos, buf_size - pos, "%s%-5s%s %s%-12s%s ",
                 color, fge_log_level_name(e->level), reset,
                 color, fge_log_cat_name(e->category), reset);
    if (n < 0 || (size_t)n >= buf_size - pos) return;
    pos += (size_t)n;

    /* file:line */
    n = snprintf(buf + pos, buf_size - pos, "[%s:%d] ", e->file ? e->file : "?", e->line);
    if (n < 0 || (size_t)n >= buf_size - pos) return;
    pos += (size_t)n;

    /* message */
    n = snprintf(buf + pos, buf_size - pos, "%s", e->message);
    if (n < 0 || (size_t)n >= buf_size - pos) return;
    pos += (size_t)n;

    /* key-value pairs */
    for (int i = 0; i < e->kv_count && pos < buf_size - 4; i++) {
        n = snprintf(buf + pos, buf_size - pos, "  %s=%s", e->kv[i].key, e->kv[i].value);
        if (n < 0 || (size_t)n >= buf_size - pos) return;
        pos += (size_t)n;
    }

    if (pos < buf_size - 1) {
        buf[pos++] = '\n';
        buf[pos] = '\0';
    }
}

static void fge_log_write_to_sink(fge_log_sink_t *sink, const fge_log_entry_t *e) {
    if (e->level < sink->min_level) return;
    if (!(sink->category_mask & (1u << e->category))) return;

    char buf[2048];
    switch (sink->type) {
        case FGE_LOG_SINK_STDOUT: {
            fge_log_format_entry(buf, sizeof(buf), e, true);
            fwrite(buf, 1, strlen(buf), stdout);
            fflush(stdout);
            break;
        }
        case FGE_LOG_SINK_FILE: {
            fge_log_format_entry(buf, sizeof(buf), e, false);
            if (sink->file) {
                fwrite(buf, 1, strlen(buf), sink->file);
            }
            break;
        }
        case FGE_LOG_SINK_CALLBACK: {
            if (sink->callback.fn) {
                sink->callback.fn(e, sink->callback.userdata);
            }
            break;
        }
        case FGE_LOG_SINK_NETWORK: {
            /* TODO: UDP log shipping */
            break;
        }
    }
}

static void fge_log_process_entry(const fge_log_entry_t *e) {
    fge_log_sink_t *sink = g_fge_logger.sinks;
    while (sink) {
        fge_log_write_to_sink(sink, e);
        sink = sink->next;
    }
}

/* -------------------------------------------------------------------------- */
/* Async output thread                                                        */
/* -------------------------------------------------------------------------- */

static pthread_t s_log_thread;
static atomic_flag s_log_thread_stop = ATOMIC_FLAG_INIT;

static void *fge_log_thread_fn(void *arg) {
    (void)arg;
    fge_log_entry_t entry;
    while (!FGE_ATOMIC_FLAG_TEST_SET(&s_log_thread_stop)) {
        FGE_ATOMIC_FLAG_CLEAR(&s_log_thread_stop); /* we just test-and-set, clear for next check */
        bool had_work = false;
        while (fge_log_ring_pop(&g_fge_logger.ring, &entry)) {
            fge_log_process_entry(&entry);
            had_work = true;
        }
        if (!had_work) {
            usleep(1000); /* 1ms sleep when idle */
        }
    }
    /* Drain remaining */
    while (fge_log_ring_pop(&g_fge_logger.ring, &entry)) {
        fge_log_process_entry(&entry);
    }
    return nullptr;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void fge_log_init(fge_log_level_t default_level) {
    memset(&g_fge_logger, 0, sizeof(g_fge_logger));
    for (int i = 0; i < FGE_LOG_CAT_COUNT; i++) {
        g_fge_logger.category_levels[i] = default_level;
    }
    fge_log_ring_init(&g_fge_logger.ring, 4096);
    g_fge_logger.async_mode = false;
    g_fge_logger.running = true;
    fge_spinlock_init(&g_fge_logger.lock);
}

void fge_log_shutdown(void) {
    if (!g_fge_logger.running) return;
    fge_log_stop_async();
    fge_log_flush();

    /* Free sinks */
    fge_log_sink_t *sink = g_fge_logger.sinks;
    while (sink) {
        fge_log_sink_t *next = sink->next;
        if (sink->type == FGE_LOG_SINK_FILE && sink->file) {
            fclose(sink->file);
        }
        FGE_FREE(sink);
        sink = next;
    }
    g_fge_logger.sinks = nullptr;
    fge_log_ring_free(&g_fge_logger.ring);
    g_fge_logger.running = false;
}

bool fge_log_start_async(void) {
    if (g_fge_logger.async_mode) return true;
    g_fge_logger.async_mode = true;
    FGE_ATOMIC_FLAG_CLEAR(&s_log_thread_stop);
    if (pthread_create(&s_log_thread, nullptr, fge_log_thread_fn, nullptr) != 0) {
        g_fge_logger.async_mode = false;
        return false;
    }
    return true;
}

void fge_log_stop_async(void) {
    if (!g_fge_logger.async_mode) return;
    FGE_ATOMIC_FLAG_TEST_SET(&s_log_thread_stop);
    pthread_join(s_log_thread, nullptr);
    g_fge_logger.async_mode = false;
}

void fge_log_add_sink_stdout(fge_log_level_t min_level) {
    fge_log_sink_t *sink = FGE_CALLOC(1, sizeof(fge_log_sink_t));
    if (!sink) return;
    sink->type = FGE_LOG_SINK_STDOUT;
    sink->min_level = min_level;
    sink->category_mask = ~0u;
    sink->next = g_fge_logger.sinks;
    g_fge_logger.sinks = sink;
}

bool fge_log_add_sink_file(fge_log_level_t min_level, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fge_log_sink_t *sink = FGE_CALLOC(1, sizeof(fge_log_sink_t));
    if (!sink) { fclose(f); return false; }
    sink->type = FGE_LOG_SINK_FILE;
    sink->min_level = min_level;
    sink->category_mask = ~0u;
    sink->file = f;
    sink->next = g_fge_logger.sinks;
    g_fge_logger.sinks = sink;
    return true;
}

void fge_log_add_sink_callback(fge_log_level_t min_level, fge_log_sink_cb_t cb, void *userdata) {
    fge_log_sink_t *sink = FGE_CALLOC(1, sizeof(fge_log_sink_t));
    if (!sink) return;
    sink->type = FGE_LOG_SINK_CALLBACK;
    sink->min_level = min_level;
    sink->category_mask = ~0u;
    sink->callback.fn = cb;
    sink->callback.userdata = userdata;
    sink->next = g_fge_logger.sinks;
    g_fge_logger.sinks = sink;
}

void fge_log_set_category_level(fge_log_category_t cat, fge_log_level_t level) {
    if ((size_t)cat < FGE_LOG_CAT_COUNT) {
        g_fge_logger.category_levels[cat] = level;
    }
}

void fge_log_flush(void) {
    if (g_fge_logger.async_mode) {
        /* Spin until ring empty */
        while (fge_log_ring_count(&g_fge_logger.ring) > 0) {
            usleep(100);
        }
    }
    /* Flush file sinks */
    fge_log_sink_t *sink = g_fge_logger.sinks;
    while (sink) {
        if (sink->type == FGE_LOG_SINK_FILE && sink->file) {
            fflush(sink->file);
        }
        sink = sink->next;
    }
}

/* -------------------------------------------------------------------------- */
/* Core submit                                                                */
/* -------------------------------------------------------------------------- */

static void fge_log_submit_entry(fge_log_entry_t *e) {
    g_fge_logger.total_count++;

    if (g_fge_logger.async_mode) {
        if (!fge_log_ring_push(&g_fge_logger.ring, e)) {
            g_fge_logger.dropped_count++;
        }
    } else {
        fge_spinlock_lock(&g_fge_logger.lock);
        fge_log_process_entry(e);
        fge_spinlock_unlock(&g_fge_logger.lock);
    }
}

void fge_log_submit(fge_log_category_t cat, fge_log_level_t level,
                    const char *file, int line, const char *func,
                    const char *fmt, ...) {
    if (!g_fge_logger.running) return;
    if (level < g_fge_logger.category_levels[cat]) return;
    if (level < FGE_LOG_COMPILE_LEVEL) return;

    fge_log_entry_t e = {0};
    e.timestamp_ns = fge_log_now_ns();
    e.level = level;
    e.category = cat;
    e.thread_id = fge_log_thread_id();
    e.file = file;
    e.line = line;
    e.func = func;

    va_list args;
    va_start(args, fmt);
    vsnprintf(e.message, sizeof(e.message), fmt, args);
    va_end(args);

    fge_log_submit_entry(&e);
}

void fge_log_submitv(fge_log_category_t cat, fge_log_level_t level,
                     const char *file, int line, const char *func,
                     const char *fmt, va_list args) {
    if (!g_fge_logger.running) return;
    if (level < g_fge_logger.category_levels[cat]) return;
    if (level < FGE_LOG_COMPILE_LEVEL) return;

    fge_log_entry_t e = {0};
    e.timestamp_ns = fge_log_now_ns();
    e.level = level;
    e.category = cat;
    e.thread_id = fge_log_thread_id();
    e.file = file;
    e.line = line;
    e.func = func;
    vsnprintf(e.message, sizeof(e.message), fmt, args);

    fge_log_submit_entry(&e);
}

void fge_log_submit_kvs(fge_log_category_t cat, fge_log_level_t level,
                        const char *file, int line, const char *func,
                        const char *fmt, int kv_count, ...) {
    if (!g_fge_logger.running) return;
    if (level < g_fge_logger.category_levels[cat]) return;
    if (level < FGE_LOG_COMPILE_LEVEL) return;

    fge_log_entry_t e = {0};
    e.timestamp_ns = fge_log_now_ns();
    e.level = level;
    e.category = cat;
    e.thread_id = fge_log_thread_id();
    e.file = file;
    e.line = line;
    e.func = func;

    va_list args;
    va_start(args, kv_count);
    vsnprintf(e.message, sizeof(e.message), fmt, args);

    e.kv_count = FGE_MIN(kv_count, FGE_LOG_MAX_KV_PAIRS);
    for (int i = 0; i < e.kv_count; i++) {
        e.kv[i].key = va_arg(args, const char *);
        e.kv[i].value = va_arg(args, const char *);
    }
    va_end(args);

    fge_log_submit_entry(&e);
}

/* -------------------------------------------------------------------------- */
/* Scope tracing                                                              */
/* -------------------------------------------------------------------------- */

void fge_log_scope_begin(fge_log_scope_t *scope, fge_log_category_t cat, const char *name) {
    if (!scope) return;
    scope->name = name;
    scope->cat = cat;
    scope->start_ns = fge_log_now_ns();
    FGE_TRACE(cat, "→ %s", name);
}

void fge_log_scope_end(fge_log_scope_t *scope) {
    if (!scope || !scope->name) return;
    uint64_t elapsed = fge_log_now_ns() - scope->start_ns;
    double ms = elapsed / 1e6;
    if (ms > 1.0) {
        FGE_DEBUG(scope->cat, "← %s  [%.2f ms] ⚠ SLOW", scope->name, ms);
    } else {
        FGE_TRACE(scope->cat, "← %s  [%.3f ms]", scope->name, ms);
    }
    scope->name = nullptr;
}

void fge_log_scope_end_auto(fge_log_scope_t *scope) {
    fge_log_scope_end(scope);
}

/* -------------------------------------------------------------------------- */
/* Panic                                                                      */
/* -------------------------------------------------------------------------- */

FGE_NORETURN
void fge_panic(const char *file, int line, const char *func, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "\033[35m[FATAL]\033[0m [%s:%d] %s: ", file, line, func);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);

    /* Try to flush any pending logs */
    fge_log_flush();

    /* Stack trace would go here if we had libunwind */
    __builtin_trap();
    _exit(1);
}
