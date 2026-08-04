/*
 * forge/telemetry.h — Event counters & frame timing (data collection only)
 *
 * No rendering. No platform dependencies. Pure data.
 * Collected here, displayed elsewhere (debug_overlay.h).
 */

#ifndef FORGE_TELEMETRY_H
#define FORGE_TELEMETRY_H

#include "forge/core.h"

/* -------------------------------------------------------------------------- */
/* Event counters                                                             */
/* -------------------------------------------------------------------------- */

typedef enum {
    FGE_TELEM_KEY_PRESS,
    FGE_TELEM_KEY_RELEASE,
    FGE_TELEM_MOUSE_MOVE,
    FGE_TELEM_MOUSE_DOWN,
    FGE_TELEM_MOUSE_UP,
    FGE_TELEM_MOUSE_SCROLL,
    FGE_TELEM_RESIZE,
    FGE_TELEM_FOCUS_GAIN,
    FGE_TELEM_FOCUS_LOST,
    FGE_TELEM_CLOSE,
    FGE_TELEM_FRAME,
    FGE_TELEM_RENDER_CALL,
    FGE_TELEM_SWAP_BUFFER,
    FGE_TELEM_NET_RECV,
    FGE_TELEM_NET_SEND,
    FGE_TELEM_PHYS_STEP,
    FGE_TELEM_ECS_QUERY,
    FGE_TELEM_COUNT
} fge_telem_event_t;

void     fge_telem_inc(fge_telem_event_t ev);
uint64_t fge_telem_get(fge_telem_event_t ev);
void     fge_telem_reset_counters(void);

/* -------------------------------------------------------------------------- */
/* Frame timing histogram (rolling, 120 samples)                              */
/* -------------------------------------------------------------------------- */

#define FGE_TELEM_HISTOGRAM_SIZE 120

typedef struct {
    double   samples[FGE_TELEM_HISTOGRAM_SIZE];
    uint32_t head;
    uint32_t count;
    double   avg_ms;
    double   min_ms;
    double   max_ms;
    double   p95_ms;
    double   p99_ms;
    double   one_sec_avg;   /* averaged over last second */
    uint32_t frames_this_sec;
} fge_telem_histogram_t;

void fge_telem_record_frame(double dt_ms);
const fge_telem_histogram_t *fge_telem_get_histogram(void);

/* -------------------------------------------------------------------------- */
/* Input snapshot (last frame's input state)                                  */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t last_key;
    uint32_t last_mouse_btn;
    float    mouse_x, mouse_y;
    float    mouse_dx, mouse_dy;
    int      key_events;
    int      mouse_events;
    char     last_text[32];
} fge_telem_input_t;

void fge_telem_record_input(uint32_t key, uint32_t mouse_btn,
                            float mx, float my, float mdx, float mdy,
                            const char *text);
const fge_telem_input_t *fge_telem_get_input(void);

/* -------------------------------------------------------------------------- */
/* Full formatted report                                                      */
/* -------------------------------------------------------------------------- */

size_t fge_telem_format_report(char *buf, size_t buflen);
void   fge_telem_log_report(void);

#endif /* FORGE_TELEMETRY_H */
