/*
 * ORDL UI — Raw Wayland wire protocol backend
 */

#define _GNU_SOURCE
#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdio.h>

/* -------------------------------------------------------------------------- */
/* Wayland wire protocol helpers                                              */
/* -------------------------------------------------------------------------- */

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

#define WL_MAX_FDS 28

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

    /* Incoming message buffer */
    uint8_t  rx_buf[8192];
    size_t   rx_len;
} wl_state_t;

/* Read at least what is already buffered; returns true if we have a full msg */
static bool wl_recv(wl_state_t *wl) {
    if (wl->rx_len < 8) {
        ssize_t n = read(wl->fd, wl->rx_buf + wl->rx_len, sizeof(wl->rx_buf) - wl->rx_len);
        if (n > 0) wl->rx_len += (size_t)n;
        if (wl->rx_len < 8) return false;
    }
    uint32_t sz = ((uint32_t)wl->rx_buf[4] << 0) |
                  ((uint32_t)wl->rx_buf[5] << 8) |
                  ((uint32_t)wl->rx_buf[6] << 16) |
                  ((uint32_t)wl->rx_buf[7] << 24);
    if (wl->rx_len < sz) {
        ssize_t n = read(wl->fd, wl->rx_buf + wl->rx_len, sizeof(wl->rx_buf) - wl->rx_len);
        if (n > 0) wl->rx_len += (size_t)n;
    }
    return wl->rx_len >= sz;
}

static void wl_consume(wl_state_t *wl, size_t n) {
    if (n >= wl->rx_len) { wl->rx_len = 0; return; }
    memmove(wl->rx_buf, wl->rx_buf + n, wl->rx_len - n);
    wl->rx_len -= n;
}

/* -------------------------------------------------------------------------- */
/* Wayland message builders                                                   */
/* -------------------------------------------------------------------------- */

static uint8_t wl_msg[1024];
static size_t  wl_msg_len;

