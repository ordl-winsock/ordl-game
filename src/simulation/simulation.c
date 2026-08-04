/*
 * src/simulation/simulation.c — Pixel Simulation Engine
 *
 * Cellular automata material simulation with optional sub-cell resolution.
 * Each grid cell can contain sub_scale×sub_scale sub-cells for finer detail.
 */

#include "forge/simulation.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Material Database                                                          */
/* -------------------------------------------------------------------------- */

static const fge_material_props_t MATERIAL_DB[FGE_MAT_COUNT] = {
    [FGE_MAT_EMPTY] = {
        .name = "Empty", .state = FGE_STATE_EMPTY, .color = 0xFF000000,
        .density = 0.0f, .melting_point = 0, .boiling_point = 0,
        .burn_temp = 9999, .burn_rate = 0, .heat_capacity = 0,
        .conductivity = 0, .flammable = false, .corrosive = false,
        .lifetime = 0,
    },
    [FGE_MAT_SAND] = {
        .name = "Sand", .state = FGE_STATE_POWDER, .color = 0xFFE6C288,
        .density = 1500.0f, .melting_point = 1700, .boiling_point = 2200,
        .burn_temp = 9999, .burn_rate = 0, .heat_capacity = 830,
        .conductivity = 20, .flammable = false, .corrosive = false,
        .lifetime = 0, .melt_product = FGE_MAT_LAVA,
    },
    [FGE_MAT_WATER] = {
        .name = "Water", .state = FGE_STATE_LIQUID, .color = 0xFF3399FF,
        .density = 1000.0f, .melting_point = 0, .boiling_point = 100,
        .burn_temp = 9999, .burn_rate = 0, .heat_capacity = 4184,
        .conductivity = 50, .flammable = false, .corrosive = false,
        .lifetime = 0, .boil_product = FGE_MAT_STEAM, .freeze_product = FGE_MAT_ICE,
    },
    [FGE_MAT_STONE] = {
        .name = "Stone", .state = FGE_STATE_SOLID, .color = 0xFF888888,
        .density = 2500.0f, .melting_point = 1200, .boiling_point = 2200,
        .burn_temp = 9999, .burn_rate = 0, .heat_capacity = 790,
        .conductivity = 40, .flammable = false, .corrosive = false,
        .lifetime = 0, .melt_product = FGE_MAT_LAVA,
    },
    [FGE_MAT_WOOD] = {
        .name = "Wood", .state = FGE_STATE_SOLID, .color = 0xFF8B5A2B,
        .density = 600.0f, .melting_point = 9999, .boiling_point = 9999,
        .burn_temp = 300, .burn_rate = 30, .heat_capacity = 1700,
        .conductivity = 15, .flammable = true, .corrosive = false,
        .lifetime = 0, .burn_product = FGE_MAT_FIRE,
    },
    [FGE_MAT_FIRE] = {
        .name = "Fire", .state = FGE_STATE_GAS, .color = 0xFFFF5500,
        .density = 0.5f, .melting_point = 9999, .boiling_point = 9999,
        .burn_temp = 0, .burn_rate = 0, .heat_capacity = 1000,
        .conductivity = 80, .flammable = false, .corrosive = false,
        .lifetime = 15, .burn_product = FGE_MAT_SMOKE,
    },
    [FGE_MAT_SMOKE] = {
        .name = "Smoke", .state = FGE_STATE_GAS, .color = 0xFF555555,
        .density = 0.8f, .melting_point = 9999, .boiling_point = 9999,
        .burn_temp = 9999, .burn_rate = 0, .heat_capacity = 1000,
        .conductivity = 10, .flammable = false, .corrosive = false,
        .lifetime = 120, .burn_product = FGE_MAT_EMPTY,
    },
    [FGE_MAT_STEAM] = {
        .name = "Steam", .state = FGE_STATE_GAS, .color = 0xFFCCCCCC,
        .density = 0.6f, .melting_point = 9999, .boiling_point = 9999,
        .burn_temp = 9999, .burn_rate = 0, .heat_capacity = 2000,
        .conductivity = 20, .flammable = false, .corrosive = false,
        .lifetime = 0, .freeze_product = FGE_MAT_WATER,
    },
    [FGE_MAT_OIL] = {
        .name = "Oil", .state = FGE_STATE_LIQUID, .color = 0xFF331100,
        .density = 800.0f, .melting_point = -40, .boiling_point = 300,
        .burn_temp = 200, .burn_rate = 20, .heat_capacity = 2000,
        .conductivity = 15, .flammable = true, .corrosive = false,
        .lifetime = 0, .burn_product = FGE_MAT_FIRE,
    },
    [FGE_MAT_LAVA] = {
        .name = "Lava", .state = FGE_STATE_LIQUID, .color = 0xFFFF3300,
        .density = 2700.0f, .melting_point = 9999, .boiling_point = 9999,
        .burn_temp = 0, .burn_rate = 0, .heat_capacity = 1000,
        .conductivity = 100, .flammable = false, .corrosive = false,
        .lifetime = 0, .freeze_product = FGE_MAT_STONE,
    },
    [FGE_MAT_ACID] = {
        .name = "Acid", .state = FGE_STATE_LIQUID, .color = 0xFF55FF00,
        .density = 1200.0f, .melting_point = -20, .boiling_point = 80,
        .burn_temp = 9999, .burn_rate = 0, .heat_capacity = 1500,
        .conductivity = 30, .flammable = false, .corrosive = true,
        .lifetime = 0,
    },
    [FGE_MAT_ICE] = {
        .name = "Ice", .state = FGE_STATE_SOLID, .color = 0xFFAAEEFF,
        .density = 917.0f, .melting_point = 0, .boiling_point = 100,
        .burn_temp = 9999, .burn_rate = 0, .heat_capacity = 2100,
        .conductivity = 60, .flammable = false, .corrosive = false,
        .lifetime = 0, .melt_product = FGE_MAT_WATER,
    },
    [FGE_MAT_SNOW] = {
        .name = "Snow", .state = FGE_STATE_POWDER, .color = 0xFFEEEEEE,
        .density = 100.0f, .melting_point = 0, .boiling_point = 100,
        .burn_temp = 9999, .burn_rate = 0, .heat_capacity = 2100,
        .conductivity = 10, .flammable = false, .corrosive = false,
        .lifetime = 0, .melt_product = FGE_MAT_WATER,
    },
    [FGE_MAT_DIRT] = {
        .name = "Dirt", .state = FGE_STATE_POWDER, .color = 0xFF5C4033,
        .density = 1200.0f, .melting_point = 9999, .boiling_point = 9999,
        .burn_temp = 400, .burn_rate = 100, .heat_capacity = 1500,
        .conductivity = 25, .flammable = false, .corrosive = false,
        .lifetime = 0,
    },
    [FGE_MAT_PLANT] = {
        .name = "Plant", .state = FGE_STATE_SOLID, .color = 0xFF228822,
        .density = 300.0f, .melting_point = 9999, .boiling_point = 9999,
        .burn_temp = 250, .burn_rate = 25, .heat_capacity = 1800,
        .conductivity = 20, .flammable = true, .corrosive = false,
        .lifetime = 0, .burn_product = FGE_MAT_FIRE,
    },
    [FGE_MAT_METAL] = {
        .name = "Metal", .state = FGE_STATE_SOLID, .color = 0xFFAAAAAA,
        .density = 7800.0f, .melting_point = 1500, .boiling_point = 2500,
        .burn_temp = 9999, .burn_rate = 0, .heat_capacity = 450,
        .conductivity = 200, .flammable = false, .corrosive = true,
        .lifetime = 0, .melt_product = FGE_MAT_LAVA,
    },
    [FGE_MAT_GLASS] = {
        .name = "Glass", .state = FGE_STATE_SOLID, .color = 0xFFCCDDEE,
        .density = 2500.0f, .melting_point = 1400, .boiling_point = 2200,
        .burn_temp = 9999, .burn_rate = 0, .heat_capacity = 840,
        .conductivity = 10, .flammable = false, .corrosive = false,
        .lifetime = 0, .melt_product = FGE_MAT_LAVA,
    },
    [FGE_MAT_GUNPOWDER] = {
        .name = "Gunpowder", .state = FGE_STATE_POWDER, .color = 0xFF333333,
        .density = 1200.0f, .melting_point = 9999, .boiling_point = 9999,
        .burn_temp = 280, .burn_rate = 2, .heat_capacity = 1000,
        .conductivity = 5, .flammable = true, .corrosive = false,
        .lifetime = 0, .burn_product = FGE_MAT_FIRE,
    },
    [FGE_MAT_COAL] = {
        .name = "Coal", .state = FGE_STATE_SOLID, .color = 0xFF111111,
        .density = 1300.0f, .melting_point = 9999, .boiling_point = 9999,
        .burn_temp = 350, .burn_rate = 200, .heat_capacity = 1200,
        .conductivity = 30, .flammable = true, .corrosive = false,
        .lifetime = 0, .burn_product = FGE_MAT_FIRE,
    },
};

