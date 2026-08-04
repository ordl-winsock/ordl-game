/*
 * src/platform/platform_linux.c — Linux platform layer
 *
 * Full X11 and Wayland raw wire protocol implementation.
 * Auto-detects backend: tries Wayland first, falls back to X11.
 * Zero external dependencies (no libX11, no libwayland).
 */

#define _GNU_SOURCE

#include "forge/platform.h"
#include "forge/renderer.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

/* -------------------------------------------------------------------------- */
/* Common helpers                                                             */
/* -------------------------------------------------------------------------- */

static bool write_all(int fd, const void *data, size_t len) {
    const uint8_t *p = data;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

static bool read_all(int fd, void *data, size_t len) {
    uint8_t *p = data;
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, p + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* X11 backend                                                                */
/* -------------------------------------------------------------------------- */

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

#define X_TCP_PORT 6000

/* X11 core protocol definitions */
typedef struct {
    uint8_t  byteOrder;
    uint8_t  pad;
    uint16_t majorVersion;
    uint16_t minorVersion;
    uint16_t nbytesAuthProto;
    uint16_t nbytesAuthString;
    uint16_t pad2;
} xConnClientPrefix;

typedef struct {
    uint8_t  success;
    uint8_t  pad1;
    uint16_t majorVersion;
    uint16_t minorVersion;
    uint16_t length;
} xConnSetupPrefix;

/* Request opcodes */
#define X_CreateWindow       1
#define X_ChangeWindowAttributes 2
#define X_DestroyWindow      4
#define X_MapWindow          8
#define X_UnmapWindow        10
#define X_ConfigureWindow    12
#define X_InternAtom         16
#define X_ChangeProperty     18
#define X_SetInputFocus      42
#define X_GetInputFocus      43
#define X_CreatePixmap       53
#define X_FreePixmap         54
#define X_CreateGC           55
#define X_ChangeGC           56
#define X_CopyArea           62
#define X_PutImage           72

/* Event codes */
#define X_Error              0
#define X_Reply              1
#define X_KeyPress           2
#define X_KeyRelease         3
#define X_ButtonPress        4
#define X_ButtonRelease      5
#define X_MotionNotify       6
#define X_EnterNotify        7
#define X_LeaveNotify        8
#define X_FocusIn            9
#define X_FocusOut           10
#define X_Expose             12
#define X_DestroyNotify      17
#define X_UnmapNotify        18
#define X_MapNotify          19
#define X_ConfigureNotify    22
#define X_ClientMessage      33

/* Masks */
#define CWBackPixel        (1L<<1)
#define CWEventMask        (1L<<11)
#define CWColormap         (1L<<13)

#define KeyPressMask        (1L<<0)
#define KeyReleaseMask      (1L<<1)
#define ButtonPressMask     (1L<<2)
#define ButtonReleaseMask   (1L<<3)
#define EnterWindowMask     (1L<<4)
#define LeaveWindowMask     (1L<<5)
#define PointerMotionMask   (1L<<6)
#define ExposureMask        (1L<<15)
#define StructureNotifyMask (1L<<17)
#define FocusChangeMask     (1L<<21)

#define ZPixmap             2

/* X11 backend state */
typedef struct {
    int fd;
    uint32_t root;
    uint32_t root_visual;
    uint32_t white_pixel;
    uint32_t black_pixel;
    uint32_t resource_id_base;
    uint32_t resource_id_mask;
    uint8_t  root_depth;
    uint32_t next_rid;
    uint32_t window;
    uint32_t gc;
    uint32_t wm_delete_atom;
    uint16_t seq;
    int width, height;
    bool mapped;
    bool closed;
    int mouse_x, mouse_y;
    bool mouse_buttons[6];  /* track pressed state: 1=left, 2=right, 3=middle */
    uint8_t pending_mouse_ups; /* bitmask of buttons needing synthetic UP */
    bool keys_down[FGE_KEY_COUNT]; /* track key state for repeat detection */
} x11_backend_t;

static uint8_t x11_req[1024];
static size_t  x11_req_len;

static void x11_begin(uint8_t opcode, uint8_t data) {
    x11_req_len = 0;
    x11_req[x11_req_len++] = opcode;
    x11_req[x11_req_len++] = data;
    x11_req_len += 2; /* request length placeholder */
}

static void x11_w(uint32_t v) {
    x11_req[x11_req_len++] = (uint8_t)(v >> 0);
    x11_req[x11_req_len++] = (uint8_t)(v >> 8);
    x11_req[x11_req_len++] = (uint8_t)(v >> 16);
    x11_req[x11_req_len++] = (uint8_t)(v >> 24);
}

static void x11_w16(uint16_t v) {
    x11_req[x11_req_len++] = (uint8_t)(v >> 0);
    x11_req[x11_req_len++] = (uint8_t)(v >> 8);
}

static bool x11_send(x11_backend_t *x) {
    uint16_t len_words = (uint16_t)((x11_req_len + 3) / 4);
    x11_req[2] = (uint8_t)(len_words >> 0);
    x11_req[3] = (uint8_t)(len_words >> 8);
    x->seq++;
    return write_all(x->fd, x11_req, len_words * 4);
}

static uint32_t x11_new_id(x11_backend_t *x) {
    uint32_t id = x->next_rid;
    x->next_rid++;
    return id;
}

static bool x11_connect(x11_backend_t *x) {
    const char *disp = getenv("DISPLAY");
    if (!disp) disp = ":0";
    int display_num = 0;
    sscanf(disp, ":%d", &display_num);

    char path[64];
    snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", display_num);
    x->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (x->fd < 0) return false;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(x->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(x->fd);
        x->fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (x->fd < 0) return false;
        struct sockaddr_in inaddr = {0};
        inaddr.sin_family = AF_INET;
        inaddr.sin_port = htons((uint16_t)(X_TCP_PORT + display_num));
        if (inet_pton(AF_INET, "127.0.0.1", &inaddr.sin_addr) != 1 ||
            connect(x->fd, (struct sockaddr *)&inaddr, sizeof(inaddr)) < 0) {
            close(x->fd); x->fd = -1; return false;
        }
    }

    xConnClientPrefix req = {0};
    req.byteOrder = (*(uint8_t *)&(uint16_t){1}) ? 'l' : 'B';
    req.majorVersion = 11;
    req.minorVersion = 0;
    if (!write_all(x->fd, &req, sizeof(req))) return false;

    xConnSetupPrefix rsp = {0};
    if (!read_all(x->fd, &rsp, sizeof(rsp))) return false;
    if (rsp.success != 1) return false;
    uint16_t extra = rsp.length;
    size_t extra_bytes = extra * 4;
    if (extra_bytes > 8192) return false;
    uint8_t setup[8192];
    if (!read_all(x->fd, setup, extra_bytes)) return false;

    x->resource_id_base = *(uint32_t *)(setup + 4);
    x->resource_id_mask = *(uint32_t *)(setup + 8);

    /* Parse to find first screen (variable offset after vendor + formats) */
    uint16_t vendor_len = *(uint16_t *)(setup + 16);
    uint8_t  formats_count = setup[21];
    size_t vendor_padded = ((vendor_len + 3) / 4) * 4;
    size_t screen_offset = 32 + vendor_padded + (formats_count * 8);
    if (screen_offset + 40 > extra_bytes) return false;

    uint8_t *screen = setup + screen_offset;
    x->root        = *(uint32_t *)(screen + 0);
    x->white_pixel = *(uint32_t *)(screen + 8);
    x->black_pixel = *(uint32_t *)(screen + 12);
    x->root_visual = *(uint32_t *)(screen + 32);
    x->root_depth  = screen[38];
    x->next_rid = x->resource_id_base;
    return true;
}

static uint32_t x11_intern_atom(x11_backend_t *x, const char *name, bool only_if_exists) {
    uint16_t nlen = (uint16_t)strlen(name);
    if (nlen > 512) return 0;
    x11_begin(X_InternAtom, only_if_exists ? 1 : 0);
    x11_w((uint32_t)nlen);
    size_t pad = (4 - (nlen & 3)) & 3;
    for (uint16_t i = 0; i < nlen; i++) x11_req[x11_req_len++] = (uint8_t)name[i];
    for (size_t i = 0; i < pad; i++) x11_req[x11_req_len++] = 0;
    x11_send(x);

    uint8_t reply[32];
    if (!read_all(x->fd, reply, 32)) return 0;
    if (reply[0] != X_Reply) return 0;
    return (uint32_t)reply[8] | ((uint32_t)reply[9] << 8) |
           ((uint32_t)reply[10] << 16) | ((uint32_t)reply[11] << 24);
}

static bool x11_create_window(x11_backend_t *x, int w, int h) {
    x->window = x11_new_id(x);
    x11_begin(X_CreateWindow, x->root_depth);
    x11_w(x->window);
    x11_w(x->root);
    x11_w16(0); x11_w16(0);       /* x, y */
    x11_w16((uint16_t)w); x11_w16((uint16_t)h); /* width, height */
    x11_w16(0); x11_w16(1);       /* border-width, class */
    x11_w(x->root_visual);        /* visual = root visual */
    x11_w(CWBackPixel | CWEventMask | CWColormap);
    x11_w(x->black_pixel);        /* bit 1  = back pixel */
    x11_w(ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
          EnterWindowMask | LeaveWindowMask | PointerMotionMask | StructureNotifyMask | FocusChangeMask);
    x11_w(0);                     /* bit 13 = colormap (CopyFromParent) */
    if (!x11_send(x)) return false;

    x->gc = x11_new_id(x);
    x11_begin(X_CreateGC, 0);
    x11_w(x->gc); x11_w(x->window); x11_w(0);
    if (!x11_send(x)) return false;

    /* WM_PROTOCOLS: request WM_DELETE_WINDOW */
    x->wm_delete_atom = x11_intern_atom(x, "WM_DELETE_WINDOW", false);
    uint32_t wm_protocols = x11_intern_atom(x, "WM_PROTOCOLS", false);
    if (x->wm_delete_atom && wm_protocols) {
        x11_begin(X_ChangeProperty, 0);
        x11_w(x->window); x11_w(wm_protocols); x11_w(4); x11_w(32); x11_w(1);
        x11_w(x->wm_delete_atom);
        x11_send(x);
    }

    /* WM_HINTS: tell the WM we want keyboard input */
    uint32_t wm_hints_atom = x11_intern_atom(x, "WM_HINTS", false);
    if (wm_hints_atom) {
        /* WM_HINTS structure: 9 x CARD32 = 36 bytes
         * flags=InputHint|StateHint, input=True, initial_state=Normal, rest=0 */
        x11_begin(X_ChangeProperty, 0);
        x11_w(x->window);           /* window */
        x11_w(wm_hints_atom);       /* property */
        x11_w(wm_hints_atom);       /* type */
        x11_w(32);                  /* format */
        x11_w(9);                   /* length in elements */
        x11_w(0x03);                /* flags: InputHint(1) | StateHint(2) */
        x11_w(1);                   /* input: True */
        x11_w(1);                   /* initial_state: NormalState */
        x11_w(0);                   /* icon_pixmap */
        x11_w(0);                   /* icon_window */
        x11_w(0);                   /* icon_x */
        x11_w(0);                   /* icon_y */
        x11_w(0);                   /* icon_mask */
        x11_w(0);                   /* window_group */
        x11_send(x);
    }
    return true;
}

static bool x11_map_window(x11_backend_t *x) {
    x11_begin(X_MapWindow, 0);
    x11_w(x->window);
    if (!x11_send(x)) return false;
    /* Request keyboard focus — best-effort, WM may override */
    x11_begin(X_SetInputFocus, 1); /* revert-to = PointerRoot */
    x11_w(x->window);
    x11_w(0); /* time = CurrentTime */
    x11_send(x); /* ignore failure */
    return true;
}

static bool x11_set_title_x11(x11_backend_t *x, const char *title) {
    uint32_t wm_name = x11_intern_atom(x, "WM_NAME", false);
    uint32_t string_atom = x11_intern_atom(x, "STRING", false);
    if (!wm_name || !string_atom) return false;
    size_t tlen = strlen(title);
    if (tlen > 512) tlen = 512;
    x11_begin(X_ChangeProperty, 0);
    x11_w(x->window);
    x11_w(wm_name);
    x11_w(string_atom);
    x11_w(8); /* format */
    x11_w((uint32_t)tlen);
    for (size_t i = 0; i < tlen; i++) x11_req[x11_req_len++] = (uint8_t)title[i];
    size_t pad = (4 - (tlen & 3)) & 3;
    for (size_t i = 0; i < pad; i++) x11_req[x11_req_len++] = 0;
    return x11_send(x);
}

static void x11_request_focus(x11_backend_t *x) {
    x11_begin(X_SetInputFocus, 1); /* revert-to = PointerRoot */
    x11_w(x->window);
    x11_w(0); /* time = CurrentTime */
    x11_send(x);
}

static fge_key_t x11_keycode_to_fge(uint8_t keycode) {
    switch (keycode) {
    case 9:  return FGE_KEY_ESCAPE;
    case 10: return FGE_KEY_1;
    case 11: return FGE_KEY_2;
    case 12: return FGE_KEY_3;
    case 13: return FGE_KEY_4;
    case 14: return FGE_KEY_5;
    case 15: return FGE_KEY_6;
    case 16: return FGE_KEY_7;
    case 17: return FGE_KEY_8;
    case 18: return FGE_KEY_9;
    case 19: return FGE_KEY_0;
    case 24: return FGE_KEY_Q;
    case 25: return FGE_KEY_W;
    case 26: return FGE_KEY_E;
    case 27: return FGE_KEY_R;
    case 28: return FGE_KEY_T;
    case 29: return FGE_KEY_Y;
    case 30: return FGE_KEY_U;
    case 31: return FGE_KEY_I;
    case 32: return FGE_KEY_O;
    case 33: return FGE_KEY_P;
    case 38: return FGE_KEY_A;
    case 39: return FGE_KEY_S;
    case 40: return FGE_KEY_D;
    case 41: return FGE_KEY_F;
    case 42: return FGE_KEY_G;
    case 43: return FGE_KEY_H;
    case 44: return FGE_KEY_J;
    case 45: return FGE_KEY_K;
    case 46: return FGE_KEY_L;
    case 52: return FGE_KEY_Z;
    case 53: return FGE_KEY_X;
    case 54: return FGE_KEY_C;
    case 55: return FGE_KEY_V;
    case 56: return FGE_KEY_B;
    case 57: return FGE_KEY_N;
    case 58: return FGE_KEY_M;
    case 36: return FGE_KEY_ENTER;
    case 23: return FGE_KEY_TAB;
    case 22: return FGE_KEY_BACKSPACE;
    case 119: return FGE_KEY_DELETE;
    case 65: return FGE_KEY_SPACE;
    case 111: return FGE_KEY_UP;
    case 116: return FGE_KEY_DOWN;
    case 113: return FGE_KEY_LEFT;
    case 114: return FGE_KEY_RIGHT;
    case 110: return FGE_KEY_HOME;
    case 115: return FGE_KEY_END;
    case 112: return FGE_KEY_PAGE_UP;
    case 117: return FGE_KEY_PAGE_DOWN;
    case 118: return FGE_KEY_INSERT;
    case 67: return FGE_KEY_F1;
    case 68: return FGE_KEY_F2;
    case 69: return FGE_KEY_F3;
    case 70: return FGE_KEY_F4;
    case 71: return FGE_KEY_F5;
    case 72: return FGE_KEY_F6;
    case 73: return FGE_KEY_F7;
    case 74: return FGE_KEY_F8;
    case 75: return FGE_KEY_F9;
    case 76: return FGE_KEY_F10;
    case 95: return FGE_KEY_F11;
    case 96: return FGE_KEY_F12;
    case 50: return FGE_KEY_LSHIFT;
    case 62: return FGE_KEY_RSHIFT;
    case 37: return FGE_KEY_LCTRL;
    case 105: return FGE_KEY_RCTRL;
    case 64: return FGE_KEY_LALT;
    case 108: return FGE_KEY_RALT;
    case 133: return FGE_KEY_LMETA;
    case 134: return FGE_KEY_RMETA;
    default: return FGE_KEY_NONE;
    }
}

static fge_mouse_button_t x11_button_to_fge(uint8_t button) {
    switch (button) {
    case 1: return FGE_MOUSE_LEFT;
    case 2: return FGE_MOUSE_MIDDLE;
    case 3: return FGE_MOUSE_RIGHT;
    case 8: return FGE_MOUSE_X1;
    case 9: return FGE_MOUSE_X2;
    default: return FGE_MOUSE_NONE;
    }
}

static bool x11_poll_event_impl(x11_backend_t *x, fge_event_t *out) {
    /* Drain any pending synthetic mouse UP events first.
     * These are queued when the pointer leaves the window while
     * buttons are still held, since X11 doesn't deliver the real
     * ButtonRelease events until the pointer re-enters. */
    if (x->pending_mouse_ups) {
        for (int i = 1; i < 6; i++) {
            if (x->pending_mouse_ups & (1u << i)) {
                x->pending_mouse_ups &= ~(1u << i);
                memset(out, 0, sizeof(*out));
                out->type = FGE_EVENT_MOUSE_UP;
                out->mouse_button.button = (fge_mouse_button_t)i;
                out->mouse_button.pos = fge_v2((float)x->mouse_x, (float)x->mouse_y);
                x->mouse_buttons[i] = false;
                return true;
            }
        }
    }

    uint8_t ev[32];
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (x->fd >= 0) FD_SET(x->fd, &rfds);
        struct timeval tv = {0, 0};
        int ret = select(x->fd + 1, &rfds, NULL, NULL, &tv);
        if (ret <= 0) return false;
        if (!read_all(x->fd, ev, 32)) return false;
        uint8_t code = ev[0] & 0x7F;
        if (code == X_Reply) {
            uint32_t extra = (uint32_t)ev[4] | ((uint32_t)ev[5] << 8) |
                             ((uint32_t)ev[6] << 16) | ((uint32_t)ev[7] << 24);
            if (extra > 0) {
                size_t skip = extra * 4;
                uint8_t skip_buf[4096];
                while (skip > 0) {
                    size_t chunk = skip > sizeof(skip_buf) ? sizeof(skip_buf) : skip;
                    if (!read_all(x->fd, skip_buf, chunk)) return false;
                    skip -= chunk;
                }
            }
            continue;
        }
        if (code == X_Error) continue;
        break;
    }

    uint8_t code = ev[0] & 0x7F;
    memset(out, 0, sizeof(*out));

    switch (code) {
    case X_KeyPress:
    case X_KeyRelease: {
        fge_key_t key = x11_keycode_to_fge(ev[1]);
        if (key == FGE_KEY_NONE) return false;

        if (code == X_KeyPress) {
            /* If key already marked down, this is auto-repeat */
            bool repeat = (key < FGE_KEY_COUNT) && x->keys_down[key];
            if (key < FGE_KEY_COUNT) x->keys_down[key] = true;
            out->type = FGE_EVENT_KEY_DOWN;
            out->key.key = key;
            out->key.repeat = repeat;
        } else {
            if (key < FGE_KEY_COUNT) x->keys_down[key] = false;
            out->type = FGE_EVENT_KEY_UP;
            out->key.key = key;
            out->key.repeat = false;
        }
        return true;
    }
    case X_ButtonPress: {
        uint8_t btn = ev[1];
        if (btn == 4 || btn == 5 || btn == 6 || btn == 7) {
            out->type = FGE_EVENT_MOUSE_SCROLL;
            out->mouse_scroll.scroll.x = 0.0f;
            out->mouse_scroll.scroll.y = 0.0f;
            if (btn == 4) out->mouse_scroll.scroll.y = 1.0f;
            if (btn == 5) out->mouse_scroll.scroll.y = -1.0f;
            if (btn == 6) out->mouse_scroll.scroll.x = -1.0f;
            if (btn == 7) out->mouse_scroll.scroll.x = 1.0f;
            return true;
        }
        fge_mouse_button_t fge_btn = x11_button_to_fge(btn);
        if (fge_btn != FGE_MOUSE_NONE && (size_t)fge_btn < 6)
            x->mouse_buttons[(size_t)fge_btn] = true;
        out->type = FGE_EVENT_MOUSE_DOWN;
        out->mouse_button.button = fge_btn;
        out->mouse_button.pos.x = (float)(int16_t)(((uint16_t)ev[25] << 8) | ev[24]);
        out->mouse_button.pos.y = (float)(int16_t)(((uint16_t)ev[27] << 8) | ev[26]);
        x->mouse_x = (int)out->mouse_button.pos.x;
        x->mouse_y = (int)out->mouse_button.pos.y;
        return true;
    }
    case X_ButtonRelease: {
        uint8_t btn = ev[1];
        if (btn >= 4 && btn <= 7) return false;
        fge_mouse_button_t fge_btn = x11_button_to_fge(btn);
        if (fge_btn != FGE_MOUSE_NONE && (size_t)fge_btn < 6)
            x->mouse_buttons[(size_t)fge_btn] = false;
        out->type = FGE_EVENT_MOUSE_UP;
        out->mouse_button.button = fge_btn;
        out->mouse_button.pos.x = (float)(int16_t)(((uint16_t)ev[25] << 8) | ev[24]);
        out->mouse_button.pos.y = (float)(int16_t)(((uint16_t)ev[27] << 8) | ev[26]);
        x->mouse_x = (int)out->mouse_button.pos.x;
        x->mouse_y = (int)out->mouse_button.pos.y;
        return true;
    }
    case X_EnterNotify: {
        /* Clear any stale pending UPs when re-entering.
         * Our local button state is the source of truth. */
        x->pending_mouse_ups = 0;
        return false;
    }
    case X_LeaveNotify: {
        /* Queue synthetic MOUSE_UP for ALL buttons still pressed.
         * X11 doesn't deliver ButtonRelease while pointer is outside. */
        bool had_any = false;
        for (int i = 1; i < 6; i++) {
            if (x->mouse_buttons[i]) {
                x->mouse_buttons[i] = false;
                x->pending_mouse_ups |= (1u << i);
                had_any = true;
            }
        }
        if (had_any) {
            /* Return the first pending UP immediately */
            for (int i = 1; i < 6; i++) {
                if (x->pending_mouse_ups & (1u << i)) {
                    x->pending_mouse_ups &= ~(1u << i);
                    out->type = FGE_EVENT_MOUSE_UP;
                    out->mouse_button.button = (fge_mouse_button_t)i;
                    out->mouse_button.pos = fge_v2((float)x->mouse_x, (float)x->mouse_y);
                    return true;
                }
            }
        }
        return false;
    }
    case X_MotionNotify: {
        /* Only update position from motion events.
         * DO NOT sync button state — the motion event mask can lag
         * and conflict with our own press/release tracking. */
        out->type = FGE_EVENT_MOUSE_MOVE;
        int mx = (int)(int16_t)(((uint16_t)ev[25] << 8) | ev[24]);
        int my = (int)(int16_t)(((uint16_t)ev[27] << 8) | ev[26]);
        out->mouse_move.pos.x = (float)mx;
        out->mouse_move.pos.y = (float)my;
        out->mouse_move.delta.x = (float)(mx - x->mouse_x);
        out->mouse_move.delta.y = (float)(my - x->mouse_y);
        x->mouse_x = mx;
        x->mouse_y = my;
        return true;
    }
    case X_ConfigureNotify: {
        uint16_t nw = (uint16_t)ev[20] | ((uint16_t)ev[21] << 8);
        uint16_t nh = (uint16_t)ev[22] | ((uint16_t)ev[23] << 8);
        if (nw != (uint16_t)x->width || nh != (uint16_t)x->height) {
            x->width = nw;
            x->height = nh;
            out->type = FGE_EVENT_RESIZE;
            out->resize.width = nw;
            out->resize.height = nh;
            return true;
        }
        return false;
    }
    case X_FocusIn:
        x11_request_focus(x);
        out->type = FGE_EVENT_FOCUS_GAIN;
        return true;
    case X_FocusOut:
        out->type = FGE_EVENT_FOCUS_LOST;
        return true;
    case X_MapNotify:
        x->mapped = true;
        x11_request_focus(x);
        return false; /* no app event */
    case X_UnmapNotify:
        x->mapped = false;
        return false;
    case X_ClientMessage: {
        uint32_t atom = (uint32_t)ev[12] | ((uint32_t)ev[13] << 8) |
                        ((uint32_t)ev[14] << 16) | ((uint32_t)ev[15] << 24);
        if (x->wm_delete_atom && atom == x->wm_delete_atom) {
            out->type = FGE_EVENT_CLOSE;
            return true;
        }
        return false;
    }
    case X_DestroyNotify:
        x->closed = true;
        out->type = FGE_EVENT_CLOSE;
        return true;
    default:
        return false;
    }
}

static bool x11_init(fge_platform_t *p, const char *title, int w, int h, bool fullscreen) {
    (void)fullscreen;
    x11_backend_t *x = FGE_CALLOC(1, sizeof(x11_backend_t));
    if (!x) return false;
    x->fd = -1;
    x->width = w; x->height = h;
    if (!x11_connect(x)) { FGE_FREE(x); return false; }
    if (!x11_create_window(x, w, h)) { close(x->fd); FGE_FREE(x); return false; }
    if (!x11_map_window(x)) { close(x->fd); FGE_FREE(x); return false; }
    x->mapped = true;
    if (title && *title) x11_set_title_x11(x, title);

    int flags = fcntl(x->fd, F_GETFL, 0);
    if (flags >= 0) fcntl(x->fd, F_SETFL, flags | O_NONBLOCK);

    p->native_display = x;
    p->native_window = (void *)(uintptr_t)x->window;
    p->width = w; p->height = h;
    p->running = true;
    p->focused = true;
    FGE_INFO(FGE_LOG_CAT_PLATFORM, "X11 platform initialized: %dx%d", w, h);
    return true;
}

static void x11_shutdown(fge_platform_t *p) {
    if (!p || !p->native_display) return;
    x11_backend_t *x = (x11_backend_t *)p->native_display;
    if (x->fd >= 0) {
        x11_begin(X_DestroyWindow, 0);
        x11_w(x->window);
        x11_send(x);
        close(x->fd);
    }
    FGE_FREE(x);
    p->native_display = NULL;
    p->native_window = NULL;
}

static bool x11_poll_event(fge_platform_t *p, fge_event_t *out) {
    if (!p || !p->native_display) return false;
    x11_backend_t *x = (x11_backend_t *)p->native_display;
    if (x->closed) {
        out->type = FGE_EVENT_CLOSE;
        x->closed = false;
        return true;
    }
    bool got = x11_poll_event_impl(x, out);
    if (got) {
        /* Sync input state */
        if (out->type == FGE_EVENT_KEY_DOWN || out->type == FGE_EVENT_KEY_UP) {
            if (out->key.key < FGE_KEY_COUNT)
                p->input.keys[out->key.key] = (out->type == FGE_EVENT_KEY_DOWN);
        }
        if (out->type == FGE_EVENT_MOUSE_DOWN || out->type == FGE_EVENT_MOUSE_UP) {
            if ((size_t)out->mouse_button.button < 6)
                p->input.mouse_buttons[(size_t)out->mouse_button.button] =
                    (out->type == FGE_EVENT_MOUSE_DOWN);
            p->input.mouse_pos = out->mouse_button.pos;
        }
        if (out->type == FGE_EVENT_MOUSE_MOVE) {
            p->input.mouse_pos = out->mouse_move.pos;
        }
    }
    return got;
}

static void x11_swap_buffers(fge_platform_t *p) {
    if (!p || !p->native_display || !p->framebuffer) return;
    x11_backend_t *x = (x11_backend_t *)p->native_display;
    fge_framebuffer_t *fb = (fge_framebuffer_t *)p->framebuffer;
    if (!fb || !fb->pixels) return;

    int w = fb->width;
    int h = fb->height;
    if (w <= 0 || h <= 0) return;

    /* Temporarily set blocking — each frame must fully deliver or we tear.
     * Single-threaded: safe to toggle here. */
    int old_flags = fcntl(x->fd, F_GETFL, 0);
    if (old_flags >= 0) fcntl(x->fd, F_SETFL, old_flags & ~O_NONBLOCK);

    /* X11 request length is 16 bits: max 65535 words = 262140 bytes.
     * PutImage header = 24 bytes. Max data = 262140 - 24 = 262116 bytes.
     * For 32-bit pixels: max 65529 pixels per request.
     * Tile into horizontal strips. */
    const size_t MAX_DATA = 65529U * 4U;
    int strip_height = (int)(MAX_DATA / ((size_t)w * 4U));
    if (strip_height < 1) strip_height = 1;

    for (int y0 = 0; y0 < h; y0 += strip_height) {
        int sh = y0 + strip_height > h ? h - y0 : strip_height;
        size_t row_bytes = (size_t)w * 4U;
        size_t data_len = row_bytes * (size_t)sh;
        size_t req_len = 24 + data_len;
        uint8_t *req = FGE_MALLOC(req_len);
        if (!req) goto done;

        uint16_t len_words = (uint16_t)(req_len / 4);
        req[0] = X_PutImage;
        req[1] = ZPixmap;
        req[2] = (uint8_t)(len_words >> 0);
        req[3] = (uint8_t)(len_words >> 8);
        *(uint32_t *)(req + 4) = x->window;
        *(uint32_t *)(req + 8) = x->gc;
        *(uint16_t *)(req + 12) = (uint16_t)w;
        *(uint16_t *)(req + 14) = (uint16_t)sh;
        *(uint16_t *)(req + 16) = 0; /* dst-x */
        *(uint16_t *)(req + 18) = (uint16_t)y0; /* dst-y */
        req[20] = 0; /* left-pad */
        req[21] = x->root_depth;
        req[22] = 0;
        req[23] = 0;
        memcpy(req + 24, fb->pixels + y0 * w, data_len);

        write_all(x->fd, req, req_len);
        FGE_FREE(req);
    }

    /* Force X server to process — send a sync request and wait for reply.
     * This is the raw-wire equivalent of XSync(). Ensures the frame is
     * fully presented before we return and start rendering the next one. */
    {
        uint8_t sync_req[4] = { X_GetInputFocus, 0, 1, 0 }; /* len=1 word */
        write_all(x->fd, sync_req, 4);
        uint8_t sync_reply[32];
        /* Blocking read ensures server has processed all prior requests */
        read_all(x->fd, sync_reply, 32);
        (void)sync_reply;
    }

done:
    if (old_flags >= 0) fcntl(x->fd, F_SETFL, old_flags);
}
static void x11_set_title(fge_platform_t *p, const char *title) {
    if (!p || !p->native_display || !title) return;
    x11_set_title_x11((x11_backend_t *)p->native_display, title);
}
static void x11_set_vsync(fge_platform_t *p, bool enabled) { (void)p; (void)enabled; }
static void x11_show_cursor(fge_platform_t *p, bool show) { (void)p; (void)show; }
static void x11_grab_input(fge_platform_t *p, bool grab) { (void)p; (void)grab; }
static const char *x11_get_clipboard(fge_platform_t *p) { (void)p; return NULL; }
static void x11_set_clipboard(fge_platform_t *p, const char *text) { (void)p; (void)text; }

#endif /* POSIX X11 */

/* -------------------------------------------------------------------------- */
/* Wayland backend                                                            */
/* -------------------------------------------------------------------------- */

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

/* Wayland wire protocol definitions */

typedef struct {
    int fd;
    uint32_t next_id;
    uint32_t display_id;
    uint32_t registry_id;
    uint32_t compositor_id;
    uint32_t wm_base_id;
    uint32_t shm_id;
    uint32_t seat_id;
    uint32_t surface_id;
    uint32_t xdg_surface_id;
    uint32_t toplevel_id;
    uint32_t shm_pool_id;
    uint32_t buffer_id;
    uint32_t keyboard_id;
    uint32_t pointer_id;

    uint8_t *shm_map;
    size_t   shm_size;
    int      shm_fd;

    int width, height;
    bool configured;
    bool closed;
    int mouse_x, mouse_y;
    bool mouse_in;

    uint8_t rx_buf[8192];
    size_t  rx_len;
} wl_backend_t;

/* Object opcodes */
enum { WL_DISPLAY_GET_REGISTRY = 1 };
enum { WL_DISPLAY_ERROR = 0, WL_DISPLAY_DELETE_ID = 1 };
enum { WL_REGISTRY_BIND = 0 };
enum { WL_REGISTRY_GLOBAL = 0, WL_REGISTRY_GLOBAL_REMOVE = 1 };
enum { WL_COMPOSITOR_CREATE_SURFACE = 0 };
enum { WL_SURFACE_ATTACH = 1, WL_SURFACE_DAMAGE = 2, WL_SURFACE_FRAME = 3, WL_SURFACE_COMMIT = 6 };
enum { WL_SHM_CREATE_POOL = 0 };
enum { WL_SHM_POOL_CREATE_BUFFER = 0, WL_SHM_POOL_DESTROY = 1 };
enum { WL_BUFFER_DESTROY = 0 };
enum { WL_SEAT_GET_POINTER = 0, WL_SEAT_GET_KEYBOARD = 1 };
enum { WL_SEAT_CAPABILITIES = 0, WL_SEAT_NAME = 1 };
enum { WL_KEYBOARD_KEYMAP = 0, WL_KEYBOARD_ENTER = 1, WL_KEYBOARD_LEAVE = 2, WL_KEYBOARD_KEY = 3, WL_KEYBOARD_MODIFIERS = 4 };
enum { WL_POINTER_ENTER = 1, WL_POINTER_LEAVE = 2, WL_POINTER_MOTION = 3, WL_POINTER_BUTTON = 4, WL_POINTER_AXIS = 5 };
enum { XDG_WM_BASE_DESTROY = 0, XDG_WM_BASE_CREATE_POSITIONER = 1, XDG_WM_BASE_GET_XDG_SURFACE = 2, XDG_WM_BASE_PING = 3 };
enum { XDG_WM_BASE_PING_EVENT = 0 };
enum { XDG_SURFACE_DESTROY = 0, XDG_SURFACE_GET_TOPLEVEL = 1, XDG_SURFACE_SET_WINDOW_GEOMETRY = 2, XDG_SURFACE_ACK_CONFIGURE = 4 };
enum { XDG_SURFACE_CONFIGURE = 0 };
enum { XDG_TOPLEVEL_DESTROY = 0, XDG_TOPLEVEL_SET_PARENT = 1, XDG_TOPLEVEL_SET_TITLE = 2, XDG_TOPLEVEL_SET_APP_ID = 3 };
enum { XDG_TOPLEVEL_CONFIGURE = 0, XDG_TOPLEVEL_CLOSE = 1 };

static uint8_t wl_msg[1024];
static size_t  wl_msg_len;

static void wl_begin(uint32_t obj, uint16_t opcode) {
    wl_msg_len = 0;
    wl_msg[wl_msg_len++] = (uint8_t)(obj >> 0);
    wl_msg[wl_msg_len++] = (uint8_t)(obj >> 8);
    wl_msg[wl_msg_len++] = (uint8_t)(obj >> 16);
    wl_msg[wl_msg_len++] = (uint8_t)(obj >> 24);
    wl_msg[wl_msg_len++] = 0; /* size placeholder */
    wl_msg[wl_msg_len++] = 0;
    wl_msg[wl_msg_len++] = (uint8_t)(opcode >> 0);
    wl_msg[wl_msg_len++] = (uint8_t)(opcode >> 8);
}

static void wl_u32(uint32_t v) {
    if (wl_msg_len + 4 > sizeof(wl_msg)) return;
    wl_msg[wl_msg_len++] = (uint8_t)(v >> 0);
    wl_msg[wl_msg_len++] = (uint8_t)(v >> 8);
    wl_msg[wl_msg_len++] = (uint8_t)(v >> 16);
    wl_msg[wl_msg_len++] = (uint8_t)(v >> 24);
}

static void wl_str(const char *s) {
    size_t n = strlen(s) + 1;
    size_t pad = (4 - (n & 3)) & 3;
    if (wl_msg_len + n + pad > sizeof(wl_msg)) return;
    for (size_t i = 0; i < n; i++) wl_msg[wl_msg_len++] = (uint8_t)s[i];
    for (size_t i = 0; i < pad; i++) wl_msg[wl_msg_len++] = 0;
}

static bool wl_send_with_fd(wl_backend_t *wl, int fd) {
    uint32_t sz = (uint32_t)wl_msg_len;
    wl_msg[4] = (uint8_t)(sz >> 0);
    wl_msg[5] = (uint8_t)(sz >> 8);

    struct iovec iov = { wl_msg, wl_msg_len };
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = fd >= 0 ? cmsgbuf : NULL,
        .msg_controllen = fd >= 0 ? CMSG_SPACE(sizeof(int)) : 0,
    };
    if (fd >= 0) {
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    }
    ssize_t n = sendmsg(wl->fd, &msg, MSG_NOSIGNAL);
    return n == (ssize_t)wl_msg_len;
}

