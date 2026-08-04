/*
 * ORDL UI — DRM/KMS framebuffer backend (Linux)
 * Direct GPU framebuffer via Kernel Mode Setting. Zero X11/Wayland.
 * Cross-platform: Linux DRM fully implemented; Windows/macOS stubs present.
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdio.h>

/* -------------------------------------------------------------------------- */
/* Minimal DRM definitions (stable uapi)                                      */
/* -------------------------------------------------------------------------- */

#if defined(__linux__)

#define DRM_IOCTL_BASE          'd'
#define DRM_IOWR(nr,type)       _IOWR(DRM_IOCTL_BASE,nr,type)
#define DRM_IOCTL_MODE_RESOURCES    DRM_IOWR(0xA0, struct drm_mode_res)
#define DRM_IOCTL_MODE_GETCONNECTOR DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_GETENCODER   DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCRTC      DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC      DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_CREATE_DUMB  DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB     DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)
#define DRM_IOCTL_MODE_ADDFB        DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB         DRM_IOWR(0xAF, unsigned int)

struct drm_mode_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width, max_width;
    uint32_t min_height, max_height;
};

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connection;
    uint32_t mm_width, mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

struct drm_mode_get_encoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x, y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct drm_mode_modeinfo mode;
    uint32_t count_connectors;
};

struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct drm_mode_destroy_dumb {
    uint32_t handle;
};

struct drm_mode_fb_cmd {
    uint32_t fb_id;
    uint32_t width, height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
};

#define DRM_MODE_CONNECTED      1
#define DRM_MODE_ENCODER_NONE   0

/* -------------------------------------------------------------------------- */
/* DRM backend state                                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    int fd;
    uint32_t crtc_id;
    uint32_t connector_id;
    uint32_t fb_id;
    uint32_t handle;
    uint64_t size;
    uint32_t pitch;
    uint8_t *map;
    struct drm_mode_modeinfo mode;
    int width, height;

    /* evdev input */
    int evdev_fds[8];
    int evdev_count;
    int mouse_x, mouse_y;
    bool mouse_btn[4]; /* 1=left, 2=right, 3=middle */
} drm_state_t;

/* -------------------------------------------------------------------------- */
/* Minimal evdev uapi                                                         */
/* -------------------------------------------------------------------------- */

