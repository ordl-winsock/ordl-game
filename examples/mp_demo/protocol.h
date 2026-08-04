/*
 * examples/mp_demo/protocol.h — Shared multiplayer protocol
 *
 * Message types, entity state, and serialization for the MP demo.
 * All messages are length-prefixed by the transport (net.h framing).
 */

#ifndef MP_PROTOCOL_H
#define MP_PROTOCOL_H

#include "forge/core.h"
#include "forge/math.h"
#include "forge/input.h"

/* -------------------------------------------------------------------------- */
/* Message types (uint16_t)                                                   */
/* -------------------------------------------------------------------------- */

#define MP_MSG_JOIN          1   /* client -> server: request join */
#define MP_MSG_JOIN_ACK      2   /* server -> client: assigned entity id */
#define MP_MSG_INPUT         3   /* client -> server: input snapshot */
#define MP_MSG_STATE         4   /* server -> client: world state snapshot */
#define MP_MSG_LEAVE         5   /* client -> server: graceful leave */
#define MP_MSG_PING          6   /* either: keepalive / RTT measurement */
#define MP_MSG_PONG          7   /* either: ping response */

/* -------------------------------------------------------------------------- */
/* Constants                                                                  */
/* -------------------------------------------------------------------------- */

#define MP_MAX_CLIENTS       16
#define MP_WORLD_W           640.0f
#define MP_WORLD_H           360.0f
#define MP_PLAYER_SPEED      200.0f
#define MP_PLAYER_RADIUS     8.0f
#define MP_TICK_RATE         60
#define MP_SNAPSHOT_RATE     30   /* server sends state at 30 Hz */
#define MP_INTERP_DELAY_MS   100  /* render remote players 100ms in past */

/* -------------------------------------------------------------------------- */
/* Entity state (server authoritative)                                        */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t entity_id;         /* 0 = invalid */
    uint32_t last_input_seq;    /* last processed input sequence */
    fge_vec2_t position;
    fge_vec2_t velocity;
    uint32_t tick;              /* server tick when state was captured */
} mp_entity_state_t;

/* -------------------------------------------------------------------------- */
/* Client -> Server: Join request                                             */
/* -------------------------------------------------------------------------- */

typedef struct {
    char name[32];
    uint32_t client_version;
} mp_join_request_t;

/* -------------------------------------------------------------------------- */
/* Server -> Client: Join acknowledgment                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t entity_id;
    uint32_t server_tick;
    uint32_t client_id;         /* index in server's client array */
    float world_w, world_h;
} mp_join_ack_t;

/* -------------------------------------------------------------------------- */
/* Client -> Server: Input snapshot                                           */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t client_id;
    uint32_t start_sequence;
    uint32_t count;
    fge_input_frame_t frames[FGE_INPUT_SNAPSHOT_MAX];
} mp_input_msg_t;

/* -------------------------------------------------------------------------- */
/* Server -> Client: State snapshot                                           */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t tick;
    uint32_t your_entity_id;
    uint32_t your_last_input_seq;   /* for client reconciliation */
    uint32_t entity_count;
    mp_entity_state_t entities[MP_MAX_CLIENTS];
} mp_state_msg_t;

/* -------------------------------------------------------------------------- */
/* Ping / Pong                                                                */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t client_time_ms;
    uint32_t sequence;
} mp_ping_t;

typedef struct {
    uint32_t client_time_ms;
    uint32_t server_time_ms;
    uint32_t sequence;
} mp_pong_t;

/* -------------------------------------------------------------------------- */
/* Serialization helpers                                                      */
/* -------------------------------------------------------------------------- */

/* All multi-byte integers are little-endian on the wire (x86/ARM native).
 * For cross-platform safety, we would add byteswap, but this demo targets
 * little-endian only. */

FGE_INLINE size_t mp_write_u32(uint8_t *buf, uint32_t v) {
    memcpy(buf, &v, 4); return 4;
}
FGE_INLINE size_t mp_write_u16(uint8_t *buf, uint16_t v) {
    memcpy(buf, &v, 2); return 2;
}
FGE_INLINE size_t mp_write_f32(uint8_t *buf, float v) {
    memcpy(buf, &v, 4); return 4;
}
FGE_INLINE size_t mp_read_u32(const uint8_t *buf, uint32_t *v) {
    memcpy(v, buf, 4); return 4;
}
FGE_INLINE size_t mp_read_u16(const uint8_t *buf, uint16_t *v) {
    memcpy(v, buf, 2); return 2;
}
FGE_INLINE size_t mp_read_f32(const uint8_t *buf, float *v) {
    memcpy(v, buf, 4); return 4;
}

/* Serialize entity state */
FGE_INLINE size_t mp_entity_serialize(const mp_entity_state_t *e, uint8_t *buf) {
    uint8_t *p = buf;
    p += mp_write_u32(p, e->entity_id);
    p += mp_write_u32(p, e->last_input_seq);
    p += mp_write_f32(p, e->position.x);
    p += mp_write_f32(p, e->position.y);
    p += mp_write_f32(p, e->velocity.x);
    p += mp_write_f32(p, e->velocity.y);
    p += mp_write_u32(p, e->tick);
    return (size_t)(p - buf);
}

FGE_INLINE size_t mp_entity_deserialize(mp_entity_state_t *e, const uint8_t *buf) {
    const uint8_t *p = buf;
    p += mp_read_u32(p, &e->entity_id);
    p += mp_read_u32(p, &e->last_input_seq);
    p += mp_read_f32(p, &e->position.x);
    p += mp_read_f32(p, &e->position.y);
    p += mp_read_f32(p, &e->velocity.x);
    p += mp_read_f32(p, &e->velocity.y);
    p += mp_read_u32(p, &e->tick);
    return (size_t)(p - buf);
}

/* Serialize input frame */
FGE_INLINE size_t mp_input_frame_serialize(const fge_input_frame_t *f, uint8_t *buf) {
    uint8_t *p = buf;
    p += mp_write_u32(p, (uint32_t)f->tick);
    p += mp_write_u32(p, f->sequence);
    p += mp_write_f32(p, f->move_x);
    p += mp_write_f32(p, f->move_y);
    p += mp_write_f32(p, f->aim_x);
    p += mp_write_f32(p, f->aim_y);
    p += mp_write_u32(p, f->buttons);
    p += mp_write_u32(p, f->modifiers);
    p += mp_write_u16(p, f->dt_ms);
    return (size_t)(p - buf);
}

FGE_INLINE size_t mp_input_frame_deserialize(fge_input_frame_t *f, const uint8_t *buf) {
    const uint8_t *p = buf;
    uint32_t tick32;
    p += mp_read_u32(p, &tick32); f->tick = tick32;
    p += mp_read_u32(p, &f->sequence);
    p += mp_read_f32(p, &f->move_x);
    p += mp_read_f32(p, &f->move_y);
    p += mp_read_f32(p, &f->aim_x);
    p += mp_read_f32(p, &f->aim_y);
    p += mp_read_u32(p, &f->buttons);
    p += mp_read_u32(p, &f->modifiers);
    p += mp_read_u16(p, &f->dt_ms);
    return (size_t)(p - buf);
}

#endif /* MP_PROTOCOL_H */