const fge_material_props_t *fge_sim_material_props(fge_material_t mat) {
    if (mat >= FGE_MAT_COUNT) mat = FGE_MAT_EMPTY;
    return &MATERIAL_DB[mat];
}

int16_t fge_sim_material_default_temp(fge_material_t mat) {
    switch (mat) {
        case FGE_MAT_ICE:      return -10;
        case FGE_MAT_SNOW:     return -5;
        case FGE_MAT_LAVA:     return 1200;
        case FGE_MAT_FIRE:     return 800;
        case FGE_MAT_STEAM:    return 120;
        case FGE_MAT_SMOKE:    return 200;
        default:               return 20;
    }
}

/* -------------------------------------------------------------------------- */
/* Grid helpers                                                               */
/* -------------------------------------------------------------------------- */

static inline fge_sim_cell_t *cell_at(fge_sim_grid_t *g, int x, int y) {
    return &g->cells[y * g->width + x];
}

static inline uint8_t *sub_at(fge_sim_grid_t *g, int x, int y) {
    if (!g->sub) return NULL;
    return &g->sub[(y * g->width + x) * g->sub_scale * g->sub_scale];
}

static inline bool cell_empty(fge_sim_cell_t *c) {
    return c->material == FGE_MAT_EMPTY;
}

static inline const fge_material_props_t *cell_props(fge_sim_cell_t *c) {
    return fge_sim_material_props((fge_material_t)c->material);
}

static inline void expand_active(fge_sim_grid_t *g, int x, int y) {
    if (x < g->active_minx) g->active_minx = x;
    if (x > g->active_maxx) g->active_maxx = x;
    if (y < g->active_miny) g->active_miny = y;
    if (y > g->active_maxy) g->active_maxy = y;
    /* Mark chunk and neighbors as dirty */
    if (g->chunk_dirty) {
        int cs = g->chunk_size;
        int cx0 = (x - 1) / cs, cx1 = (x + 1) / cs;
        int cy0 = (y - 1) / cs, cy1 = (y + 1) / cs;
        if (cx0 < 0) cx0 = 0; if (cy0 < 0) cy0 = 0;
        if (cx1 >= g->chunks_x) cx1 = g->chunks_x - 1;
        if (cy1 >= g->chunks_y) cy1 = g->chunks_y - 1;
        for (int cy = cy0; cy <= cy1; cy++) {
            for (int cx = cx0; cx <= cx1; cx++) {
                g->chunk_dirty[cy * g->chunks_x + cx] = 1;
            }
        }
    }
}

static inline void recalc_active(fge_sim_grid_t *g) {
    int w = g->width, h = g->height;
    g->active_minx = w; g->active_maxx = 0;
    g->active_miny = h; g->active_maxy = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (!cell_empty(cell_at(g, x, y))) {
                expand_active(g, x, y);
            }
        }
    }
    if (g->active_minx > g->active_maxx) {
        g->active_minx = 0; g->active_maxx = 0;
        g->active_miny = 0; g->active_maxy = 0;
    }
    g->active_dirty = false;
}