struct input_event {
    struct timeval time;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

#define EV_SYN       0x00
#define EV_KEY       0x01
#define EV_REL       0x02
#define EV_ABS       0x03

#define REL_X        0x00
#define REL_Y        0x01
#define REL_WHEEL    0x08
#define REL_HWHEEL   0x06

#define BTN_MOUSE    0x110
#define BTN_LEFT     0x110
#define BTN_RIGHT    0x111
#define BTN_MIDDLE   0x112
#define BTN_SIDE     0x113
#define BTN_EXTRA    0x114

#define KEY_ESC      1
#define KEY_1        2
#define KEY_2        3
#define KEY_3        4
#define KEY_4        5
#define KEY_5        6
#define KEY_6        7
#define KEY_7        8
#define KEY_8        9
#define KEY_9        10
#define KEY_0        11
#define KEY_MINUS    12
#define KEY_EQUAL    13
#define KEY_BACKSPACE 14
#define KEY_TAB      15
#define KEY_Q        16
#define KEY_W        17
#define KEY_E        18
#define KEY_R        19
#define KEY_T        20
#define KEY_Y        21
#define KEY_U        22
#define KEY_I        23
#define KEY_O        24
#define KEY_P        25
#define KEY_LEFTBRACE 26
#define KEY_RIGHTBRACE 27
#define KEY_ENTER    28
#define KEY_LEFTCTRL 29
#define KEY_A        30
#define KEY_S        31
#define KEY_D        32
#define KEY_F        33
#define KEY_G        34
#define KEY_H        35
#define KEY_J        36
#define KEY_K        37
#define KEY_L        38
#define KEY_SEMICOLON 39
#define KEY_APOSTROPHE 40
#define KEY_GRAVE    41
#define KEY_LEFTSHIFT 42
#define KEY_BACKSLASH 43
#define KEY_Z        44
#define KEY_X        45
#define KEY_C        46
#define KEY_V        47
#define KEY_B        48
#define KEY_N        49
#define KEY_M        50
#define KEY_COMMA    51
#define KEY_DOT      52
#define KEY_SLASH    53
#define KEY_RIGHTSHIFT 54
#define KEY_KPASTERISK 55
#define KEY_LEFTALT  56
#define KEY_SPACE    57
#define KEY_CAPSLOCK 58
#define KEY_F1       59
#define KEY_F2       60
#define KEY_F3       61
#define KEY_F4       62
#define KEY_F5       63
#define KEY_F6       64
#define KEY_F7       65
#define KEY_F8       66
#define KEY_F9       67
#define KEY_F10      68
#define KEY_F11      87
#define KEY_F12      88
#define KEY_UP       103
#define KEY_LEFT     105
#define KEY_RIGHT    106
#define KEY_DOWN     108
#define KEY_PAGEUP   104
#define KEY_PAGEDOWN 109
#define KEY_HOME     102
#define KEY_END      107
#define KEY_INSERT   110
#define KEY_DELETE   111

static bool drm_evdev_open(drm_state_t *drm) {
    drm->evdev_count = 0;
    for (int i = 0; i < 32 && drm->evdev_count < 8; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        drm->evdev_fds[drm->evdev_count++] = fd;
    }
    return drm->evdev_count > 0;
}

static void drm_evdev_close(drm_state_t *drm) {
    for (int i = 0; i < drm->evdev_count; i++) {
        if (drm->evdev_fds[i] >= 0) close(drm->evdev_fds[i]);
    }
    drm->evdev_count = 0;
}

static ui_key_t evdev_key_to_ui(uint16_t code) {
    switch (code) {
    case KEY_ENTER:      return UI_KEY_ENTER;
    case KEY_TAB:        return UI_KEY_TAB;
    case KEY_BACKSPACE:  return UI_KEY_BACKSPACE;
    case KEY_DELETE:     return UI_KEY_DELETE;
    case KEY_ESC:        return UI_KEY_ESCAPE;
    case KEY_SPACE:      return UI_KEY_SPACE;
    case KEY_UP:         return UI_KEY_UP;
    case KEY_DOWN:       return UI_KEY_DOWN;
    case KEY_LEFT:       return UI_KEY_LEFT;
    case KEY_RIGHT:      return UI_KEY_RIGHT;
    case KEY_HOME:       return UI_KEY_HOME;
    case KEY_END:        return UI_KEY_END;
    case KEY_PAGEUP:     return UI_KEY_PAGE_UP;
    case KEY_PAGEDOWN:   return UI_KEY_PAGE_DOWN;
    case KEY_INSERT:     return UI_KEY_INSERT;
    case KEY_F1:         return UI_KEY_F1;
    case KEY_F2:         return UI_KEY_F2;
    case KEY_F3:         return UI_KEY_F3;
    case KEY_F4:         return UI_KEY_F4;
    case KEY_F5:         return UI_KEY_F5;
    case KEY_F6:         return UI_KEY_F6;
    case KEY_F7:         return UI_KEY_F7;
    case KEY_F8:         return UI_KEY_F8;
    case KEY_F9:         return UI_KEY_F9;
    case KEY_F10:        return UI_KEY_F10;
    case KEY_F11:        return UI_KEY_F11;
    case KEY_F12:        return UI_KEY_F12;
    default:             return UI_KEY_NONE;
    }
}

static bool drm_open(drm_state_t *drm, const char *path) {
    drm->fd = open(path, O_RDWR | O_CLOEXEC);
    if (drm->fd < 0) return false;

    struct drm_mode_res res = {0};
    if (ioctl(drm->fd, DRM_IOCTL_MODE_RESOURCES, &res) < 0) {
        close(drm->fd);
        drm->fd = -1;
        return false;
    }

    uint32_t *conn_ids = calloc(res.count_connectors, sizeof(uint32_t));
    res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
    if (ioctl(drm->fd, DRM_IOCTL_MODE_RESOURCES, &res) < 0) {
        free(conn_ids);
        close(drm->fd);
        drm->fd = -1;
        return false;
    }

    /* Find first connected connector */
    bool found = false;
    for (uint32_t i = 0; i < res.count_connectors && !found; i++) {
        struct drm_mode_get_connector conn = {0};
        conn.connector_id = conn_ids[i];
        if (ioctl(drm->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;

        struct drm_mode_modeinfo *modes = calloc(conn.count_modes, sizeof(struct drm_mode_modeinfo));
        conn.modes_ptr = (uint64_t)(uintptr_t)modes;
        uint32_t *encoders = calloc(conn.count_encoders, sizeof(uint32_t));
        conn.encoders_ptr = (uint64_t)(uintptr_t)encoders;
        if (ioctl(drm->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
            free(modes); free(encoders); continue;
        }

        if (conn.connection == DRM_MODE_CONNECTED && conn.count_modes > 0) {
            drm->connector_id = conn.connector_id;
            drm->mode = modes[0];
            drm->width = modes[0].hdisplay;
            drm->height = modes[0].vdisplay;

            struct drm_mode_get_encoder enc = {0};
            enc.encoder_id = conn.encoder_id;
            if (ioctl(drm->fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0 || enc.crtc_id == 0) {
                /* Fallback: pick first possible CRTC */
                uint32_t *crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
                if (!crtc_ids) { free(modes); free(encoders); continue; }
                res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
                ioctl(drm->fd, DRM_IOCTL_MODE_RESOURCES, &res);
                if (res.count_crtcs > 0) enc.crtc_id = crtc_ids[0];
                free(crtc_ids);
            }
            drm->crtc_id = enc.crtc_id;
            found = true;
        }
        free(modes);
        free(encoders);
    }
    free(conn_ids);

    if (!found) {
        close(drm->fd);
        drm->fd = -1;
        return false;
    }
    return true;
}

static bool drm_create_fb(drm_state_t *drm) {
    struct drm_mode_create_dumb create = {
        .width = (uint32_t)drm->width,
        .height = (uint32_t)drm->height,
        .bpp = 32,
    };
    if (ioctl(drm->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) return false;
    drm->handle = create.handle;
    drm->pitch = create.pitch;
    drm->size = create.size;

    struct drm_mode_fb_cmd fb = {
        .width = (uint32_t)drm->width,
        .height = (uint32_t)drm->height,
        .pitch = drm->pitch,
        .bpp = 32,
        .depth = 24,
        .handle = drm->handle,
    };
    if (ioctl(drm->fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) goto err_destroy;
    drm->fb_id = fb.fb_id;

    struct drm_mode_map_dumb map = { .handle = drm->handle };
    if (ioctl(drm->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) goto err_rmfb;

    drm->map = mmap(0, drm->size, PROT_READ | PROT_WRITE, MAP_SHARED, drm->fd, (off_t)map.offset);
    if (drm->map == MAP_FAILED) goto err_rmfb;
    memset(drm->map, 0, drm->size);
    return true;

err_rmfb:
    ioctl(drm->fd, DRM_IOCTL_MODE_RMFB, &drm->fb_id);
err_destroy:
    {
        struct drm_mode_destroy_dumb destroy = { .handle = drm->handle };
        ioctl(drm->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    return false;
}

static void drm_destroy(drm_state_t *drm) {
    if (drm->map && drm->map != MAP_FAILED) munmap(drm->map, drm->size);
    if (drm->fb_id) {
        struct drm_mode_crtc crtc = { .crtc_id = drm->crtc_id, .set_connectors_ptr = 0, .count_connectors = 0 };
        ioctl(drm->fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
        ioctl(drm->fd, DRM_IOCTL_MODE_RMFB, &drm->fb_id);
    }
    if (drm->handle) {
        struct drm_mode_destroy_dumb destroy = { .handle = drm->handle };
        ioctl(drm->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    if (drm->fd >= 0) close(drm->fd);
    memset(drm, 0, sizeof(*drm));
    drm->fd = -1;
}

/* -------------------------------------------------------------------------- */
/* Backend vtable                                                             */
/* -------------------------------------------------------------------------- */

static bool be_drm_init(ui_backend_t *be, int w, int h) {
    (void)w; (void)h;
    drm_state_t *drm = calloc(1, sizeof(drm_state_t));
    if (!drm) return false;
    drm->fd = -1;

    const char *paths[] = { "/dev/dri/card0", "/dev/dri/card1", NULL };
    bool ok = false;
    for (int i = 0; paths[i] && !ok; i++) {
        ok = drm_open(drm, paths[i]);
    }
    if (!ok) { free(drm); return false; }

    if (!drm_create_fb(drm)) {
        drm_destroy(drm);
        free(drm);
        return false;
    }

    /* Set CRTC */
    struct drm_mode_crtc crtc = {0};
    crtc.crtc_id = drm->crtc_id;
    crtc.fb_id = drm->fb_id;
    crtc.x = 0; crtc.y = 0;
    crtc.mode = drm->mode;
    crtc.mode_valid = 1;
    crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&drm->connector_id;
    crtc.count_connectors = 1;
    if (ioctl(drm->fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
        drm_destroy(drm);
        free(drm);
        return false;
    }

    be->canvas = ui_canvas_new_fb(drm->width, drm->height);
    if (!be->canvas) {
        drm_destroy(drm);
        free(drm);
        return false;
    }
    be->user_data = drm;
    be->supports_mouse = true;
    be->supports_color = true;
    be->supports_unicode = false; /* FB is pixels */
    be->max_colors = 0xFFFFFF;

    /* Open evdev input devices */
    drm_evdev_open(drm);
    return true;
}

static void be_drm_shutdown(ui_backend_t *be) {
    if (!be) return;
    drm_state_t *drm = (drm_state_t *)be->user_data;
    if (drm) {
        drm_evdev_close(drm);
        drm_destroy(drm);
        free(drm);
    }
    if (be->canvas) {
        ui_canvas_free(be->canvas);
        be->canvas = NULL;
    }
}

static void rgba8888_to_argb32(const uint32_t *src, uint8_t *dst, int w, int h, uint32_t pitch) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t rgba = src[y * w + x];
            uint8_t r = (rgba >> 0) & 0xFF;
            uint8_t g = (rgba >> 8) & 0xFF;
            uint8_t b = (rgba >> 16) & 0xFF;
            uint8_t a = (rgba >> 24) & 0xFF;
            /* ARGB32 little-endian: B G R A */
            uint8_t *p = dst + y * pitch + x * 4;
            p[0] = b; p[1] = g; p[2] = r; p[3] = a;
        }
    }
}

static void be_drm_present(ui_backend_t *be) {
    if (!be || !be->canvas || be->canvas->type != UI_CANVAS_FB) return;
    drm_state_t *drm = (drm_state_t *)be->user_data;
    if (!drm || !drm->map) return;
    /* Validate dimensions match to prevent buffer overruns */
    if (be->canvas->w != drm->width || be->canvas->h != drm->height) return;
    rgba8888_to_argb32(be->canvas->pixels, drm->map,
                       drm->width, drm->height, drm->pitch);
}

static bool be_drm_poll_event(ui_backend_t *be, ui_event_t *out, int timeout_ms) {
    if (!be || !be->user_data) return false;
    drm_state_t *drm = (drm_state_t *)be->user_data;
    if (drm->evdev_count == 0) return false;

    fd_set rfds;
    FD_ZERO(&rfds);
    int maxfd = -1;
    for (int i = 0; i < drm->evdev_count; i++) {
        if (drm->evdev_fds[i] >= 0) {
            FD_SET(drm->evdev_fds[i], &rfds);
            if (drm->evdev_fds[i] > maxfd) maxfd = drm->evdev_fds[i];
        }
    }
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int ret = select(maxfd + 1, &rfds, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
    if (ret <= 0) return false;

    struct input_event ie;
    for (int i = 0; i < drm->evdev_count; i++) {
        int fd = drm->evdev_fds[i];
        if (fd < 0 || !FD_ISSET(fd, &rfds)) continue;
        ssize_t n = read(fd, &ie, sizeof(ie));
        if (n != (ssize_t)sizeof(ie)) continue;

        memset(out, 0, sizeof(*out));
        if (ie.type == EV_KEY) {
            if (ie.code >= BTN_LEFT && ie.code <= BTN_MIDDLE) {
                int btn = (int)(ie.code - BTN_LEFT + 1);
                bool pressed = ie.value != 0;
                if (pressed && !drm->mouse_btn[btn]) {
                    out->type = UI_EVENT_MOUSE_PRESS;
                    out->mouse.x = drm->mouse_x;
                    out->mouse.y = drm->mouse_y;
                    out->mouse.button = btn;
                    drm->mouse_btn[btn] = true;
                    return true;
                } else if (!pressed && drm->mouse_btn[btn]) {
                    out->type = UI_EVENT_MOUSE_RELEASE;
                    out->mouse.x = drm->mouse_x;
                    out->mouse.y = drm->mouse_y;
                    out->mouse.button = btn;
                    drm->mouse_btn[btn] = false;
                    return true;
                }
            } else {
                if (ie.value == 0) continue; /* ignore key release for now */
                out->type = UI_EVENT_KEY;
                out->key.key = evdev_key_to_ui(ie.code);
                return true;
            }
        } else if (ie.type == EV_REL) {
            if (ie.code == REL_X) {
                drm->mouse_x += ie.value;
                if (drm->mouse_x < 0) drm->mouse_x = 0;
                if (drm->mouse_x >= drm->width) drm->mouse_x = drm->width - 1;
            } else if (ie.code == REL_Y) {
                drm->mouse_y += ie.value;
                if (drm->mouse_y < 0) drm->mouse_y = 0;
                if (drm->mouse_y >= drm->height) drm->mouse_y = drm->height - 1;
            } else if (ie.code == REL_WHEEL) {
                out->type = UI_EVENT_MOUSE_SCROLL;
                out->mouse.x = drm->mouse_x;
                out->mouse.y = drm->mouse_y;
                out->mouse.scroll_dy = -ie.value; /* invert for natural scroll */
                return true;
            }
        }
    }
    return false;
}

ui_backend_t *ui_backend_drm_new(void) {
    ui_backend_t *be = calloc(1, sizeof(ui_backend_t));
    if (!be) return NULL;
    be->name = "drm";
    be->init = be_drm_init;
    be->shutdown = be_drm_shutdown;
    be->poll_event = be_drm_poll_event;
    be->present = be_drm_present;
    return be;
}

#else /* !__linux__ */

ui_backend_t *ui_backend_drm_new(void) {
    return NULL; /* DRM/KMS is Linux-only; Windows/macOS use their own backends */
}

#endif /* __linux__ */
