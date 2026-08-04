/*
 * examples/mp_demo/client.c — Multiplayer game client
 *
 * Uses platform_linux.c (proven X11 window path) with software renderer.
 */

#include "forge/core.h"
#include "forge/input.h"
#include "forge/log.h"
#include "forge/math.h"
#include "forge/memory.h"
#include "forge/net.h"
#include "forge/netcode.h"
#include "forge/platform.h"
#include "forge/renderer.h"
#include "forge/time.h"

#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_W 1280
#define WINDOW_H 720

/* -------------------------------------------------------------------------- */
/* Client state                                                               */
/* -------------------------------------------------------------------------- */

typedef enum {
  MP_STATE_CONNECTING,
  MP_STATE_CONNECTED,
  MP_STATE_DISCONNECTED,
} mp_client_state_t;

typedef struct {
  /* Connection */
  mp_client_state_t state;
  uint32_t client_id;
  uint32_t entity_id;
  uint32_t server_tick;
  uint32_t last_input_seq;

  /* Network */
  fge_net_context_t *net;
  fge_net_addr_t server_addr;
  uint32_t ping_seq;
  uint32_t last_ping_ms;
  float rtt_ms;

  /* Prediction */
  fge_input_history_t input_history;
  fge_nc_prediction_t prediction;
  fge_vec2_t predicted_pos;
  fge_vec2_t predicted_vel;
  float prediction_error;

  /* Remote entities */
  struct {
    uint32_t entity_id;
    fge_nc_interp_t interp;
    fge_vec2_t latest_pos;
    bool active;
  } remotes[MP_MAX_CLIENTS];

  /* Platform + renderer */
  fge_platform_t *platform;
  fge_renderer_t *renderer;

  /* Timing */
  fge_clock_t clock;
  fge_frame_time_t ft;
  uint64_t frame_count;
  double fps;
} mp_client_t;

static mp_client_t g_cli;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static uint32_t now_ms(void) {
  return (uint32_t)(fge_clock_elapsed_sec(&g_cli.clock) * 1000.0);
}

static void send_to_server(uint16_t msg_type, const uint8_t *data,
                           uint32_t len) {
  fge_net_send_udp(g_cli.net, &g_cli.server_addr, msg_type, data, len);
}

/* -------------------------------------------------------------------------- */
/* Connection                                                                 */
/* -------------------------------------------------------------------------- */

static void client_connect(const char *host, uint16_t port) {
  if (!fge_net_addr_from_string(&g_cli.server_addr, host, port)) {
    FGE_ERROR(FGE_LOG_CAT_NETWORK, "Invalid server address %s:%u", host, port);
    return;
  }
  g_cli.state = MP_STATE_CONNECTING;

  uint8_t buf[64];
  memset(buf, 0, sizeof(buf));
  const char *name = "Player";
  memcpy(buf, name, strlen(name) + 1);
  uint8_t *p = buf + 32;
  p += mp_write_u32(p, 1);
  send_to_server(MP_MSG_JOIN, buf, (uint32_t)(p - buf));

  FGE_INFO(FGE_LOG_CAT_NETWORK, "Connecting to %s:%u...", host, port);
}

static void handle_join_ack(const uint8_t *payload, uint32_t len) {
  if (len < 20)
    return;
  const uint8_t *p = payload;
  p += mp_read_u32(p, &g_cli.entity_id);
  p += mp_read_u32(p, &g_cli.server_tick);
  p += mp_read_u32(p, &g_cli.client_id);
  float world_w, world_h;
  p += mp_read_f32(p, &world_w);
  p += mp_read_f32(p, &world_h);

  g_cli.state = MP_STATE_CONNECTED;
  FGE_INFO(FGE_LOG_CAT_NETWORK,
           "Connected! Entity %u, client %u, world %.0fx%.0f", g_cli.entity_id,
           g_cli.client_id, world_w, world_h);
}