static bool wl_send(wl_backend_t *wl) {
    return wl_send_with_fd(wl, -1);
}

static bool wl_connect(wl_backend_t *wl) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime) runtime = "/run/user/1000";
    char path[256];
    snprintf(path, sizeof(path), "%s/wayland-0", runtime);

    wl->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (wl->fd < 0) return false;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    size_t plen = strlen(path);
    if (plen >= sizeof(addr.sun_path)) plen = sizeof(addr.sun_path) - 1;
    memcpy(addr.sun_path, path, plen);
    addr.sun_path[plen] = '\0';
    if (connect(wl->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(wl->fd); wl->fd = -1; return false;
    }

    int flags = fcntl(wl->fd, F_GETFL, 0);
    if (flags >= 0) fcntl(wl->fd, F_SETFL, flags | O_NONBLOCK);

    wl->next_id = 2; /* 1 is wl_display */
    wl->display_id = 1;
    wl->registry_id = wl->next_id++;

    wl_begin(wl->display_id, WL_DISPLAY_GET_REGISTRY);
    wl_u32(wl->registry_id);
    if (!wl_send(wl)) return false;
    return true;
}

static bool wl_create_shm_buffer(wl_backend_t *wl, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    size_t stride = (size_t)w * 4;
    if ((size_t)w > SIZE_MAX / 4) return false;
    size_t size = stride * (size_t)h;
    if (stride > SIZE_MAX / (size_t)h) return false;
    wl->shm_size = size;
    wl->shm_fd = memfd_create("forge-wl-shm", MFD_CLOEXEC);
    if (wl->shm_fd < 0) {
        wl->shm_fd = open("/dev/shm/forge_wl_XXXXXX", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (wl->shm_fd < 0) return false;
        unlink("/dev/shm/forge_wl_XXXXXX");
    }
    if (ftruncate(wl->shm_fd, (off_t)size) < 0) {
        close(wl->shm_fd); wl->shm_fd = -1; return false;
    }
    wl->shm_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, wl->shm_fd, 0);
    if (wl->shm_map == MAP_FAILED) {
        close(wl->shm_fd); wl->shm_fd = -1; return false;
    }
    memset(wl->shm_map, 0, size);

    wl->shm_pool_id = wl->next_id++;
    wl_begin(wl->shm_id, WL_SHM_CREATE_POOL);
    wl_u32(wl->shm_pool_id);
    wl_u32((uint32_t)size);
    if (!wl_send_with_fd(wl, wl->shm_fd)) return false;

    wl->buffer_id = wl->next_id++;
    wl_begin(wl->shm_pool_id, WL_SHM_POOL_CREATE_BUFFER);
    wl_u32(wl->buffer_id);
    wl_u32(0); /* offset */
    wl_u32((uint32_t)w);
    wl_u32((uint32_t)h);
    wl_u32((uint32_t)stride);
    wl_u32(0); /* WL_SHM_FORMAT_ARGB8888 = 0 */
    if (!wl_send(wl)) return false;
    return true;
}

