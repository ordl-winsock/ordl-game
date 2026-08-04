/*
 * src/ecs/ecs.c — ECS implementation
 */

#include "forge/ecs.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Sparse set                                                                 */
/* -------------------------------------------------------------------------- */

bool fge_sparse_set_init(fge_sparse_set_t *s, uint32_t max_entities) {
    if (!s || max_entities == 0) return false;
    s->sparse = FGE_CALLOC(max_entities, sizeof(uint32_t));
    if (!s->sparse) return false;
    for (uint32_t i = 0; i < max_entities; i++) s->sparse[i] = ~0u;
    s->dense = FGE_CALLOC(max_entities, sizeof(uint32_t));
    if (!s->dense) { FGE_FREE(s->sparse); return false; }
    s->capacity = max_entities;
    s->count = 0;
    return true;
}

void fge_sparse_set_free(fge_sparse_set_t *s) {
    if (!s) return;
    FGE_FREE(s->sparse);
    FGE_FREE(s->dense);
    s->sparse = NULL;
    s->dense = NULL;
    s->capacity = s->count = 0;
}

void fge_sparse_set_clear(fge_sparse_set_t *s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->capacity; i++) s->sparse[i] = ~0u;
    s->count = 0;
}

bool fge_sparse_set_add(fge_sparse_set_t *s, uint32_t entity) {
    if (!s || entity >= s->capacity || s->count >= s->capacity) return false;
    if (s->sparse[entity] != ~0u) return true;
    s->sparse[entity] = s->count;
    s->dense[s->count] = entity;
    s->count++;
    return true;
}

bool fge_sparse_set_remove(fge_sparse_set_t *s, uint32_t entity) {
    if (!s || entity >= s->capacity) return false;
    uint32_t idx = s->sparse[entity];
    if (idx == ~0u) return false;
    uint32_t last_entity = s->dense[s->count - 1];
    s->dense[idx] = last_entity;
    s->sparse[last_entity] = idx;
    s->sparse[entity] = ~0u;
    s->count--;
    return true;
}

bool fge_sparse_set_contains(const fge_sparse_set_t *s, uint32_t entity) {
    return s && entity < s->capacity && s->sparse[entity] != ~0u;
}

/* -------------------------------------------------------------------------- */
/* Component registry                                                         */
/* -------------------------------------------------------------------------- */

uint32_t fge_ecs_register_component(fge_component_registry_t *reg,
                                    const char *name, size_t size, size_t align) {
    if (!reg || reg->count >= FGE_MAX_COMPONENT_TYPES) return ~0u;
    uint32_t idx = reg->count++;
    reg->descs[idx] = (fge_component_desc_t){ name, size, align, NULL, NULL, NULL };
    return idx;
}

/* -------------------------------------------------------------------------- */
/* Component storage                                                          */
/* -------------------------------------------------------------------------- */

bool fge_component_storage_init(fge_component_storage_t *st, const fge_component_desc_t *desc,
                                uint32_t max_entities) {
    if (!st || !desc) return false;
    if (!fge_sparse_set_init(&st->entities, max_entities)) return false;
    st->data = FGE_CALLOC(max_entities, desc->size);
    if (!st->data) { fge_sparse_set_free(&st->entities); return false; }
    st->capacity = max_entities;
    st->count = 0;
    st->desc = desc;
    return true;
}

void fge_component_storage_free(fge_component_storage_t *st) {
    if (!st) return;
    if (st->desc && st->desc->destroy) {
        for (uint32_t i = 0; i < st->entities.count; i++) {
            st->desc->destroy(st->data + i * st->desc->size);
        }
    }
    fge_sparse_set_free(&st->entities);
    FGE_FREE(st->data);
    st->data = NULL;
    st->capacity = st->count = 0;
}

void *fge_component_storage_get(fge_component_storage_t *st, uint32_t entity) {
    if (!st || entity >= st->capacity) return NULL;
    uint32_t idx = st->entities.sparse[entity];
    if (idx == ~0u) return NULL;
    return st->data + idx * st->desc->size;
}

