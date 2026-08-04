/*
 * forge/netcode.h — Real-time MMORPG netcode architecture
 *
 * Features:
 *   - Reliable UDP (sequencing, ACK, selective resend, ordered channels)
 *   - Client-side prediction with input replay
 *   - Server reconciliation (authoritative state + input acknowledgment)
 *   - Entity interpolation (remote entities rendered in the past)
 *   - Delta compression for state snapshots
 *
 * Pure C23, zero external dependencies.
 */

#ifndef FORGE_NETCODE_H
#define FORGE_NETCODE_H

#include "forge/core.h"
#include "forge/math.h"
#include "forge/input.h"
#include "forge/ecs.h"
#include "forge/net.h"

/* -------------------------------------------------------------------------- */
/* Reliable UDP packet header                                                 */
/* -------------------------------------------------------------------------- */

#define FGE_NC_MAX_PACKET_SIZE 1200   /* below typical MTU */
#define FGE_NC_MAX_CHANNELS 8
#define FGE_NC_RESEND_MS 100
#define FGE_NC_TIMEOUT_MS 5000
#define FGE_NC_MAX_RELIABLE_PENDING 256

/* Channel types */
typedef enum {
    FGE_NC_CHANNEL_UNRELIABLE,       /* no guarantees, lowest latency */
    FGE_NC_CHANNEL_UNRELIABLE_ORDERED, /* ordered per-channel, drop old */
    FGE_NC_CHANNEL_RELIABLE,         /* guaranteed delivery, unordered */
    FGE_NC_CHANNEL_RELIABLE_ORDERED, /* guaranteed + ordered */
} fge_nc_channel_type_t;

typedef struct {
    uint32_t sequence;        /* packet sequence number */
    uint32_t ack;             /* last received remote sequence */
    uint32_t ack_bits;        /* bitmask of previous 32 received packets */
    uint16_t channel;         /* logical channel */
    uint16_t flags;           /* compressed, etc. */
    uint16_t payload_len;
} fge_nc_packet_header_t;

/* -------------------------------------------------------------------------- */
/* Reliable connection state                                                  */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t sequence;
    uint32_t sent_time_ms;
    uint32_t size;
    uint8_t data[FGE_NC_MAX_PACKET_SIZE];
    uint8_t resend_count;
    bool acked;
} fge_nc_pending_t;

typedef struct {
    /* Local/remote sequence tracking */
    uint32_t local_sequence;
    uint32_t remote_sequence;
    uint32_t received_mask;   /* 32-bit sliding window */
    uint32_t acked_mask;

    /* Reliable resend buffer */
    fge_nc_pending_t pending[FGE_NC_MAX_RELIABLE_PENDING];
    uint32_t pending_head;
    uint32_t pending_count;

    /* Ordered channel state */
    uint32_t channel_order[FGE_NC_MAX_CHANNELS]; /* last received ordered seq */

    /* Timing */
    uint32_t last_recv_time_ms;
    uint32_t last_send_time_ms;
    float rtt_ms;
    float rtt_variance;

    /* Remote address */
    fge_net_addr_t addr;
    bool connected;
} fge_nc_connection_t;

/* -------------------------------------------------------------------------- */
/* Netcode context                                                            */
/* -------------------------------------------------------------------------- */

typedef struct fge_net_context fge_net_context_t; /* from net.h */

typedef struct {
    fge_net_context_t *net;   /* underlying transport */
    fge_nc_connection_t conn;
    uint32_t protocol_id;
    uint64_t tick;            /* local simulation tick */
    double tick_accumulator;
    double tick_rate;         /* Hz, e.g. 60 */
} fge_nc_t;

/* Initialize netcode over existing net context */
bool fge_nc_init(fge_nc_t *nc, fge_net_context_t *net, uint32_t protocol_id,
                  double tick_rate);

/* Shutdown */
void fge_nc_shutdown(fge_nc_t *nc);

/* Connect to remote address (client) or accept (server-side init) */
bool fge_nc_connect(fge_nc_t *nc, const char *host, uint16_t port);

/* Process incoming/outgoing packets. Call every frame. */
void fge_nc_update(fge_nc_t *nc, uint32_t now_ms);

/* Send unreliable packet on channel */
bool fge_nc_send_unreliable(fge_nc_t *nc, uint16_t channel,
                             const uint8_t *data, uint32_t len);

