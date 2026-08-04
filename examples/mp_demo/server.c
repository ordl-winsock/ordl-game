/*
 * examples/mp_demo/server.c — Authoritative game server
 *
 * - UDP socket on port 7777
 * - Accepts JOIN, assigns entity IDs
 * - Receives INPUT snapshots, applies to authoritative state
 * - Broadcasts STATE snapshots at 30 Hz
 * - Handles PING/PONG for RTT
 */

#include "forge/core.h"
#include "forge/math.h"
#include "forge/net.h"
#include "forge/time.h"
#include "forge/log.h"

#include "protocol.h"

#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Server state                                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
    bool active;
    fge_net_addr_t addr;
    uint32_t client_id;
    uint32_t entity_id;
    char name[32];
    uint32_t last_input_seq;
    fge_input_frame_t pending_inputs[FGE_INPUT_SNAPSHOT_MAX];
    uint32_t pending_count;
    uint32_t last_seen_ms;
} mp_client_t;

typedef struct {
    fge_net_context_t *net;
    fge_clock_t clock;
    uint32_t tick;
    uint32_t next_entity_id;

    mp_client_t clients[MP_MAX_CLIENTS];
    mp_entity_state_t entities[MP_MAX_CLIENTS];
    uint32_t entity_count;

    uint32_t snapshot_accum_ms;
} mp_server_t;

static mp_server_t g_srv;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static uint32_t now_ms(void) {
    return (uint32_t)(fge_clock_elapsed_sec(&g_srv.clock) * 1000.0);
}

static mp_client_t *find_client_by_addr(const fge_net_addr_t *addr) {
    for (int i = 0; i < MP_MAX_CLIENTS; i++) {
        if (!g_srv.clients[i].active) continue;
        if (memcmp(&g_srv.clients[i].addr.addr, &addr->addr, addr->len) == 0) {
            return &g_srv.clients[i];
        }
    }
    return NULL;
}

static mp_client_t *alloc_client(void) {
    for (int i = 0; i < MP_MAX_CLIENTS; i++) {
        if (!g_srv.clients[i].active) return &g_srv.clients[i];
    }
    return NULL;
}

static void remove_client(mp_client_t *c) {
    if (!c || !c->active) return;
    FGE_INFO(FGE_LOG_CAT_NETWORK, "Client %u (%s) disconnected", c->client_id, c->name);
    /* Remove entity */
    for (uint32_t i = 0; i < g_srv.entity_count; i++) {
        if (g_srv.entities[i].entity_id == c->entity_id) {
            g_srv.entities[i] = g_srv.entities[g_srv.entity_count - 1];
            g_srv.entity_count--;
            break;
        }
    }
    c->active = false;
}

/* -------------------------------------------------------------------------- */
/* Message handlers                                                           */
/* -------------------------------------------------------------------------- */

static void handle_join(const fge_net_addr_t *addr, const uint8_t *payload, uint32_t len) {
    if (len < 36) return; /* name[32] + version[4] */

    mp_client_t *c = find_client_by_addr(addr);
    if (c) return; /* already joined */

    c = alloc_client();
    if (!c) {
        FGE_WARN(FGE_LOG_CAT_NETWORK, "Server full, rejecting join");
        return;
    }

    memcpy(c->name, payload, 32);
    c->name[31] = '\0';
    c->addr = *addr;
    c->client_id = (uint32_t)(c - g_srv.clients);
    c->entity_id = g_srv.next_entity_id++;
    c->last_input_seq = 0;
    c->pending_count = 0;
    c->last_seen_ms = now_ms();
    c->active = true;

    /* Spawn entity */
    if (g_srv.entity_count < MP_MAX_CLIENTS) {
        mp_entity_state_t *e = &g_srv.entities[g_srv.entity_count++];
        e->entity_id = c->entity_id;
        e->last_input_seq = 0;
        e->position = fge_v2(MP_WORLD_W / 2.0f, MP_WORLD_H / 2.0f);
        e->velocity = fge_v2(0, 0);
        e->tick = g_srv.tick;
    }

    FGE_INFO(FGE_LOG_CAT_NETWORK, "Client %u (%s) joined, entity %u",
             c->client_id, c->name, c->entity_id);

    /* Send JOIN_ACK */
    uint8_t buf[64];
    uint8_t *p = buf;
    p += mp_write_u32(p, c->entity_id);
    p += mp_write_u32(p, g_srv.tick);
    p += mp_write_u32(p, c->client_id);
    p += mp_write_f32(p, MP_WORLD_W);
    p += mp_write_f32(p, MP_WORLD_H);
    fge_net_send_udp(g_srv.net, addr, MP_MSG_JOIN_ACK, buf, (uint32_t)(p - buf));
}

