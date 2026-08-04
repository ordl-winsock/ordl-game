/*
 * forge/ecs.h — High-performance Entity-Component-System
 *
 * Features:
 *   - Sparse-set storage: O(1) add/remove/has/lookup
 *   - SoA component storage for cache-friendly iteration
 *   - Component type IDs via macro registration
 *   - System execution with archetype filtering
 *   - 32-bit entity IDs with generation for ABA safety
 *   - Handle-based entity references (safe dangling detection)
 *
 * Pure C23, zero external dependencies.
 */

#ifndef FORGE_ECS_H
#define FORGE_ECS_H

#include "forge/core.h"
#include "forge/memory.h"
#include "forge/log.h"

/* -------------------------------------------------------------------------- */
/* Entity                                                                     */
/* -------------------------------------------------------------------------- */

typedef uint32_t fge_entity_t;
#define FGE_ENTITY_INVALID 0u
#define FGE_ENTITY_INDEX(e) ((e) & 0x00FFFFFFu)
#define FGE_ENTITY_GENERATION(e) (((e) >> 24) & 0xFFu)
#define FGE_ENTITY_MAKE(idx, gen) (((uint32_t)(gen) << 24) | ((idx) & 0x00FFFFFFu))
#define FGE_MAX_ENTITIES 0x00FFFFFFu /* ~16.7M entities */

/* -------------------------------------------------------------------------- */
/* Component type registration                                                */
/* -------------------------------------------------------------------------- */

#define FGE_MAX_COMPONENT_TYPES 64

typedef uint64_t fge_component_mask_t;

typedef struct {
    const char *name;
    size_t size;
    size_t align;
    void (*init)(void *component);
    void (*destroy)(void *component);
    void (*copy)(void *dst, const void *src);
} fge_component_desc_t;

typedef struct {
    fge_component_desc_t descs[FGE_MAX_COMPONENT_TYPES];
    uint32_t count;
} fge_component_registry_t;

/* Register a component type, returns type index */
uint32_t fge_ecs_register_component(fge_component_registry_t *reg,
                                    const char *name, size_t size, size_t align);

#define FGE_ECS_REGISTER_COMPONENT(reg, T) \
    fge_ecs_register_component((reg), #T, sizeof(T), alignof(T))

/* -------------------------------------------------------------------------- */
/* Sparse set — O(1) add/remove/contains, cache-friendly iteration            */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t *sparse;   /* entity index -> dense index, or ~0u */
    uint32_t *dense;    /* dense index -> entity */
    uint32_t  capacity;
    uint32_t  count;
} fge_sparse_set_t;

bool fge_sparse_set_init(fge_sparse_set_t *s, uint32_t max_entities);
void fge_sparse_set_free(fge_sparse_set_t *s);
void fge_sparse_set_clear(fge_sparse_set_t *s);
bool fge_sparse_set_add(fge_sparse_set_t *s, uint32_t entity);
bool fge_sparse_set_remove(fge_sparse_set_t *s, uint32_t entity);
bool fge_sparse_set_contains(const fge_sparse_set_t *s, uint32_t entity);

/* -------------------------------------------------------------------------- */
/* Component storage — SoA arrays per component type                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    fge_sparse_set_t entities;   /* which entities have this component */
    uint8_t *data;               /* SoA: component data, stride = desc.size */
    uint32_t capacity;
    uint32_t count;
    const fge_component_desc_t *desc;
} fge_component_storage_t;

bool fge_component_storage_init(fge_component_storage_t *st, const fge_component_desc_t *desc,
                                uint32_t max_entities);
void fge_component_storage_free(fge_component_storage_t *st);

/* Get component pointer for entity. Returns nullptr if entity doesn't have it. */
void *fge_component_storage_get(fge_component_storage_t *st, uint32_t entity);

/* Add component to entity. Returns pointer to new component (zeroed). */
void *fge_component_storage_add(fge_component_storage_t *st, uint32_t entity);

/* Remove component from entity. */
void fge_component_storage_remove(fge_component_storage_t *st, uint32_t entity);

/* Iterate all entities with this component */
#define FGE_COMPONENT_EACH(storage, T, var, body) do { \
    fge_component_storage_t *_st = (storage); \
    for (uint32_t _i = 0; _i < _st->entities.count; _i++) { \
        uint32_t var##_entity = _st->entities.dense[_i]; \
        T *var = (T *)(_st->data + _i * _st->desc->size); \
        (void)var##_entity; /* suppress unused if user doesn't use it */ \
        body \
    } \
} while (0)