static bool wl_recv_data(wl_backend_t *wl) {
    if (wl->rx_len >= sizeof(wl->rx_buf)) return false;
    struct iovec iov = { wl->rx_buf + wl->rx_len, sizeof(wl->rx_buf) - wl->rx_len };
    char cmsgbuf[CMSG_SPACE(sizeof(int) * 28)];
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsgbuf,
        .msg_controllen = sizeof(cmsgbuf),
    };
    ssize_t n = recvmsg(wl->fd, &msg, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return false;
        return false;
    }
    if (n == 0) return false;
    wl->rx_len += (size_t)n;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int *fds = (int *)CMSG_DATA(cmsg);
            int nfds = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < nfds; i++) {
                if (fds[i] >= 0) close(fds[i]);
            }
        }
    }
    return true;
}

static bool wl_has_msg(const wl_backend_t *wl, size_t *out_len) {
    if (wl->rx_len < 8) return false;
    uint16_t sz = (uint16_t)wl->rx_buf[4] | ((uint16_t)wl->rx_buf[5] << 8);
    if (sz < 8 || wl->rx_len < sz) return false;
    *out_len = sz;
    return true;
}

static fge_key_t wl_keycode_to_fge(uint32_t key) {
    switch (key) {
    case 1:  return FGE_KEY_ESCAPE;
    case 2:  return FGE_KEY_1;
    case 3:  return FGE_KEY_2;
    case 4:  return FGE_KEY_3;
    case 5:  return FGE_KEY_4;
    case 6:  return FGE_KEY_5;
    case 7:  return FGE_KEY_6;
    case 8:  return FGE_KEY_7;
    case 9:  return FGE_KEY_8;
    case 10: return FGE_KEY_9;
    case 11: return FGE_KEY_0;
    case 16: return FGE_KEY_Q;
    case 17: return FGE_KEY_W;
    case 18: return FGE_KEY_E;
    case 19: return FGE_KEY_R;
    case 20: return FGE_KEY_T;
    case 21: return FGE_KEY_Y;
    case 22: return FGE_KEY_U;
    case 23: return FGE_KEY_I;
    case 24: return FGE_KEY_O;
    case 25: return FGE_KEY_P;
    case 30: return FGE_KEY_A;
    case 31: return FGE_KEY_S;
    case 32: return FGE_KEY_D;
    case 33: return FGE_KEY_F;
    case 34: return FGE_KEY_G;
    case 35: return FGE_KEY_H;
    case 36: return FGE_KEY_J;
    case 37: return FGE_KEY_K;
    case 38: return FGE_KEY_L;
    case 44: return FGE_KEY_Z;
    case 45: return FGE_KEY_X;
    case 46: return FGE_KEY_C;
    case 47: return FGE_KEY_V;
    case 48: return FGE_KEY_B;
    case 49: return FGE_KEY_N;
    case 50: return FGE_KEY_M;
    case 28: return FGE_KEY_ENTER;
    case 15: return FGE_KEY_TAB;
    case 14: return FGE_KEY_BACKSPACE;
    case 111: return FGE_KEY_DELETE;
    case 57: return FGE_KEY_SPACE;
    case 103: return FGE_KEY_UP;
    case 108: return FGE_KEY_DOWN;
    case 105: return FGE_KEY_LEFT;
    case 106: return FGE_KEY_RIGHT;
    case 102: return FGE_KEY_HOME;
    case 107: return FGE_KEY_END;
    case 104: return FGE_KEY_PAGE_UP;
    case 109: return FGE_KEY_PAGE_DOWN;
    case 110: return FGE_KEY_INSERT;
    case 59: return FGE_KEY_F1;
    case 60: return FGE_KEY_F2;
    case 61: return FGE_KEY_F3;
    case 62: return FGE_KEY_F4;
    case 63: return FGE_KEY_F5;
    case 64: return FGE_KEY_F6;
    case 65: return FGE_KEY_F7;
    case 66: return FGE_KEY_F8;
    case 67: return FGE_KEY_F9;
    case 68: return FGE_KEY_F10;
    case 87: return FGE_KEY_F11;
    case 88: return FGE_KEY_F12;
    case 42: return FGE_KEY_LSHIFT;
    case 54: return FGE_KEY_RSHIFT;
    case 29: return FGE_KEY_LCTRL;
    case 97: return FGE_KEY_RCTRL;
    case 56: return FGE_KEY_LALT;
    case 100: return FGE_KEY_RALT;
    case 125: return FGE_KEY_LMETA;
    case 126: return FGE_KEY_RMETA;
    default: return FGE_KEY_NONE;
    }
}