static void wl_begin(uint32_t obj, uint16_t opcode) {
    wl_msg_len = 0;
    if (wl_msg_len + 8 > sizeof(wl_msg)) return;
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

/* Send a Wayland message with an optional fd via SCM_RIGHTS */
static bool wl_send_with_fd(wl_state_t *wl, int fd) {
    uint32_t sz = (uint32_t)wl_msg_len;
    wl_msg[4] = (uint8_t)(sz >> 0);
    wl_msg[5] = (uint8_t)(sz >> 8);
    wl_msg[6] = (uint8_t)(sz >> 16);
    wl_msg[7] = (uint8_t)(sz >> 24);

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

static bool wl_send(wl_state_t *wl) {
    return wl_send_with_fd(wl, -1);
}

/* -------------------------------------------------------------------------- */
/* Wayland object IDs (stable protocol opcodes)                               */
/* -------------------------------------------------------------------------- */

/* wl_display requests */
enum { WL_DISPLAY_GET_REGISTRY = 1 };
/* wl_display events */
enum { WL_DISPLAY_ERROR = 0, WL_DISPLAY_DELETE_ID = 1 };

/* wl_registry requests */
enum { WL_REGISTRY_BIND = 0 };
/* wl_registry events */
enum { WL_REGISTRY_GLOBAL = 0, WL_REGISTRY_GLOBAL_REMOVE = 1 };

/* wl_compositor requests */
enum { WL_COMPOSITOR_CREATE_SURFACE = 0 };

/* wl_surface requests */
enum { WL_SURFACE_ATTACH = 1, WL_SURFACE_DAMAGE = 2, WL_SURFACE_FRAME = 3, WL_SURFACE_COMMIT = 6 };
/* wl_surface events */
enum { WL_SURFACE_ENTER = 0, WL_SURFACE_LEAVE = 1 };

/* wl_shm requests */
enum { WL_SHM_CREATE_POOL = 0 };
/* wl_shm events */
enum { WL_SHM_FORMAT = 0 };

/* wl_shm_pool requests */
enum { WL_SHM_POOL_CREATE_BUFFER = 0, WL_SHM_POOL_DESTROY = 1 };

/* wl_buffer requests */
enum { WL_BUFFER_DESTROY = 0 };
/* wl_buffer events */
enum { WL_BUFFER_RELEASE = 0 };

/* wl_seat requests */
enum { WL_SEAT_GET_POINTER = 0, WL_SEAT_GET_KEYBOARD = 1 };
/* wl_seat events */
enum { WL_SEAT_CAPABILITIES = 0, WL_SEAT_NAME = 1 };

/* wl_keyboard events */
enum { WL_KEYBOARD_KEYMAP = 0, WL_KEYBOARD_ENTER = 1, WL_KEYBOARD_LEAVE = 2, WL_KEYBOARD_KEY = 3, WL_KEYBOARD_MODIFIERS = 4 };

/* wl_pointer events */
enum { WL_POINTER_ENTER = 1, WL_POINTER_LEAVE = 2, WL_POINTER_MOTION = 3, WL_POINTER_BUTTON = 4, WL_POINTER_AXIS = 5 };

/* xdg_wm_base requests */
enum { XDG_WM_BASE_DESTROY = 0, XDG_WM_BASE_CREATE_POSITIONER = 1, XDG_WM_BASE_GET_XDG_SURFACE = 2, XDG_WM_BASE_PING = 3 };
/* xdg_wm_base events */
enum { XDG_WM_BASE_PING_EVENT = 0 };

/* xdg_surface requests */
enum { XDG_SURFACE_DESTROY = 0, XDG_SURFACE_GET_TOPLEVEL = 1, XDG_SURFACE_SET_WINDOW_GEOMETRY = 2, XDG_SURFACE_ACK_CONFIGURE = 4 };
/* xdg_surface events */
enum { XDG_SURFACE_CONFIGURE = 0 };

/* xdg_toplevel requests */
enum { XDG_TOPLEVEL_DESTROY = 0, XDG_TOPLEVEL_SET_PARENT = 1, XDG_TOPLEVEL_SET_TITLE = 2, XDG_TOPLEVEL_SET_APP_ID = 3 };
/* xdg_toplevel events */
enum { XDG_TOPLEVEL_CONFIGURE = 0, XDG_TOPLEVEL_CLOSE = 1 };

/* -------------------------------------------------------------------------- */
/* Connection & registry                                                      */
/* -------------------------------------------------------------------------- */

static bool wl_connect(wl_state_t *wl) {
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
        close(wl->fd); wl->fd = -1;
        return false;
    }

    wl->next_id = 2; /* 1 is wl_display */
    wl->display_id = 1;
    wl->registry_id = wl->next_id++;

    /* get_registry */
    wl_begin(wl->display_id, WL_DISPLAY_GET_REGISTRY);
    wl_u32(wl->registry_id);
    if (!wl_send(wl)) return false;

    /* sync (roundtrip) */
    wl->shm_pool_id = wl->next_id++; /* temporary callback object */
    wl_begin(wl->display_id, 0); /* wl_display.sync = 0 in some versions, skip for now */
    return true;
}

static bool wl_create_shm_buffer(wl_state_t *wl, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    size_t stride = (size_t)w * 4;
    if ((size_t)w > SIZE_MAX / 4) return false;
    size_t size = stride * (size_t)h;
    if (stride > SIZE_MAX / (size_t)h) return false;
    wl->shm_size = size;
    wl->shm_fd = memfd_create("ordl-wl-shm", MFD_CLOEXEC);
    if (wl->shm_fd < 0) {
        wl->shm_fd = open("/dev/shm/ordl_wl_XXXXXX", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (wl->shm_fd < 0) return false;
        unlink("/dev/shm/ordl_wl_XXXXXX"); /* anonymous */
    }
    if (ftruncate(wl->shm_fd, (off_t)size) < 0) { close(wl->shm_fd); wl->shm_fd = -1; return false; }

    wl->shm_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, wl->shm_fd, 0);
    if (wl->shm_map == MAP_FAILED) { close(wl->shm_fd); wl->shm_fd = -1; return false; }

    wl->shm_pool_id = wl->next_id++;
    wl_begin(wl->shm_id, WL_SHM_CREATE_POOL);
    wl_u32(wl->shm_pool_id);
    /* fd is sent via SCM_RIGHTS out-of-band using sendmsg */
    if (!wl_send_with_fd(wl, wl->shm_fd)) return false;
    /* After fd is sent, send the size as a separate regular message */
    wl_begin(wl->shm_pool_id, 0); /* dummy opcode to send size continuation */
    wl_u32((uint32_t)size);
    if (!wl_send(wl)) return false;

    wl->buffer_id = wl->next_id++;
    wl_begin(wl->shm_pool_id, WL_SHM_POOL_CREATE_BUFFER);
    wl_u32(wl->buffer_id);
    wl_u32(0); /* offset */
    wl_u32((uint32_t)w);
    wl_u32((uint32_t)h);
    wl_u32((uint32_t)stride);
    wl_u32(1); /* WL_SHM_FORMAT_ARGB8888 = 0, XRBG = 1, RGBA = ...; use ARGB8888 = 0? */
    /* Actually ARGB8888 = 0 in wayland.  Let's use 0. */
    wl_msg[wl_msg_len - 4] = 0; wl_msg[wl_msg_len - 3] = 0;
    wl_msg[wl_msg_len - 2] = 0; wl_msg[wl_msg_len - 1] = 0;
    if (!wl_send(wl)) return false;
    return true;
}

static bool wl_poll_event_once(wl_state_t *wl, ui_event_t *out, int timeout_ms) {
    (void)out;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(wl->fd, &rfds);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int ret = select(wl->fd + 1, &rfds, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
    if (ret <= 0) return false;

    if (!wl_recv(wl)) return false;
    uint32_t obj = ((uint32_t)wl->rx_buf[0] << 0) | ((uint32_t)wl->rx_buf[1] << 8) |
                   ((uint32_t)wl->rx_buf[2] << 16) | ((uint32_t)wl->rx_buf[3] << 24);
    uint32_t sz  = ((uint32_t)wl->rx_buf[4] << 0) | ((uint32_t)wl->rx_buf[5] << 8) |
                   ((uint32_t)wl->rx_buf[6] << 16) | ((uint32_t)wl->rx_buf[7] << 24);
    uint16_t opcode = (uint16_t)(((uint32_t)wl->rx_buf[6] << 0) | ((uint32_t)wl->rx_buf[7] << 8));

    /* Handle ping/pong, configure, etc. */
    if (obj == wl->wm_base_id && opcode == XDG_WM_BASE_PING_EVENT) {
        uint32_t serial = ((uint32_t)wl->rx_buf[8] << 0) | ((uint32_t)wl->rx_buf[9] << 8) |
                          ((uint32_t)wl->rx_buf[10] << 16) | ((uint32_t)wl->rx_buf[11] << 24);
        wl_begin(wl->wm_base_id, XDG_WM_BASE_PING);
        wl_u32(serial);
        wl_send(wl);
    } else if (obj == wl->xdg_surface_id && opcode == XDG_SURFACE_CONFIGURE) {
        uint32_t serial = ((uint32_t)wl->rx_buf[8] << 0) | ((uint32_t)wl->rx_buf[9] << 8) |
                          ((uint32_t)wl->rx_buf[10] << 16) | ((uint32_t)wl->rx_buf[11] << 24);
        wl_begin(wl->xdg_surface_id, XDG_SURFACE_ACK_CONFIGURE);
        wl_u32(serial);
        wl_send(wl);
        wl->configured = true;
    } else if (obj == wl->toplevel_id && opcode == XDG_TOPLEVEL_CONFIGURE) {
        int32_t ww = (int32_t)(((uint32_t)wl->rx_buf[8] << 0) | ((uint32_t)wl->rx_buf[9] << 8) |
                               ((uint32_t)wl->rx_buf[10] << 16) | ((uint32_t)wl->rx_buf[11] << 24));
        int32_t hh = (int32_t)(((uint32_t)wl->rx_buf[12] << 0) | ((uint32_t)wl->rx_buf[13] << 8) |
                               ((uint32_t)wl->rx_buf[14] << 16) | ((uint32_t)wl->rx_buf[15] << 24));
        if (ww > 0 && hh > 0) { wl->width = ww; wl->height = hh; }
    } else if (obj == wl->toplevel_id && opcode == XDG_TOPLEVEL_CLOSE) {
        wl->closed = true;
    }

    wl_consume(wl, sz);
    return false; /* no ui_event produced yet */
}

/* -------------------------------------------------------------------------- */
/* Backend vtable                                                             */
/* -------------------------------------------------------------------------- */

static bool be_wl_init(ui_backend_t *be, int w, int h) {
    wl_state_t *wl = calloc(1, sizeof(wl_state_t));
    if (!wl) return false;
    wl->fd = -1;
    wl->width = w; wl->height = h;

    if (!wl_connect(wl)) { free(wl); return false; }

    /* Wait for registry globals (compositor, xdg_wm_base, shm, seat) */
    for (int guard = 0; guard < 256; guard++) {
        if (!wl_recv(wl)) break;
        uint32_t obj = ((uint32_t)wl->rx_buf[0] << 0) | ((uint32_t)wl->rx_buf[1] << 8) |
                       ((uint32_t)wl->rx_buf[2] << 16) | ((uint32_t)wl->rx_buf[3] << 24);
        uint32_t sz  = ((uint32_t)wl->rx_buf[4] << 0) | ((uint32_t)wl->rx_buf[5] << 8) |
                       ((uint32_t)wl->rx_buf[6] << 16) | ((uint32_t)wl->rx_buf[7] << 24);
        uint16_t opcode = (uint16_t)(((uint32_t)wl->rx_buf[6] << 0) | ((uint32_t)wl->rx_buf[7] << 8));

        if (obj == wl->registry_id && opcode == WL_REGISTRY_GLOBAL) {
            uint32_t name = ((uint32_t)wl->rx_buf[8] << 0) | ((uint32_t)wl->rx_buf[9] << 8) |
                            ((uint32_t)wl->rx_buf[10] << 16) | ((uint32_t)wl->rx_buf[11] << 24);
            const char *iface = (const char *)(wl->rx_buf + 12);
            uint32_t ver = ((uint32_t)wl->rx_buf[sz - 4] << 0) | ((uint32_t)wl->rx_buf[sz - 3] << 8) |
                           ((uint32_t)wl->rx_buf[sz - 2] << 16) | ((uint32_t)wl->rx_buf[sz - 1] << 24);

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
        wl_consume(wl, sz);
        if (wl->compositor_id && wl->wm_base_id && wl->shm_id) break;
    }

    if (!wl->compositor_id || !wl->wm_base_id || !wl->shm_id) {
        close(wl->fd); free(wl); return false;
    }

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

    wl_begin(wl->toplevel_id, XDG_TOPLEVEL_SET_TITLE);
    wl_str("ORDL UI");
    wl_send(wl);

    wl_begin(wl->surface_id, WL_SURFACE_COMMIT);
    wl_send(wl);

    /* Wait for configure */
    for (int guard = 0; guard < 256 && !wl->configured; guard++) {
        wl_poll_event_once(wl, NULL, 100);
    }
    if (!wl->configured) { close(wl->fd); free(wl); return false; }

    /* Create shm buffer */
    if (!wl_create_shm_buffer(wl, wl->width, wl->height)) {
        close(wl->fd); free(wl); return false;
    }

    be->canvas = ui_canvas_new_fb(wl->width, wl->height);
    if (!be->canvas) { close(wl->fd); free(wl); return false; }

    be->user_data = wl;
    be->supports_mouse = true;
    be->supports_color = true;
    be->supports_unicode = false;
    be->max_colors = 0xFFFFFF;
    return true;
}

static void be_wl_shutdown(ui_backend_t *be) {
    if (!be) return;
    wl_state_t *wl = (wl_state_t *)be->user_data;
    if (wl) {
        if (wl->shm_map && wl->shm_map != MAP_FAILED) munmap(wl->shm_map, wl->shm_size);
        if (wl->shm_fd >= 0) close(wl->shm_fd);
        if (wl->fd >= 0) close(wl->fd);
        free(wl);
    }
    if (be->canvas) { ui_canvas_free(be->canvas); be->canvas = NULL; }
}

static void be_wl_present(ui_backend_t *be) {
    if (!be || !be->canvas || be->canvas->type != UI_CANVAS_FB) return;
    wl_state_t *wl = (wl_state_t *)be->user_data;
    if (!wl || !wl->shm_map) return;
    /* Copy RGBA8888 canvas → ARGB8888 shm */
    const uint32_t *src = be->canvas->pixels;
    uint8_t *dst = wl->shm_map;
    for (int y = 0; y < wl->height; y++) {
        for (int x = 0; x < wl->width; x++) {
            uint32_t rgba = src[y * wl->width + x];
            uint8_t r = (rgba >> 0) & 0xFF;
            uint8_t g = (rgba >> 8) & 0xFF;
            uint8_t b = (rgba >> 16) & 0xFF;
            uint8_t a = (rgba >> 24) & 0xFF;
            uint8_t *p = dst + (y * wl->width + x) * 4;
            p[0] = b; p[1] = g; p[2] = r; p[3] = a;
        }
    }
    wl_begin(wl->surface_id, WL_SURFACE_DAMAGE);
    wl_u32(0); wl_u32(0); wl_u32((uint32_t)wl->width); wl_u32((uint32_t)wl->height);
    wl_send(wl);
    wl_begin(wl->surface_id, WL_SURFACE_ATTACH);
    wl_u32(wl->buffer_id);
    wl_u32(0); /* x */
    wl_u32(0); /* y */
    wl_send(wl);
    wl_begin(wl->surface_id, WL_SURFACE_COMMIT);
    wl_send(wl);
}

static bool be_wl_poll_event(ui_backend_t *be, ui_event_t *out, int timeout_ms) {
    if (!be || !be->user_data) return false;
    wl_state_t *wl = (wl_state_t *)be->user_data;
    if (wl->closed) {
        out->type = UI_EVENT_QUIT;
        return true;
    }
    return wl_poll_event_once(wl, out, timeout_ms);
}

ui_backend_t *ui_backend_wayland_new(void) {
    ui_backend_t *be = calloc(1, sizeof(ui_backend_t));
    if (!be) return NULL;
    be->name = "wayland";
    be->init = be_wl_init;
    be->shutdown = be_wl_shutdown;
    be->poll_event = be_wl_poll_event;
    be->present = be_wl_present;
    return be;
}

#else /* non-POSIX */

ui_backend_t *ui_backend_wayland_new(void) {
    return NULL;
}

#endif
