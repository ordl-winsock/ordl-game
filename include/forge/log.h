/*
 * forge/log.h — Comprehensive logging system for real-time debugging
 *
 * Features:
 *   - Log categories (up to 64) with independent levels
 *   - Lock-free ring buffer for high-throughput logging
 *   - Async output thread (file, stdout, network sink)
 *   - Structured logging with key-value pairs
 *   - Compile-time level filtering
 *   - Scope-based timing traces
 *   - Memory allocation tracking integration
 *
 * Pure C23, zero external dependencies.
 */

#ifndef FORGE_LOG_H
#define FORGE_LOG_H

#include "forge/core.h"
#include "forge/memory.h"
#include <stdio.h>
#include <stdarg.h>

/* -------------------------------------------------------------------------- */
/* Compile-time level — set to FGE_LOG_LEVEL_TRACE for verbose builds         */
/* -------------------------------------------------------------------------- */

#ifndef FGE_LOG_COMPILE_LEVEL
#  ifdef NDEBUG
#    define FGE_LOG_COMPILE_LEVEL FGE_LOG_LEVEL_INFO
#  else
#    define FGE_LOG_COMPILE_LEVEL FGE_LOG_LEVEL_TRACE
#  endif
#endif

typedef enum {
    FGE_LOG_LEVEL_TRACE = 0,
    FGE_LOG_LEVEL_DEBUG,
    FGE_LOG_LEVEL_INFO,
    FGE_LOG_LEVEL_WARN,
    FGE_LOG_LEVEL_ERROR,
    FGE_LOG_LEVEL_FATAL,
    FGE_LOG_LEVEL_NONE,
    FGE_LOG_LEVEL_COUNT
} fge_log_level_t;

/* -------------------------------------------------------------------------- */
/* Log categories — each subsystem gets its own category                      */
/* -------------------------------------------------------------------------- */

typedef enum {
    FGE_LOG_CAT_GENERAL = 0,
    FGE_LOG_CAT_MEMORY,
    FGE_LOG_CAT_PLATFORM,
    FGE_LOG_CAT_RENDERER,
    FGE_LOG_CAT_AUDIO,
    FGE_LOG_CAT_PHYSICS,
    FGE_LOG_CAT_SCENE,
    FGE_LOG_CAT_NETWORK,
    FGE_LOG_CAT_NET_TCP,
    FGE_LOG_CAT_NET_UDP,
    FGE_LOG_CAT_NET_PROTOCOL,
    FGE_LOG_CAT_ASSET,
    FGE_LOG_CAT_ECS,
    FGE_LOG_CAT_INPUT,
    FGE_LOG_CAT_UI,
    FGE_LOG_CAT_SCRIPT,
    FGE_LOG_CAT_AI,
    FGE_LOG_CAT_PROFILE,
    FGE_LOG_CAT_SECURITY,
    FGE_LOG_CAT_COUNT
} fge_log_category_t;

static inline const char *fge_log_cat_name(fge_log_category_t cat) {
    static const char *names[] = {
        "GENERAL", "MEMORY", "PLATFORM", "RENDERER", "AUDIO",
        "PHYSICS", "SCENE", "NETWORK", "NET_TCP", "NET_UDP",
        "NET_PROTOCOL", "ASSET", "ECS", "INPUT", "UI",
        "SCRIPT", "AI", "PROFILE", "SECURITY"
    };
    return (size_t)cat < FGE_ARRAY_COUNT(names) ? names[cat] : "UNKNOWN";
}

static inline const char *fge_log_level_name(fge_log_level_t level) {
    static const char *names[] = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "NONE" };
    return (size_t)level < FGE_ARRAY_COUNT(names) ? names[level] : "?";
}

/* -------------------------------------------------------------------------- */
/* Log entry — stored in ring buffer                                          */
/* -------------------------------------------------------------------------- */

#define FGE_LOG_MAX_MESSAGE 1024
#define FGE_LOG_MAX_KV_PAIRS 8

typedef struct {
    const char *key;
    const char *value;
} fge_log_kv_t;