/* -------------------------------------------------------------------------- */
/* Grid API                                                                   */
/* -------------------------------------------------------------------------- */

bool fge_sim_grid_init(fge_sim_grid_t *g, int w, int h, int sub_scale) {
    if (!g || w <= 0 || h <= 0 || sub_scale < 1) return false;
    size_t n = (size_t)w * (size_t)h;
    fge_memzero(g, sizeof(*g));
    g->cells = (fge_sim_cell_t *)FGE_CALLOC(n, sizeof(fge_sim_cell_t));
    g->back = (fge_sim_cell_t *)FGE_CALLOC(n, sizeof(fge_sim_cell_t));
    if (!g->cells || !g->back) {
        FGE_FREE(g->cells); FGE_FREE(g->back);
        return false;
    }
    if (sub_scale > 1) {
        size_t sub_n = n * (size_t)sub_scale * (size_t)sub_scale;
        g->sub = (uint8_t *)FGE_CALLOC(sub_n, sizeof(uint8_t));
        if (!g->sub) {
            FGE_FREE(g->cells); FGE_FREE(g->back);
            return false;
        }
    }
    g->width = w;
    g->height = h;
    g->sub_scale = sub_scale;
    g->seed = 42;
    g->active_minx = w; g->active_maxx = 0;
    g->active_miny = h; g->active_maxy = 0;

    /* Initialize chunk tracking (64x64 cells per chunk) */
    g->chunk_size = 64;
    g->chunks_x = (w + g->chunk_size - 1) / g->chunk_size;
    g->chunks_y = (h + g->chunk_size - 1) / g->chunk_size;
    size_t num_chunks = (size_t)g->chunks_x * (size_t)g->chunks_y;
    g->chunk_dirty = (uint8_t *)FGE_CALLOC(num_chunks, 1);
    g->chunk_active = (uint32_t *)FGE_CALLOC((num_chunks + 31) / 32, sizeof(uint32_t));
    if (!g->chunk_dirty || !g->chunk_active) {
        FGE_FREE(g->cells); FGE_FREE(g->back); FGE_FREE(g->sub);
        FGE_FREE(g->chunk_dirty); FGE_FREE(g->chunk_active);
        return false;
    }
    return true;
}

void fge_sim_grid_free(fge_sim_grid_t *g) {
    if (!g) return;
    FGE_FREE(g->cells);
    FGE_FREE(g->back);
    FGE_FREE(g->sub);
    FGE_FREE(g->chunk_dirty);
    FGE_FREE(g->chunk_active);
    fge_memzero(g, sizeof(*g));
}

void fge_sim_grid_clear(fge_sim_grid_t *g) {
    if (!g || !g->cells) return;
    size_t n = (size_t)g->width * (size_t)g->height;
    fge_memzero(g->cells, n * sizeof(fge_sim_cell_t));
    if (g->back) fge_memzero(g->back, n * sizeof(fge_sim_cell_t));
    if (g->sub) fge_memzero(g->sub, n * (size_t)g->sub_scale * (size_t)g->sub_scale);
    g->active_minx = g->width; g->active_maxx = 0;
    g->active_miny = g->height; g->active_maxy = 0;
    if (g->chunk_dirty) {
        size_t num_chunks = (size_t)g->chunks_x * (size_t)g->chunks_y;
        fge_memzero(g->chunk_dirty, num_chunks);
        fge_memzero(g->chunk_active, ((num_chunks + 31) / 32) * sizeof(uint32_t));
    }
}

fge_sim_cell_t *fge_sim_grid_get(fge_sim_grid_t *g, int x, int y) {
    if (!g || x < 0 || x >= g->width || y < 0 || y >= g->height) return NULL;
    return cell_at(g, x, y);
}

fge_sim_cell_t *fge_sim_grid_get_clamped(fge_sim_grid_t *g, int x, int y) {
    if (!g) return NULL;
    if (x < 0) x = 0;
    if (x >= g->width) x = g->width - 1;
    if (y < 0) y = 0;
    if (y >= g->height) y = g->height - 1;
    return cell_at(g, x, y);
}

uint8_t fge_sim_grid_get_sub(fge_sim_grid_t *g, int x, int y, int sx, int sy) {
    if (!g || x < 0 || x >= g->width || y < 0 || y >= g->height) return FGE_MAT_EMPTY;
    if (g->sub_scale <= 1) return cell_at(g, x, y)->material;
    if (sx < 0 || sx >= g->sub_scale || sy < 0 || sy >= g->sub_scale) return FGE_MAT_EMPTY;
    return g->sub[((y * g->width + x) * g->sub_scale + sy) * g->sub_scale + sx];
}

void fge_sim_grid_set_sub(fge_sim_grid_t *g, int x, int y, int sx, int sy, fge_material_t mat) {
    if (!g || x < 0 || x >= g->width || y < 0 || y >= g->height) return;
    fge_sim_cell_t *c = cell_at(g, x, y);
    if (g->sub_scale > 1) {
        if (sx >= 0 && sx < g->sub_scale && sy >= 0 && sy < g->sub_scale) {
            g->sub[((y * g->width + x) * g->sub_scale + sy) * g->sub_scale + sx] = (uint8_t)mat;
        }
        /* Update dominant material */
        if (mat != FGE_MAT_EMPTY) c->material = (uint8_t)mat;
    } else {
        c->material = (uint8_t)mat;
    }
    c->temperature = fge_sim_material_default_temp(mat);
    c->life = 0;
    c->flags = 0;
    expand_active(g, x, y);
}

