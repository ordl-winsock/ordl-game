/*
 * src/core/memory.c — Arena, pool, scratch, and heap tracking
 */

#define _GNU_SOURCE
#include "forge/memory.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>

/* -------------------------------------------------------------------------- */
/* Arena                                                                      */
/* -------------------------------------------------------------------------- */

bool fge_arena_init(fge_arena_t *a, size_t size, const char *name) {
    if (!a || size == 0) return false;
    a->base = FGE_MALLOC(size);
    if (!a->base) {
        FGE_ERROR(FGE_LOG_CAT_MEMORY, "Arena '%s' allocation failed: %zu bytes", name ? name : "?", size);
        return false;
    }
    a->size = size;
    a->used = 0;
    a->align = 16;
    a->name = name;
    a->alloc_count = 0;
    FGE_DEBUG(FGE_LOG_CAT_MEMORY, "Arena '%s' created: %zu bytes @ %p", name ? name : "?", size, (void*)a->base);
    return true;
}

void fge_arena_init_fixed(fge_arena_t *a, void *buffer, size_t size, const char *name) {
    if (!a || !buffer || size == 0) return;
    a->base = buffer;
    a->size = size;
    a->used = 0;
    a->align = 16;
    a->name = name;
    a->alloc_count = 0;
    FGE_DEBUG(FGE_LOG_CAT_MEMORY, "Arena '%s' fixed: %zu bytes @ %p", name ? name : "?", size, buffer);
}

void fge_arena_free(fge_arena_t *a) {
    if (!a) return;
    if (a->base) {
        FGE_DEBUG(FGE_LOG_CAT_MEMORY, "Arena '%s' freed: %zu/%zu bytes used (%.1f%%)",
                  a->name ? a->name : "?", a->used, a->size, fge_arena_utilization(a) * 100.0f);
        FGE_FREE(a->base);
        a->base = nullptr;
    }
    a->size = a->used = 0;
}

void *fge_arena_alloc_aligned(fge_arena_t *a, size_t size, size_t align) {
    if (!a || !a->base || size == 0) return nullptr;
    if (align < 1) align = 1;
    /* Align 'used' */
    size_t aligned = (a->used + align - 1) & ~(align - 1);
    if (aligned + size > a->size) {
        FGE_WARN(FGE_LOG_CAT_MEMORY, "Arena '%s' OOM: requested %zu, available %zu",
                 a->name ? a->name : "?", size, a->size - aligned);
        return nullptr;
    }
    void *p = a->base + aligned;
    a->used = aligned + size;
    a->alloc_count++;
    return p;
}

void *fge_arena_alloc(fge_arena_t *a, size_t size) {
    return fge_arena_alloc_aligned(a, size, a ? a->align : 16);
}

char *fge_arena_strdup(fge_arena_t *a, const char *s) {
    if (!a || !s) return nullptr;
    size_t len = strlen(s) + 1;
    char *p = fge_arena_alloc(a, len);
    if (p) memcpy(p, s, len);
    return p;
}

/* -------------------------------------------------------------------------- */
/* Pool                                                                       */
/* -------------------------------------------------------------------------- */

bool fge_pool_init(fge_pool_t *p, size_t obj_size, size_t obj_align, size_t block_cap, const char *name) {
    if (!p || obj_size == 0 || block_cap == 0) return false;
    if (obj_size < sizeof(void *)) obj_size = sizeof(void *);
    if (obj_align < alignof(void *)) obj_align = alignof(void *);

    p->obj_size = obj_size;
    p->obj_align = obj_align;
    p->block_cap = block_cap;
    p->total_objs = 0;
    p->free_count = 0;
    p->free_list = nullptr;
    p->blocks = nullptr;
    p->blocks_cap = 0;
    p->blocks_count = 0;
    p->name = name;
    fge_spinlock_init(&p->lock);
    return true;
}

void fge_pool_free(fge_pool_t *p) {
    if (!p) return;
    FGE_DEBUG(FGE_LOG_CAT_MEMORY, "Pool '%s' freed: %zu blocks, %zu total objs",
              p->name ? p->name : "?", p->blocks_count, p->total_objs);
    for (size_t i = 0; i < p->blocks_count; i++) {
        FGE_FREE(p->blocks[i]);
    }
    FGE_FREE(p->blocks);
    memset(p, 0, sizeof(*p));
}

void fge_pool_reset(fge_pool_t *p) {
    if (!p || !p->blocks) return;
    p->free_list = nullptr;
    p->free_count = 0;
    for (size_t i = 0; i < p->blocks_count; i++) {
        uint8_t *block = p->blocks[i];
        for (size_t j = 0; j < p->block_cap; j++) {
            fge_pool_chunk_t *chunk = (fge_pool_chunk_t *)(block + j * p->obj_size);
            chunk->next = p->free_list;
            p->free_list = chunk;
            p->free_count++;
        }
    }
}