static void handle_input(const fge_net_addr_t *addr, const uint8_t *payload, uint32_t len) {
    mp_client_t *c = find_client_by_addr(addr);
    if (!c) return;

    const uint8_t *p = payload;
    uint32_t client_id, start_seq, count;
    p += mp_read_u32(p, &client_id);
    p += mp_read_u32(p, &start_seq);
    p += mp_read_u32(p, &count);

    if (count > FGE_INPUT_SNAPSHOT_MAX) return;
    if ((size_t)(p - payload) + count * 30 > len) return;

    c->last_seen_ms = now_ms();

    /* Store new inputs (ignore duplicates/olds) */
    for (uint32_t i = 0; i < count; i++) {
        fge_input_frame_t f;
        p += mp_input_frame_deserialize(&f, p);
        if (f.sequence > c->last_input_seq) {
            /* Shift pending if full */
            if (c->pending_count >= FGE_INPUT_SNAPSHOT_MAX) {
                memmove(&c->pending_inputs[0], &c->pending_inputs[1],
                        (FGE_INPUT_SNAPSHOT_MAX - 1) * sizeof(fge_input_frame_t));
                c->pending_count--;
            }
            c->pending_inputs[c->pending_count++] = f;
        }
    }
}

static void handle_ping(const fge_net_addr_t *addr, const uint8_t *payload, uint32_t len) {
    if (len < 8) return;
    uint32_t client_time, seq;
    const uint8_t *p = payload;
    p += mp_read_u32(p, &client_time);
    p += mp_read_u32(p, &seq);

    uint8_t buf[16];
    uint8_t *w = buf;
    w += mp_write_u32(w, client_time);
    w += mp_write_u32(w, now_ms());
    w += mp_write_u32(w, seq);
    fge_net_send_udp(g_srv.net, addr, MP_MSG_PONG, buf, (uint32_t)(w - buf));
}

static void handle_leave(const fge_net_addr_t *addr) {
    mp_client_t *c = find_client_by_addr(addr);
    if (c) remove_client(c);
}

/* -------------------------------------------------------------------------- */
/* Simulation                                                                 */
/* -------------------------------------------------------------------------- */

static void apply_inputs(mp_client_t *c, mp_entity_state_t *e, float dt) {
    /* Process all pending inputs */
    for (uint32_t i = 0; i < c->pending_count; i++) {
        const fge_input_frame_t *f = &c->pending_inputs[i];
        float frame_dt = (float)f->dt_ms * 0.001f;
        if (frame_dt <= 0.0f) frame_dt = dt;

        /* Movement */
        fge_vec2_t dir = fge_v2(f->move_x, f->move_y);
        if (fge_v2_len2(dir) > 1.0f) dir = fge_v2_norm(dir);
        e->velocity = fge_v2_mulf(dir, MP_PLAYER_SPEED);
        e->position = fge_v2_add(e->position, fge_v2_mulf(e->velocity, frame_dt));

        /* Bounds */
        e->position.x = FGE_CLAMP(e->position.x, MP_PLAYER_RADIUS, MP_WORLD_W - MP_PLAYER_RADIUS);
        e->position.y = FGE_CLAMP(e->position.y, MP_PLAYER_RADIUS, MP_WORLD_H - MP_PLAYER_RADIUS);

        e->last_input_seq = f->sequence;
        c->last_input_seq = f->sequence;
    }
    c->pending_count = 0;
    e->tick = g_srv.tick;
}

static void server_tick(float dt) {
    g_srv.tick++;

    /* Apply inputs for each client */
    for (int i = 0; i < MP_MAX_CLIENTS; i++) {
        mp_client_t *c = &g_srv.clients[i];
        if (!c->active) continue;
        /* Find entity */
        mp_entity_state_t *e = NULL;
        for (uint32_t j = 0; j < g_srv.entity_count; j++) {
            if (g_srv.entities[j].entity_id == c->entity_id) { e = &g_srv.entities[j]; break; }
        }
        if (e) apply_inputs(c, e, dt);
    }

    /* Timeout stale clients */
    uint32_t now = now_ms();
    for (int i = 0; i < MP_MAX_CLIENTS; i++) {
        mp_client_t *c = &g_srv.clients[i];
        if (!c->active) continue;
        if (now - c->last_seen_ms > 5000) {
            remove_client(c);
        }
    }
}