void fge_sim_grid_set(fge_sim_grid_t *g, int x, int y, fge_material_t mat) {
    if (!g || x < 0 || x >= g->width || y < 0 || y >= g->height) return;
    fge_sim_cell_t *c = cell_at(g, x, y);
    c->material = (uint8_t)mat;
    c->temperature = fge_sim_material_default_temp(mat);
    c->life = 0;
    c->flags = 0;
    c->variant = (uint8_t)(fge_hash_u32((uint32_t)(x * 73856093 + y * 19349663)) & 7);
    if (mat == FGE_MAT_FIRE) {
        const fge_material_props_t *p = fge_sim_material_props(mat);
        c->life = (uint8_t)p->lifetime;
    }
    if (g->sub && g->sub_scale > 1) {
        uint8_t *s = sub_at(g, x, y);
        int ss = g->sub_scale * g->sub_scale;
        for (int i = 0; i < ss; i++) s[i] = (uint8_t)mat;
    }
    expand_active(g, x, y);
}

void fge_sim_grid_paint_circle(fge_sim_grid_t *g, int cx, int cy, int r, fge_material_t mat) {
    if (!g || r <= 0) return;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                fge_sim_grid_set(g, cx + dx, cy + dy, mat);
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* RNG                                                                        */
/* -------------------------------------------------------------------------- */

static uint32_t sim_rng(fge_sim_grid_t *g) {
    g->seed = g->seed * 1103515245u + 12345u;
    return g->seed;
}

/* -------------------------------------------------------------------------- */
/* Movement helpers                                                           */
/* -------------------------------------------------------------------------- */

static bool can_displace(fge_sim_cell_t *src, fge_sim_cell_t *dst) {
    if (cell_empty(dst)) return true;
    const fge_material_props_t *sp = cell_props(src);
    const fge_material_props_t *dp = cell_props(dst);
    if (sp->state == FGE_STATE_SOLID || sp->state == FGE_STATE_POWDER) {
        if (dp->state == FGE_STATE_GAS) return true;
        if (dp->state == FGE_STATE_LIQUID && sp->density > dp->density) return true;
    }
    if (sp->state == FGE_STATE_LIQUID) {
        if (dp->state == FGE_STATE_GAS) return true;
        if (dp->state == FGE_STATE_LIQUID && sp->density > dp->density) return true;
    }
    if (sp->state == FGE_STATE_GAS) {
        if (dp->state == FGE_STATE_GAS && sp->density > dp->density) return true;
    }
    return false;
}

static inline void swap_cells(fge_sim_cell_t *a, fge_sim_cell_t *b) {
    fge_sim_cell_t tmp = *a;
    *a = *b;
    *b = tmp;
}

static void try_move_down(fge_sim_grid_t *g, int x, int y) {
    if (y >= g->height - 1) return;
    fge_sim_cell_t *c = cell_at(g, x, y);
    fge_sim_cell_t *below = cell_at(g, x, y + 1);
    if (can_displace(c, below)) {
        swap_cells(c, below);
        c->flags |= FGE_CELL_MOVED;
        below->flags |= FGE_CELL_MOVED;
        expand_active(g, x, y);
        expand_active(g, x, y + 1);
        /* Swap sub-cells too if present */
        if (g->sub) {
            uint8_t *sa = sub_at(g, x, y);
            uint8_t *sb = sub_at(g, x, y + 1);
            int ss = g->sub_scale * g->sub_scale;
            for (int i = 0; i < ss; i++) {
                uint8_t t = sa[i]; sa[i] = sb[i]; sb[i] = t;
            }
        }
    }
}

static void try_move_down_diagonal(fge_sim_grid_t *g, int x, int y, bool right_first) {
    if (y >= g->height - 1) return;
    fge_sim_cell_t *c = cell_at(g, x, y);
    int dirs[2] = { right_first ? 1 : -1, right_first ? -1 : 1 };
    for (int i = 0; i < 2; i++) {
        int nx = x + dirs[i];
        if (nx < 0 || nx >= g->width) continue;
        fge_sim_cell_t *diag = cell_at(g, nx, y + 1);
        if (can_displace(c, diag)) {
            swap_cells(c, diag);
            c->flags |= FGE_CELL_MOVED;
            diag->flags |= FGE_CELL_MOVED;
            expand_active(g, x, y);
            expand_active(g, nx, y + 1);
            if (g->sub) {
                uint8_t *sa = sub_at(g, x, y);
                uint8_t *sb = sub_at(g, nx, y + 1);
                int ss = g->sub_scale * g->sub_scale;
                for (int k = 0; k < ss; k++) {
                    uint8_t t = sa[k]; sa[k] = sb[k]; sb[k] = t;
                }
            }
            return;
        }
    }
}

static void try_move_up(fge_sim_grid_t *g, int x, int y) {
    if (y <= 0) return;
    fge_sim_cell_t *c = cell_at(g, x, y);
    fge_sim_cell_t *above = cell_at(g, x, y - 1);
    if (can_displace(c, above)) {
        swap_cells(c, above);
        c->flags |= FGE_CELL_MOVED;
        above->flags |= FGE_CELL_MOVED;
        expand_active(g, x, y);
        expand_active(g, x, y - 1);
        if (g->sub) {
            uint8_t *sa = sub_at(g, x, y);
            uint8_t *sb = sub_at(g, x, y - 1);
            int ss = g->sub_scale * g->sub_scale;
            for (int i = 0; i < ss; i++) {
                uint8_t t = sa[i]; sa[i] = sb[i]; sb[i] = t;
            }
        }
    }
}

static void try_move_up_diagonal(fge_sim_grid_t *g, int x, int y, bool right_first) {
    if (y <= 0) return;
    fge_sim_cell_t *c = cell_at(g, x, y);
    int dirs[2] = { right_first ? 1 : -1, right_first ? -1 : 1 };
    for (int i = 0; i < 2; i++) {
        int nx = x + dirs[i];
        if (nx < 0 || nx >= g->width) continue;
        fge_sim_cell_t *diag = cell_at(g, nx, y - 1);
        if (can_displace(c, diag)) {
            swap_cells(c, diag);
            c->flags |= FGE_CELL_MOVED;
            diag->flags |= FGE_CELL_MOVED;
            expand_active(g, x, y);
            expand_active(g, nx, y - 1);
            if (g->sub) {
                uint8_t *sa = sub_at(g, x, y);
                uint8_t *sb = sub_at(g, nx, y - 1);
                int ss = g->sub_scale * g->sub_scale;
                for (int k = 0; k < ss; k++) {
                    uint8_t t = sa[k]; sa[k] = sb[k]; sb[k] = t;
                }
            }
            return;
        }
    }
}

static void try_move_sideways(fge_sim_grid_t *g, int x, int y) {
    fge_sim_cell_t *c = cell_at(g, x, y);
    int dir = (sim_rng(g) & 1) ? 1 : -1;
    int nx = x + dir;
    if (nx < 0 || nx >= g->width) return;
    fge_sim_cell_t *side = cell_at(g, nx, y);
    if (can_displace(c, side)) {
        swap_cells(c, side);
        c->flags |= FGE_CELL_MOVED;
        side->flags |= FGE_CELL_MOVED;
        expand_active(g, x, y);
        expand_active(g, nx, y);
        if (g->sub) {
            uint8_t *sa = sub_at(g, x, y);
            uint8_t *sb = sub_at(g, nx, y);
            int ss = g->sub_scale * g->sub_scale;
            for (int i = 0; i < ss; i++) {
                uint8_t t = sa[i]; sa[i] = sb[i]; sb[i] = t;
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Heat & Phase Changes                                                       */
/* -------------------------------------------------------------------------- */

static void do_heat_transfer(fge_sim_grid_t *g, int x, int y) {
    fge_sim_cell_t *c = cell_at(g, x, y);
    if (cell_empty(c)) return;
    const fge_material_props_t *cp = cell_props(c);
    if (cp->conductivity == 0) return;

    int16_t avg_temp = 0;
    int count = 0;
    int dirs[4][2] = { {0,1}, {0,-1}, {1,0}, {-1,0} };
    for (int i = 0; i < 4; i++) {
        int nx = x + dirs[i][0], ny = y + dirs[i][1];
        if (nx < 0 || nx >= g->width || ny < 0 || ny >= g->height) continue;
        fge_sim_cell_t *n = cell_at(g, nx, ny);
        if (!cell_empty(n)) {
            avg_temp += n->temperature;
            count++;
        }
    }
    if (count == 0) return;
    avg_temp /= count;

    int16_t diff = avg_temp - c->temperature;
    int16_t transfer = (int16_t)(diff * cp->conductivity / 255);
    if (transfer == 0 && diff > 0) transfer = 1;
    if (transfer == 0 && diff < 0) transfer = -1;
    c->temperature += transfer;
}

static void do_phase_changes(fge_sim_grid_t *g, int x, int y) {
    fge_sim_cell_t *c = cell_at(g, x, y);
    if (cell_empty(c)) return;
    const fge_material_props_t *cp = cell_props(c);
    int16_t t = c->temperature;

    if (cp->boil_product != FGE_MAT_EMPTY && t >= cp->boiling_point) {
        c->material = (uint8_t)cp->boil_product;
        c->temperature = fge_sim_material_default_temp(cp->boil_product);
        c->life = 0;
        if (g->sub) {
            uint8_t *s = sub_at(g, x, y);
            int ss = g->sub_scale * g->sub_scale;
            for (int i = 0; i < ss; i++) s[i] = c->material;
        }
        return;
    }
    if (cp->melt_product != FGE_MAT_EMPTY && t >= cp->melting_point) {
        c->material = (uint8_t)cp->melt_product;
        c->temperature = fge_sim_material_default_temp(cp->melt_product);
        c->life = 0;
        if (g->sub) {
            uint8_t *s = sub_at(g, x, y);
            int ss = g->sub_scale * g->sub_scale;
            for (int i = 0; i < ss; i++) s[i] = c->material;
        }
        return;
    }
    if (cp->freeze_product != FGE_MAT_EMPTY && t <= 0) {
        c->material = (uint8_t)cp->freeze_product;
        c->temperature = fge_sim_material_default_temp(cp->freeze_product);
        c->life = 0;
        if (g->sub) {
            uint8_t *s = sub_at(g, x, y);
            int ss = g->sub_scale * g->sub_scale;
            for (int i = 0; i < ss; i++) s[i] = c->material;
        }
        return;
    }
    if (c->material == FGE_MAT_STEAM && t <= 95) {
        c->material = FGE_MAT_WATER;
        c->temperature = 95;
        c->life = 0;
        if (g->sub) {
            uint8_t *s = sub_at(g, x, y);
            int ss = g->sub_scale * g->sub_scale;
            for (int i = 0; i < ss; i++) s[i] = c->material;
        }
        return;
    }
    if (c->material == FGE_MAT_LAVA && t <= 1000) {
        c->material = FGE_MAT_STONE;
        c->temperature = 999;
        c->life = 0;
        if (g->sub) {
            uint8_t *s = sub_at(g, x, y);
            int ss = g->sub_scale * g->sub_scale;
            for (int i = 0; i < ss; i++) s[i] = c->material;
        }
        return;
    }
}

/* -------------------------------------------------------------------------- */
/* Reactions & Fire                                                           */
/* -------------------------------------------------------------------------- */

static void do_reactions(fge_sim_grid_t *g, int x, int y) {
    fge_sim_cell_t *c = cell_at(g, x, y);
    if (cell_empty(c)) return;
    const fge_material_props_t *cp = cell_props(c);

    if (cp->flammable && c->temperature >= cp->burn_temp) {
        c->material = (uint8_t)cp->burn_product;
        c->temperature = fge_sim_material_default_temp(cp->burn_product);
        const fge_material_props_t *bp = fge_sim_material_props(cp->burn_product);
        c->life = (uint8_t)bp->lifetime;
        c->flags |= FGE_CELL_BURNING;
        if (g->sub) {
            uint8_t *s = sub_at(g, x, y);
            int ss = g->sub_scale * g->sub_scale;
            for (int i = 0; i < ss; i++) s[i] = c->material;
        }
        return;
    }

    if (c->material == FGE_MAT_FIRE) {
        if (c->life > 0) {
            c->life--;
            int dirs[8][2] = { {0,1},{0,-1},{1,0},{-1,0},{1,1},{1,-1},{-1,1},{-1,-1} };
            for (int i = 0; i < 8; i++) {
                int nx = x + dirs[i][0], ny = y + dirs[i][1];
                if (nx < 0 || nx >= g->width || ny < 0 || ny >= g->height) continue;
                fge_sim_cell_t *n = cell_at(g, nx, ny);
                if (!cell_empty(n)) {
                    n->temperature += 50;
                }
            }
        } else {
            c->material = FGE_MAT_SMOKE;
            c->temperature = 150;
            c->life = 60;
            if (g->sub) {
                uint8_t *s = sub_at(g, x, y);
                int ss = g->sub_scale * g->sub_scale;
                for (int i = 0; i < ss; i++) s[i] = c->material;
            }
        }
        return;
    }

    if (c->material == FGE_MAT_SMOKE) {
        if (c->life > 0) {
            c->life--;
        } else if ((sim_rng(g) & 0xFF) < 4) {
            c->material = FGE_MAT_EMPTY;
            c->temperature = 20;
            if (g->sub) {
                uint8_t *s = sub_at(g, x, y);
                int ss = g->sub_scale * g->sub_scale;
                for (int i = 0; i < ss; i++) s[i] = FGE_MAT_EMPTY;
            }
        }
        return;
    }

    if (cp->corrosive) {
        int dirs[4][2] = { {0,1},{0,-1},{1,0},{-1,0} };
        for (int i = 0; i < 4; i++) {
            int nx = x + dirs[i][0], ny = y + dirs[i][1];
            if (nx < 0 || nx >= g->width || ny < 0 || ny >= g->height) continue;
            fge_sim_cell_t *n = cell_at(g, nx, ny);
            if (n->material == FGE_MAT_METAL || n->material == FGE_MAT_STONE ||
                n->material == FGE_MAT_WOOD || n->material == FGE_MAT_DIRT) {
                if ((sim_rng(g) & 0xFF) < 32) {
                    n->material = FGE_MAT_EMPTY;
                    if (g->sub) {
                        uint8_t *s = sub_at(g, nx, ny);
                        int ss = g->sub_scale * g->sub_scale;
                        for (int j = 0; j < ss; j++) s[j] = FGE_MAT_EMPTY;
                    }
                    if ((sim_rng(g) & 0xFF) < 64) {
                        c->material = FGE_MAT_EMPTY;
                        if (g->sub) {
                            uint8_t *s = sub_at(g, x, y);
                            int ss = g->sub_scale * g->sub_scale;
                            for (int j = 0; j < ss; j++) s[j] = FGE_MAT_EMPTY;
                        }
                    }
                }
            }
        }
        return;
    }

    if (c->material == FGE_MAT_WATER) {
        int dirs[4][2] = { {0,1},{0,-1},{1,0},{-1,0} };
        for (int i = 0; i < 4; i++) {
            int nx = x + dirs[i][0], ny = y + dirs[i][1];
            if (nx < 0 || nx >= g->width || ny < 0 || ny >= g->height) continue;
            fge_sim_cell_t *n = cell_at(g, nx, ny);
            if (n->material == FGE_MAT_FIRE || n->material == FGE_MAT_LAVA) {
                n->temperature -= 100;
                c->temperature += 20;
                if (n->temperature < 100) {
                    n->material = FGE_MAT_SMOKE;
                    n->life = 30;
                    if (g->sub) {
                        uint8_t *s = sub_at(g, nx, ny);
                        int ss = g->sub_scale * g->sub_scale;
                        for (int j = 0; j < ss; j++) s[j] = FGE_MAT_SMOKE;
                    }
                }
            }
        }
        return;
    }

    if (c->material == FGE_MAT_PLANT && c->life == 0) {
        if (y > 0) {
            bool has_water = false;
            int wdirs[4][2] = { {0,1},{1,0},{-1,0},{0,-1} };
            for (int i = 0; i < 4; i++) {
                int nx = x + wdirs[i][0], ny = y + wdirs[i][1];
                if (nx < 0 || nx >= g->width || ny < 0 || ny >= g->height) continue;
                if (cell_at(g, nx, ny)->material == FGE_MAT_WATER) {
                    has_water = true;
                    break;
                }
            }
            if (has_water) {
                fge_sim_cell_t *above = cell_at(g, x, y - 1);
                if (cell_empty(above) && (sim_rng(g) & 0xFF) < 8) {
                    above->material = FGE_MAT_PLANT;
                    above->temperature = 20;
                    above->variant = (uint8_t)(sim_rng(g) & 7);
                    if (g->sub) {
                        uint8_t *s = sub_at(g, x, y - 1);
                        int ss = g->sub_scale * g->sub_scale;
                        for (int j = 0; j < ss; j++) s[j] = FGE_MAT_PLANT;
                    }
                }
            }
        }
        return;
    }
}

/* -------------------------------------------------------------------------- */
/* Main Simulation Step                                                       */
/* -------------------------------------------------------------------------- */

void fge_sim_step(fge_sim_grid_t *g) {
    if (!g || !g->cells) return;

    int w = g->width, h = g->height;
    int cs = g->chunk_size;

    /* Build active chunk mask from dirty flags */
    size_t num_chunks = (size_t)g->chunks_x * (size_t)g->chunks_y;
    size_t mask_words = (num_chunks + 31) / 32;
    fge_memzero(g->chunk_active, mask_words * sizeof(uint32_t));

    bool any_active = false;
    for (size_t i = 0; i < num_chunks; i++) {
        if (g->chunk_dirty[i]) {
            g->chunk_active[i / 32] |= (1u << (i % 32));
            g->chunk_dirty[i] = 0; /* clear for next frame */
            any_active = true;
        }
    }

    /* Fallback: if no chunks tracked or first frame, use full active region */
    if (!any_active) {
        if (g->active_dirty || g->active_minx > g->active_maxx) {
            recalc_active(g);
        }
        /* Mark all chunks in active region */
        int cx0 = g->active_minx / cs, cx1 = g->active_maxx / cs;
        int cy0 = g->active_miny / cs, cy1 = g->active_maxy / cs;
        for (int cy = cy0; cy <= cy1; cy++) {
            for (int cx = cx0; cx <= cx1; cx++) {
                int idx = cy * g->chunks_x + cx;
                g->chunk_active[idx / 32] |= (1u << (idx % 32));
            }
        }
    }

    /* Clear movement flags only in active chunks */
    for (int cy = 0; cy < g->chunks_y; cy++) {
        for (int cx = 0; cx < g->chunks_x; cx++) {
            int idx = cy * g->chunks_x + cx;
            if (!(g->chunk_active[idx / 32] & (1u << (idx % 32)))) continue;
            int x0 = cx * cs, x1 = FGE_MIN(x0 + cs, w);
            int y0 = cy * cs, y1 = FGE_MIN(y0 + cs, h);
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    cell_at(g, x, y)->flags &= ~(FGE_CELL_MOVED | FGE_CELL_UPDATED);
                }
            }
        }
    }

    /* Phase 1: Heat transfer and reactions */
    for (int cy = 0; cy < g->chunks_y; cy++) {
        for (int cx = 0; cx < g->chunks_x; cx++) {
            int idx = cy * g->chunks_x + cx;
            if (!(g->chunk_active[idx / 32] & (1u << (idx % 32)))) continue;
            int x0 = cx * cs, x1 = FGE_MIN(x0 + cs, w);
            int y0 = cy * cs, y1 = FGE_MIN(y0 + cs, h);
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    do_heat_transfer(g, x, y);
                    do_phase_changes(g, x, y);
                    do_reactions(g, x, y);
                }
            }
        }
    }

    /* Phase 2: Movement — powders and solids (bottom-up) */
    for (int cy = g->chunks_y - 1; cy >= 0; cy--) {
        bool right_first = (sim_rng(g) & 1) != 0;
        for (int cx = 0; cx < g->chunks_x; cx++) {
            int idx = cy * g->chunks_x + cx;
            if (!(g->chunk_active[idx / 32] & (1u << (idx % 32)))) continue;
            int x0 = cx * cs, x1 = FGE_MIN(x0 + cs, w);
            int y0 = cy * cs, y1 = FGE_MIN(y0 + cs, h);
            for (int y = y1 - 1; y >= y0; y--) {
                for (int x = x0; x < x1; x++) {
                    fge_sim_cell_t *c = cell_at(g, x, y);
                    if (cell_empty(c)) continue;
                    if (c->flags & FGE_CELL_MOVED) continue;
                    const fge_material_props_t *p = cell_props(c);
                    if (p->state == FGE_STATE_POWDER || p->state == FGE_STATE_SOLID) {
                        try_move_down(g, x, y);
                        if (!(c->flags & FGE_CELL_MOVED)) {
                            try_move_down_diagonal(g, x, y, right_first);
                        }
                    }
                }
            }
        }
    }

    /* Phase 3: Liquids (bottom-up) */
    for (int cy = g->chunks_y - 1; cy >= 0; cy--) {
        bool right_first = (sim_rng(g) & 1) != 0;
        for (int cx = 0; cx < g->chunks_x; cx++) {
            int idx = cy * g->chunks_x + cx;
            if (!(g->chunk_active[idx / 32] & (1u << (idx % 32)))) continue;
            int x0 = cx * cs, x1 = FGE_MIN(x0 + cs, w);
            int y0 = cy * cs, y1 = FGE_MIN(y0 + cs, h);
            for (int y = y1 - 1; y >= y0; y--) {
                for (int x = x0; x < x1; x++) {
                    fge_sim_cell_t *c = cell_at(g, x, y);
                    if (cell_empty(c)) continue;
                    if (c->flags & FGE_CELL_MOVED) continue;
                    const fge_material_props_t *p = cell_props(c);
                    if (p->state == FGE_STATE_LIQUID) {
                        try_move_down(g, x, y);
                        if (!(c->flags & FGE_CELL_MOVED)) {
                            try_move_down_diagonal(g, x, y, right_first);
                        }
                        if (!(c->flags & FGE_CELL_MOVED)) {
                            try_move_sideways(g, x, y);
                        }
                    }
                }
            }
        }
    }

    /* Phase 4: Gases (top-down) */
    for (int cy = 0; cy < g->chunks_y; cy++) {
        bool right_first = (sim_rng(g) & 1) != 0;
        for (int cx = 0; cx < g->chunks_x; cx++) {
            int idx = cy * g->chunks_x + cx;
            if (!(g->chunk_active[idx / 32] & (1u << (idx % 32)))) continue;
            int x0 = cx * cs, x1 = FGE_MIN(x0 + cs, w);
            int y0 = cy * cs, y1 = FGE_MIN(y0 + cs, h);
            for (int y = y0 + 1; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    fge_sim_cell_t *c = cell_at(g, x, y);
                    if (cell_empty(c)) continue;
                    if (c->flags & FGE_CELL_MOVED) continue;
                    const fge_material_props_t *p = cell_props(c);
                    if (p->state == FGE_STATE_GAS) {
                        try_move_up(g, x, y);
                        if (!(c->flags & FGE_CELL_MOVED)) {
                            try_move_up_diagonal(g, x, y, right_first);
                        }
                        if (!(c->flags & FGE_CELL_MOVED)) {
                            try_move_sideways(g, x, y);
                        }
                    }
                }
            }
        }
    }

    /* Shrink active region periodically (every 60 frames approx) */
    if ((sim_rng(g) & 0x3F) == 0) {
        g->active_dirty = true;
    }
}

