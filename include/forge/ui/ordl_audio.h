/*
 * ORDL Audio — Zero-dependency PCM/WAV playback
 * Pure C23. Platform abstraction: Linux OSS, Windows WinMM, macOS AudioQueue.
 *
 * Provides accessibility notification sounds and WAV playback
 * without external libraries.
 */

#ifndef ORDL_AUDIO_H
#define ORDL_AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Audio state                                                                */
/* -------------------------------------------------------------------------- */

typedef enum {
    UI_AUDIO_NONE,
    UI_AUDIO_OSS,       /* Linux /dev/dsp or OSS */
    UI_AUDIO_WINMM,     /* Windows waveOut */
    UI_AUDIO_MACAQ,     /* macOS AudioQueue */
    UI_AUDIO_BELL,      /* Terminal bell fallback */
} ui_audio_backend_t;

typedef struct {
    ui_audio_backend_t backend;
    int fd;             /* OSS file descriptor (-1 if closed) */
    int sample_rate;
    int channels;
    int bits;           /* 8 or 16 */
} ui_audio_ctx_t;

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

bool ui_audio_init(ui_audio_ctx_t *ctx);
void ui_audio_shutdown(ui_audio_ctx_t *ctx);

/* -------------------------------------------------------------------------- */
/* Notification beep (accessibility)                                          */
/* -------------------------------------------------------------------------- */

/* Play a simple tone. freq_hz=0 uses terminal bell fallback. */
bool ui_audio_beep(ui_audio_ctx_t *ctx, int freq_hz, int duration_ms);

/* -------------------------------------------------------------------------- */
/* WAV playback                                                               */
/* -------------------------------------------------------------------------- */

/* Parse and play a WAV file. Supports PCM 8/16-bit, mono/stereo. */
bool ui_audio_play_wav(ui_audio_ctx_t *ctx, const char *path);

/* Play raw PCM data (interleaved, little-endian, 16-bit signed) */
bool ui_audio_play_pcm(ui_audio_ctx_t *ctx, const int16_t *samples,
                       size_t sample_count, int sample_rate, int channels);

/* -------------------------------------------------------------------------- */
/* Synthesized sounds                                                         */
/* -------------------------------------------------------------------------- */

/* Generate and play a square wave tone */
bool ui_audio_tone(ui_audio_ctx_t *ctx, int freq_hz, int duration_ms,
                   float volume /* 0.0 .. 1.0 */);

/* Predefined accessibility sounds */
bool ui_audio_notify(ui_audio_ctx_t *ctx);   /* Short pleasant chime */
bool ui_audio_alert(ui_audio_ctx_t *ctx);    /* Attention sound */
bool ui_audio_success(ui_audio_ctx_t *ctx);  /* Confirmation sound */
bool ui_audio_error(ui_audio_ctx_t *ctx);    /* Error buzz */

#ifdef __cplusplus
}
#endif

#endif /* ORDL_AUDIO_H */