static bool fge_pool_grow(fge_pool_t *p) {
    size_t block_bytes = p->block_cap * p->obj_size + p->obj_align;
    uint8_t *block = FGE_MALLOC(block_bytes);
    if (!block) return false;
    /* Align block start */
    uintptr_t addr = (uintptr_t)block;
    uintptr_t aligned = (addr + p->obj_align - 1) & ~(p->obj_align - 1);
    uint8_t *data = (uint8_t *)aligned;

    /* Grow blocks array */
    if (p->blocks_count >= p->blocks_cap) {
        size_t new_cap = p->blocks_cap ? p->blocks_cap * 2 : 4;
        uint8_t **new_blocks = FGE_REALLOC(p->blocks, new_cap * sizeof(uint8_t *));
        if (!new_blocks) { FGE_FREE(block); return false; }
        p->blocks = new_blocks;
        p->blocks_cap = new_cap;
    }
    p->blocks[p->blocks_count++] = block;

    /* Add to free list */
    for (size_t i = 0; i < p->block_cap; i++) {
        fge_pool_chunk_t *chunk = (fge_pool_chunk_t *)(data + i * p->obj_size);
        chunk->next = p->free_list;
        p->free_list = chunk;
    }
    p->free_count += p->block_cap;
    p->total_objs += p->block_cap;
    return true;
}

void *fge_pool_alloc(fge_pool_t *p) {
    if (!p) return nullptr;
    fge_spinlock_lock(&p->lock);
    if (!p->free_list) {
        if (!fge_pool_grow(p)) {
            fge_spinlock_unlock(&p->lock);
            FGE_ERROR(FGE_LOG_CAT_MEMORY, "Pool '%s' OOM", p->name ? p->name : "?");
            return nullptr;
        }
    }
    fge_pool_chunk_t *chunk = p->free_list;
    p->free_list = chunk->next;
    p->free_count--;
    fge_spinlock_unlock(&p->lock);
    memset(chunk, 0, p->obj_size);
    return chunk;
}

void fge_pool_free_obj(fge_pool_t *p, void *obj) {
    if (!p || !obj) return;
    fge_spinlock_lock(&p->lock);
    fge_pool_chunk_t *chunk = (fge_pool_chunk_t *)obj;
    chunk->next = p->free_list;
    p->free_list = chunk;
    p->free_count++;
    fge_spinlock_unlock(&p->lock);
}

/* -------------------------------------------------------------------------- */
/* Lock-free pool (SPSC)                                                      */
/* -------------------------------------------------------------------------- */

bool fge_lf_pool_init(fge_lf_pool_t *p, uint32_t capacity) {
    if (!p || capacity == 0) return false;
    if (!fge_ispow2_u32(capacity)) capacity = fge_next_pow2_u32(capacity);
    p->buffer = FGE_CALLOC(capacity, sizeof(void *));
    if (!p->buffer) return false;
    p->capacity = capacity;
    FGE_ATOMIC_STORE(&p->head, 0);
    FGE_ATOMIC_STORE(&p->tail, 0);
    return true;
}

void fge_lf_pool_free(fge_lf_pool_t *p) {
    if (!p) return;
    FGE_FREE(p->buffer);
    p->buffer = nullptr;
    p->capacity = 0;
}

bool fge_lf_pool_push(fge_lf_pool_t *p, void *obj) {
    if (!p || !obj) return false;
    uint32_t head = FGE_ATOMIC_LOAD(&p->head);
    uint32_t next = (head + 1) & (p->capacity - 1);
    if (next == FGE_ATOMIC_LOAD_ACQ(&p->tail)) return false; /* full */
    p->buffer[head & (p->capacity - 1)] = obj;
    FGE_ATOMIC_STORE_REL(&p->head, next);
    return true;
}

bool fge_lf_pool_pop(fge_lf_pool_t *p, void **out) {
    if (!p || !out) return false;
    uint32_t tail = FGE_ATOMIC_LOAD(&p->tail);
    if (tail == FGE_ATOMIC_LOAD_ACQ(&p->head)) return false; /* empty */
    *out = p->buffer[tail & (p->capacity - 1)];
    FGE_ATOMIC_STORE_REL(&p->tail, (tail + 1) & (p->capacity - 1));
    return true;
}

/* -------------------------------------------------------------------------- */
/* Scratch allocator — thread-local frame arena                               */
/* -------------------------------------------------------------------------- */