void *fge_component_storage_add(fge_component_storage_t *st, uint32_t entity) {
    if (!st || entity >= st->capacity) return NULL;
    uint32_t idx = st->entities.sparse[entity];
    if (idx != ~0u) return st->data + idx * st->desc->size;
    if (!fge_sparse_set_add(&st->entities, entity)) return NULL;
    idx = st->entities.sparse[entity];
    void *p = st->data + idx * st->desc->size;
    memset(p, 0, st->desc->size);
    st->count++;
    if (st->desc->init) st->desc->init(p);
    return p;
}

void fge_component_storage_remove(fge_component_storage_t *st, uint32_t entity) {
    if (!st) return;
    uint32_t idx = st->entities.sparse[entity];
    if (idx == ~0u) return;
    if (st->desc->destroy) st->desc->destroy(st->data + idx * st->desc->size);

    uint32_t last_idx = st->entities.count - 1;
    if (idx != last_idx) {
        /* Swap data with last element to maintain dense packing (SoA) */
        memcpy(st->data + idx * st->desc->size,
               st->data + last_idx * st->desc->size,
               st->desc->size);
    }

    fge_sparse_set_remove(&st->entities, entity);
    st->count--;
}

/* -------------------------------------------------------------------------- */
/* World                                                                      */
/* -------------------------------------------------------------------------- */

/* Entity generation encoding in entity_generations array:
 *   bits 0-7  : generation counter (matches the 8-bit gen in entity handle)
 *   bit 8     : alive flag (1 = alive, 0 = dead)
 * This provides ABA-safe handles while allowing O(1) alive checks. */
#define FGE_ECS_GEN_MASK   0x00FFu
#define FGE_ECS_ALIVE_FLAG 0x0100u

bool fge_ecs_world_init(fge_ecs_world_t *w, fge_component_registry_t *reg,
                        uint32_t max_entities, fge_arena_t *arena) {
    if (!w || !reg || max_entities == 0 || max_entities > FGE_MAX_ENTITIES) return false;
    memset(w, 0, sizeof(*w));
    w->entity_generations = FGE_CALLOC(max_entities, sizeof(uint32_t));
    if (!w->entity_generations) return false;
    w->free_entities = FGE_CALLOC(max_entities, sizeof(uint32_t));
    if (!w->free_entities) { FGE_FREE(w->entity_generations); return false; }
    w->max_entities = max_entities;
    w->next_entity = 1;
    w->registry = reg;
    w->arena = arena;
    return true;
}

void fge_ecs_world_free(fge_ecs_world_t *w) {
    if (!w) return;
    for (uint32_t i = 0; i < w->storage_count; i++) {
        if (w->storages[i]) {
            fge_component_storage_free(w->storages[i]);
            FGE_FREE(w->storages[i]);
        }
    }
    FGE_FREE(w->entity_generations);
    FGE_FREE(w->free_entities);
    memset(w, 0, sizeof(*w));
}

fge_entity_t fge_ecs_spawn(fge_ecs_world_t *w) {
    if (!w) return FGE_ENTITY_INVALID;
    uint32_t idx;
    uint32_t gen;
    if (w->free_count > 0) {
        idx = w->free_entities[--w->free_count];
        uint32_t stored = w->entity_generations[idx];
        gen = ((stored & FGE_ECS_GEN_MASK) + 1u) & FGE_ECS_GEN_MASK;
        w->entity_generations[idx] = FGE_ECS_ALIVE_FLAG | gen;
    } else {
        idx = w->next_entity++;
        if (idx >= w->max_entities) {
            FGE_ERROR(FGE_LOG_CAT_ECS, "Entity limit reached: %u", w->max_entities);
            return FGE_ENTITY_INVALID;
        }
        gen = 0;
        w->entity_generations[idx] = FGE_ECS_ALIVE_FLAG | gen;
    }
    return FGE_ENTITY_MAKE(idx, gen);
}