/* -------------------------------------------------------------------------- */
/* Rendering                                                                  */
/* -------------------------------------------------------------------------- */

void fge_sim_render(fge_sim_grid_t *g, uint32_t *fb, int fb_w, int fb_h) {
    if (!g || !g->cells || !fb) return;
    int w = g->width, h = g->height;
    int ss = g->sub_scale;

    /* If buffer is large enough and we have sub-cells, render each sub-cell
     * as one pixel. This gives true sub-pixel detail — e.g. a 1-sub-cell
     * outline around an object is visible as a 1-pixel outline. */
    if (ss > 1 && g->sub && fb_w >= w * ss && fb_h >= h * ss) {
        for (int cy = 0; cy < h; cy++) {
            for (int cx = 0; cx < w; cx++) {
                fge_sim_cell_t *c = cell_at(g, cx, cy);
                uint8_t *sub = sub_at(g, cx, cy);
                int base_row = cy * ss;
                int base_col = cx * ss;
                /* Temperature shift from the parent cell applies to all sub-cells */
                int temp_shift_r = 0, temp_shift_g = 0, temp_shift_b = 0;
                if (!cell_empty(c)) {
                    int16_t diff = c->temperature - fge_sim_material_default_temp((fge_material_t)c->material);
                    if (diff > 100) {
                        int s = diff / 50; if (s > 80) s = 80;
                        temp_shift_r = s;
                        temp_shift_g = s / 2;
                        temp_shift_b = s / 4;
                    } else if (diff < -20) {
                        int s = -diff / 20; if (s > 60) s = 60;
                        temp_shift_r = s / 2;
                        temp_shift_g = s / 2;
                        temp_shift_b = s;
                    }
                }
                for (int sy = 0; sy < ss; sy++) {
                    for (int sx = 0; sx < ss; sx++) {
                        uint8_t mat = sub[sy * ss + sx];
                        uint32_t color;
                        if (mat == FGE_MAT_EMPTY) {
                            color = 0xFF000000;
                        } else {
                            const fge_material_props_t *mp = fge_sim_material_props((fge_material_t)mat);
                            color = mp->color;
                            uint8_t r = (uint8_t)FGE_MIN(255, (int)((color >> 16) & 0xFF) + temp_shift_r);
                            uint8_t gr = (uint8_t)FGE_MIN(255, (int)((color >> 8) & 0xFF) + temp_shift_g);
                            uint8_t b = (uint8_t)FGE_MIN(255, (int)(color & 0xFF) + temp_shift_b);
                            color = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)gr << 8) | b;
                        }
                        int px = base_col + sx;
                        int py = base_row + sy;
                        fb[py * fb_w + px] = color;
                    }
                }
            }
        }
        return;
    }

    /* Fallback: per-cell averaged rendering */
    if (fb_w < w || fb_h < h) return;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            fge_sim_cell_t *c = cell_at(g, x, y);
            uint32_t color;
            if (cell_empty(c)) {
                color = 0xFF000000;
            } else {
                const fge_material_props_t *p = cell_props(c);
                color = p->color;
                if (g->sub && ss > 1) {
                    uint8_t *s = sub_at(g, x, y);
                    int ssn = ss * ss;
                    uint32_t r_sum = 0, g_sum = 0, b_sum = 0, n_solid = 0;
                    for (int i = 0; i < ssn; i++) {
                        if (s[i] != FGE_MAT_EMPTY) {
                            uint32_t sc = fge_sim_material_props((fge_material_t)s[i])->color;
                            r_sum += (sc >> 16) & 0xFF;
                            g_sum += (sc >> 8) & 0xFF;
                            b_sum += sc & 0xFF;
                            n_solid++;
                        }
                    }
                    if (n_solid > 0 && n_solid < (uint32_t)ssn) {
                        color = 0xFF000000 | ((r_sum / n_solid) << 16)
                                          | ((g_sum / n_solid) << 8)
                                          | (b_sum / n_solid);
                    }
                }
                int16_t t = c->temperature;
                int16_t default_t = fge_sim_material_default_temp((fge_material_t)c->material);
                int16_t diff = t - default_t;
                if (diff > 100) {
                    int shift = diff / 50; if (shift > 80) shift = 80;
                    uint8_t r = (uint8_t)FGE_MIN(255, (int)((color >> 16) & 0xFF) + shift);
                    uint8_t gr = (uint8_t)FGE_MIN(255, (int)((color >> 8) & 0xFF) + shift / 2);
                    uint8_t b = (uint8_t)FGE_MIN(255, (int)(color & 0xFF) + shift / 4);
                    color = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)gr << 8) | b;
                } else if (diff < -20) {
                    int shift = -diff / 20; if (shift > 60) shift = 60;
                    uint8_t r = (uint8_t)FGE_MIN(255, (int)((color >> 16) & 0xFF) + shift / 2);
                    uint8_t gr = (uint8_t)FGE_MIN(255, (int)((color >> 8) & 0xFF) + shift / 2);
                    uint8_t b = (uint8_t)FGE_MIN(255, (int)(color & 0xFF) + shift);
                    color = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)gr << 8) | b;
                }
                if (c->variant) {
                    int noise = (c->variant - 4) * 8;
                    uint8_t r = (uint8_t)((color >> 16) & 0xFF);
                    uint8_t gr = (uint8_t)((color >> 8) & 0xFF);
                    uint8_t b = (uint8_t)(color & 0xFF);
                    int nr = (int)r + noise; if (nr < 0) nr = 0; if (nr > 255) nr = 255;
                    int ng = (int)gr + noise; if (ng < 0) ng = 0; if (ng > 255) ng = 255;
                    int nb = (int)b + noise; if (nb < 0) nb = 0; if (nb > 255) nb = 255;
                    color = (color & 0xFF000000) | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
                }
            }
            fb[y * fb_w + x] = color;
        }
    }
}