/* -------------------------------------------------------------------------- */
/* World                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    /* Entity management */
    uint32_t *entity_generations;
    uint32_t *free_entities;
    uint32_t  free_count;
    uint32_t  next_entity;
    uint32_t  max_entities;

    /* Component storage */
    fge_component_storage_t *storages[FGE_MAX_COMPONENT_TYPES];
    uint32_t storage_count;

    /* Registry */
    fge_component_registry_t *registry;

    /* Memory */
    fge_arena_t *arena;
} fge_ecs_world_t;

bool fge_ecs_world_init(fge_ecs_world_t *w, fge_component_registry_t *reg,
                        uint32_t max_entities, fge_arena_t *arena);
void fge_ecs_world_free(fge_ecs_world_t *w);

/* Entity lifecycle */
fge_entity_t fge_ecs_spawn(fge_ecs_world_t *w);
void fge_ecs_destroy(fge_ecs_world_t *w, fge_entity_t e);
bool fge_ecs_is_alive(const fge_ecs_world_t *w, fge_entity_t e);

/* Component operations */
void *fge_ecs_add_component(fge_ecs_world_t *w, fge_entity_t e, uint32_t type);
void fge_ecs_remove_component(fge_ecs_world_t *w, fge_entity_t e, uint32_t type);
void *fge_ecs_get_component(const fge_ecs_world_t *w, fge_entity_t e, uint32_t type);
bool fge_ecs_has_component(const fge_ecs_world_t *w, fge_entity_t e, uint32_t type);

/* Get mask of all components on entity */
fge_component_mask_t fge_ecs_get_mask(const fge_ecs_world_t *w, fge_entity_t e);

/* Convenience macros */
#define FGE_ADD_COMPONENT(w, e, T)    ((T *)fge_ecs_add_component((w), (e), FGE_ECS_TYPE_##T))
#define FGE_GET_COMPONENT(w, e, T)    ((T *)fge_ecs_get_component((w), (e), FGE_ECS_TYPE_##T))
#define FGE_REMOVE_COMPONENT(w, e, T) fge_ecs_remove_component((w), (e), FGE_ECS_TYPE_##T)
#define FGE_HAS_COMPONENT(w, e, T)    fge_ecs_has_component((w), (e), FGE_ECS_TYPE_##T)

/* -------------------------------------------------------------------------- */
/* System                                                                     */
/* -------------------------------------------------------------------------- */

typedef void (*fge_system_fn_t)(fge_ecs_world_t *w, fge_entity_t e, void *userdata);

typedef struct {
    const char *name;
    fge_component_mask_t required;
    fge_component_mask_t excluded;
    fge_system_fn_t fn;
    void *userdata;
} fge_system_t;

void fge_ecs_run_system(fge_ecs_world_t *w, const fge_system_t *system);

/* Batch system: callback receives arrays for SIMD-friendly processing */
typedef void (*fge_system_batch_fn_t)(fge_ecs_world_t *w,
                                      uint32_t count,
                                      const uint32_t *entities,
                                      void **components[],
                                      void *userdata);

typedef struct {
    const char *name;
    uint32_t num_components;
    uint32_t component_types[8];
    fge_system_batch_fn_t fn;
    void *userdata;
} fge_system_batch_t;

void fge_ecs_run_system_batch(fge_ecs_world_t *w, const fge_system_batch_t *system);

/* -------------------------------------------------------------------------- */
/* Query                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    fge_ecs_world_t *world;
    fge_component_mask_t required;
    fge_component_mask_t excluded;
    uint32_t current;
} fge_ecs_query_t;

fge_ecs_query_t fge_ecs_query(fge_ecs_world_t *w, fge_component_mask_t required);
fge_entity_t fge_ecs_query_next(fge_ecs_query_t *q);

#define FGE_QUERY_EACH(q, e_var, body) do { \
    fge_entity_t e_var; \
    while ((e_var = fge_ecs_query_next(&(q))) != FGE_ENTITY_INVALID) { \
        body \
    } \
} while (0)

#endif /* FORGE_ECS_H */