void fge_ecs_destroy(fge_ecs_world_t *w, fge_entity_t e) {
    if (!w || e == FGE_ENTITY_INVALID) return;
    if (!fge_ecs_is_alive(w, e)) return;
    uint32_t idx = FGE_ENTITY_INDEX(e);
    for (uint32_t i = 0; i < w->storage_count; i++) {
        if (w->storages[i]) {
            fge_component_storage_remove(w->storages[i], idx);
        }
    }
    uint32_t stored = w->entity_generations[idx];
    uint32_t gen = (stored & FGE_ECS_GEN_MASK) + 1u;
    w->entity_generations[idx] = gen & FGE_ECS_GEN_MASK; /* dead: clear alive flag, increment gen */
    w->free_entities[w->free_count++] = idx;
}

bool fge_ecs_is_alive(const fge_ecs_world_t *w, fge_entity_t e) {
    if (!w || e == FGE_ENTITY_INVALID) return false;
    uint32_t idx = FGE_ENTITY_INDEX(e);
    uint32_t gen = FGE_ENTITY_GENERATION(e);
    if (idx >= w->max_entities) return false;
    uint32_t stored = w->entity_generations[idx];
    uint32_t stored_gen = stored & FGE_ECS_GEN_MASK;
    bool alive = (stored & FGE_ECS_ALIVE_FLAG) != 0;
    return stored_gen == gen && alive;
}

static fge_component_storage_t *fge_ecs_get_or_create_storage(fge_ecs_world_t *w, uint32_t type) {
    if (type >= FGE_MAX_COMPONENT_TYPES) return NULL;
    if (w->storages[type]) return w->storages[type];
    if (type >= w->registry->count) return NULL;
    fge_component_storage_t *st = FGE_CALLOC(1, sizeof(fge_component_storage_t));
    if (!st) return NULL;
    if (!fge_component_storage_init(st, &w->registry->descs[type], w->max_entities)) {
        FGE_FREE(st);
        return NULL;
    }
    w->storages[type] = st;
    if (type >= w->storage_count) w->storage_count = type + 1;
    return st;
}

void *fge_ecs_add_component(fge_ecs_world_t *w, fge_entity_t e, uint32_t type) {
    if (!fge_ecs_is_alive(w, e)) return NULL;
    fge_component_storage_t *st = fge_ecs_get_or_create_storage(w, type);
    if (!st) return NULL;
    return fge_component_storage_add(st, FGE_ENTITY_INDEX(e));
}

void fge_ecs_remove_component(fge_ecs_world_t *w, fge_entity_t e, uint32_t type) {
    if (!fge_ecs_is_alive(w, e)) return;
    if (type >= w->storage_count || !w->storages[type]) return;
    fge_component_storage_remove(w->storages[type], FGE_ENTITY_INDEX(e));
}

void *fge_ecs_get_component(const fge_ecs_world_t *w, fge_entity_t e, uint32_t type) {
    if (!fge_ecs_is_alive(w, e)) return NULL;
    if (type >= w->storage_count || !w->storages[type]) return NULL;
    return fge_component_storage_get(w->storages[type], FGE_ENTITY_INDEX(e));
}

bool fge_ecs_has_component(const fge_ecs_world_t *w, fge_entity_t e, uint32_t type) {
    return fge_ecs_get_component(w, e, type) != NULL;
}

fge_component_mask_t fge_ecs_get_mask(const fge_ecs_world_t *w, fge_entity_t e) {
    if (!fge_ecs_is_alive(w, e)) return 0;
    fge_component_mask_t mask = 0;
    for (uint32_t i = 0; i < w->storage_count; i++) {
        if (w->storages[i] && fge_sparse_set_contains(&w->storages[i]->entities, FGE_ENTITY_INDEX(e))) {
            mask |= (1ull << i);
        }
    }
    return mask;
}

