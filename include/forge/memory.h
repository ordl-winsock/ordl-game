/*
 * forge/memory.h — High-performance memory allocators for real-time games
 *
 * Features:
 *   - Arena: linear bump, O(1) alloc, O(1) reset, no fragmentation
 *   - Pool: fixed-size O(1) alloc/free, cache-friendly
 *   - Scratch: per-thread frame allocator, zero contention
 *   - Tracking: allocation graphs, leak detection, double-free detection
 *   - Virtual memory: large page reservations, commit/decommit
 *
 * Pure C23, zero external dependencies.
 */

#ifndef FORGE_MEMORY_H
#define FORGE_MEMORY_H

#include "forge/core.h"
#include "forge/log.h"
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Arena — linear bump allocator                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t *base;
    size_t   size;
    size_t   used;
    size_t   align;
    const char *name;       /* for debugging */
    uint32_t alloc_count;   /* for debugging */
} fge_arena_t;

bool fge_arena_init(fge_arena_t *a, size_t size, const char *name);
void fge_arena_init_fixed(fge_arena_t *a, void *buffer, size_t size, const char *name);
void fge_arena_free(fge_arena_t *a);

FGE_INLINE void fge_arena_reset(fge_arena_t *a) {
    if (a) { a->used = 0; a->alloc_count = 0; }
}

FGE_INLINE size_t fge_arena_remaining(const fge_arena_t *a) {
    return a ? a->size - a->used : 0;
}

FGE_INLINE float fge_arena_utilization(const fge_arena_t *a) {
    return (a && a->size > 0) ? (float)a->used / (float)a->size : 0.0f;
}

[[nodiscard]] void *fge_arena_alloc(fge_arena_t *a, size_t size);
[[nodiscard]] void *fge_arena_alloc_aligned(fge_arena_t *a, size_t size, size_t align);

[[nodiscard]]
FGE_INLINE void *fge_arena_calloc(fge_arena_t *a, size_t count, size_t size) {
    size_t total = count * size;
    void *p = fge_arena_alloc(a, total);
    if (p) memset(p, 0, total);
    return p;
}

[[nodiscard]] char *fge_arena_strdup(fge_arena_t *a, const char *s);

FGE_INLINE size_t fge_arena_checkpoint(fge_arena_t *a) { return a ? a->used : 0; }
FGE_INLINE void fge_arena_pop_to(fge_arena_t *a, size_t checkpoint) {
    if (a && checkpoint <= a->size) a->used = checkpoint;
}

#define FGE_ARENA_ALLOC(a, T)       ((T *)fge_arena_alloc((a), sizeof(T)))
#define FGE_ARENA_ALLOC_N(a, T, n)  ((T *)fge_arena_alloc((a), sizeof(T) * (n)))
#define FGE_ARENA_CALLOC(a, T)      ((T *)fge_arena_calloc((a), 1, sizeof(T)))
#define FGE_ARENA_CALLOC_N(a, T, n) ((T *)fge_arena_calloc((a), (n), sizeof(T)))

/* -------------------------------------------------------------------------- */
/* Pool — fixed-size object pool                                              */
/* -------------------------------------------------------------------------- */

typedef struct fge_pool_chunk fge_pool_chunk_t;
struct fge_pool_chunk {
    fge_pool_chunk_t *next;
};

typedef struct {
    size_t obj_size;
    size_t obj_align;
    size_t block_cap;       /* objects per block */
    size_t total_objs;      /* total allocated objects */
    size_t free_count;      /* objects in free list */
    fge_pool_chunk_t *free_list;
    uint8_t **blocks;       /* array of block pointers */
    size_t blocks_cap;
    size_t blocks_count;
    const char *name;
    fge_spinlock_t lock;
} fge_pool_t;

bool fge_pool_init(fge_pool_t *p, size_t obj_size, size_t obj_align, size_t block_cap, const char *name);
void fge_pool_free(fge_pool_t *p);
void fge_pool_reset(fge_pool_t *p);

[[nodiscard]] void *fge_pool_alloc(fge_pool_t *p);
void fge_pool_free_obj(fge_pool_t *p, void *obj);

