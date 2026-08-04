/*
 * ORDL UI — Terminal backend
 * VT100 / xterm-256color escape sequences. No ncurses, no terminfo.
 */

#include "forge/ui/ordl_ui.h"
#include "forge/ui/ordl_ui_debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

static volatile sig_atomic_t term_resize_pending = 0;
static struct termios term_emergency_restore_copy;
static volatile sig_atomic_t term_emergency_ready = 0;

static void term_sigwinch_handler(int sig) {
    (void)sig;
    term_resize_pending = 1;
}

static void term_emergency_handler(int sig) {
    (void)sig;
    /* Restore terminal via write-to-self-pipe pattern. */
    const char restore_seq[] = "\033[?1006l\033[?1002l\033[?1000l\033[?1049l\033[?25h\033[0m\033[2J\033[H";
    ssize_t _w = write(STDOUT_FILENO, restore_seq, sizeof(restore_seq) - 1);
    (void)_w;
    if (term_emergency_ready) {
        tcsetattr(STDIN_FILENO, TCSANOW, (struct termios *)&term_emergency_restore_copy);
    }
    _exit(128 + sig);
}

static void term_atexit_cleanup(void) {
    const char restore_seq[] = "\033[?1006l\033[?1002l\033[?1000l\033[?1049l\033[?25h\033[0m\033[2J\033[H";
    ssize_t _w = write(STDOUT_FILENO, restore_seq, sizeof(restore_seq) - 1);
    (void)_w;
    if (term_emergency_ready) {
        tcsetattr(STDIN_FILENO, TCSANOW, (struct termios *)&term_emergency_restore_copy);
    }
}

/* -------------------------------------------------------------------------- */
/* Unbuffered terminal output                                                 */
/* -------------------------------------------------------------------------- */

static void term_out(const char *s, size_t n) {
    ssize_t _w = write(STDOUT_FILENO, s, n);
    (void)_w;
}

static void term_out_str(const char *s) {
    term_out(s, strlen(s));
}

static void term_out_char(char c) {
    ssize_t _w = write(STDOUT_FILENO, &c, 1);
    (void)_w;
}

/* -------------------------------------------------------------------------- */
/* Terminal state                                                             */
/* -------------------------------------------------------------------------- */

typedef struct {
    ui_backend_t base;
    struct termios orig_termios;
    int orig_flags;
    bool raw_mode;
    int cols, rows;
    /* Last rendered frame for diffing */
    ui_cell_t *last_cells;
    size_t last_cells_size;
    /* Mouse tracking */
    bool mouse_enabled;
    /* Cursor position for TUI */
    bool cursor_visible;
    int cursor_x, cursor_y;
    /* Input buffering (per-backend, not static) */
    char input_buf[256];
    size_t input_buf_len;
} term_backend_t;

/* -------------------------------------------------------------------------- */
/* Escape sequences — all unbuffered write()                                  */
/* -------------------------------------------------------------------------- */

#define ESC "\033"
#define CSI ESC "["

static void set_cursor(int x, int y) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), CSI "%d;%dH", y + 1, x + 1);
    if (n > 0) term_out(buf, (size_t)n);
}

static void hide_cursor(void)      { term_out_str(CSI "?25l"); }
static void show_cursor(void)      { term_out_str(CSI "?25h"); }
static void clear_screen(void)     { term_out_str(CSI "2J" CSI "1;1H"); }
static void enter_alt_screen(void) { term_out_str(CSI "?1049h"); }
static void exit_alt_screen(void)  { term_out_str(CSI "?1049l"); }
static void enable_mouse(void)     { term_out_str(CSI "?1000h" CSI "?1002h" CSI "?1006h"); }
static void disable_mouse(void)    { term_out_str(CSI "?1006l" CSI "?1002l" CSI "?1000l"); }

/* xterm-256color RGB */
static void set_fg_rgb(uint8_t r, uint8_t g, uint8_t b) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), CSI "38;2;%d;%d;%dm", (int)r, (int)g, (int)b);
    if (n > 0) term_out(buf, (size_t)n);
}

static void set_bg_rgb(uint8_t r, uint8_t g, uint8_t b) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), CSI "48;2;%d;%d;%dm", (int)r, (int)g, (int)b);
    if (n > 0) term_out(buf, (size_t)n);
}