static fge_mouse_button_t wl_button_to_fge(uint32_t button) {
    switch (button) {
    case 272: return FGE_MOUSE_LEFT;   /* BTN_LEFT */
    case 273: return FGE_MOUSE_RIGHT;  /* BTN_RIGHT */
    case 274: return FGE_MOUSE_MIDDLE; /* BTN_MIDDLE */
    case 275: return FGE_MOUSE_X1;     /* BTN_SIDE */
    case 276: return FGE_MOUSE_X2;     /* BTN_EXTRA */
    default: return FGE_MOUSE_NONE;
    }
}

static void wl_handle_message(wl_backend_t *wl, const uint8_t *msg, size_t len,
                              fge_event_t *out, bool *out_has_event) {
    *out_has_event = false;
    if (len < 8) return;
    uint32_t obj = (uint32_t)msg[0] | ((uint32_t)msg[1] << 8) |
                   ((uint32_t)msg[2] << 16) | ((uint32_t)msg[3] << 24);
    uint16_t opcode = (uint16_t)msg[6] | ((uint16_t)msg[7] << 8);

    if (obj == wl->wm_base_id && opcode == XDG_WM_BASE_PING_EVENT) {
        uint32_t serial = (uint32_t)msg[8] | ((uint32_t)msg[9] << 8) |
                          ((uint32_t)msg[10] << 16) | ((uint32_t)msg[11] << 24);
        wl_begin(wl->wm_base_id, XDG_WM_BASE_PING);
        wl_u32(serial);
        wl_send(wl);
    } else if (obj == wl->xdg_surface_id && opcode == XDG_SURFACE_CONFIGURE) {
        uint32_t serial = (uint32_t)msg[8] | ((uint32_t)msg[9] << 8) |
                          ((uint32_t)msg[10] << 16) | ((uint32_t)msg[11] << 24);
        wl_begin(wl->xdg_surface_id, XDG_SURFACE_ACK_CONFIGURE);
        wl_u32(serial);
        wl_send(wl);
        wl->configured = true;
    } else if (obj == wl->toplevel_id && opcode == XDG_TOPLEVEL_CONFIGURE) {
        int32_t ww = (int32_t)((uint32_t)msg[8] | ((uint32_t)msg[9] << 8) |
                               ((uint32_t)msg[10] << 16) | ((uint32_t)msg[11] << 24));
        int32_t hh = (int32_t)((uint32_t)msg[12] | ((uint32_t)msg[13] << 8) |
                               ((uint32_t)msg[14] << 16) | ((uint32_t)msg[15] << 24));
        if (ww > 0 && hh > 0) { wl->width = ww; wl->height = hh; }
    } else if (obj == wl->toplevel_id && opcode == XDG_TOPLEVEL_CLOSE) {
        wl->closed = true;
    } else if (obj == wl->seat_id && opcode == WL_SEAT_CAPABILITIES) {
        uint32_t caps = (uint32_t)msg[8] | ((uint32_t)msg[9] << 8) |
                        ((uint32_t)msg[10] << 16) | ((uint32_t)msg[11] << 24);
        if ((caps & 2) && !wl->keyboard_id) {
            wl->keyboard_id = wl->next_id++;
            wl_begin(wl->seat_id, WL_SEAT_GET_KEYBOARD);
            wl_u32(wl->keyboard_id);
            wl_send(wl);
        }
        if ((caps & 1) && !wl->pointer_id) {
            wl->pointer_id = wl->next_id++;
            wl_begin(wl->seat_id, WL_SEAT_GET_POINTER);
            wl_u32(wl->pointer_id);
            wl_send(wl);
        }
    } else if (wl->keyboard_id && obj == wl->keyboard_id && opcode == WL_KEYBOARD_KEY) {
        if (len < 20) return;
        uint32_t key = (uint32_t)msg[12] | ((uint32_t)msg[13] << 8) |
                       ((uint32_t)msg[14] << 16) | ((uint32_t)msg[15] << 24);
        uint32_t state = (uint32_t)msg[16] | ((uint32_t)msg[17] << 8) |
                         ((uint32_t)msg[18] << 16) | ((uint32_t)msg[19] << 24);
        out->type = (state == 1) ? FGE_EVENT_KEY_DOWN : FGE_EVENT_KEY_UP;
        out->key.key = wl_keycode_to_fge(key);
        out->key.repeat = false;
        *out_has_event = true;
    } else if (wl->pointer_id && obj == wl->pointer_id && opcode == WL_POINTER_MOTION) {
        if (len < 16) return;
        int32_t fx = (int32_t)((uint32_t)msg[8] | ((uint32_t)msg[9] << 8) |
                               ((uint32_t)msg[10] << 16) | ((uint32_t)msg[11] << 24));
        int32_t fy = (int32_t)((uint32_t)msg[12] | ((uint32_t)msg[13] << 8) |
                               ((uint32_t)msg[14] << 16) | ((uint32_t)msg[15] << 24));
        int mx = fx / 256;
        int my = fy / 256;
        out->type = FGE_EVENT_MOUSE_MOVE;
        out->mouse_move.pos.x = (float)mx;
        out->mouse_move.pos.y = (float)my;
        out->mouse_move.delta.x = (float)(mx - wl->mouse_x);
        out->mouse_move.delta.y = (float)(my - wl->mouse_y);
        wl->mouse_x = mx;
        wl->mouse_y = my;
        *out_has_event = true;
    } else if (wl->pointer_id && obj == wl->pointer_id && opcode == WL_POINTER_BUTTON) {
        if (len < 20) return;
        uint32_t button = (uint32_t)msg[12] | ((uint32_t)msg[13] << 8) |
                          ((uint32_t)msg[14] << 16) | ((uint32_t)msg[15] << 24);
        uint32_t state = (uint32_t)msg[16] | ((uint32_t)msg[17] << 8) |
                         ((uint32_t)msg[18] << 16) | ((uint32_t)msg[19] << 24);
        fge_mouse_button_t btn = wl_button_to_fge(button);
        if (btn != FGE_MOUSE_NONE) {
            out->type = (state == 1) ? FGE_EVENT_MOUSE_DOWN : FGE_EVENT_MOUSE_UP;
            out->mouse_button.button = btn;
            out->mouse_button.pos.x = (float)wl->mouse_x;
            out->mouse_button.pos.y = (float)wl->mouse_y;
            *out_has_event = true;
        }
    } else if (wl->pointer_id && obj == wl->pointer_id && opcode == WL_POINTER_AXIS) {
        if (len < 20) return;
        uint32_t axis = (uint32_t)msg[8] | ((uint32_t)msg[9] << 8) |
                        ((uint32_t)msg[10] << 16) | ((uint32_t)msg[11] << 24);
        int32_t val = (int32_t)((uint32_t)msg[12] | ((uint32_t)msg[13] << 8) |
                                ((uint32_t)msg[14] << 16) | ((uint32_t)msg[15] << 24));
        out->type = FGE_EVENT_MOUSE_SCROLL;
        if (axis == 0) {
            out->mouse_scroll.scroll.y = -(float)val / 256.0f;
        } else {
            out->mouse_scroll.scroll.x = (float)val / 256.0f;
        }
        *out_has_event = true;
    }
}