/* Send reliable packet on channel */
bool fge_nc_send_reliable(fge_nc_t *nc, uint16_t channel,
                           const uint8_t *data, uint32_t len);

/* -------------------------------------------------------------------------- */
/* Client-side prediction                                                     */
/* -------------------------------------------------------------------------- */

/* Predicted entity state for rollback */
typedef struct {
    fge_vec2_t position;
    fge_vec2_t velocity;
    uint32_t tick;
} fge_nc_predicted_state_t;

typedef struct {
    fge_nc_predicted_state_t states[FGE_INPUT_HISTORY];
    uint32_t head;
    uint32_t count;
} fge_nc_prediction_t;

/* Save state before applying input */
void fge_nc_predict_save(fge_nc_prediction_t *p, uint32_t tick,
                          fge_vec2_t pos, fge_vec2_t vel);

/* Apply input to predicted state (client-side simulation) */
void fge_nc_predict_apply(fge_nc_prediction_t *p, const fge_input_frame_t *input,
                           fge_vec2_t *pos, fge_vec2_t *vel, float dt);

/* Rollback to tick and replay inputs forward. Returns corrected state. */
void fge_nc_predict_reconcile(fge_nc_prediction_t *p,
                               fge_input_history_t *inputs,
                               uint32_t server_tick,
                               fge_vec2_t server_pos,
                               fge_vec2_t server_vel,
                               fge_vec2_t *out_pos,
                               fge_vec2_t *out_vel);

/* -------------------------------------------------------------------------- */
/* Entity interpolation                                                       */
/* -------------------------------------------------------------------------- */

#define FGE_NC_INTERP_DELAY_MS 100   /* render remote entities 100ms in past */
#define FGE_NC_INTERP_BUFFER_SIZE 32

typedef struct {
    fge_vec2_t position;
    fge_vec2_t velocity;
    uint32_t tick;
    uint32_t received_ms;
} fge_nc_interp_state_t;

typedef struct {
    fge_nc_interp_state_t states[FGE_NC_INTERP_BUFFER_SIZE];
    uint32_t head;
    uint32_t count;
    uint32_t interp_delay_ms;
} fge_nc_interp_t;

/* Push a new authoritative state for interpolation */
void fge_nc_interp_push(fge_nc_interp_t *it, uint32_t tick,
                         fge_vec2_t pos, fge_vec2_t vel, uint32_t now_ms);

/* Sample interpolated state at render time */
bool fge_nc_interp_sample(const fge_nc_interp_t *it, uint32_t render_time_ms,
                           fge_vec2_t *out_pos, fge_vec2_t *out_vel);

/* -------------------------------------------------------------------------- */
/* Delta compression — bit-level field encoding                               */
/* -------------------------------------------------------------------------- */

/* Quantize float to fixed-point for network */
FGE_INLINE int16_t fge_nc_quantize_f32(float v, float min, float max, int bits) {
    float range = max - min;
    float t = (v - min) / range;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return (int16_t)(t * (float)((1 << bits) - 1));
}

FGE_INLINE float fge_nc_dequantize_f32(int16_t q, float min, float max, int bits) {
    float range = max - min;
    return min + ((float)q / (float)((1 << bits) - 1)) * range;
}

/* Quantize position to 16-bit within world bounds */
FGE_INLINE int16_t fge_nc_quantize_pos(float v, float world_min, float world_max) {
    return fge_nc_quantize_f32(v, world_min, world_max, 16);
}

FGE_INLINE float fge_nc_dequantize_pos(int16_t q, float world_min, float world_max) {
    return fge_nc_dequantize_f32(q, world_min, world_max, 16);
}

/* Delta-encode a component field against baseline */
typedef struct {
    uint32_t field_mask;   /* which fields changed */
    uint8_t data[256];     /* packed changed fields */
    uint32_t data_len;
} fge_nc_delta_t;

/* Build delta between current and baseline state. Returns true if changed. */
bool fge_nc_delta_build(const void *current, const void *baseline,
                         size_t struct_size, const uint32_t *field_offsets,
                         const uint8_t *field_sizes, uint32_t num_fields,
                         fge_nc_delta_t *out);

/* Apply delta to baseline state */
void fge_nc_delta_apply(void *baseline, const fge_nc_delta_t *delta,
                        const uint32_t *field_offsets, const uint8_t *field_sizes,
                        uint32_t num_fields);

#endif /* FORGE_NETCODE_H */
