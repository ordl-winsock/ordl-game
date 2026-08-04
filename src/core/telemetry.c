/*
 * src/core/telemetry.c — Event counters & frame timing
 *
 * Thread-safe via atomics. No dependencies on platform or renderer.
 */

#include "forge/telemetry.h"
#include <stdarg.h>
#include <stdio.h>

/* -------------------------------------------------------------------------- */
/* Counters (atomics for thread safety)                                       */
/* -------------------------------------------------------------------------- */

static _Atomic uint64_t g_counters[FGE_TELEM_COUNT];

void fge_telem_inc(fge_telem_event_t ev) {
    if (ev >= FGE_TELEM_COUNT) return;
    atomic_fetch_add_explicit(&g_counters[ev], 1, memory_order_relaxed);
}

uint64_t fge_telem_get(fge_telem_event_t ev) {
    if (ev >= FGE_TELEM_COUNT) return 0;
    return atomic_load_explicit(&g_counters[ev], memory_order_relaxed);
}

void fge_telem_reset_counters(void) {
    for (int i = 0; i < FGE_TELEM_COUNT; i++) {
        atomic_store_explicit(&g_counters[i], 0, memory_order_relaxed);
    }
}

/* -------------------------------------------------------------------------- */
/* Frame timing histogram                                                     */
/* -------------------------------------------------------------------------- */

static fge_telem_histogram_t g_hist;
static double g_sec_accumulator;
static uint32_t g_sec_frame_count;

void fge_telem_record_frame(double dt_ms) {
    g_hist.samples[g_hist.head] = dt_ms;
    g_hist.head = (g_hist.head + 1) % FGE_TELEM_HISTOGRAM_SIZE;
    if (g_hist.count < FGE_TELEM_HISTOGRAM_SIZE) g_hist.count++;

    /* Rolling averages */
    double sum = 0.0, min = 999999.0, max = 0.0;
    uint32_t n = g_hist.count;
    for (uint32_t i = 0; i < n; i++) {
        double v = g_hist.samples[i];
        sum += v;
        if (v < min) min = v;
        if (v > max) max = v;
    }
    g_hist.avg_ms = sum / n;
    g_hist.min_ms = min;
    g_hist.max_ms = max;

    /* Percentiles (simple sort copy) */
    double sorted[128];
    uint32_t sn = n < 128 ? n : 128;
    for (uint32_t i = 0; i < sn; i++) sorted[i] = g_hist.samples[i];
    /* bubble sort for small N */
    for (uint32_t i = 0; i < sn; i++) {
        for (uint32_t j = i + 1; j < sn; j++) {
            if (sorted[j] < sorted[i]) {
                double t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t;
            }
        }
    }
    g_hist.p95_ms = sorted[(uint32_t)(sn * 0.95)];
    g_hist.p99_ms = sorted[(uint32_t)(sn * 0.99)];

    /* Per-second average */
    g_sec_accumulator += dt_ms;
    g_sec_frame_count++;
    if (g_sec_accumulator >= 1000.0) {
        g_hist.one_sec_avg = g_sec_accumulator / g_sec_frame_count;
        g_hist.frames_this_sec = g_sec_frame_count;
        g_sec_accumulator = 0.0;
        g_sec_frame_count = 0;
    }
}

const fge_telem_histogram_t *fge_telem_get_histogram(void) {
    return &g_hist;
}

/* -------------------------------------------------------------------------- */
/* Input snapshot                                                             */
/* -------------------------------------------------------------------------- */

static fge_telem_input_t g_input_snap;

void fge_telem_record_input(uint32_t key, uint32_t mouse_btn,
                            float mx, float my, float mdx, float mdy,
                            const char *text) {
    if (key) { g_input_snap.last_key = key; g_input_snap.key_events++; }
    if (mouse_btn) { g_input_snap.last_mouse_btn = mouse_btn; g_input_snap.mouse_events++; }
    g_input_snap.mouse_x = mx;
    g_input_snap.mouse_y = my;
    g_input_snap.mouse_dx = mdx;
    g_input_snap.mouse_dy = mdy;
    if (text && text[0]) {
        for (int i = 0; i < 31 && text[i]; i++)
            g_input_snap.last_text[i] = text[i];
        g_input_snap.last_text[31] = '\0';
    }
}

const fge_telem_input_t *fge_telem_get_input(void) {
    return &g_input_snap;
}

/* -------------------------------------------------------------------------- */
/* Format report                                                              */
/* -------------------------------------------------------------------------- */

static int fmt(char *buf, size_t n, const char *fmt_str, ...) {
    va_list ap;
    va_start(ap, fmt_str);
    int r = vsnprintf(buf, n, fmt_str, ap);
    va_end(ap);
    return r > 0 ? r : 0;
}

size_t fge_telem_format_report(char *buf, size_t buflen) {
    if (!buf || buflen < 256) return 0;
    char *p = buf;
    size_t rem = buflen;
    int n;

#define APPEND(...) do { \
    n = fmt(p, rem, __VA_ARGS__); \
    if ((size_t)n >= rem) n = (int)rem - 1; \
    p += n; rem -= n; \
} while (0)

    const fge_telem_histogram_t *h = fge_telem_get_histogram();
    APPEND("=== FORGE TELEMETRY ===\n");
    APPEND("FPS: %.1f (avg %.2f ms)\n",
           h->frames_this_sec > 0 ? (1000.0 / h->one_sec_avg) : 0.0,
           h->avg_ms);
    APPEND("Frame: min=%.2f max=%.2f p95=%.2f p99=%.2f ms\n",
           h->min_ms, h->max_ms, h->p95_ms, h->p99_ms);
    APPEND("Frames: %llu  Renders: %llu  Swaps: %llu\n",
           (unsigned long long)fge_telem_get(FGE_TELEM_FRAME),
           (unsigned long long)fge_telem_get(FGE_TELEM_RENDER_CALL),
           (unsigned long long)fge_telem_get(FGE_TELEM_SWAP_BUFFER));
    APPEND("Keys: %llu  Mouse: %llu  Net recv/send: %llu/%llu\n",
           (unsigned long long)fge_telem_get(FGE_TELEM_KEY_PRESS),
           (unsigned long long)fge_telem_get(FGE_TELEM_MOUSE_MOVE),
           (unsigned long long)fge_telem_get(FGE_TELEM_NET_RECV),
           (unsigned long long)fge_telem_get(FGE_TELEM_NET_SEND));

#undef APPEND
    return (size_t)(p - buf);
}

void fge_telem_log_report(void) {
    char buf[1024];
    fge_telem_format_report(buf, sizeof(buf));
    fprintf(stdout, "%s", buf);
    fflush(stdout);
}
