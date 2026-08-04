/*
 * forge/simulation.h — Noita-like Pixel Simulation Engine
 *
 * A cellular-automata based material simulation engine where every pixel
 * is a physically simulated cell. Supports solids, liquids, gases, fire,
 * heat transfer, chemical reactions, and phase changes.
 *
 * SUB-CELL RESOLUTION: Each grid cell can contain N×N sub-cells for finer
 * detail and wilder effects. Sub-scale of 1 = classic pixel sim.
 * Sub-scale of 4 = each display pixel has 4×4 simulated sub-pixels.
 *
 * Pure C23, zero external dependencies.
 */

#ifndef FORGE_SIMULATION_H
#define FORGE_SIMULATION_H

#include "forge/core.h"
#include "forge/math.h"
#include "forge/memory.h"

/* -------------------------------------------------------------------------- */
/* Material types                                                             */
/* -------------------------------------------------------------------------- */

typedef enum {
    FGE_MAT_EMPTY = 0,
    FGE_MAT_SAND,
    FGE_MAT_WATER,
    FGE_MAT_STONE,
    FGE_MAT_WOOD,
    FGE_MAT_FIRE,
    FGE_MAT_SMOKE,
    FGE_MAT_STEAM,
    FGE_MAT_OIL,
    FGE_MAT_LAVA,
    FGE_MAT_ACID,
    FGE_MAT_ICE,
    FGE_MAT_SNOW,
    FGE_MAT_DIRT,
    FGE_MAT_PLANT,
    FGE_MAT_METAL,
    FGE_MAT_GLASS,
    FGE_MAT_GUNPOWDER,
    FGE_MAT_COAL,
    FGE_MAT_COUNT
} fge_material_t;

typedef enum {
    FGE_STATE_EMPTY,
    FGE_STATE_SOLID,
    FGE_STATE_LIQUID,
    FGE_STATE_GAS,
    FGE_STATE_POWDER,
} fge_matter_state_t;

/* Material properties (read-only database) */
typedef struct {
    const char *name;
    fge_matter_state_t state;
    uint32_t color;           /* ARGB */
    float density;            /* kg/m^3 approx */
    int16_t melting_point;    /* Celsius */
    int16_t boiling_point;    /* Celsius */
    int16_t burn_temp;        /* temp at which it ignites */
    int16_t burn_rate;        /* how fast it burns (frames) */
    int16_t heat_capacity;    /* J/(kg·K) approx */
    int16_t conductivity;     /* heat transfer rate 0-255 */
    bool flammable;
    bool corrosive;
    bool radioactive;
    uint8_t lifetime;         /* 0 = infinite, else frames before death */
    fge_material_t burn_product;
    fge_material_t melt_product;
    fge_material_t boil_product;
    fge_material_t freeze_product;
} fge_material_props_t;

/* -------------------------------------------------------------------------- */
/* Cell                                                                       */
/* -------------------------------------------------------------------------- */

/* Sub-cell: each grid cell can contain sub_scale×sub_scale sub-cells.
 * A sub-cell is just a material byte. The cell's primary material is
 * the dominant one for quick broad-phase checks. */

typedef struct {
    uint8_t material;         /* dominant material */
    uint8_t flags;
    int16_t temperature;      /* Celsius */
    uint8_t life;             /* countdown timer (fire, plant, etc) */
    uint8_t variant;          /* color variation 0-7 */
} fge_sim_cell_t;

/* Cell flags */
#define FGE_CELL_UPDATED   0x01
#define FGE_CELL_SLEEPING  0x02
#define FGE_CELL_MOVED     0x04
#define FGE_CELL_BURNING   0x08

/* -------------------------------------------------------------------------- */
/* World Grid                                                                 */
/* -------------------------------------------------------------------------- */

typedef struct {
    fge_sim_cell_t *cells;    /* current frame */
    fge_sim_cell_t *back;     /* next frame (double buffer) */
    uint8_t *sub;             /* sub-cells: NULL if sub_scale==1 */
    int width;
    int height;
    int sub_scale;            /* 1 = no sub-cells, N = N×N per cell */
    int seed;                 /* RNG seed for deterministic noise */
    /* Active region: bounding box of non-empty cells (for performance) */
    int active_minx, active_maxx;
    int active_miny, active_maxy;
    bool active_dirty;        /* true if active region needs recalc */

    /* Chunked dirty tracking (for large worlds) */
    int chunk_size;           /* cells per chunk side (e.g. 64) */
    int chunks_x, chunks_y;   /* chunk grid dimensions */
    uint8_t *chunk_dirty;     /* 1 = chunk needs simulation */
    uint32_t *chunk_active;   /* bitset of active chunks */
} fge_sim_grid_t;

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

/* Get material properties by ID */
const fge_material_props_t *fge_sim_material_props(fge_material_t mat);

/* Get default temperature for a material */
int16_t fge_sim_material_default_temp(fge_material_t mat);

/* Initialize a grid (allocates cells + back buffer + optional sub-cells) */
bool fge_sim_grid_init(fge_sim_grid_t *g, int w, int h, int sub_scale);

/* Free grid memory */
void fge_sim_grid_free(fge_sim_grid_t *g);

/* Clear grid to EMPTY */
void fge_sim_grid_clear(fge_sim_grid_t *g);

/* Get cell at (x,y). Returns NULL if out of bounds. */
fge_sim_cell_t *fge_sim_grid_get(fge_sim_grid_t *g, int x, int y);

/* Get cell, clamped to edges (never returns NULL) */
fge_sim_cell_t *fge_sim_grid_get_clamped(fge_sim_grid_t *g, int x, int y);

/* Get sub-cell material. If sub_scale==1, returns cell material. */
uint8_t fge_sim_grid_get_sub(fge_sim_grid_t *g, int x, int y, int sx, int sy);

/* Set sub-cell material. Expands active region. */
void fge_sim_grid_set_sub(fge_sim_grid_t *g, int x, int y, int sx, int sy, fge_material_t mat);

/* Set entire cell (and all sub-cells) to a material */
void fge_sim_grid_set(fge_sim_grid_t *g, int x, int y, fge_material_t mat);

/* Paint a circle of material in world coordinates (sub-cell aware) */
void fge_sim_grid_paint_circle(fge_sim_grid_t *g, int cx, int cy, int r, fge_material_t mat);

/* -------------------------------------------------------------------------- */
/* Simulation Step                                                            */
/* -------------------------------------------------------------------------- */

/* Run one simulation step. Processes movement, heat, reactions, phase changes. */
void fge_sim_step(fge_sim_grid_t *g);

/* -------------------------------------------------------------------------- */
/* Rendering                                                                  */
/* -------------------------------------------------------------------------- */

/* Render grid into an RGBA framebuffer. fb must be width*height*4 bytes.
 * If grid has sub-cells, renders at grid resolution (caller downscales). */
void fge_sim_render(fge_sim_grid_t *g, uint32_t *fb, int fb_w, int fb_h);

#endif /* FORGE_SIMULATION_H */