/* -------------------------------------------------------------------------- */
/* System                                                                     */
/* -------------------------------------------------------------------------- */

void fge_ecs_run_system(fge_ecs_world_t *w, const fge_system_t *system) {
    if (!w || !system || !system->fn) return;
    uint32_t smallest_type = ~0u;
    uint32_t smallest_count = ~0u;
    for (uint32_t i = 0; i < FGE_MAX_COMPONENT_TYPES; i++) {
        if (system->required & (1ull << i)) {
            if (i < w->storage_count && w->storages[i] && w->storages[i]->entities.count < smallest_count) {
                smallest_count = w->storages[i]->entities.count;
                smallest_type = i;
            }
        }
    }
    if (smallest_type == ~0u) return;

    fge_component_storage_t *st = w->storages[smallest_type];
    for (uint32_t i = 0; i < st->entities.count; i++) {
        uint32_t idx = st->entities.dense[i];
        uint32_t gen = w->entity_generations[idx] & FGE_ECS_GEN_MASK;
        fge_entity_t e = FGE_ENTITY_MAKE(idx, gen);
        if (!fge_ecs_is_alive(w, e)) continue;
        fge_component_mask_t mask = fge_ecs_get_mask(w, e);
        if ((mask & system->required) != system->required) continue;
        if (mask & system->excluded) continue;
        system->fn(w, e, system->userdata);
    }
}

void fge_ecs_run_system_batch(fge_ecs_world_t *w, const fge_system_batch_t *system) {
    if (!w || !system || !system->fn || system->num_components == 0) return;

    /* Find the smallest storage among requested component types */
    uint32_t smallest_type = ~0u;
    uint32_t smallest_count = ~0u;
    for (uint32_t i = 0; i < system->num_components; i++) {
        uint32_t type = system->component_types[i];
        if (type < w->storage_count && w->storages[type] &&
            w->storages[type]->entities.count < smallest_count) {
            smallest_count = w->storages[type]->entities.count;
            smallest_type = type;
        }
    }
    if (smallest_type == ~0u) return;

    /* Batch size tuned for cache-friendly SIMD processing */
    const uint32_t batch_size = 256;
    uint32_t *entities = FGE_MALLOC(batch_size * sizeof(uint32_t));
    void **comp_ptrs = FGE_MALLOC(batch_size * system->num_components * sizeof(void *));
    if (!entities || !comp_ptrs) {
        FGE_FREE(entities);
        FGE_FREE(comp_ptrs);
        return;
    }

    /* comp_arrays[i] points to the array of component pointers for type i */
    void **comp_arrays[8];
    for (uint32_t i = 0; i < system->num_components; i++) {
        comp_arrays[i] = comp_ptrs + i * batch_size;
    }

    fge_component_storage_t *st = w->storages[smallest_type];
    uint32_t count = 0;

    for (uint32_t i = 0; i < st->entities.count; i++) {
        uint32_t idx = st->entities.dense[i];
        uint32_t gen = w->entity_generations[idx] & FGE_ECS_GEN_MASK;
        fge_entity_t e = FGE_ENTITY_MAKE(idx, gen);
        if (!fge_ecs_is_alive(w, e)) continue;

        /* Verify all requested components are present */
        bool has_all = true;
        for (uint32_t j = 0; j < system->num_components; j++) {
            uint32_t type = system->component_types[j];
            if (type >= w->storage_count || !w->storages[type] ||
                !fge_sparse_set_contains(&w->storages[type]->entities, idx)) {
                has_all = false;
                break;
            }
        }
        if (!has_all) continue;

        entities[count] = idx;
        for (uint32_t j = 0; j < system->num_components; j++) {
            uint32_t type = system->component_types[j];
            uint32_t dense_idx = w->storages[type]->entities.sparse[idx];
            comp_arrays[j][count] = w->storages[type]->data +
                                    dense_idx * w->storages[type]->desc->size;
        }
        count++;

        if (count == batch_size) {
            system->fn(w, count, entities, comp_arrays, system->userdata);
            count = 0;
        }
    }

    if (count > 0) {
        system->fn(w, count, entities, comp_arrays, system->userdata);
    }

    FGE_FREE(entities);
    FGE_FREE(comp_ptrs);
}