static void handle_state(const uint8_t *payload, uint32_t len) {
  const uint8_t *p = payload;
  uint32_t tick, your_id, your_last_seq, count;
  p += mp_read_u32(p, &tick);
  p += mp_read_u32(p, &your_id);
  p += mp_read_u32(p, &your_last_seq);
  p += mp_read_u32(p, &count);

  if (count > MP_MAX_CLIENTS)
    return;

  g_cli.server_tick = tick;
  g_cli.last_input_seq = your_last_seq;

  fge_input_acknowledge(&g_cli.input_history, your_last_seq);

  fge_vec2_t server_pos = g_cli.predicted_pos;
  fge_vec2_t server_vel = g_cli.predicted_vel;
  for (uint32_t i = 0; i < count; i++) {
    mp_entity_state_t e;
    p += mp_entity_deserialize(&e, p);
    if (e.entity_id == g_cli.entity_id) {
      server_pos = e.position;
      server_vel = e.velocity;
      break;
    }
  }

  g_cli.prediction_error = fge_v2_dist(g_cli.predicted_pos, server_pos);

  fge_vec2_t out_pos, out_vel;
  fge_nc_predict_reconcile(&g_cli.prediction, &g_cli.input_history,
                           your_last_seq, server_pos, server_vel, &out_pos,
                           &out_vel);
  g_cli.predicted_pos = out_pos;
  g_cli.predicted_vel = out_vel;

  for (uint32_t i = 0; i < count; i++) {
    mp_entity_state_t e;
    const uint8_t *ep = payload + 16 + i * 28;
    mp_entity_deserialize(&e, ep);
    if (e.entity_id == g_cli.entity_id)
      continue;

    int idx = -1;
    for (int j = 0; j < MP_MAX_CLIENTS; j++) {
      if (g_cli.remotes[j].active &&
          g_cli.remotes[j].entity_id == e.entity_id) {
        idx = j;
        break;
      }
      if (idx < 0 && !g_cli.remotes[j].active)
        idx = j;
    }
    if (idx < 0)
      continue;

    if (!g_cli.remotes[idx].active) {
      g_cli.remotes[idx].active = true;
      g_cli.remotes[idx].entity_id = e.entity_id;
      g_cli.remotes[idx].interp.head = 0;
      g_cli.remotes[idx].interp.count = 0;
      g_cli.remotes[idx].interp.interp_delay_ms = MP_INTERP_DELAY_MS;
    }
    fge_nc_interp_push(&g_cli.remotes[idx].interp, e.tick, e.position,
                       e.velocity, now_ms());
    g_cli.remotes[idx].latest_pos = e.position;
  }
}

static void handle_pong(const uint8_t *payload, uint32_t len) {
  if (len < 12)
    return;
  const uint8_t *p = payload;
  uint32_t client_time, server_time, seq;
  p += mp_read_u32(p, &client_time);
  p += mp_read_u32(p, &server_time);
  p += mp_read_u32(p, &seq);

  uint32_t rtt = now_ms() - client_time;
  g_cli.rtt_ms = g_cli.rtt_ms * 0.9f + (float)rtt * 0.1f;
}

/* -------------------------------------------------------------------------- */
/* Input & Prediction                                                         */
/* -------------------------------------------------------------------------- */

static void capture_input(float dt) {
  fge_event_t ev;
  float move_x = 0.0f, move_y = 0.0f;
  uint32_t buttons = 0;

  while (fge_platform_poll_event(g_cli.platform, &ev)) {
    if (ev.type == FGE_EVENT_CLOSE) {
      g_cli.platform->running = false;
    }
    if (ev.type == FGE_EVENT_KEY_DOWN || ev.type == FGE_EVENT_KEY_UP) {
      bool down = (ev.type == FGE_EVENT_KEY_DOWN);
      switch (ev.key.key) {
      case FGE_KEY_A:
      case FGE_KEY_LEFT:
        if (down)
          move_x = -1.0f;
        break;
      case FGE_KEY_D:
      case FGE_KEY_RIGHT:
        if (down)
          move_x = 1.0f;
        break;
      case FGE_KEY_W:
      case FGE_KEY_UP:
        if (down)
          move_y = -1.0f;
        break;
      case FGE_KEY_S:
      case FGE_KEY_DOWN:
        if (down)
          move_y = 1.0f;
        break;
      case FGE_KEY_SPACE:
        if (down)
          buttons |= FGE_BTN_JUMP;
        break;
      default:
        break;
      }
    }
  }

  uint32_t seq = fge_input_capture(&g_cli.input_history, move_x, move_y, 0, 0,
                                   buttons, 0, (uint16_t)(dt * 1000.0f));
  (void)seq;

  const fge_input_frame_t *f = fge_input_latest(&g_cli.input_history);
  if (f) {
    fge_nc_predict_apply(&g_cli.prediction, f, &g_cli.predicted_pos,
                         &g_cli.predicted_vel, dt);
  }

  if (g_cli.state == MP_STATE_CONNECTED) {
    fge_input_snapshot_t snap;
    uint32_t packed = fge_input_snapshot_pack(&g_cli.input_history, &snap,
                                              g_cli.last_input_seq, 8);
    if (packed > 0) {
      uint8_t buf[256];
      uint8_t *p = buf;
      p += mp_write_u32(p, g_cli.client_id);
      p += mp_write_u32(p, snap.start_sequence);
      p += mp_write_u32(p, snap.count);
      for (uint32_t i = 0; i < snap.count; i++) {
        p += mp_input_frame_serialize(&snap.frames[i], p);
      }
      send_to_server(MP_MSG_INPUT, buf, (uint32_t)(p - buf));
    }
  }
}

