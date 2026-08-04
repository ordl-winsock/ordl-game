/*
 * forge/input.h — Timestamped input frames for real-time netcode
 *
 * Captures input with tick/sequence timing for client-side prediction,
 * server reconciliation, and replay. Ring-buffer history for rollback.
 *
 * Pure C23, zero external dependencies.
 */

#ifndef FORGE_INPUT_H
#define FORGE_INPUT_H

#include "forge/core.h"
#include "forge/math.h"

/* -------------------------------------------------------------------------- */
/* Input frame — one tick of player input                                     */
/* -------------------------------------------------------------------------- */

#define FGE_INPUT_MAX_BUTTONS 32
#define FGE_INPUT_HISTORY 128   /* power of 2 */

typedef struct {
    uint64_t tick;            /* client/server tick number */
    uint32_t sequence;        /* monotonic input sequence number */
    float    move_x;          /* -1..1 analog stick or WASD composite */
    float    move_y;
    float    aim_x;           /* mouse world position or right-stick */
    float    aim_y;
    uint32_t buttons;         /* bitfield: jump, attack, ability, etc. */
    uint32_t modifiers;       /* shift, ctrl, alt */
    uint16_t dt_ms;           /* frame delta in milliseconds */
} fge_input_frame_t;

/* Button bit assignments (game-specific, but common defaults) */
#define FGE_BTN_ATTACK      (1u << 0)
#define FGE_BTN_JUMP        (1u << 1)
#define FGE_BTN_DASH        (1u << 2)
#define FGE_BTN_ABILITY_1   (1u << 3)
#define FGE_BTN_ABILITY_2   (1u << 4)
#define FGE_BTN_ABILITY_3   (1u << 5)
#define FGE_BTN_INTERACT    (1u << 6)
#define FGE_BTN_BLOCK       (1u << 7)
#define FGE_BTN_RELOAD      (1u << 8)

/* -------------------------------------------------------------------------- */
/* Input history — ring buffer for rollback/prediction                        */
/* -------------------------------------------------------------------------- */

typedef struct {
    fge_input_frame_t frames[FGE_INPUT_HISTORY];
    uint32_t head;            /* next write index */
    uint32_t count;           /* number of valid frames (0..HISTORY) */
    uint32_t next_sequence;   /* sequence counter */
} fge_input_history_t;

/* Initialize history */
void fge_input_history_init(fge_input_history_t *h);

/* Capture a new input frame into history. Returns assigned sequence. */
uint32_t fge_input_capture(fge_input_history_t *h,
                            float move_x, float move_y,
                            float aim_x, float aim_y,
                            uint32_t buttons, uint32_t modifiers,
                            uint16_t dt_ms);

/* Get frame by sequence number. Returns NULL if not in history. */
const fge_input_frame_t *fge_input_get(const fge_input_history_t *h, uint32_t sequence);

/* Get latest frame. Returns NULL if empty. */
const fge_input_frame_t *fge_input_latest(const fge_input_history_t *h);

/* Discard frames up to and including sequence (after server acknowledgment). */
void fge_input_acknowledge(fge_input_history_t *h, uint32_t sequence);

/* Reset history to empty */
void fge_input_history_clear(fge_input_history_t *h);

/* -------------------------------------------------------------------------- */
/* Input snapshot — packed for network transmission                           */
/* -------------------------------------------------------------------------- */

#define FGE_INPUT_SNAPSHOT_MAX 32

typedef struct {
    uint32_t start_sequence;
    uint32_t count;
    fge_input_frame_t frames[FGE_INPUT_SNAPSHOT_MAX];
} fge_input_snapshot_t;

/* Pack up to max_frames unacknowledged inputs into snapshot. */
size_t fge_input_snapshot_pack(const fge_input_history_t *h,
                                fge_input_snapshot_t *out,
                                uint32_t last_acked_sequence,
                                uint32_t max_frames);

/* Serialize snapshot to byte buffer. Returns bytes written. */
size_t fge_input_snapshot_serialize(const fge_input_snapshot_t *snap,
                                     uint8_t *buf, size_t buf_len);

/* Deserialize snapshot from byte buffer. Returns bytes read, 0 on error. */
size_t fge_input_snapshot_deserialize(fge_input_snapshot_t *snap,
                                       const uint8_t *buf, size_t buf_len);

#endif /* FORGE_INPUT_H */