static pthread_key_t s_scratch_key;
static pthread_once_t s_scratch_once = PTHREAD_ONCE_INIT;

static void fge_scratch_destroy(void *ptr) {
    fge_arena_t *a = (fge_arena_t *)ptr;
    if (a) {
        fge_arena_free(a);
        FGE_FREE(a);
    }
}

static void fge_scratch_init_once(void) {
    pthread_key_create(&s_scratch_key, fge_scratch_destroy);
}

void fge_scratch_init_thread(void) {
    pthread_once(&s_scratch_once, fge_scratch_init_once);
    fge_arena_t *a = FGE_MALLOC(sizeof(fge_arena_t));
    if (!a) return;
    if (!fge_arena_init(a, FGE_SCRATCH_SIZE, "scratch")) {
        FGE_FREE(a);
        return;
    }
    pthread_setspecific(s_scratch_key, a);
}

void fge_scratch_shutdown_thread(void) {
    fge_arena_t *a = pthread_getspecific(s_scratch_key);
    if (a) {
        fge_scratch_destroy(a);
        pthread_setspecific(s_scratch_key, nullptr);
    }
}

fge_arena_t *fge_scratch_get(void) {
    pthread_once(&s_scratch_once, fge_scratch_init_once);
    fge_arena_t *a = pthread_getspecific(s_scratch_key);
    if (FGE_UNLIKELY(!a)) {
        fge_scratch_init_thread();
        a = pthread_getspecific(s_scratch_key);
    }
    return a;
}

void fge_scratch_reset(void) {
    fge_arena_t *a = fge_scratch_get();
    if (a) fge_arena_reset(a);
}

/* -------------------------------------------------------------------------- */
/* Virtual memory                                                             */
/* -------------------------------------------------------------------------- */

void *fge_vm_reserve(size_t size) {
    void *p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
}

bool fge_vm_commit(void *ptr, size_t size) {
    if (!ptr || size == 0) return false;
    return mprotect(ptr, size, PROT_READ | PROT_WRITE) == 0;
}

void fge_vm_decommit(void *ptr, size_t size) {
    if (!ptr || size == 0) return;
    mprotect(ptr, size, PROT_NONE);
    madvise(ptr, size, MADV_DONTNEED);
}

void fge_vm_release(void *ptr, size_t size) {
    if (!ptr) return;
    munmap(ptr, size);
}

size_t fge_vm_page_size(void) {
    return (size_t)sysconf(_SC_PAGESIZE);
}

/* -------------------------------------------------------------------------- */
/* Heap tracking                                                              */
/* -------------------------------------------------------------------------- */

#ifdef FGE_ENABLE_HEAP_TRACKING

#define FGE_HEAP_TRACK_MAX 65536

typedef struct {
    void *ptr;
    size_t size;
    const char *file;
    int line;
    const char *func;
    bool active;
} fge_heap_track_entry_t;

static struct {
    fge_heap_track_entry_t entries[FGE_HEAP_TRACK_MAX];
    fge_spinlock_t lock;
    fge_heap_stats_t stats;
} s_heap_track = {0};

static int fge_heap_find_slot(void) {
    for (int i = 0; i < FGE_HEAP_TRACK_MAX; i++) {
        if (!s_heap_track.entries[i].active) return i;
    }
    return -1;
}

static int fge_heap_find_ptr(void *p) {
    for (int i = 0; i < FGE_HEAP_TRACK_MAX; i++) {
        if (s_heap_track.entries[i].active && s_heap_track.entries[i].ptr == p) return i;
    }
    return -1;
}

void *fge_tracked_alloc(size_t size, const char *file, int line, const char *func) {
    void *p = malloc(size);
    if (!p) return nullptr;
    fge_spinlock_lock(&s_heap_track.lock);
    int idx = fge_heap_find_slot();
    if (idx >= 0) {
        s_heap_track.entries[idx] = (fge_heap_track_entry_t){p, size, file, line, func, true};
    }
    s_heap_track.stats.alloc_count++;
    s_heap_track.stats.total_bytes_allocated += size;
    s_heap_track.stats.current_bytes += size;
    if (s_heap_track.stats.current_bytes > s_heap_track.stats.peak_bytes)
        s_heap_track.stats.peak_bytes = s_heap_track.stats.current_bytes;
    if (s_heap_track.stats.alloc_count - s_heap_track.stats.free_count > s_heap_track.stats.peak_allocs)
        s_heap_track.stats.peak_allocs = s_heap_track.stats.alloc_count - s_heap_track.stats.free_count;
    fge_spinlock_unlock(&s_heap_track.lock);
    return p;
}

