/*
 * src/net/netcode.c — Real-time MMORPG netcode implementation
 */

#include "forge/netcode.h"
#include "forge/net.h"
#include "forge/memory.h"
#include "forge/time.h"

#include <string.h>

/* -------------------------------------------------------------------------- */
/* CRC32 for packet integrity                                                 */
/* -------------------------------------------------------------------------- */

static uint32_t crc32_table[256];
static bool crc32_init_done = false;

static void crc32_init(void) {
    if (crc32_init_done) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_init_done = true;
}

static uint32_t crc32(const void *data, size_t len) {
    crc32_init();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = crc32_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* -------------------------------------------------------------------------- */
/* Netcode                                                                    */
/* -------------------------------------------------------------------------- */

bool fge_nc_init(fge_nc_t *nc, fge_net_context_t *net, uint32_t protocol_id,
                  double tick_rate) {
    if (!nc || !net) return false;
    fge_memzero(nc, sizeof(*nc));
    nc->net = net;
    nc->protocol_id = protocol_id;
    nc->tick_rate = tick_rate > 0.0 ? tick_rate : 60.0;
    nc->tick = 0;
    return true;
}

void fge_nc_shutdown(fge_nc_t *nc) {
    if (!nc) return;
    fge_memzero(nc, sizeof(*nc));
}

bool fge_nc_connect(fge_nc_t *nc, const char *host, uint16_t port) {
    if (!nc) return false;
    fge_net_addr_t addr;
    if (!fge_net_addr_from_string(&addr, host, port)) return false;
    nc->conn.addr = addr;
    nc->conn.connected = true;
    nc->conn.local_sequence = 0;
    nc->conn.remote_sequence = 0;
    nc->conn.received_mask = 0;
    nc->conn.last_recv_time_ms = 0;
    return true;
}

static void fge_nc_process_ack(fge_nc_t *nc, uint32_t ack, uint32_t ack_bits) {
    fge_nc_connection_t *c = &nc->conn;
    /* Mark acked packets */
    for (uint32_t i = 0; i < FGE_NC_MAX_RELIABLE_PENDING; i++) {
        uint32_t idx = (c->pending_head + i) & (FGE_NC_MAX_RELIABLE_PENDING - 1);
        fge_nc_pending_t *p = &c->pending[idx];
        if (p->acked) continue;
        if (p->sequence == ack) {
            p->acked = true;
        } else if (ack_bits & (1u << (p->sequence - ack - 1))) {
            p->acked = true;
        }
    }
    /* Advance head past acked packets */
    while (c->pending_count > 0) {
        fge_nc_pending_t *p = &c->pending[c->pending_head];
        if (!p->acked) break;
        c->pending_head = (c->pending_head + 1) & (FGE_NC_MAX_RELIABLE_PENDING - 1);
        c->pending_count--;
    }
}

static void fge_nc_resend_unacked(fge_nc_t *nc, uint32_t now_ms) {
    fge_nc_connection_t *c = &nc->conn;
    for (uint32_t i = 0; i < c->pending_count; i++) {
        uint32_t idx = (c->pending_head + i) & (FGE_NC_MAX_RELIABLE_PENDING - 1);
        fge_nc_pending_t *p = &c->pending[idx];
        if (p->acked) continue;
        if (now_ms - p->sent_time_ms < FGE_NC_RESEND_MS) continue;
        /* Resend */
        fge_net_send_udp(nc->net, &c->addr, 0, p->data, p->size);
        p->sent_time_ms = now_ms;
        p->resend_count++;
        if (p->resend_count > 20) {
            /* Connection likely dead */
            c->connected = false;
        }
    }
}

void fge_nc_update(fge_nc_t *nc, uint32_t now_ms) {
    if (!nc || !nc->conn.connected) return;
    fge_nc_resend_unacked(nc, now_ms);
    /* Update RTT estimate based on latest ack */
    if (nc->conn.rtt_ms == 0.0f) nc->conn.rtt_ms = 50.0f;
}

bool fge_nc_send_unreliable(fge_nc_t *nc, uint16_t channel,
                             const uint8_t *data, uint32_t len) {
    if (!nc || !nc->conn.connected || len > FGE_NC_MAX_PACKET_SIZE - 16) return false;

    uint8_t packet[FGE_NC_MAX_PACKET_SIZE];
    fge_nc_packet_header_t *hdr = (fge_nc_packet_header_t *)packet;
    hdr->sequence = nc->conn.local_sequence++;
    hdr->ack = nc->conn.remote_sequence;
    hdr->ack_bits = nc->conn.received_mask;
    hdr->channel = channel;
    hdr->flags = 0;
    hdr->payload_len = (uint16_t)len;
    memcpy(packet + sizeof(*hdr), data, len);

    /* CRC32 over header (excluding CRC field) + payload */
    uint32_t crc = crc32(packet, sizeof(*hdr) + len);
    memcpy(packet + sizeof(*hdr) + len, &crc, 4);

    return fge_net_send_udp(nc->net, &nc->conn.addr, 0, packet, sizeof(*hdr) + len + 4);
}

bool fge_nc_send_reliable(fge_nc_t *nc, uint16_t channel,
                           const uint8_t *data, uint32_t len) {
    if (!nc || !nc->conn.connected || len > FGE_NC_MAX_PACKET_SIZE - 16) return false;
    if (nc->conn.pending_count >= FGE_NC_MAX_RELIABLE_PENDING) return false;

    uint32_t seq = nc->conn.local_sequence++;
    uint32_t idx = (nc->conn.pending_head + nc->conn.pending_count) & (FGE_NC_MAX_RELIABLE_PENDING - 1);
    fge_nc_pending_t *p = &nc->conn.pending[idx];
    p->sequence = seq;
    p->sent_time_ms = 0; /* will be set on first send */
    p->size = (uint32_t)(sizeof(fge_nc_packet_header_t) + len + 4);
    p->resend_count = 0;
    p->acked = false;

    fge_nc_packet_header_t *hdr = (fge_nc_packet_header_t *)p->data;
    hdr->sequence = seq;
    hdr->ack = nc->conn.remote_sequence;
    hdr->ack_bits = nc->conn.received_mask;
    hdr->channel = channel;
    hdr->flags = 1; /* reliable */
    hdr->payload_len = (uint16_t)len;
    memcpy(p->data + sizeof(*hdr), data, len);

    uint32_t crc = crc32(p->data, sizeof(*hdr) + len);
    memcpy(p->data + sizeof(*hdr) + len, &crc, 4);

    nc->conn.pending_count++;
    /* Send immediately */
    return fge_net_send_udp(nc->net, &nc->conn.addr, 0, p->data, p->size);
}

/* -------------------------------------------------------------------------- */
/* Prediction                                                                 */
/* -------------------------------------------------------------------------- */

void fge_nc_predict_save(fge_nc_prediction_t *p, uint32_t tick,
                          fge_vec2_t pos, fge_vec2_t vel) {
    if (!p) return;
    fge_nc_predicted_state_t *s = &p->states[p->head];
    s->position = pos;
    s->velocity = vel;
    s->tick = tick;
    p->head = (p->head + 1) & (FGE_INPUT_HISTORY - 1);
    if (p->count < FGE_INPUT_HISTORY) p->count++;
}

void fge_nc_predict_apply(fge_nc_prediction_t *p, const fge_input_frame_t *input,
                           fge_vec2_t *pos, fge_vec2_t *vel, float dt) {
    if (!p || !input || !pos || !vel) return;
    /* Simple movement: accelerate toward input direction */
    float accel = 400.0f;
    float max_speed = 200.0f;
    fge_vec2_t dir = fge_v2(input->move_x, input->move_y);
    if (fge_v2_len2(dir) > 1.0f) dir = fge_v2_norm(dir);
    *vel = fge_v2_add(*vel, fge_v2_mulf(dir, accel * dt));
    float speed = fge_v2_len(*vel);
    if (speed > max_speed) *vel = fge_v2_mulf(fge_v2_norm(*vel), max_speed);
    *pos = fge_v2_add(*pos, fge_v2_mulf(*vel, dt));
    /* Save predicted state for this tick */
    fge_nc_predict_save(p, input->tick, *pos, *vel);
}

void fge_nc_predict_reconcile(fge_nc_prediction_t *p,
                               fge_input_history_t *inputs,
                               uint32_t server_tick,
                               fge_vec2_t server_pos,
                               fge_vec2_t server_vel,
                               fge_vec2_t *out_pos,
                               fge_vec2_t *out_vel) {
    if (!p || !inputs || !out_pos || !out_vel) return;

    /* Find prediction state at server tick */
    fge_nc_predicted_state_t *pred = NULL;
    for (uint32_t i = 0; i < p->count; i++) {
        uint32_t idx = (p->head - 1 - i + FGE_INPUT_HISTORY) & (FGE_INPUT_HISTORY - 1);
        if (p->states[idx].tick == server_tick) {
            pred = &p->states[idx];
            break;
        }
    }

    /* Start from server state (authoritative) */
    fge_vec2_t pos = server_pos;
    fge_vec2_t vel = server_vel;

    /* If prediction differs significantly, snap to server */
    if (pred) {
        float err = fge_v2_dist(pred->position, server_pos);
        if (err > 32.0f) {
            /* Large error: hard snap */
            pred->position = server_pos;
            pred->velocity = server_vel;
        } else {
            /* Small error: blend */
            pred->position = fge_v2_lerp(pred->position, server_pos, 0.5f);
            pred->velocity = fge_v2_lerp(pred->velocity, server_vel, 0.5f);
        }
        pos = pred->position;
        vel = pred->velocity;
    }

    /* Replay unacknowledged inputs */
    uint32_t last_ack = server_tick;
    for (uint32_t i = 0; i < inputs->count; i++) {
        uint32_t idx = (inputs->head - inputs->count + i + FGE_INPUT_HISTORY) & (FGE_INPUT_HISTORY - 1);
        const fge_input_frame_t *f = &inputs->frames[idx];
        if (f->sequence <= last_ack) continue;
        float dt = (float)f->dt_ms * 0.001f;
        if (dt <= 0.0f) dt = 1.0f / 60.0f;
        fge_nc_predict_apply(p, f, &pos, &vel, dt);
    }

    *out_pos = pos;
    *out_vel = vel;
}

/* -------------------------------------------------------------------------- */
/* Interpolation                                                              */
/* -------------------------------------------------------------------------- */

void fge_nc_interp_push(fge_nc_interp_t *it, uint32_t tick,
                         fge_vec2_t pos, fge_vec2_t vel, uint32_t now_ms) {
    if (!it) return;
    fge_nc_interp_state_t *s = &it->states[it->head];
    s->position = pos;
    s->velocity = vel;
    s->tick = tick;
    s->received_ms = now_ms;
    it->head = (it->head + 1) & (FGE_NC_INTERP_BUFFER_SIZE - 1);
    if (it->count < FGE_NC_INTERP_BUFFER_SIZE) it->count++;
}

bool fge_nc_interp_sample(const fge_nc_interp_t *it, uint32_t render_time_ms,
                           fge_vec2_t *out_pos, fge_vec2_t *out_vel) {
    if (!it || !out_pos || it->count == 0) return false;

    uint32_t target = render_time_ms - it->interp_delay_ms;

    /* Find two states bracketing target time */
    const fge_nc_interp_state_t *a = NULL, *b = NULL;
    for (uint32_t i = 0; i < it->count - 1; i++) {
        uint32_t idx0 = (it->head - it->count + i + FGE_NC_INTERP_BUFFER_SIZE) & (FGE_NC_INTERP_BUFFER_SIZE - 1);
        uint32_t idx1 = (idx0 + 1) & (FGE_NC_INTERP_BUFFER_SIZE - 1);
        const fge_nc_interp_state_t *s0 = &it->states[idx0];
        const fge_nc_interp_state_t *s1 = &it->states[idx1];
        if (s0->received_ms <= target && s1->received_ms >= target) {
            a = s0; b = s1;
            break;
        }
    }

    if (!a || !b) {
        /* Use latest if target is beyond buffer */
        uint32_t idx = (it->head - 1 + FGE_NC_INTERP_BUFFER_SIZE) & (FGE_NC_INTERP_BUFFER_SIZE - 1);
        *out_pos = it->states[idx].position;
        if (out_vel) *out_vel = it->states[idx].velocity;
        return true;
    }

    float t = 0.0f;
    if (b->received_ms > a->received_ms) {
        t = (float)(target - a->received_ms) / (float)(b->received_ms - a->received_ms);
    }
    *out_pos = fge_v2_lerp(a->position, b->position, t);
    if (out_vel) *out_vel = fge_v2_lerp(a->velocity, b->velocity, t);
    return true;
}

/* -------------------------------------------------------------------------- */
/* Delta compression                                                          */
/* -------------------------------------------------------------------------- */

bool fge_nc_delta_build(const void *current, const void *baseline,
                         size_t struct_size, const uint32_t *field_offsets,
                         const uint8_t *field_sizes, uint32_t num_fields,
                         fge_nc_delta_t *out) {
    if (!current || !baseline || !out || !field_offsets || !field_sizes) return false;
    fge_memzero(out, sizeof(*out));
    const uint8_t *cur = (const uint8_t *)current;
    const uint8_t *base = (const uint8_t *)baseline;
    bool any_changed = false;

    for (uint32_t i = 0; i < num_fields; i++) {
        uint32_t off = field_offsets[i];
        uint8_t sz = field_sizes[i];
        if (off + sz > struct_size) continue;
        if (memcmp(cur + off, base + off, sz) != 0) {
            out->field_mask |= (1u << i);
            if (out->data_len + sz > sizeof(out->data)) return false;
            memcpy(out->data + out->data_len, cur + off, sz);
            out->data_len += sz;
            any_changed = true;
        }
    }
    return any_changed;
}

void fge_nc_delta_apply(void *baseline, const fge_nc_delta_t *delta,
                        const uint32_t *field_offsets, const uint8_t *field_sizes,
                        uint32_t num_fields) {
    if (!baseline || !delta || !field_offsets || !field_sizes) return;
    uint8_t *base = (uint8_t *)baseline;
    const uint8_t *p = delta->data;
    for (uint32_t i = 0; i < num_fields; i++) {
        if (!(delta->field_mask & (1u << i))) continue;
        uint32_t off = field_offsets[i];
        uint8_t sz = field_sizes[i];
        memcpy(base + off, p, sz);
        p += sz;
    }
}
