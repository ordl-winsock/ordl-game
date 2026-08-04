/*
 * ORDL Audio — Zero-dependency PCM/WAV playback
 * Pure C23. Platform abstraction: Linux OSS (raw ioctls), Windows kernel32,
 * macOS AudioToolbox (system framework, optional).
 *
 * No external audio libraries linked. OSS constants defined inline.
 * Provides accessibility notification sounds and WAV playback.
 */

#include "forge/ui/ordl_audio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* -------------------------------------------------------------------------- */
/* Platform detection                                                         */
/* -------------------------------------------------------------------------- */

#if defined(__linux__)
#define UI_AUDIO_PLATFORM_LINUX 1
#elif defined(_WIN32)
#define UI_AUDIO_PLATFORM_WIN32 1
#elif defined(__APPLE__)
#define UI_AUDIO_PLATFORM_MACOS 1
#endif

/* -------------------------------------------------------------------------- */
/* Linux OSS — raw ioctls, no <sys/soundcard.h>                               */
/* -------------------------------------------------------------------------- */

#if UI_AUDIO_PLATFORM_LINUX

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

/* OSS ioctl constants defined inline to avoid libasound dependency */
#define OSS_IOCTL_MAGIC 'P'
#define OSS_SNDCTL_DSP_RESET       _IO(OSS_IOCTL_MAGIC, 0)
#define OSS_SNDCTL_DSP_SYNC        _IO(OSS_IOCTL_MAGIC, 1)
#define OSS_SNDCTL_DSP_SPEED       _IOWR(OSS_IOCTL_MAGIC, 2, int)
#define OSS_SNDCTL_DSP_STEREO      _IOWR(OSS_IOCTL_MAGIC, 3, int)
#define OSS_SNDCTL_DSP_GETBLKSIZE  _IOWR(OSS_IOCTL_MAGIC, 4, int)
#define OSS_SNDCTL_DSP_SETFMT      _IOWR(OSS_IOCTL_MAGIC, 5, int)
#define OSS_SNDCTL_DSP_CHANNELS    _IOWR(OSS_IOCTL_MAGIC, 6, int)

#define OSS_AFMT_U8        0x00000008
#define OSS_AFMT_S16_LE    0x00000010
#define OSS_AFMT_S16_BE    0x00000020
#define OSS_AFMT_S24_LE    0x00000080
#define OSS_AFMT_S32_LE    0x00001000

static bool linux_oss_open(ui_audio_ctx_t *ctx) {
    if (ctx->fd >= 0) return true;
    int fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0) fd = open("/dev/dsp1", O_WRONLY);
    if (fd < 0) fd = open("/dev/audio", O_WRONLY);
    if (fd < 0) return false;

    int fmt = OSS_AFMT_S16_LE;
    int ch = ctx->channels;
    int rate = ctx->sample_rate;

    if (ioctl(fd, OSS_SNDCTL_DSP_SETFMT, &fmt) < 0 ||
        ioctl(fd, OSS_SNDCTL_DSP_CHANNELS, &ch) < 0 ||
        ioctl(fd, OSS_SNDCTL_DSP_SPEED, &rate) < 0) {
        close(fd);
        return false;
    }
    ctx->fd = fd;
    return true;
}

static void linux_oss_close(ui_audio_ctx_t *ctx) {
    if (ctx->fd >= 0) {
        ioctl(ctx->fd, OSS_SNDCTL_DSP_SYNC, NULL);
        close(ctx->fd);
        ctx->fd = -1;
    }
}