static void set_fg_mono(bool bright) {
    char buf[16];
    int n = snprintf(buf, sizeof(buf), CSI "%dm", bright ? 37 : 90);
    if (n > 0) term_out(buf, (size_t)n);
}

static void set_bg_mono(bool bright) {
    char buf[16];
    int n = snprintf(buf, sizeof(buf), CSI "%dm", bright ? 47 : 100);
    if (n > 0) term_out(buf, (size_t)n);
}

static void reset_color(void) { term_out_str(CSI "0m"); }

/* Luminance-based brightness threshold */
static bool is_bright(uint8_t r, uint8_t g, uint8_t b) {
    return (0.299f * r + 0.587f * g + 0.114f * b) > 128.0f;
}

static bool g_mono_mode = false;

static void detect_mono_mode(void) {
    const char *term = getenv("TERM");
    if (term && (strcmp(term, "dumb") == 0 || strstr(term, "mono"))) {
        g_mono_mode = true;
        return;
    }
    const char *colors = getenv("COLORS");
    if (colors && atoi(colors) <= 2) {
        g_mono_mode = true;
        return;
    }
    g_mono_mode = false;
}

/* Attributes */
static void set_attrs(uint32_t attrs) {
    if (attrs & UI_ATTR_BOLD)      term_out_str(CSI "1m");
    if (attrs & UI_ATTR_ITALIC)    term_out_str(CSI "3m");
    if (attrs & UI_ATTR_UNDERLINE) term_out_str(CSI "4m");
    if (attrs & UI_ATTR_STRIKE)    term_out_str(CSI "9m");
    if (attrs & UI_ATTR_BLINK)     term_out_str(CSI "5m");
}

/* -------------------------------------------------------------------------- */
/* Raw mode                                                                   */
/* -------------------------------------------------------------------------- */

static bool term_raw_mode(struct termios *orig) {
    if (tcgetattr(STDIN_FILENO, orig) < 0) return false;
    struct termios raw = *orig;
    raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    raw.c_oflag &= ~OPOST;
    raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
}

static bool term_restore_mode(const struct termios *orig) {
    return tcsetattr(STDIN_FILENO, TCSAFLUSH, orig) == 0;
}

/* -------------------------------------------------------------------------- */
/* Size detection                                                             */
/* -------------------------------------------------------------------------- */

static void term_get_size(int *cols, int *rows) {
    struct winsize ws = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    } else {
        *cols = 80;
        *rows = 24;
    }
    /* Guard against terminals that report 0x0 during init or resize */
    if (*cols <= 0) *cols = 80;
    if (*rows <= 0) *rows = 24;
}

/* -------------------------------------------------------------------------- */
/* Event parsing                                                              */
/* -------------------------------------------------------------------------- */