/* Lock-free pool (single producer, single consumer) */
typedef struct {
    fge_atomic_uint_t head;
    fge_atomic_uint_t tail;
    uint32_t capacity;
    void **buffer; /* array of pointers */
} fge_lf_pool_t;

bool fge_lf_pool_init(fge_lf_pool_t *p, uint32_t capacity);
void fge_lf_pool_free(fge_lf_pool_t *p);
bool fge_lf_pool_push(fge_lf_pool_t *p, void *obj);
bool fge_lf_pool_pop(fge_lf_pool_t *p, void **out);

/* -------------------------------------------------------------------------- */
/* Scratch allocator — per-thread frame arena                                 */
/* -------------------------------------------------------------------------- */

#define FGE_SCRATCH_SIZE FGE_MIB(4)

fge_arena_t *fge_scratch_get(void);
void fge_scratch_reset(void);
void fge_scratch_init_thread(void);
void fge_scratch_shutdown_thread(void);

#define FGE_SCRATCH_ALLOC(size)      fge_arena_alloc(fge_scratch_get(), (size))
#define FGE_SCRATCH_ALLOC_N(T, n)    ((T *)fge_arena_alloc(fge_scratch_get(), sizeof(T) * (n)))
#define FGE_SCRATCH_CALLOC_N(T, n)   ((T *)fge_arena_calloc(fge_scratch_get(), (n), sizeof(T)))

/* -------------------------------------------------------------------------- */
/* Virtual memory abstraction                                                 */
/* -------------------------------------------------------------------------- */

[[nodiscard]] void *fge_vm_reserve(size_t size);
bool fge_vm_commit(void *ptr, size_t size);
void fge_vm_decommit(void *ptr, size_t size);
void fge_vm_release(void *ptr, size_t size);
size_t fge_vm_page_size(void);

/* -------------------------------------------------------------------------- */
/* Heap tracking — allocation graphs, leak detection                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t realloc_count;
    uint64_t total_bytes_allocated;
    uint64_t total_bytes_freed;
    uint64_t current_bytes;
    uint64_t peak_bytes;
    uint64_t peak_allocs;
} fge_heap_stats_t;

#ifdef FGE_ENABLE_HEAP_TRACKING

void *fge_tracked_alloc(size_t size, const char *file, int line, const char *func);
void *fge_tracked_calloc(size_t count, size_t size, const char *file, int line, const char *func);
void *fge_tracked_realloc(void *p, size_t size, const char *file, int line, const char *func);
void  fge_tracked_free(void *p, const char *file, int line, const char *func);
void  fge_heap_stats_get(fge_heap_stats_t *out);
void  fge_heap_dump_leaks(void);
void  fge_heap_dump_stats(void);

#define FGE_MALLOC(size)       fge_tracked_alloc((size), __FILE__, __LINE__, __func__)
#define FGE_CALLOC(n, size)    fge_tracked_calloc((n), (size), __FILE__, __LINE__, __func__)
#define FGE_REALLOC(p, size)   fge_tracked_realloc((p), (size), __FILE__, __LINE__, __func__)
#define FGE_FREE(p)            fge_tracked_free((p), __FILE__, __LINE__, __func__)

#else

#define FGE_MALLOC(size)       malloc(size)
#define FGE_CALLOC(n, size)    calloc((n), (size))
#define FGE_REALLOC(p, size)   realloc((p), (size))
#define FGE_FREE(p)            free(p)
#define fge_heap_stats_get(o)  memset((o), 0, sizeof(fge_heap_stats_t))
#define fge_heap_dump_leaks()  ((void)0)
#define fge_heap_dump_stats()  ((void)0)

#endif /* FGE_ENABLE_HEAP_TRACKING */

/* -------------------------------------------------------------------------- */
/* Memory fill / copy helpers                                                 */
/* -------------------------------------------------------------------------- */

FGE_INLINE void fge_memzero(void *p, size_t n) { memset(p, 0, n); }
FGE_INLINE void fge_memcpy(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
FGE_INLINE int  fge_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }

/* Secure zeroize — prevents dead-store elimination */
FGE_INLINE void fge_secure_zero(void *p, size_t n) {
    volatile uint8_t *vp = (volatile uint8_t *)p;
    for (size_t i = 0; i < n; i++) vp[i] = 0;
}

#endif /* FORGE_MEMORY_H */