/* -------------------------------------------------------------------------- */
/* Rendering                                                                  */
/* -------------------------------------------------------------------------- */

static void render(void) {
  fge_renderer_begin(g_cli.renderer, 0xFF201410);

  float scale = (float)WINDOW_W / MP_WORLD_W;
  float cx = WINDOW_W / 2.0f;
  float cy = WINDOW_H / 2.0f;

  /* World bounds */
  fge_draw_rect(&g_cli.renderer->fb, (int)(cx - MP_WORLD_W * scale / 2),
                (int)(cy - MP_WORLD_H * scale / 2), (int)(MP_WORLD_W * scale),
                2, 0xFF333344);
  fge_draw_rect(&g_cli.renderer->fb, (int)(cx - MP_WORLD_W * scale / 2),
                (int)(cy + MP_WORLD_H * scale / 2), (int)(MP_WORLD_W * scale),
                2, 0xFF333344);
  fge_draw_rect(&g_cli.renderer->fb, (int)(cx - MP_WORLD_W * scale / 2),
                (int)(cy - MP_WORLD_H * scale / 2), 2,
                (int)(MP_WORLD_H * scale), 0xFF333344);
  fge_draw_rect(&g_cli.renderer->fb, (int)(cx + MP_WORLD_W * scale / 2),
                (int)(cy - MP_WORLD_H * scale / 2), 2,
                (int)(MP_WORLD_H * scale), 0xFF333344);

  /* Remote entities */
  uint32_t render_time = now_ms();
  for (int i = 0; i < MP_MAX_CLIENTS; i++) {
    if (!g_cli.remotes[i].active)
      continue;
    fge_vec2_t pos, vel;
    if (fge_nc_interp_sample(&g_cli.remotes[i].interp, render_time, &pos,
                             &vel)) {
      int sx = (int)(cx + (pos.x - MP_WORLD_W / 2.0f) * scale);
      int sy = (int)(cy + (pos.y - MP_WORLD_H / 2.0f) * scale);
      int r = (int)(MP_PLAYER_RADIUS * scale);
      fge_draw_circle(&g_cli.renderer->fb, fge_v2((float)sx, (float)sy),
                      (float)r, 0xFF4488FF);
    }
  }

  /* Local player */
  int px = (int)(cx + (g_cli.predicted_pos.x - MP_WORLD_W / 2.0f) * scale);
  int py = (int)(cy + (g_cli.predicted_pos.y - MP_WORLD_H / 2.0f) * scale);
  int pr = (int)(MP_PLAYER_RADIUS * scale);
  fge_draw_circle(&g_cli.renderer->fb, fge_v2((float)px, (float)py), (float)pr,
                  0xFF44FF44);

  /* Prediction error */
  if (g_cli.prediction_error > 1.0f) {
    int err = (int)FGE_MIN(g_cli.prediction_error * 2.0f, 32.0f);
    fge_draw_rect(&g_cli.renderer->fb, px - err / 2, py - pr - 20, err, err,
                  0x80FF4444);
  }

  /* Status text */
  char status[128];
  snprintf(status, sizeof(status), "FPS:%.0f RTT:%.0fms Err:%.1fpx Tick:%u",
           g_cli.fps, g_cli.rtt_ms, g_cli.prediction_error, g_cli.server_tick);
  fge_draw_text(&g_cli.renderer->fb, status, 4, 4, 0xFFCCCCCC, 1.0f);

  fge_renderer_end(g_cli.renderer);

  /* Present framebuffer to platform */
  if (g_cli.platform->swap_buffers) {
    g_cli.platform->framebuffer = &g_cli.renderer->fb;
    g_cli.platform->swap_buffers(g_cli.platform);
  }
}

/* -------------------------------------------------------------------------- */
/* Network event callback                                                     */
/* -------------------------------------------------------------------------- */

static void on_net_event(const fge_net_event_t *ev, void *userdata) {
  (void)userdata;
  if (ev->type != FGE_NET_EVENT_DATA)
    return;

  const uint8_t *payload = ev->msg.payload;
  uint32_t len = ev->msg.header.len;

  switch (ev->msg.header.type) {
  case MP_MSG_JOIN_ACK:
    handle_join_ack(payload, len);
    break;
  case MP_MSG_STATE:
    handle_state(payload, len);
    break;
  case MP_MSG_PONG:
    handle_pong(payload, len);
    break;
  default:
    break;
  }
}