static bool parse_esc_key(const char *buf, size_t len, size_t *off, ui_event_t *ev) {
    if (*off >= len || buf[*off] != '\033') return false;
    size_t i = *off + 1;
    if (i >= len) return false;

    /* CSI sequences: ESC [ ... */
    if (buf[i] == '[') {
        i++;
        bool sgr_mouse = false;
        if (i < len && buf[i] == '<') {
            sgr_mouse = true;
            i++;
        }
        int params[16] = {0};
        int p = 0;
        while (i < len && ((buf[i] >= '0' && buf[i] <= '9') || buf[i] == ';')) {
            if (buf[i] == ';') {
                if (p < 15) p++;
            } else {
                if (p < 16) params[p] = params[p] * 10 + (buf[i] - '0');
            }
            i++;
        }
        if (i >= len) return false;
        char final = buf[i++];

        if (sgr_mouse) {
            if (final == 'M' || final == 'm') {
                if (p >= 2) {
                    *off = i;
                    int btn = params[0];
                    /* SGR extended: scroll wheel = 64 (up) / 65 (down) */
                    if (btn >= 64) {
                        ev->type = UI_EVENT_MOUSE_SCROLL;
                        /* Strip modifier bits (shift=+4, meta=+8, ctrl=+16) */
                        int base_btn = btn & ~0x1C;
                        if (base_btn == 64) ev->mouse.scroll_dy = -3;
                        else if (base_btn == 65) ev->mouse.scroll_dy = 3;
                        else { ev->type = UI_EVENT_NONE; return false; }
                        ev->mouse.x = params[1] - 1;
                        ev->mouse.y = params[2] - 1;
                        return true;
                    }
                    ev->type = (final == 'M') ? UI_EVENT_MOUSE_PRESS : UI_EVENT_MOUSE_RELEASE;
                    ev->mouse.button = (btn & 3) + 1;
                    ev->mouse.x = params[1] - 1;
                    ev->mouse.y = params[2] - 1;
                    return true;
                }
            }
            return false; /* Unrecognized SGR mouse */
        }

        *off = i;
        ev->type = UI_EVENT_KEY;

        switch (final) {
            case 'A': ev->key.key = UI_KEY_UP; return true;
            case 'B': ev->key.key = UI_KEY_DOWN; return true;
            case 'C': ev->key.key = UI_KEY_RIGHT; return true;
            case 'D': ev->key.key = UI_KEY_LEFT; return true;
            case 'H': ev->key.key = UI_KEY_HOME; return true;
            case 'F': ev->key.key = UI_KEY_END; return true;
            case '~':
                switch (params[0]) {
                    case 1: ev->key.key = UI_KEY_HOME; return true;
                    case 2: ev->key.key = UI_KEY_INSERT; ev->key.key = UI_KEY_NONE; return true;
                    case 3: ev->key.key = UI_KEY_DELETE; return true;
                    case 4: ev->key.key = UI_KEY_END; return true;
                    case 5: ev->key.key = UI_KEY_PAGE_UP; return true;
                    case 6: ev->key.key = UI_KEY_PAGE_DOWN; return true;
                    case 11: case 12: case 13: case 14: case 15:
                        ev->key.key = (ui_key_t)(UI_KEY_F1 + params[0] - 11); return true;
                    case 17: case 18: case 19: case 20: case 21:
                        ev->key.key = (ui_key_t)(UI_KEY_F6 + params[0] - 17); return true;
                    case 23: case 24:
                        ev->key.key = (ui_key_t)(UI_KEY_F11 + params[0] - 23); return true;
                }
                break;
            case 'M': /* X10 mouse */
                if (i + 2 < len) {
                    unsigned char b = (unsigned char)buf[i++];
                    unsigned char x = (unsigned char)buf[i++];
                    unsigned char y = (unsigned char)buf[i++];
                    *off = i;
                    ev->type = UI_EVENT_MOUSE_PRESS;
                    ev->mouse.button = (b & 3) + 1;
                    ev->mouse.x = x - 33;
                    ev->mouse.y = y - 33;
                    return true;
                }
                break;
        }
        return false; /* Unrecognized CSI */
    }

    /* ESC O sequences (SS3) */
    if (buf[i] == 'O' && i + 1 < len) {
        *off = i + 2;
        ev->type = UI_EVENT_KEY;
        switch (buf[i + 1]) {
            case 'P': ev->key.key = UI_KEY_F1; return true;
            case 'Q': ev->key.key = UI_KEY_F2; return true;
            case 'R': ev->key.key = UI_KEY_F3; return true;
            case 'S': ev->key.key = UI_KEY_F4; return true;
            case 'H': ev->key.key = UI_KEY_HOME; return true;
            case 'F': ev->key.key = UI_KEY_END; return true;
        }
        return false;
    }

    /* ESC letter (Alt+key) */
    if (i < len && buf[i] >= 32 && buf[i] < 127) {
        *off = i + 1;
        ev->type = UI_EVENT_KEY;
        ev->key.alt = true;
        ev->key.codepoint = (uint32_t)(unsigned char)buf[i];
        return true;
    }

    return false;
}

