/*
 * ORDL UI — Raw HID gamepad / joystick input
 * Pure C23, zero external dependencies.
 *
 * Linux: reads /dev/input/js0 via classic joystick API.
 * Other platforms: no-op stubs.
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#if defined(__linux__)
#include <sys/ioctl.h>
#include <linux/joystick.h>
#endif

typedef struct {
    int fd;
    char name[128];
    ui_gamepad_state_t state;
    bool connected;
} gamepad_ctx_t;

static gamepad_ctx_t g_gp = {0};

/* -------------------------------------------------------------------------- */
/* Platform: Linux joystick API                                               */
/* -------------------------------------------------------------------------- */

#if defined(__linux__)

bool ui_gamepad_open(int device_index) {
    ui_gamepad_close();
    char path[64];
    snprintf(path, sizeof(path), "/dev/input/js%d", device_index);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    char name[128] = {0};
    if (ioctl(fd, JSIOCGNAME(sizeof(name)), name) >= 0) {
        snprintf(g_gp.name, sizeof(g_gp.name), "%s", name);
    }
    g_gp.fd = fd;
    g_gp.connected = true;
    memset(&g_gp.state, 0, sizeof(g_gp.state));
    return true;
}

void ui_gamepad_close(void) {
    if (g_gp.fd >= 0) close(g_gp.fd);
    g_gp.fd = -1;
    g_gp.connected = false;
    memset(&g_gp.state, 0, sizeof(g_gp.state));
    g_gp.name[0] = '\0';
}

bool ui_gamepad_poll(ui_gamepad_state_t *out) {
    if (!g_gp.connected || g_gp.fd < 0) {
        if (out) memset(out, 0, sizeof(*out));
        return false;
    }
    bool changed = false;
    struct js_event ev;
    while (read(g_gp.fd, &ev, sizeof(ev)) == sizeof(ev)) {
        ev.type &= ~JS_EVENT_INIT; /* ignore synthetic init events */
        switch (ev.type) {
        case JS_EVENT_AXIS:
            changed = true;
            switch (ev.number) {
            case 0: g_gp.state.lx = ev.value / 32767.0f; break;
            case 1: g_gp.state.ly = -ev.value / 32767.0f; break; /* invert Y */
            case 2: g_gp.state.rx = ev.value / 32767.0f; break;
            case 3: g_gp.state.ry = -ev.value / 32767.0f; break;
            case 4: g_gp.state.lt = (ev.value + 32767.0f) / 65534.0f; break;
            case 5: g_gp.state.rt = (ev.value + 32767.0f) / 65534.0f; break;
            default: break;
            }
            break;
        case JS_EVENT_BUTTON:
            changed = true;
            switch (ev.number) {
            case 0: g_gp.state.a = ev.value; break;
            case 1: g_gp.state.b = ev.value; break;
            case 2: g_gp.state.x = ev.value; break;
            case 3: g_gp.state.y = ev.value; break;
            case 4: g_gp.state.lb = ev.value; break;
            case 5: g_gp.state.rb = ev.value; break;
            case 6: g_gp.state.back = ev.value; break;
            case 7: g_gp.state.start = ev.value; break;
            case 8: g_gp.state.guide = ev.value; break;
            case 9: g_gp.state.lthumb = ev.value; break;
            case 10: g_gp.state.rthumb = ev.value; break;
            default: break;
            }
            break;
        }
    }
    if (out) *out = g_gp.state;
    return changed;
}

const char *ui_gamepad_name(void) {
    return g_gp.connected ? g_gp.name : NULL;
}

#else /* Non-Linux: no-op stubs */

bool ui_gamepad_open(int device_index) {
    (void)device_index;
    return false;
}
void ui_gamepad_close(void) {}
bool ui_gamepad_poll(ui_gamepad_state_t *out) {
    if (out) memset(out, 0, sizeof(*out));
    return false;
}
const char *ui_gamepad_name(void) { return NULL; }

#endif