/* -------------------------------------------------------------------------- */
/* Main                                                                       */
/* -------------------------------------------------------------------------- */

int main(int argc, char **argv) {
  const char *host = "127.0.0.1";
  uint16_t port = FGE_NET_DEFAULT_PORT;
  if (argc > 1)
    host = argv[1];
  if (argc > 2)
    port = (uint16_t)atoi(argv[2]);

  fge_log_init(FGE_LOG_LEVEL_INFO);
  fge_log_add_sink_stdout(FGE_LOG_LEVEL_INFO);
  fge_memzero(&g_cli, sizeof(g_cli));

  /* Platform (proven X11 window path) */
  g_cli.platform =
      fge_platform_create("FORGE MP Demo", WINDOW_W, WINDOW_H, false);
  if (!g_cli.platform) {
    FGE_ERROR(FGE_LOG_CAT_GENERAL, "Failed to create platform");
    return 1;
  }

  /* Software renderer */
  g_cli.renderer = malloc(sizeof(fge_renderer_t));
  if (!fge_renderer_init(g_cli.renderer, WINDOW_W, WINDOW_H)) {
    FGE_ERROR(FGE_LOG_CAT_GENERAL, "Failed to create renderer");
    return 1;
  }

  /* Network */
  g_cli.net = fge_net_create(on_net_event, NULL);
  if (!g_cli.net) {
    FGE_ERROR(FGE_LOG_CAT_NETWORK, "Failed to create net context");
    return 1;
  }
  if (!fge_net_bind_udp(g_cli.net, 0)) {
    FGE_ERROR(FGE_LOG_CAT_NETWORK, "Failed to bind UDP");
    return 1;
  }

  /* Prediction state */
  g_cli.predicted_pos = fge_v2(MP_WORLD_W / 2.0f, MP_WORLD_H / 2.0f);
  g_cli.predicted_vel = fge_v2(0, 0);

  /* Timing */
  fge_clock_init(&g_cli.clock);
  fge_frame_time_init(&g_cli.ft, 144.0);

  /* Connect */
  client_connect(host, port);

  FGE_INFO(FGE_LOG_CAT_GENERAL, "Client running. WASD to move.");

  g_cli.platform->running = true;
  while (g_cli.platform->running) {
    uint64_t frame_start = fge_clock_now(&g_cli.clock);

    fge_frame_time_update(&g_cli.ft, &g_cli.clock);
    float dt = (float)g_cli.ft.dt;
    if (dt > 0.1f)
      dt = 0.1f;

    /* Network */
    fge_net_poll(g_cli.net, 0);

    /* Ping every second */
    uint32_t now = now_ms();
    if (now - g_cli.last_ping_ms > 1000 && g_cli.state == MP_STATE_CONNECTED) {
      uint8_t buf[8];
      uint8_t *p = buf;
      p += mp_write_u32(p, now);
      p += mp_write_u32(p, g_cli.ping_seq++);
      send_to_server(MP_MSG_PING, buf, (uint32_t)(p - buf));
      g_cli.last_ping_ms = now;
    }

    /* Input & prediction */
    capture_input(dt);

    /* Render */
    render();

    /* Frame pacing */
    fge_frame_time_pace(&g_cli.ft, &g_cli.clock);

    g_cli.frame_count++;
    uint64_t frame_end = fge_clock_now(&g_cli.clock);
    double frame_ms =
        (double)(frame_end - frame_start) * 1000.0 / (double)g_cli.clock.freq;
    if (frame_ms > 0.0)
      g_cli.fps = 1000.0 / frame_ms;

    if ((g_cli.frame_count & 0xFF) == 0) {
      int remote_count = 0;
      for (int i = 0; i < MP_MAX_CLIENTS; i++) {
        if (g_cli.remotes[i].active)
          remote_count++;
      }
      FGE_INFO(FGE_LOG_CAT_GENERAL,
               "FPS: %.1f | RTT: %.0fms | Err: %.1fpx | Tick: %u | Remotes: %d",
               g_cli.fps, g_cli.rtt_ms, g_cli.prediction_error,
               g_cli.server_tick, remote_count);
    }
  }

  /* Cleanup */
  if (g_cli.state == MP_STATE_CONNECTED) {
    send_to_server(MP_MSG_LEAVE, NULL, 0);
  }
  if (g_cli.net)
    fge_net_destroy(g_cli.net);
  if (g_cli.renderer) {
    fge_renderer_shutdown(g_cli.renderer);
    free(g_cli.renderer);
  }
  if (g_cli.platform)
    fge_platform_destroy(g_cli.platform);

  return 0;
}