static bool term_poll_event(ui_backend_t *be, ui_event_t *out, int timeout_ms) {
    term_backend_t *tbe = (term_backend_t *)be;
    char *buf = tbe->input_buf;
    size_t *buf_len_p = &tbe->input_buf_len;
    size_t buf_len = *buf_len_p;
    ui_debug_log("[term_poll] called timeout=%d buf_len=%zu", timeout_ms, buf_len);
    if (buf_len > 0) ui_debug_log_raw("[term_poll] buffered", buf, buf_len);

    while (true) {
        if (term_resize_pending) {
            term_resize_pending = 0;
            int old_cols = tbe->cols, old_rows = tbe->rows;
            term_get_size(&tbe->cols, &tbe->rows);
            ui_debug_log("[term_poll] RESIZE old=%dx%d new=%dx%d", old_cols, old_rows, tbe->cols, tbe->rows);
            if (tbe->cols != old_cols || tbe->rows != old_rows) {
                out->type = UI_EVENT_RESIZE;
                out->resize.w = tbe->cols;
                out->resize.h = tbe->rows;
                return true;
            }
        }

        /* Try to parse a complete event from the buffer */
        size_t consumed = 0;
        bool got_event = false;

        while (consumed < buf_len) {
            unsigned char ch = (unsigned char)buf[consumed];
            ui_debug_log("[term_poll] parsing byte[%zu]=0x%02x (%c) total=%zu", consumed, ch, (ch>=32&&ch<127)?ch:'?', buf_len);

            /* Escape sequences */
            if (ch == '\033') {
                size_t prev = consumed;
                if (parse_esc_key(buf, buf_len, &consumed, out)) {
                    ui_debug_log("[term_poll] ESC event type=%d key=%d codepoint=%u", out->type, out->key.key, out->key.codepoint);
                    got_event = true;
                    break;
                }
                if (consumed == prev) {
                    /* If there are more bytes after ESC, consume ESC and continue */
                    if (buf_len > prev + 1) {
                        ui_debug_log("[term_poll] ESC unknown, consuming ESC byte");
                        consumed = prev + 1;
                        continue;
                    }
                    ui_debug_log("[term_poll] ESC incomplete, need more bytes");
                    break;
                }
                ui_debug_log("[term_poll] ESC parse failed, skipping");
                continue;
            }

            /* Special control chars */
            if (ch == 127 || ch == 8) {
                consumed++;
                out->type = UI_EVENT_KEY;
                out->key.key = UI_KEY_BACKSPACE;
                ui_debug_log("[term_poll] BACKSPACE");
                got_event = true;
                break;
            }
            if (ch == '\r' || ch == '\n') {
                consumed++;
                out->type = UI_EVENT_KEY;
                out->key.key = UI_KEY_ENTER;
                ui_debug_log("[term_poll] ENTER");
                got_event = true;
                break;
            }
            if (ch == '\t') {
                consumed++;
                out->type = UI_EVENT_KEY;
                out->key.key = UI_KEY_TAB;
                ui_debug_log("[term_poll] TAB");
                got_event = true;
                break;
            }

            /* Ctrl+letter */
            if (ch < 32) {
                consumed++;
                out->type = UI_EVENT_KEY;
                out->key.ctrl = true;
                out->key.codepoint = ch + 64; /* Ctrl+A = 1 -> 'A' = 65 */
                ui_debug_log("[term_poll] CTRL+%c", (char)out->key.codepoint);
                got_event = true;
                break;
            }

            /* UTF-8 text input */
            if (ch >= 32) {
                uint32_t cp;
                size_t off = consumed;
                if (ui_utf8_decode(buf, buf_len, &off, &cp)) {
                    consumed = off;
                    out->type = UI_EVENT_KEY;
                    out->key.codepoint = cp;
                    ui_debug_log("[term_poll] TEXT codepoint=%u ('%c')", cp, (cp>=32&&cp<127)?cp:'?');
                    got_event = true;
                    break;
                }
                ui_debug_log("[term_poll] UTF8 decode failed at %zu", consumed);
            }

            consumed++;
        }

        /* Shift remaining bytes */
        if (consumed > 0) {
            if (consumed > buf_len) consumed = buf_len;
            buf_len -= consumed;
            memmove(buf, buf + consumed, buf_len);
            ui_debug_log("[term_poll] shifted %zu bytes, remaining=%zu", consumed, buf_len);
        }

        if (got_event) {
            *buf_len_p = buf_len;
            ui_debug_log("[term_poll] returning event type=%d", out->type);
            return true;
        }

        /* Need more input */
        if (buf_len >= sizeof(tbe->input_buf)) {
            ui_debug_log("[term_poll] buffer full, dropping");
            buf_len = 0; /* buffer full with incomplete data — drop it */
            *buf_len_p = buf_len;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        ui_debug_log("[term_poll] select(timeout=%d)...", timeout_ms);
        int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
        if (ret < 0) {
            ui_debug_log("[term_poll] select error errno=%d", errno);
            if (errno == EINTR) continue;
            *buf_len_p = buf_len;
            return false;
        }
        if (ret == 0) {
            *buf_len_p = buf_len;
            ui_debug_log("[term_poll] select timeout");
            return false;
        }

        ssize_t n = read(STDIN_FILENO, buf + buf_len, sizeof(tbe->input_buf) - buf_len);
        ui_debug_log("[term_poll] read() returned %zd", n);
        if (n <= 0) { *buf_len_p = buf_len; return false; }
        buf_len += (size_t)n;
        *buf_len_p = buf_len;
        ui_debug_log_raw("[term_poll] read data", buf + buf_len - n, (size_t)n);
    }
}

/* -------------------------------------------------------------------------- */
/* Rendering / presentation                                                   */
/* -------------------------------------------------------------------------- */

static void term_present(ui_backend_t *be) {
    term_backend_t *tbe = (term_backend_t *)be;
    ui_canvas_t *c = be->canvas;
    if (!c || c->type != UI_CANVAS_TERM) return;

    size_t needed = (size_t)c->w * (size_t)c->h;
    if (needed != tbe->last_cells_size) {
        ui_cell_t *new_last = realloc(tbe->last_cells, needed * sizeof(ui_cell_t));
        if (new_last) {
            if (needed > tbe->last_cells_size)
                memset(new_last + tbe->last_cells_size, 0,
                       (needed - tbe->last_cells_size) * sizeof(ui_cell_t));
            tbe->last_cells = new_last;
            tbe->last_cells_size = needed;
            tbe->cols = c->w;
            tbe->rows = c->h;
        }
    }

    /* Diff against last frame */
    int output_cells = 0;
    int skipped_cells = 0;
    for (int y = 0; y < c->h; y++) {
        for (int x = 0; x < c->w; x++) {
            size_t idx = (size_t)y * (size_t)c->w + x;
            if (idx >= tbe->last_cells_size) { skipped_cells++; continue; }
            ui_cell_t *cell = &c->cells[idx];
            ui_cell_t *last = &tbe->last_cells[idx];

            if (cell->codepoint == last->codepoint &&
                cell->fg.r == last->fg.r && cell->fg.g == last->fg.g &&
                cell->fg.b == last->fg.b && cell->fg.a == last->fg.a &&
                cell->bg.r == last->bg.r && cell->bg.g == last->bg.g &&
                cell->bg.b == last->bg.b && cell->bg.a == last->bg.a &&
                cell->attrs == last->attrs) {
                continue;
            }

            *last = *cell;
            output_cells++;

            /* Filter control characters that corrupt terminal state */
            uint32_t cp = cell->codepoint;
            if (cp < 32 || cp == 127 || (cp >= 0x80 && cp <= 0x9F)) cp = ' ';

            set_cursor(x, y);
            reset_color();
            set_attrs(cell->attrs);
            if (g_mono_mode) {
                bool bright = is_bright(cell->fg.r, cell->fg.g, cell->fg.b);
                bool bg_bright = is_bright(cell->bg.r, cell->bg.g, cell->bg.b);
                if (bright) {
                    set_fg_mono(true);
                    if (cell->attrs & UI_ATTR_BOLD) set_fg_mono(true);
                } else {
                    set_fg_mono(false);
                }
                if (bg_bright) set_bg_mono(true);
                else set_bg_mono(false);
            } else {
                set_fg_rgb(cell->fg.r, cell->fg.g, cell->fg.b);
                set_bg_rgb(cell->bg.r, cell->bg.g, cell->bg.b);
            }

            /* Print UTF-8 for codepoint via unbuffered write */
            if (cp < 0x80) {
                term_out_char((char)cp);
            } else if (cp < 0x800) {
                char u[2] = {
                    (char)(0xC0 | (cp >> 6)),
                    (char)(0x80 | (cp & 0x3F))
                };
                term_out(u, 2);
            } else if (cp < 0x10000) {
                char u[3] = {
                    (char)(0xE0 | (cp >> 12)),
                    (char)(0x80 | ((cp >> 6) & 0x3F)),
                    (char)(0x80 | (cp & 0x3F))
                };
                term_out(u, 3);
            } else {
                char u[4] = {
                    (char)(0xF0 | (cp >> 18)),
                    (char)(0x80 | ((cp >> 12) & 0x3F)),
                    (char)(0x80 | ((cp >> 6) & 0x3F)),
                    (char)(0x80 | (cp & 0x3F))
                };
                term_out(u, 4);
            }
        }
    }

    ui_debug_log("[term_present] canvas=%dx%d output=%d skipped=%d last_size=%zu",
                 c->w, c->h, output_cells, skipped_cells, tbe->last_cells_size);

    /* Restore cursor position */
    if (tbe->cursor_visible) {
        show_cursor();
        set_cursor(tbe->cursor_x, tbe->cursor_y);
    } else {
        hide_cursor();
    }

    /* Clear damage */
    c->damage_count = 0;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

static bool term_init(ui_backend_t *be, int w, int h) {
    (void)w; (void)h;
    term_backend_t *tbe = (term_backend_t *)be;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;

    /* Disable buffering on stdout/stderr so escape sequences are immediate */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    tbe->orig_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (!term_raw_mode(&tbe->orig_termios)) return false;
    tbe->raw_mode = true;

    /* Register emergency restore handler with a static copy of termios */
    term_emergency_restore_copy = tbe->orig_termios;
    term_emergency_ready = 1;
    (void)signal(SIGSEGV, term_emergency_handler);
    (void)signal(SIGABRT, term_emergency_handler);
    (void)signal(SIGFPE, term_emergency_handler);
    (void)signal(SIGILL, term_emergency_handler);
    (void)signal(SIGBUS, term_emergency_handler);

    atexit(term_atexit_cleanup);

    term_get_size(&tbe->cols, &tbe->rows);
    (void)signal(SIGWINCH, term_sigwinch_handler);

    /* Allocate canvas */
    be->canvas = ui_canvas_new_term(tbe->cols, tbe->rows);
    if (!be->canvas) { term_restore_mode(&tbe->orig_termios); return false; }

    /* Allocate last frame buffer */
    size_t cell_count = (size_t)tbe->cols * (size_t)tbe->rows;
    tbe->last_cells = calloc(cell_count, sizeof(ui_cell_t));
    if (!tbe->last_cells) {
        ui_canvas_free(be->canvas);
        be->canvas = NULL;
        term_restore_mode(&tbe->orig_termios);
        return false;
    }
    tbe->last_cells_size = cell_count;

    clear_screen();
    enter_alt_screen();
    enable_mouse();
    hide_cursor();

    be->supports_mouse = true;
    be->supports_color = true;
    be->supports_unicode = true;
    be->max_colors = 256 * 256 * 256;

    return true;
}

static void term_shutdown(ui_backend_t *be) {
    term_backend_t *tbe = (term_backend_t *)be;
    if (!tbe) return;

    disable_mouse();
    exit_alt_screen();
    show_cursor();
    reset_color();

    if (tbe->raw_mode) {
        term_restore_mode(&tbe->orig_termios);
        tbe->raw_mode = false;
    }

    if (tbe->orig_flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, tbe->orig_flags);
    }

    term_emergency_ready = 0;
    (void)signal(SIGWINCH, SIG_DFL);
    (void)signal(SIGSEGV, SIG_DFL);
    (void)signal(SIGABRT, SIG_DFL);
    (void)signal(SIGFPE, SIG_DFL);
    (void)signal(SIGILL, SIG_DFL);
    (void)signal(SIGBUS, SIG_DFL);

    free(tbe->last_cells);
    ui_canvas_free(be->canvas);
    be->canvas = NULL;
    free(tbe);
}

/* -------------------------------------------------------------------------- */
/* Constructor                                                                */
/* -------------------------------------------------------------------------- */

ui_backend_t *ui_backend_term_new(void) {
    detect_mono_mode();
    term_backend_t *tbe = calloc(1, sizeof(term_backend_t));
    if (!tbe) return NULL;
    tbe->base.name = "term";
    tbe->base.init = term_init;
    tbe->base.shutdown = term_shutdown;
    tbe->base.poll_event = term_poll_event;
    tbe->base.present = term_present;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
    return &tbe->base;
#pragma GCC diagnostic pop
}
