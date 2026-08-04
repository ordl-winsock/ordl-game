/*
 * ORDL UI — Debug logging utility
 * Comprehensive trace logging for TUI diagnostics.
 */

#ifndef ORDL_UI_DEBUG_H
#define ORDL_UI_DEBUG_H

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static inline void ui_debug_log(const char *fmt, ...) {
    FILE *f = fopen("/tmp/infercli_tui_debug.log", "a");
    if (!f) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "[%ld.%06ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fflush(f);
    fclose(f);
}

static inline void ui_debug_log_raw(const char *label, const char *data, size_t len) {
    FILE *f = fopen("/tmp/infercli_tui_debug.log", "a");
    if (!f) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "[%ld.%06ld] %s: ", (long)ts.tv_sec, ts.tv_nsec / 1000, label);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c >= 32 && c < 127) {
            fprintf(f, "%c", c);
        } else {
            fprintf(f, "\\x%02x", c);
        }
    }
    fprintf(f, " (%zu bytes)\n", len);
    fflush(f);
    fclose(f);
}

static inline void ui_debug_clear(void) {
    FILE *f = fopen("/tmp/infercli_tui_debug.log", "w");
    if (f) fclose(f);
}

#endif /* ORDL_UI_DEBUG_H */