static bool linux_oss_write(ui_audio_ctx_t *ctx, const void *data, size_t len) {
    if (ctx->fd < 0 && !linux_oss_open(ctx)) return false;
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        ssize_t n = write(ctx->fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

#endif /* UI_AUDIO_PLATFORM_LINUX */

/* -------------------------------------------------------------------------- */
/* Windows — kernel32 Beep(), no winmm.dll                                    */
/* -------------------------------------------------------------------------- */

#if UI_AUDIO_PLATFORM_WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

typedef struct {
    HWAVEOUT hwo;
    WAVEHDR hdr;
    bool busy;
} winmm_state_t;

static winmm_state_t g_winmm = {0};

/* Note: waveOut* functions are in winmm.dll which is a system DLL.
 * We dynamically load it to avoid linking against it explicitly. */
typedef MMRESULT (WINAPI *pfn_waveOutOpen)(HWAVEOUT*,UINT,LPCWAVEFORMATEX,DWORD,DWORD,DWORD);
typedef MMRESULT (WINAPI *pfn_waveOutPrepareHeader)(HWAVEOUT,LPWAVEHDR,UINT);
typedef MMRESULT (WINAPI *pfn_waveOutWrite)(HWAVEOUT,LPWAVEHDR,UINT);
typedef MMRESULT (WINAPI *pfn_waveOutUnprepareHeader)(HWAVEOUT,LPWAVEHDR,UINT);
typedef MMRESULT (WINAPI *pfn_waveOutClose)(HWAVEOUT);

static HMODULE g_winmm_dll = NULL;
static pfn_waveOutOpen p_waveOutOpen = NULL;
static pfn_waveOutPrepareHeader p_waveOutPrepareHeader = NULL;
static pfn_waveOutWrite p_waveOutWrite = NULL;
static pfn_waveOutUnprepareHeader p_waveOutUnprepareHeader = NULL;
static pfn_waveOutClose p_waveOutClose = NULL;

static bool winmm_load(void) {
    if (g_winmm_dll) return true;
    g_winmm_dll = LoadLibraryA("winmm.dll");
    if (!g_winmm_dll) return false;
    p_waveOutOpen = (pfn_waveOutOpen)GetProcAddress(g_winmm_dll, "waveOutOpen");
    p_waveOutPrepareHeader = (pfn_waveOutPrepareHeader)GetProcAddress(g_winmm_dll, "waveOutPrepareHeader");
    p_waveOutWrite = (pfn_waveOutWrite)GetProcAddress(g_winmm_dll, "waveOutWrite");
    p_waveOutUnprepareHeader = (pfn_waveOutUnprepareHeader)GetProcAddress(g_winmm_dll, "waveOutUnprepareHeader");
    p_waveOutClose = (pfn_waveOutClose)GetProcAddress(g_winmm_dll, "waveOutClose");
    return p_waveOutOpen && p_waveOutPrepareHeader && p_waveOutWrite &&
           p_waveOutUnprepareHeader && p_waveOutClose;
}

static bool winmm_init(ui_audio_ctx_t *ctx) {
    if (g_winmm.busy) return true;
    if (!winmm_load()) return false;
    WAVEFORMATEX wfx = {
        .wFormatTag = WAVE_FORMAT_PCM,
        .nChannels = (WORD)ctx->channels,
        .nSamplesPerSec = (DWORD)ctx->sample_rate,
        .wBitsPerSample = (WORD)ctx->bits,
        .nBlockAlign = (WORD)(ctx->channels * ctx->bits / 8),
        .nAvgBytesPerSec = (DWORD)(ctx->sample_rate * ctx->channels * ctx->bits / 8),
        .cbSize = 0,
    };
    MMRESULT r = p_waveOutOpen(&g_winmm.hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) return false;
    g_winmm.busy = true;
    return true;
}

static void winmm_shutdown(void) {
    if (g_winmm.busy && p_waveOutClose) {
        p_waveOutClose(g_winmm.hwo);
        g_winmm.busy = false;
    }
    if (g_winmm_dll) {
        FreeLibrary(g_winmm_dll);
        g_winmm_dll = NULL;
    }
}

static bool winmm_play(const void *data, size_t len) {
    if (!g_winmm.busy || !p_waveOutPrepareHeader) return false;
    g_winmm.hdr.lpData = (LPSTR)data;
    g_winmm.hdr.dwBufferLength = (DWORD)len;
    g_winmm.hdr.dwFlags = 0;
    p_waveOutPrepareHeader(g_winmm.hwo, &g_winmm.hdr, sizeof(g_winmm.hdr));
    p_waveOutWrite(g_winmm.hwo, &g_winmm.hdr, sizeof(g_winmm.hdr));
    while ((g_winmm.hdr.dwFlags & WHDR_DONE) == 0) {
        Sleep(1);
    }
    p_waveOutUnprepareHeader(g_winmm.hwo, &g_winmm.hdr, sizeof(g_winmm.hdr));
    return true;
}

#endif /* UI_AUDIO_PLATFORM_WIN32 */

/* -------------------------------------------------------------------------- */
/* macOS — AudioToolbox (system framework, no third-party install)            */
/* -------------------------------------------------------------------------- */

#if UI_AUDIO_PLATFORM_MACOS

#include <AudioToolbox/AudioQueue.h>
#include <CoreAudio/CoreAudioTypes.h>

typedef struct {
    AudioQueueRef queue;
    AudioQueueBufferRef buf;
    bool done;
} macaq_state_t;

static macaq_state_t g_macaq = {0};

static void macaq_callback(void *userData, AudioQueueRef inAQ,
                           AudioQueueBufferRef inBuffer) {
    (void)inAQ; (void)inBuffer;
    macaq_state_t *s = (macaq_state_t *)userData;
    s->done = true;
}

static bool macaq_init(ui_audio_ctx_t *ctx) {
    AudioStreamBasicDescription fmt = {
        .mSampleRate = (Float64)ctx->sample_rate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = (UInt32)(ctx->channels * ctx->bits / 8),
        .mFramesPerPacket = 1,
        .mBytesPerFrame = (UInt32)(ctx->channels * ctx->bits / 8),
        .mChannelsPerFrame = (UInt32)ctx->channels,
        .mBitsPerChannel = (UInt32)ctx->bits,
    };
    OSStatus st = AudioQueueNewOutput(&fmt, macaq_callback, &g_macaq,
                                      NULL, NULL, 0, &g_macaq.queue);
    if (st != noErr) return false;
    return true;
}

static bool macaq_play(const void *data, size_t len) {
    if (!g_macaq.queue) return false;
    g_macaq.done = false;
    OSStatus st = AudioQueueAllocateBuffer(g_macaq.queue, (UInt32)len, &g_macaq.buf);
    if (st != noErr) return false;
    memcpy(g_macaq.buf->mAudioData, data, len);
    g_macaq.buf->mAudioDataByteSize = (UInt32)len;
    AudioQueueEnqueueBuffer(g_macaq.queue, g_macaq.buf, 0, NULL);
    AudioQueueStart(g_macaq.queue, NULL);
    while (!g_macaq.done) {
        usleep(1000);
    }
    AudioQueueStop(g_macaq.queue, true);
    AudioQueueFreeBuffer(g_macaq.queue, g_macaq.buf);
    g_macaq.buf = NULL;
    return true;
}

static void macaq_shutdown(void) {
    if (g_macaq.queue) {
        AudioQueueDispose(g_macaq.queue, true);
        g_macaq.queue = NULL;
    }
}

#endif /* UI_AUDIO_PLATFORM_MACOS */

/* -------------------------------------------------------------------------- */
/* PCM synthesis                                                              */
/* -------------------------------------------------------------------------- */

static int16_t *generate_square_wave(int freq_hz, int sample_rate,
                                      int duration_ms, float volume,
                                      size_t *out_count) {
    size_t samples = (size_t)((sample_rate * duration_ms) / 1000);
    int16_t *buf = (int16_t *)malloc(samples * sizeof(int16_t));
    if (!buf) return NULL;
    int period = sample_rate / freq_hz;
    if (period < 1) period = 1;
    float amp = volume * 32767.0f;
    for (size_t i = 0; i < samples; i++) {
        int phase = (int)(i % (size_t)period);
        buf[i] = (phase < period / 2) ? (int16_t)amp : (int16_t)(-amp);
    }
    *out_count = samples;
    return buf;
}

static int16_t *generate_sine_wave(int freq_hz, int sample_rate,
                                    int duration_ms, float volume,
                                    size_t *out_count) {
    size_t samples = (size_t)((sample_rate * duration_ms) / 1000);
    int16_t *buf = (int16_t *)malloc(samples * sizeof(int16_t));
    if (!buf) return NULL;
    float amp = volume * 32767.0f;
    for (size_t i = 0; i < samples; i++) {
        float t = (float)i / (float)sample_rate;
        buf[i] = (int16_t)(amp * sinf(2.0f * 3.14159265f * freq_hz * t));
    }
    *out_count = samples;
    return buf;
}

/* -------------------------------------------------------------------------- */
/* WAV parser                                                                 */
/* -------------------------------------------------------------------------- */

#pragma pack(push, 1)
typedef struct {
    char riff[4];
    uint32_t file_size;
    char wave[4];
    char fmt_id[4];
    uint32_t fmt_size;
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits;
} wav_header_base_t;
#pragma pack(pop)

static bool parse_wav(const uint8_t *data, size_t len,
                      const int16_t **pcm, size_t *pcm_samples,
                      int *sample_rate, int *channels) {
    if (len < 44) return false;
    const wav_header_base_t *h = (const wav_header_base_t *)data;
    if (memcmp(h->riff, "RIFF", 4) != 0 ||
        memcmp(h->wave, "WAVE", 4) != 0 ||
        memcmp(h->fmt_id, "fmt ", 4) != 0) return false;
    if (h->format != 1) return false; /* PCM only */
    if (h->bits != 8 && h->bits != 16) return false;

    /* Find data chunk */
    size_t pos = 12 + 8 + h->fmt_size;
    uint32_t data_size = 0;
    const uint8_t *pcm_data = NULL;
    while (pos + 8 <= len) {
        const char *id = (const char *)(data + pos);
        uint32_t chunk_size = *(const uint32_t *)(data + pos + 4);
        if (memcmp(id, "data", 4) == 0) {
            data_size = chunk_size;
            pcm_data = data + pos + 8;
            break;
        }
        pos += 8 + chunk_size;
        if (chunk_size & 1) pos++;
    }
    if (!pcm_data || pcm_data + data_size > data + len) return false;

    *sample_rate = (int)h->sample_rate;
    *channels = (int)h->channels;

    if (h->bits == 16) {
        *pcm = (const int16_t *)pcm_data;
        *pcm_samples = data_size / sizeof(int16_t);
        return true;
    } else {
        size_t n = data_size;
        int16_t *converted = (int16_t *)malloc(n * sizeof(int16_t));
        if (!converted) return false;
        for (size_t i = 0; i < n; i++) {
            converted[i] = (int16_t)(((int)pcm_data[i] - 128) * 256);
        }
        *pcm = converted;
        *pcm_samples = n;
        return true; /* caller must free converted if it points outside data */
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

bool ui_audio_init(ui_audio_ctx_t *ctx) {
    if (!ctx) return false;
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
    ctx->sample_rate = 44100;
    ctx->channels = 1;
    ctx->bits = 16;

#if UI_AUDIO_PLATFORM_LINUX
    int test = open("/dev/dsp", O_WRONLY | O_NONBLOCK);
    if (test < 0) test = open("/dev/dsp1", O_WRONLY | O_NONBLOCK);
    if (test < 0) test = open("/dev/audio", O_WRONLY | O_NONBLOCK);
    if (test >= 0) {
        close(test);
        ctx->backend = UI_AUDIO_OSS;
        return true;
    }
    ctx->backend = UI_AUDIO_BELL;
    return true;
#elif UI_AUDIO_PLATFORM_WIN32
    ctx->backend = UI_AUDIO_WINMM;
    return true;
#elif UI_AUDIO_PLATFORM_MACOS
    ctx->backend = UI_AUDIO_MACAQ;
    return true;
#else
    ctx->backend = UI_AUDIO_BELL;
    return true;
#endif
}

void ui_audio_shutdown(ui_audio_ctx_t *ctx) {
    if (!ctx) return;
#if UI_AUDIO_PLATFORM_LINUX
    linux_oss_close(ctx);
#elif UI_AUDIO_PLATFORM_WIN32
    winmm_shutdown();
#elif UI_AUDIO_PLATFORM_MACOS
    macaq_shutdown();
#endif
    ctx->backend = UI_AUDIO_NONE;
}

bool ui_audio_beep(ui_audio_ctx_t *ctx, int freq_hz, int duration_ms) {
    if (!ctx || ctx->backend == UI_AUDIO_NONE) return false;
    if (freq_hz <= 0 || duration_ms <= 0) {
        putchar('\a');
        fflush(stdout);
        return true;
    }

#if UI_AUDIO_PLATFORM_LINUX
    if (ctx->backend == UI_AUDIO_OSS) {
        size_t count = 0;
        int16_t *samples = generate_square_wave(freq_hz, ctx->sample_rate,
                                                 duration_ms, 0.5f, &count);
        if (!samples) return false;
        bool ok = ui_audio_play_pcm(ctx, samples, count, ctx->sample_rate, 1);
        free(samples);
        return ok;
    }
#elif UI_AUDIO_PLATFORM_WIN32
    (void)ctx;
    Beep((DWORD)freq_hz, (DWORD)duration_ms);
    return true;
#elif UI_AUDIO_PLATFORM_MACOS
    if (ctx->backend == UI_AUDIO_MACAQ) {
        size_t count = 0;
        int16_t *samples = generate_square_wave(freq_hz, ctx->sample_rate,
                                                 duration_ms, 0.5f, &count);
        if (!samples) return false;
        bool ok = ui_audio_play_pcm(ctx, samples, count, ctx->sample_rate, 1);
        free(samples);
        return ok;
    }
#endif
    putchar('\a');
    fflush(stdout);
    return true;
}

bool ui_audio_play_pcm(ui_audio_ctx_t *ctx, const int16_t *samples,
                       size_t sample_count, int sample_rate, int channels) {
    if (!ctx || !samples || sample_count == 0) return false;

    ctx->sample_rate = sample_rate;
    ctx->channels = channels;
    ctx->bits = 16;

    size_t byte_len = sample_count * sizeof(int16_t);

#if UI_AUDIO_PLATFORM_LINUX
    if (ctx->backend == UI_AUDIO_OSS) {
        return linux_oss_write(ctx, samples, byte_len);
    }
#elif UI_AUDIO_PLATFORM_WIN32
    if (ctx->backend == UI_AUDIO_WINMM) {
        if (!winmm_init(ctx)) return false;
        return winmm_play(samples, byte_len);
    }
#elif UI_AUDIO_PLATFORM_MACOS
    if (ctx->backend == UI_AUDIO_MACAQ) {
        if (!macaq_init(ctx)) return false;
        return macaq_play(samples, byte_len);
    }
#endif
    return false;
}

bool ui_audio_play_wav(ui_audio_ctx_t *ctx, const char *path) {
    if (!ctx || !path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)len);
    if (!data) { fclose(f); return false; }
    if (fread(data, 1, (size_t)len, f) != (size_t)len) {
        free(data); fclose(f); return false;
    }
    fclose(f);

    const int16_t *pcm = NULL;
    size_t pcm_samples = 0;
    int sample_rate = 0, channels = 0;
    bool needs_free = false;
    bool ok = parse_wav(data, (size_t)len, &pcm, &pcm_samples,
                        &sample_rate, &channels);
    if (ok) {
        /* Detect if parse_wav allocated a converted buffer */
        if (pcm < (const int16_t *)data ||
            pcm >= (const int16_t *)(data + len)) {
            needs_free = true;
        }
        ok = ui_audio_play_pcm(ctx, pcm, pcm_samples, sample_rate, channels);
    }
    if (needs_free) free((void *)pcm);
    free(data);
    return ok;
}

bool ui_audio_tone(ui_audio_ctx_t *ctx, int freq_hz, int duration_ms,
                   float volume) {
    if (!ctx || freq_hz <= 0 || duration_ms <= 0) return false;
    size_t count = 0;
    int16_t *samples = generate_sine_wave(freq_hz, ctx->sample_rate,
                                           duration_ms, volume, &count);
    if (!samples) return false;
    bool ok = ui_audio_play_pcm(ctx, samples, count, ctx->sample_rate, 1);
    free(samples);
    return ok;
}

/* -------------------------------------------------------------------------- */
/* Predefined accessibility sounds                                            */
/* -------------------------------------------------------------------------- */

bool ui_audio_notify(ui_audio_ctx_t *ctx) {
    return ui_audio_tone(ctx, 880, 150, 0.3f);
}

bool ui_audio_alert(ui_audio_ctx_t *ctx) {
    bool a = ui_audio_tone(ctx, 880, 100, 0.4f);
    bool b = ui_audio_tone(ctx, 440, 200, 0.4f);
    return a && b;
}

bool ui_audio_success(ui_audio_ctx_t *ctx) {
    bool a = ui_audio_tone(ctx, 523, 100, 0.3f);
    bool b = ui_audio_tone(ctx, 659, 100, 0.3f);
    bool c = ui_audio_tone(ctx, 784, 150, 0.3f);
    return a && b && c;
}

bool ui_audio_error(ui_audio_ctx_t *ctx) {
    bool a = ui_audio_tone(ctx, 200, 200, 0.5f);
    bool b = ui_audio_tone(ctx, 150, 300, 0.5f);
    return a && b;
}