typedef struct {
    uint64_t timestamp_ns;
    fge_log_level_t level;
    fge_log_category_t category;
    uint32_t thread_id;
    const char *file;
    int line;
    const char *func;
    char message[FGE_LOG_MAX_MESSAGE];
    fge_log_kv_t kv[FGE_LOG_MAX_KV_PAIRS];
    int kv_count;
} fge_log_entry_t;

/* -------------------------------------------------------------------------- */
/* Ring buffer — lock-free SPSC or MPMC with spinlock                         */
/* -------------------------------------------------------------------------- */

typedef struct {
    fge_log_entry_t *entries;
    uint32_t capacity;      /* must be power of 2 */
    fge_atomic_uint_t head; /* write index */
    fge_atomic_uint_t tail; /* read index */
} fge_log_ring_t;

bool fge_log_ring_init(fge_log_ring_t *ring, uint32_t capacity);
void fge_log_ring_free(fge_log_ring_t *ring);
bool fge_log_ring_push(fge_log_ring_t *ring, const fge_log_entry_t *entry);
bool fge_log_ring_pop(fge_log_ring_t *ring, fge_log_entry_t *out);
uint32_t fge_log_ring_count(const fge_log_ring_t *ring);

/* -------------------------------------------------------------------------- */
/* Sink — output destination                                                  */
/* -------------------------------------------------------------------------- */

typedef enum {
    FGE_LOG_SINK_STDOUT,
    FGE_LOG_SINK_FILE,
    FGE_LOG_SINK_CALLBACK,
    FGE_LOG_SINK_NETWORK,
} fge_log_sink_type_t;

typedef void (*fge_log_sink_cb_t)(const fge_log_entry_t *entry, void *userdata);

typedef struct fge_log_sink fge_log_sink_t;
struct fge_log_sink {
    fge_log_sink_type_t type;
    fge_log_level_t min_level;
    uint32_t category_mask; /* bitmask of enabled categories */
    union {
        FILE *file;
        struct { fge_log_sink_cb_t fn; void *userdata; } callback;
        struct { const char *host; uint16_t port; int fd; } network;
    };
    fge_log_sink_t *next;
};

/* -------------------------------------------------------------------------- */
/* Logger — global state                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    fge_log_level_t category_levels[FGE_LOG_CAT_COUNT];
    fge_log_ring_t ring;
    fge_log_sink_t *sinks;
    fge_spinlock_t lock;
    bool async_mode;        /* if true, logging goes to ring buffer */
    bool running;
    uint64_t dropped_count; /* entries dropped due to full ring */
    uint64_t total_count;
} fge_logger_t;

extern fge_logger_t g_fge_logger;

/* -------------------------------------------------------------------------- */
/* Initialization / shutdown                                                  */
/* -------------------------------------------------------------------------- */

void fge_log_init(fge_log_level_t default_level);
void fge_log_shutdown(void);

/* Start async output thread. Must call before any logging if async_mode desired. */
bool fge_log_start_async(void);
void fge_log_stop_async(void);

/* -------------------------------------------------------------------------- */
/* Sink management                                                            */
/* -------------------------------------------------------------------------- */

void fge_log_add_sink_stdout(fge_log_level_t min_level);
bool fge_log_add_sink_file(fge_log_level_t min_level, const char *path);
void fge_log_add_sink_callback(fge_log_level_t min_level, fge_log_sink_cb_t cb, void *userdata);
void fge_log_set_category_level(fge_log_category_t cat, fge_log_level_t level);

/* -------------------------------------------------------------------------- */
/* Core logging                                                               */
/* -------------------------------------------------------------------------- */

void fge_log_submit(fge_log_category_t cat, fge_log_level_t level,
                    const char *file, int line, const char *func,
                    const char *fmt, ...);

void fge_log_submitv(fge_log_category_t cat, fge_log_level_t level,
                     const char *file, int line, const char *func,
                     const char *fmt, va_list args);

/* With key-value structured data */
void fge_log_submit_kvs(fge_log_category_t cat, fge_log_level_t level,
                        const char *file, int line, const char *func,
                        const char *fmt, int kv_count, ...);

/* Flush all sinks (blocks until async queue drained) */
void fge_log_flush(void);