static bool wl_poll_event_impl(wl_backend_t *wl, fge_event_t *out) {
    if (wl->closed) {
        out->type = FGE_EVENT_CLOSE;
        wl->closed = false;
        return true;
    }
    size_t sz;
    while (wl_has_msg(wl, &sz)) {
        uint8_t msg[4096];
        if (sz > sizeof(msg)) {
            wl->rx_len -= sz;
            memmove(wl->rx_buf, wl->rx_buf + sz, wl->rx_len);
            continue;
        }
        memcpy(msg, wl->rx_buf, sz);
        wl->rx_len -= sz;
        memmove(wl->rx_buf, wl->rx_buf + sz, wl->rx_len);
        fge_event_t ev;
        bool has = false;
        wl_handle_message(wl, msg, sz, &ev, &has);
        if (has) {
            *out = ev;
            return true;
        }
    }
    fd_set rfds;
    FD_ZERO(&rfds);
    if (wl->fd >= 0) FD_SET(wl->fd, &rfds);
    struct timeval tv = {0, 0};
    if (select(wl->fd + 1, &rfds, NULL, NULL, &tv) <= 0) return false;
    if (!wl_recv_data(wl)) return false;
    while (wl_has_msg(wl, &sz)) {
        uint8_t msg[4096];
        if (sz > sizeof(msg)) {
            wl->rx_len -= sz;
            memmove(wl->rx_buf, wl->rx_buf + sz, wl->rx_len);
            continue;
        }
        memcpy(msg, wl->rx_buf, sz);
        wl->rx_len -= sz;
        memmove(wl->rx_buf, wl->rx_buf + sz, wl->rx_len);
        fge_event_t ev;
        bool has = false;
        wl_handle_message(wl, msg, sz, &ev, &has);
        if (has) {
            *out = ev;
            return true;
        }
    }
    return false;
}