static void send_snapshot(void) {
    uint8_t buf[1024];
    uint8_t *p = buf;

    p += mp_write_u32(p, g_srv.tick);
    p += mp_write_u32(p, 0); /* your_entity_id filled per-client */
    p += mp_write_u32(p, 0); /* your_last_input_seq filled per-client */
    p += mp_write_u32(p, g_srv.entity_count);

    size_t header_len = (size_t)(p - buf);
    size_t entity_len = 28; /* serialized entity size */

    for (uint32_t i = 0; i < g_srv.entity_count; i++) {
        if (header_len + (i + 1) * entity_len > sizeof(buf)) break;
        p += mp_entity_serialize(&g_srv.entities[i], p);
    }

    /* Send to each client with their own entity info */
    for (int i = 0; i < MP_MAX_CLIENTS; i++) {
        mp_client_t *c = &g_srv.clients[i];
        if (!c->active) continue;

        uint8_t out[1024];
        memcpy(out, buf, (size_t)(p - buf));
        uint8_t *q = out;
        q += 4; /* skip tick */
        q += mp_write_u32(q, c->entity_id);
        q += mp_write_u32(q, c->last_input_seq);

        fge_net_send_udp(g_srv.net, &c->addr, MP_MSG_STATE, out, (uint32_t)(p - buf));
    }
}

/* -------------------------------------------------------------------------- */
/* Network event callback                                                     */
/* -------------------------------------------------------------------------- */

static void on_net_event(const fge_net_event_t *ev, void *userdata) {
    (void)userdata;
    if (ev->type == FGE_NET_EVENT_DATA && ev->msg.header.type != 0) {
        const uint8_t *payload = ev->msg.payload;
        uint32_t len = ev->msg.header.len;
        const fge_net_addr_t *addr = &ev->addr;

        switch (ev->msg.header.type) {
            case MP_MSG_JOIN:  handle_join(addr, payload, len); break;
            case MP_MSG_INPUT: handle_input(addr, payload, len); break;
            case MP_MSG_PING:  handle_ping(addr, payload, len); break;
            case MP_MSG_LEAVE: handle_leave(addr); break;
            default: break;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Main                                                                       */
/* -------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    fge_log_init(FGE_LOG_LEVEL_INFO);
    fge_log_add_sink_stdout(FGE_LOG_LEVEL_INFO);

    FGE_INFO(FGE_LOG_CAT_GENERAL, "Starting MP demo server on port %u", FGE_NET_DEFAULT_PORT);

    g_srv.net = fge_net_create(on_net_event, NULL);
    if (!g_srv.net) {
        FGE_ERROR(FGE_LOG_CAT_NETWORK, "Failed to create net context");
        return 1;
    }

    if (!fge_net_bind_udp(g_srv.net, FGE_NET_DEFAULT_PORT)) {
        FGE_ERROR(FGE_LOG_CAT_NETWORK, "Failed to bind UDP port %u", FGE_NET_DEFAULT_PORT);
        fge_net_destroy(g_srv.net);
        return 1;
    }

    fge_clock_init(&g_srv.clock);
    g_srv.tick = 0;
    g_srv.next_entity_id = 1;
    g_srv.entity_count = 0;

    FGE_INFO(FGE_LOG_CAT_GENERAL, "Server running. Tick rate %d Hz, snapshot %d Hz",
             MP_TICK_RATE, MP_SNAPSHOT_RATE);

    const float tick_dt = 1.0f / (float)MP_TICK_RATE;
    const float snapshot_dt = 1.0f / (float)MP_SNAPSHOT_RATE;
    float snapshot_accum = 0.0f;

    uint64_t last_tick_ns = 0;
    const uint64_t tick_ns = (uint64_t)(tick_dt * 1e9);

    while (1) {
        /* Poll network */
        fge_net_poll(g_srv.net, 0);

        /* Fixed-rate tick */
        uint64_t now_ns = (uint64_t)(fge_clock_elapsed_sec(&g_srv.clock) * 1e9);
        if (now_ns - last_tick_ns >= tick_ns) {
            server_tick(tick_dt);
            snapshot_accum += tick_dt;
            last_tick_ns = now_ns;

            /* Send snapshots at 30 Hz */
            if (snapshot_accum >= snapshot_dt) {
                send_snapshot();
                snapshot_accum = 0.0f;
            }
        }

        /* Yield */
        struct timespec ts = {0, 1000000}; /* 1ms */
        nanosleep(&ts, NULL);
    }

    fge_net_destroy(g_srv.net);
    return 0;
}