/* -------------------------------------------------------------------------- */
/* Query                                                                      */
/* -------------------------------------------------------------------------- */

/* Query state encoding: upper 8 bits = storage index + 1 (0 = linear scan),
 * lower 24 bits = position within storage or entity index. */
#define FGE_QUERY_STORAGE_SHIFT 24
#define FGE_QUERY_STORAGE_MASK  0xFF000000u
#define FGE_QUERY_INDEX_MASK    0x00FFFFFFu

fge_ecs_query_t fge_ecs_query(fge_ecs_world_t *w, fge_component_mask_t required) {
    if (!w || required == 0) {
        return (fge_ecs_query_t){ w, required, 0, 0 };
    }

    /* Find smallest matching component storage for efficient iteration */
    uint32_t smallest_type = ~0u;
    uint32_t smallest_count = ~0u;
    for (uint32_t i = 0; i < w->storage_count; i++) {
        if (w->storages[i] && (required & (1ull << i)) &&
            w->storages[i]->entities.count < smallest_count) {
            smallest_count = w->storages[i]->entities.count;
            smallest_type = i;
        }
    }

    uint32_t current;
    if (smallest_type == ~0u) {
        current = 0; /* linear fallback — no matching storage */
    } else {
        current = ((smallest_type + 1u) << FGE_QUERY_STORAGE_SHIFT);
    }
    return (fge_ecs_query_t){ w, required, 0, current };
}

fge_entity_t fge_ecs_query_next(fge_ecs_query_t *q) {
    if (!q || !q->world) return FGE_ENTITY_INVALID;

    uint32_t storage_idx = (q->current & FGE_QUERY_STORAGE_MASK) >> FGE_QUERY_STORAGE_SHIFT;
    uint32_t pos = q->current & FGE_QUERY_INDEX_MASK;

    if (storage_idx == 0) {
        /* Linear scan fallback (used when required == 0 or no storage found) */
        while (pos < q->world->max_entities) {
            uint32_t idx = pos++;
            q->current = pos;
            if (idx == 0 || idx >= q->world->next_entity) continue;
            uint32_t stored = q->world->entity_generations[idx];
            uint32_t gen = stored & FGE_ECS_GEN_MASK;
            bool alive = (stored & FGE_ECS_ALIVE_FLAG) != 0;
            if (!alive) continue;
            fge_entity_t e = FGE_ENTITY_MAKE(idx, gen);
            fge_component_mask_t mask = fge_ecs_get_mask(q->world, e);
            if ((mask & q->required) == q->required) {
                if (q->excluded && (mask & q->excluded)) continue;
                return e;
            }
        }
        return FGE_ENTITY_INVALID;
    }

    /* Iterate over dense array of the smallest matching component storage */
    fge_component_storage_t *st = q->world->storages[storage_idx - 1];
    if (!st) return FGE_ENTITY_INVALID;

    while (pos < st->entities.count) {
        uint32_t idx = st->entities.dense[pos];
        q->current = (storage_idx << FGE_QUERY_STORAGE_SHIFT) | (pos + 1);
        pos++;

        uint32_t gen = q->world->entity_generations[idx] & FGE_ECS_GEN_MASK;
        fge_entity_t e = FGE_ENTITY_MAKE(idx, gen);
        if (!fge_ecs_is_alive(q->world, e)) continue;

        fge_component_mask_t mask = fge_ecs_get_mask(q->world, e);
        if ((mask & q->required) == q->required) {
            if (q->excluded && (mask & q->excluded)) continue;
            return e;
        }
    }

    return FGE_ENTITY_INVALID;
}