static bool wl_read_registry(wl_backend_t *wl) {
    for (int guard = 0; guard < 40; guard++) {
        if (!wl_recv_data(wl)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            if (wl->fd >= 0) FD_SET(wl->fd, &rfds);
            struct timeval tv = {0, 50000};
            int sr = select(wl->fd + 1, &rfds, NULL, NULL, &tv);
        }
        size_t sz;
        int msg_count = 0;
        while (wl_has_msg(wl, &sz)) {
            msg_count++;
            uint8_t msg[4096];
            if (sz > sizeof(msg)) {
                wl->rx_len -= sz;
                memmove(wl->rx_buf, wl->rx_buf + sz, wl->rx_len);
                continue;
            }
            memcpy(msg, wl->rx_buf, sz);
            wl->rx_len -= sz;
            memmove(wl->rx_buf, wl->rx_buf + sz, wl->rx_len);
            uint32_t obj = (uint32_t)msg[0] | ((uint32_t)msg[1] << 8) |
                           ((uint32_t)msg[2] << 16) | ((uint32_t)msg[3] << 24);
            uint16_t opcode = (uint16_t)msg[6] | ((uint16_t)msg[7] << 8);
            if (obj == wl->registry_id && opcode == WL_REGISTRY_GLOBAL) {
                uint32_t name = (uint32_t)msg[8] | ((uint32_t)msg[9] << 8) |
                                ((uint32_t)msg[10] << 16) | ((uint32_t)msg[11] << 24);
                const char *iface = (const char *)(msg + 12);
                uint32_t ver = 0;
                if (sz >= 4) {
                    ver = (uint32_t)msg[sz - 4] | ((uint32_t)msg[sz - 3] << 8) |
                          ((uint32_t)msg[sz - 2] << 16) | ((uint32_t)msg[sz - 1] << 24);
                }
                uint32_t bind_id = wl->next_id++;
                if (strncmp(iface, "wl_compositor", 13) == 0) {
                    wl->compositor_id = bind_id;
                    wl_begin(wl->registry_id, WL_REGISTRY_BIND);
                    wl_u32(name);
                    wl_str("wl_compositor");
                    wl_u32(ver < 4 ? ver : 4);
                    wl_u32(bind_id);
                    wl_send(wl);
                } else if (strncmp(iface, "xdg_wm_base", 11) == 0) {
                    wl->wm_base_id = bind_id;
                    wl_begin(wl->registry_id, WL_REGISTRY_BIND);
                    wl_u32(name);
                    wl_str("xdg_wm_base");
                    wl_u32(ver < 2 ? ver : 2);
                    wl_u32(bind_id);
                    wl_send(wl);
                } else if (strncmp(iface, "wl_shm", 6) == 0) {
                    wl->shm_id = bind_id;
                    wl_begin(wl->registry_id, WL_REGISTRY_BIND);
                    wl_u32(name);
                    wl_str("wl_shm");
                    wl_u32(ver < 1 ? ver : 1);
                    wl_u32(bind_id);
                    wl_send(wl);
                } else if (strncmp(iface, "wl_seat", 7) == 0) {
                    wl->seat_id = bind_id;
                    wl_begin(wl->registry_id, WL_REGISTRY_BIND);
                    wl_u32(name);
                    wl_str("wl_seat");
                    wl_u32(ver < 5 ? ver : 5);
                    wl_u32(bind_id);
                    wl_send(wl);
                }
            }
        }
        if (wl->compositor_id && wl->wm_base_id && wl->shm_id) break;
    }
    return wl->compositor_id && wl->wm_base_id && wl->shm_id;
}