void *fge_tracked_calloc(size_t count, size_t size, const char *file, int line, const char *func) {
    void *p = calloc(count, size);
    if (!p) return nullptr;
    size_t total = count * size;
    fge_spinlock_lock(&s_heap_track.lock);
    int idx = fge_heap_find_slot();
    if (idx >= 0) {
        s_heap_track.entries[idx] = (fge_heap_track_entry_t){p, total, file, line, func, true};
    }
    s_heap_track.stats.alloc_count++;
    s_heap_track.stats.total_bytes_allocated += total;
    s_heap_track.stats.current_bytes += total;
    if (s_heap_track.stats.current_bytes > s_heap_track.stats.peak_bytes)
        s_heap_track.stats.peak_bytes = s_heap_track.stats.current_bytes;
    fge_spinlock_unlock(&s_heap_track.lock);
    return p;
}

void *fge_tracked_realloc(void *p, size_t size, const char *file, int line, const char *func) {
    fge_spinlock_lock(&s_heap_track.lock);
    int idx = fge_heap_find_ptr(p);
    size_t old_size = (idx >= 0) ? s_heap_track.entries[idx].size : 0;
    fge_spinlock_unlock(&s_heap_track.lock);

    void *np = realloc(p, size);
    if (!np) return nullptr;

    fge_spinlock_lock(&s_heap_track.lock);
    if (idx >= 0) s_heap_track.entries[idx].active = false;
    int nidx = fge_heap_find_slot();
    if (nidx >= 0) {
        s_heap_track.entries[nidx] = (fge_heap_track_entry_t){np, size, file, line, func, true};
    }
    s_heap_track.stats.realloc_count++;
    s_heap_track.stats.total_bytes_allocated += size;
    s_heap_track.stats.current_bytes += size - old_size;
    if (s_heap_track.stats.current_bytes > s_heap_track.stats.peak_bytes)
        s_heap_track.stats.peak_bytes = s_heap_track.stats.current_bytes;
    fge_spinlock_unlock(&s_heap_track.lock);
    return np;
}

void fge_tracked_free(void *p, const char *file, int line, const char *func) {
    (void)file; (void)line; (void)func;
    if (!p) return;
    fge_spinlock_lock(&s_heap_track.lock);
    int idx = fge_heap_find_ptr(p);
    size_t size = 0;
    if (idx >= 0) {
        size = s_heap_track.entries[idx].size;
        s_heap_track.entries[idx].active = false;
    } else {
        FGE_WARN(FGE_LOG_CAT_MEMORY, "Double-free or untracked free: %p", p);
    }
    s_heap_track.stats.free_count++;
    s_heap_track.stats.total_bytes_freed += size;
    s_heap_track.stats.current_bytes -= size;
    fge_spinlock_unlock(&s_heap_track.lock);
    free(p);
}

void fge_heap_stats_get(fge_heap_stats_t *out) {
    if (!out) return;
    fge_spinlock_lock(&s_heap_track.lock);
    *out = s_heap_track.stats;
    fge_spinlock_unlock(&s_heap_track.lock);
}

void fge_heap_dump_leaks(void) {
    fge_spinlock_lock(&s_heap_track.lock);
    int leak_count = 0;
    size_t leak_bytes = 0;
    for (int i = 0; i < FGE_HEAP_TRACK_MAX; i++) {
        if (s_heap_track.entries[i].active) {
            fge_heap_track_entry_t *e = &s_heap_track.entries[i];
            FGE_ERROR(FGE_LOG_CAT_MEMORY, "LEAK: %p  %zu bytes  %s:%d %s",
                      e->ptr, e->size, e->file ? e->file : "?", e->line, e->func ? e->func : "?");
            leak_count++;
            leak_bytes += e->size;
        }
    }
    fge_spinlock_unlock(&s_heap_track.lock);
    if (leak_count > 0) {
        FGE_ERROR(FGE_LOG_CAT_MEMORY, "=== %d leaks, %zu bytes total ===", leak_count, leak_bytes);
    } else {
        FGE_INFO(FGE_LOG_CAT_MEMORY, "=== No memory leaks detected ===");
    }
}

void fge_heap_dump_stats(void) {
    fge_heap_stats_t s;
    fge_heap_stats_get(&s);
    FGE_INFO(FGE_LOG_CAT_MEMORY,
             "Heap stats: allocs=%lu frees=%lu reallocs=%lu current=%lu peak=%lu peak_allocs=%lu",
             (unsigned long)s.alloc_count, (unsigned long)s.free_count, (unsigned long)s.realloc_count,
             (unsigned long)s.current_bytes, (unsigned long)s.peak_bytes, (unsigned long)s.peak_allocs);
}

#endif /* FGE_ENABLE_HEAP_TRACKING */