/* -------------------------------------------------------------------------- */
/* Convenience macros — category-aware, compile-time filtered                 */
/* -------------------------------------------------------------------------- */

#define FGE_LOG_ENABLED(cat, lvl) ((lvl) >= FGE_LOG_COMPILE_LEVEL)

#if FGE_LOG_COMPILE_LEVEL <= FGE_LOG_LEVEL_TRACE
#  define FGE_TRACE(cat, ...) fge_log_submit((cat), FGE_LOG_LEVEL_TRACE, __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define FGE_TRACE(cat, ...) ((void)0)
#endif

#if FGE_LOG_COMPILE_LEVEL <= FGE_LOG_LEVEL_DEBUG
#  define FGE_DEBUG(cat, ...) fge_log_submit((cat), FGE_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#  define FGE_DEBUG(cat, ...) ((void)0)
#endif

#define FGE_INFO(cat, ...)  fge_log_submit((cat), FGE_LOG_LEVEL_INFO,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#define FGE_WARN(cat, ...)  fge_log_submit((cat), FGE_LOG_LEVEL_WARN,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#define FGE_ERROR(cat, ...) fge_log_submit((cat), FGE_LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define FGE_FATAL(cat, ...) fge_log_submit((cat), FGE_LOG_LEVEL_FATAL, __FILE__, __LINE__, __func__, __VA_ARGS__)

/* Structured logging with key-value pairs */
#define FGE_TRACE_KV(cat, fmt, ...) fge_log_submit_kvs((cat), FGE_LOG_LEVEL_TRACE, __FILE__, __LINE__, __func__, fmt, __VA_ARGS__)
#define FGE_DEBUG_KV(cat, fmt, ...) fge_log_submit_kvs((cat), FGE_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, fmt, __VA_ARGS__)

/* Conditional logging */
#define FGE_LOG_IF(cond, lvl, cat, ...) do { if (cond) fge_log_submit((cat), (lvl), __FILE__, __LINE__, __func__, __VA_ARGS__); } while (0)

/* -------------------------------------------------------------------------- */
/* Scope tracing — automatic function/scope timing                            */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    fge_log_category_t cat;
    uint64_t start_ns;
} fge_log_scope_t;

void fge_log_scope_begin(fge_log_scope_t *scope, fge_log_category_t cat, const char *name);
void fge_log_scope_end(fge_log_scope_t *scope);

#define FGE_SCOPE(cat, name) \
    fge_log_scope_t _fge_scope __attribute__((cleanup(fge_log_scope_end_auto))) = {0}; \
    fge_log_scope_begin(&_fge_scope, (cat), (name))

/* Internal: used by cleanup attribute */
void fge_log_scope_end_auto(fge_log_scope_t *scope);

/* -------------------------------------------------------------------------- */
/* Panic / fatal error                                                        */
/* -------------------------------------------------------------------------- */

FGE_NORETURN
void fge_panic(const char *file, int line, const char *func, const char *fmt, ...);

#define FGE_PANIC(...) fge_panic(__FILE__, __LINE__, __func__, __VA_ARGS__)

#define FGE_CHECK(cond, ...) do { \
    if (FGE_UNLIKELY(!(cond))) { \
        FGE_ERROR(FGE_LOG_CAT_GENERAL, "CHECK failed: " #cond " — " __VA_ARGS__); \
        FGE_PANIC("CHECK failed: " #cond); \
    } \
} while (0)

#define FGE_CHECK_RET(cond, ret, ...) do { \
    if (FGE_UNLIKELY(!(cond))) { \
        FGE_ERROR(FGE_LOG_CAT_GENERAL, "CHECK failed: " #cond " — " __VA_ARGS__); \
        return (ret); \
    } \
} while (0)

#define FGE_CHECK_GOTO(cond, label, ...) do { \
    if (FGE_UNLIKELY(!(cond))) { \
        FGE_ERROR(FGE_LOG_CAT_GENERAL, "CHECK failed: " #cond " — " __VA_ARGS__); \
        goto label; \
    } \
} while (0)

#endif /* FORGE_LOG_H */