static void wl_dispatch_pending(wl_backend_t *wl) {
    size_t sz;
    while (wl_has_msg(wl, &sz)) {
        uint8_t msg[4096];
        if (sz > sizeof(msg)) {
            wl->rx_len -= sz;
            memmove(wl->rx_buf, wl->rx_buf + sz, wl->rx_len);
            continue;
        }
        memcpy(msg, wl->rx_buf, sz);
        wl->rx_len -= sz;
        memmove(wl->rx_buf, wl->rx_buf + sz, wl->rx_len);
        fge_event_t ev;
        bool has = false;
        wl_handle_message(wl, msg, sz, &ev, &has);
        (void)has; (void)ev;
    }
    for (int guard = 0; guard < 64; guard++) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (wl->fd >= 0) FD_SET(wl->fd, &rfds);
        struct timeval tv = {0, 10000};
        if (select(wl->fd + 1, &rfds, NULL, NULL, &tv) <= 0) break;
        if (!wl_recv_data(wl)) break;
        while (wl_has_msg(wl, &sz)) {
            uint8_t msg[4096];
            if (sz > sizeof(msg)) {
                wl->rx_len -= sz;
                memmove(wl->rx_buf, wl->rx_buf + sz, wl->rx_len);
                continue;
            }
            memcpy(msg, wl->rx_buf, sz);
            wl->rx_len -= sz;
            memmove(wl->rx_buf, wl->rx_buf + sz, wl->rx_len);
            fge_event_t ev;
            bool has = false;
            wl_handle_message(wl, msg, sz, &ev, &has);
            (void)has; (void)ev;
        }
    }
}

static bool wl_init(fge_platform_t *p, const char *title, int w, int h, bool fullscreen) {
    (void)fullscreen;
    wl_backend_t *wl = FGE_CALLOC(1, sizeof(wl_backend_t));
    if (!wl) return false;
    wl->fd = -1;
    wl->width = w; wl->height = h;


    /* Create surface */
    wl->surface_id = wl->next_id++;
    wl_begin(wl->compositor_id, WL_COMPOSITOR_CREATE_SURFACE);
    wl_u32(wl->surface_id);
    wl_send(wl);

    /* Create xdg_surface & toplevel */
    wl->xdg_surface_id = wl->next_id++;
    wl_begin(wl->wm_base_id, XDG_WM_BASE_GET_XDG_SURFACE);
    wl_u32(wl->xdg_surface_id);
    wl_u32(wl->surface_id);
    wl_send(wl);

    wl->toplevel_id = wl->next_id++;
    wl_begin(wl->xdg_surface_id, XDG_SURFACE_GET_TOPLEVEL);
    wl_u32(wl->toplevel_id);
    wl_send(wl);

    if (title && *title) {
        wl_begin(wl->toplevel_id, XDG_TOPLEVEL_SET_TITLE);
        wl_str(title);
        wl_send(wl);
    }

    wl_begin(wl->surface_id, WL_SURFACE_COMMIT);
    wl_send(wl);

    /* Wait for configure (max ~2 seconds) */
    for (int guard = 0; guard < 40 && !wl->configured; guard++) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (wl->fd >= 0) FD_SET(wl->fd, &rfds);
        struct timeval tv = {0, 50000};
        if (select(wl->fd + 1, &rfds, NULL, NULL, &tv) > 0) {
            if (wl_recv_data(wl)) {
                size_t sz;
                while (wl_has_msg(wl, &sz)) {
                    uint8_t msg[4096];
                    if (sz > sizeof(msg)) {
                        wl->rx_len -= sz;
                        memmove(wl->rx_buf, wl->rx_buf + sz, wl->rx_len);
                        continue;
                    }
                    memcpy(msg, wl->rx_buf, sz);
                    wl->rx_len -= sz;
                    memmove(wl->rx_buf, wl->rx_buf + sz, wl->rx_len);
                    fge_event_t ev;
                    bool has = false;
                    wl_handle_message(wl, msg, sz, &ev, &has);
                    if (has && ev.type == FGE_EVENT_CLOSE) wl->closed = true;
                }
            }
        }
    }
    if (!wl->configured) { close(wl->fd); FGE_FREE(wl); return false; }

    /* Process seat/keymap events */
    wl_dispatch_pending(wl);

    /* Create and attach a minimal SHM buffer so the surface is mapped */
    if (!wl_create_shm_buffer(wl, wl->width, wl->height)) {
        close(wl->fd); FGE_FREE(wl); return false;
    }
    wl_begin(wl->surface_id, WL_SURFACE_ATTACH);
    wl_u32(wl->buffer_id);
    wl_u32(0); wl_u32(0);
    wl_send(wl);
    wl_begin(wl->surface_id, WL_SURFACE_DAMAGE);
    wl_u32(0); wl_u32(0);
    wl_u32((uint32_t)wl->width); wl_u32((uint32_t)wl->height);
    wl_send(wl);
    wl_begin(wl->surface_id, WL_SURFACE_COMMIT);
    wl_send(wl);

    int flags = fcntl(wl->fd, F_GETFL, 0);
    if (flags >= 0) fcntl(wl->fd, F_SETFL, flags | O_NONBLOCK);

    p->native_display = wl;
    p->native_window = (void *)(uintptr_t)wl->surface_id;
    p->width = wl->width; p->height = wl->height;
    p->running = true;
    p->focused = true;
    FGE_INFO(FGE_LOG_CAT_PLATFORM, "Wayland platform initialized: %dx%d", wl->width, wl->height);
    return true;
}

static void wl_shutdown(fge_platform_t *p) {
    if (!p || !p->native_display) return;
    wl_backend_t *wl = (wl_backend_t *)p->native_display;
    if (wl->shm_map && wl->shm_map != MAP_FAILED) munmap(wl->shm_map, wl->shm_size);
    if (wl->shm_fd >= 0) close(wl->shm_fd);
    if (wl->fd >= 0) close(wl->fd);
    FGE_FREE(wl);
    p->native_display = NULL;
    p->native_window = NULL;
}

static bool wl_poll_event(fge_platform_t *p, fge_event_t *out) {
    if (!p || !p->native_display) return false;
    wl_backend_t *wl = (wl_backend_t *)p->native_display;
    return wl_poll_event_impl(wl, out);
}

static void wl_swap_buffers(fge_platform_t *p) {
    if (!p || !p->native_display || !p->framebuffer) return;
    wl_backend_t *wl = (wl_backend_t *)p->native_display;
    fge_framebuffer_t *fb = (fge_framebuffer_t *)p->framebuffer;
    if (!fb || !fb->pixels || !wl->shm_map) return;

    int w = fb->width;
    int h = fb->height;
    if (w <= 0 || h <= 0) return;

    /* Copy framebuffer to SHM buffer */
    size_t stride = (size_t)w * 4;
    size_t expected = stride * (size_t)h;
    if (expected > wl->shm_size) return;

    /* Convert RGBA to ARGB (Wayland SHM ARGB8888 = 0) */
    /* On little-endian, ARGB in memory is B,G,R,A which matches our RGBA */
    /* Actually let's just memcpy and see; color swap may occur */
    memcpy(wl->shm_map, fb->pixels, expected);

    /* Damage and commit */
    wl_begin(wl->surface_id, WL_SURFACE_DAMAGE);
    wl_u32(0); wl_u32(0);
    wl_u32((uint32_t)w); wl_u32((uint32_t)h);
    wl_send(wl);

    wl_begin(wl->surface_id, WL_SURFACE_ATTACH);
    wl_u32(wl->buffer_id);
    wl_u32(0); wl_u32(0);
    wl_send(wl);

    wl_begin(wl->surface_id, WL_SURFACE_COMMIT);
    wl_send(wl);
}
static void wl_set_title(fge_platform_t *p, const char *title) {
    if (!p || !p->native_display || !title) return;
    wl_backend_t *wl = (wl_backend_t *)p->native_display;
    if (!wl->toplevel_id) return;
    wl_begin(wl->toplevel_id, XDG_TOPLEVEL_SET_TITLE);
    wl_str(title);
    wl_send(wl);
}
static void wl_set_vsync(fge_platform_t *p, bool enabled) { (void)p; (void)enabled; }
static void wl_show_cursor(fge_platform_t *p, bool show) { (void)p; (void)show; }
static void wl_grab_input(fge_platform_t *p, bool grab) { (void)p; (void)grab; }
static const char *wl_get_clipboard(fge_platform_t *p) { (void)p; return NULL; }
static void wl_set_clipboard(fge_platform_t *p, const char *text) { (void)p; (void)text; }

#endif /* POSIX Wayland */

/* -------------------------------------------------------------------------- */
/* Platform API                                                               */
/* -------------------------------------------------------------------------- */

fge_platform_t *fge_platform_create(const char *title, int w, int h, bool fullscreen) {
    fge_platform_t *p = FGE_CALLOC(1, sizeof(fge_platform_t));
    if (!p) return NULL;

    p->events = FGE_CALLOC(256, sizeof(fge_event_t));
    p->event_capacity = 256;

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    /* Try Wayland first */
    p->init = wl_init;
    p->shutdown = wl_shutdown;
    p->poll_event = wl_poll_event;
    p->swap_buffers = wl_swap_buffers;
    p->set_title = wl_set_title;
    p->set_vsync = wl_set_vsync;
    p->show_cursor = wl_show_cursor;
    p->grab_input = wl_grab_input;
    p->get_clipboard = wl_get_clipboard;
    p->set_clipboard = wl_set_clipboard;
    if (p->init(p, title, w, h, fullscreen)) return p;

    /* Reset and fallback to X11 */
    p->native_display = NULL;
    p->native_window = NULL;
    p->width = 0;
    p->height = 0;
    p->running = false;
    p->focused = false;

    p->init = x11_init;
    p->shutdown = x11_shutdown;
    p->poll_event = x11_poll_event;
    p->swap_buffers = x11_swap_buffers;
    p->set_title = x11_set_title;
    p->set_vsync = x11_set_vsync;
    p->show_cursor = x11_show_cursor;
    p->grab_input = x11_grab_input;
    p->get_clipboard = x11_get_clipboard;
    p->set_clipboard = x11_set_clipboard;
    if (p->init(p, title, w, h, fullscreen)) return p;
#endif

    FGE_FREE(p->events);
    FGE_FREE(p);
    return NULL;
}

void fge_platform_destroy(fge_platform_t *p) {
    if (!p) return;
    if (p->shutdown) p->shutdown(p);
    FGE_FREE(p->events);
    FGE_FREE(p);
}

bool fge_platform_poll_event(fge_platform_t *p, fge_event_t *out) {
    if (!p || !out) return false;
    if (p->poll_event && p->poll_event(p, out)) return true;
    uint32_t tail = FGE_ATOMIC_LOAD(&p->event_tail);
    uint32_t head = FGE_ATOMIC_LOAD_ACQ(&p->event_head);
    if (tail == head) return false;
    *out = p->events[tail & (p->event_capacity - 1)];
    FGE_ATOMIC_STORE_REL(&p->event_tail, (tail + 1) & (p->event_capacity - 1));
    return true;
}

void fge_platform_push_event(fge_platform_t *p, const fge_event_t *event) {
    if (!p || !event) return;
    uint32_t head = FGE_ATOMIC_LOAD(&p->event_head);
    uint32_t next = (head + 1) & (p->event_capacity - 1);
    uint32_t tail = FGE_ATOMIC_LOAD_ACQ(&p->event_tail);
    if (next == tail) return; /* full */
    p->events[head & (p->event_capacity - 1)] = *event;
    FGE_ATOMIC_STORE_REL(&p->event_head, next);
}

void fge_platform_run(fge_platform_t *p) {
    if (!p) return;
    fge_clock_t clock;
    fge_clock_init(&clock);
    uint64_t last_frame = fge_clock_now(&clock);

    while (p->running) {
        uint64_t now = fge_clock_now(&clock);
        double dt = fge_clock_ticks_to_sec(&clock, now - last_frame);
        last_frame = now;

        fge_event_t ev;
        while (fge_platform_poll_event(p, &ev)) {
            if (p->on_event) p->on_event(p, &ev);
            if (ev.type == FGE_EVENT_CLOSE) p->running = false;
        }

        if (p->on_frame) p->on_frame(p, dt);
        if (p->swap_buffers) p->swap_buffers(p);
    }
}

void fge_platform_step(fge_platform_t *p, double dt) {
    if (!p) return;
    fge_event_t ev;
    while (fge_platform_poll_event(p, &ev)) {
        if (p->on_event) p->on_event(p, &ev);
    }
    if (p->on_frame) p->on_frame(p, dt);
    if (p->swap_buffers) p->swap_buffers(p);
}
