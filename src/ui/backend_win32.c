/*
 * ORDL UI — Windows Win32 backend
 * Pure C23, zero external dependencies (uses Win32 API natively on Windows).
 *
 * Creates a standard Win32 window and presents the RGBA8888 canvas
 * via GDI SetDIBitsToDevice.  Input handled through standard Win32
 * message loop (keyboard, mouse, resize).
 *
 * On non-Windows platforms, returns NULL.
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Windows-specific includes and types                                        */
/* -------------------------------------------------------------------------- */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

/* Define our own constants to avoid dependency on outdated windows headers */
#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A
#endif

/* Virtual key mapping to ui_key_t */
static ui_key_t vk_to_ui_key(int vk) {
    switch (vk) {
    case VK_RETURN:    return UI_KEY_ENTER;
    case VK_TAB:       return UI_KEY_TAB;
    case VK_BACK:      return UI_KEY_BACKSPACE;
    case VK_DELETE:    return UI_KEY_DELETE;
    case VK_ESCAPE:    return UI_KEY_ESCAPE;
    case VK_SPACE:     return UI_KEY_SPACE;
    case VK_UP:        return UI_KEY_UP;
    case VK_DOWN:      return UI_KEY_DOWN;
    case VK_LEFT:      return UI_KEY_LEFT;
    case VK_RIGHT:     return UI_KEY_RIGHT;
    case VK_HOME:      return UI_KEY_HOME;
    case VK_END:       return UI_KEY_END;
    case VK_PRIOR:     return UI_KEY_PAGE_UP;
    case VK_NEXT:      return UI_KEY_PAGE_DOWN;
    case VK_INSERT:    return UI_KEY_INSERT;
    case VK_F1:        return UI_KEY_F1;
    case VK_F2:        return UI_KEY_F2;
    case VK_F3:        return UI_KEY_F3;
    case VK_F4:        return UI_KEY_F4;
    case VK_F5:        return UI_KEY_F5;
    case VK_F6:        return UI_KEY_F6;
    case VK_F7:        return UI_KEY_F7;
    case VK_F8:        return UI_KEY_F8;
    case VK_F9:        return UI_KEY_F9;
    case VK_F10:       return UI_KEY_F10;
    case VK_F11:       return UI_KEY_F11;
    case VK_F12:       return UI_KEY_F12;
    default:           return UI_KEY_NONE;
    }
}

/* -------------------------------------------------------------------------- */
/* Win32 state                                                                */
/* -------------------------------------------------------------------------- */

typedef struct {
    HWND hwnd;
    HDC hdc;
    HBITMAP dib;
    uint8_t *pixels;
    int width, height;
    int mouse_x, mouse_y;
    bool mouse_btn[4];
    bool closed;
    bool resized;
    int new_w, new_h;
    /* Input queue (simple ring buffer) */
    ui_event_t events[64];
    int event_head, event_tail;
} win32_state_t;

static const wchar_t *WIN_CLASS_NAME = L"ORDL_UI_Window";
static win32_state_t *g_win32 = NULL; /* Single window for now */

/* -------------------------------------------------------------------------- */
/* Window procedure                                                           */
/* -------------------------------------------------------------------------- */

static LRESULT CALLBACK win32_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    win32_state_t *st = g_win32;
    if (!st) return DefWindowProcW(hwnd, msg, wparam, lparam);

    switch (msg) {
    case WM_CLOSE:
        st->closed = true;
        PostQuitMessage(0);
        return 0;
    case WM_SIZE: {
        int nw = LOWORD(lparam);
        int nh = HIWORD(lparam);
        if (nw > 0 && nh > 0) {
            st->resized = true;
            st->new_w = nw;
            st->new_h = nh;
        }
        return 0;
    }
    case WM_KEYDOWN: {
        int tail = (st->event_tail + 1) & 63;
        if (tail != st->event_head) {
            ui_event_t *ev = &st->events[st->event_tail];
            memset(ev, 0, sizeof(*ev));
            ev->type = UI_EVENT_KEY;
            ev->key.key = vk_to_ui_key((int)wparam);
            ev->key.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            ev->key.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            ev->key.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            st->event_tail = tail;
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
        int btn = 0;
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) btn = 1;
        else if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) btn = 2;
        else if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) btn = 3;
        bool pressed = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
        st->mouse_btn[btn] = pressed;
        int tail = (st->event_tail + 1) & 63;
        if (tail != st->event_head) {
            ui_event_t *ev = &st->events[st->event_tail];
            memset(ev, 0, sizeof(*ev));
            ev->type = pressed ? UI_EVENT_MOUSE_PRESS : UI_EVENT_MOUSE_RELEASE;
            ev->mouse.x = (int)(short)LOWORD(lparam);
            ev->mouse.y = (int)(short)HIWORD(lparam);
            ev->mouse.button = btn;
            st->event_tail = tail;
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        st->mouse_x = (int)(short)LOWORD(lparam);
        st->mouse_y = (int)(short)HIWORD(lparam);
        int tail = (st->event_tail + 1) & 63;
        if (tail != st->event_head) {
            ui_event_t *ev = &st->events[st->event_tail];
            memset(ev, 0, sizeof(*ev));
            ev->type = UI_EVENT_MOUSE_MOVE;
            ev->mouse.x = st->mouse_x;
            ev->mouse.y = st->mouse_y;
            st->event_tail = tail;
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = (int)(short)HIWORD(wparam);
        int tail = (st->event_tail + 1) & 63;
        if (tail != st->event_head) {
            ui_event_t *ev = &st->events[st->event_tail];
            memset(ev, 0, sizeof(*ev));
            ev->type = UI_EVENT_MOUSE_SCROLL;
            ev->mouse.x = st->mouse_x;
            ev->mouse.y = st->mouse_y;
            ev->mouse.scroll_dy = -delta / WHEEL_DELTA;
            st->event_tail = tail;
        }
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* -------------------------------------------------------------------------- */
/* DIB management                                                             */
/* -------------------------------------------------------------------------- */

static bool win32_create_dib(win32_state_t *st, int w, int h) {
    if (st->dib) {
        DeleteObject(st->dib);
        st->dib = NULL;
        st->pixels = NULL;
    }
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; /* Top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    st->dib = CreateDIBSection(st->hdc, &bmi, DIB_RGB_COLORS, (void **)&st->pixels, NULL, 0);
    if (!st->dib) return false;
    st->width = w;
    st->height = h;
    return true;
}

/* -------------------------------------------------------------------------- */
/* Backend vtable                                                             */
/* -------------------------------------------------------------------------- */

static bool be_win32_init(ui_backend_t *be, int w, int h) {
    win32_state_t *st = calloc(1, sizeof(win32_state_t));
    if (!st) return false;
    g_win32 = st;

    HINSTANCE hinst = GetModuleHandleW(NULL);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = win32_wndproc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = WIN_CLASS_NAME;
    if (!RegisterClassExW(&wc)) {
        free(st);
        g_win32 = NULL;
        return false;
    }

    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rc = {0, 0, w, h};
    AdjustWindowRect(&rc, style, FALSE);
    int ww = rc.right - rc.left;
    int wh = rc.bottom - rc.top;

    st->hwnd = CreateWindowExW(0, WIN_CLASS_NAME, L"ORDL UI", style,
                               CW_USEDEFAULT, CW_USEDEFAULT, ww, wh,
                               NULL, NULL, hinst, NULL);
    if (!st->hwnd) {
        free(st);
        g_win32 = NULL;
        return false;
    }

    st->hdc = GetDC(st->hwnd);
    if (!win32_create_dib(st, w, h)) {
        DestroyWindow(st->hwnd);
        free(st);
        g_win32 = NULL;
        return false;
    }

    ShowWindow(st->hwnd, SW_SHOW);
    UpdateWindow(st->hwnd);

    be->canvas = ui_canvas_new_fb(w, h);
    if (!be->canvas) {
        DestroyWindow(st->hwnd);
        free(st);
        g_win32 = NULL;
        return false;
    }

    be->user_data = st;
    be->supports_mouse = true;
    be->supports_color = true;
    be->supports_unicode = true;
    be->max_colors = 0xFFFFFF;
    return true;
}

static void be_win32_shutdown(ui_backend_t *be) {
    if (!be) return;
    win32_state_t *st = (win32_state_t *)be->user_data;
    if (st) {
        if (st->dib) DeleteObject(st->dib);
        if (st->hwnd) {
            ReleaseDC(st->hwnd, st->hdc);
            DestroyWindow(st->hwnd);
        }
        free(st);
        g_win32 = NULL;
    }
    if (be->canvas) {
        ui_canvas_free(be->canvas);
        be->canvas = NULL;
    }
}

static void be_win32_present(ui_backend_t *be) {
    if (!be || !be->canvas || be->canvas->type != UI_CANVAS_FB) return;
    win32_state_t *st = (win32_state_t *)be->user_data;
    if (!st || !st->pixels || !st->hwnd) return;

    /* Copy RGBA canvas → BGRA DIB (swizzle R and B) */
    const uint32_t *src = be->canvas->pixels;
    uint8_t *dst = st->pixels;
    int count = st->width * st->height;
    for (int i = 0; i < count; i++) {
        uint32_t rgba = src[i];
        dst[i * 4 + 0] = (rgba >> 16) & 0xFF; /* B */
        dst[i * 4 + 1] = (rgba >> 8)  & 0xFF; /* G */
        dst[i * 4 + 2] = (rgba >> 0)  & 0xFF; /* R */
        dst[i * 4 + 3] = (rgba >> 24) & 0xFF; /* A */
    }

    HDC memdc = CreateCompatibleDC(st->hdc);
    HGDIOBJ old = SelectObject(memdc, st->dib);
    BitBlt(st->hdc, 0, 0, st->width, st->height, memdc, 0, 0, SRCCOPY);
    SelectObject(memdc, old);
    DeleteDC(memdc);
}

static bool be_win32_poll_event(ui_backend_t *be, ui_event_t *out, int timeout_ms) {
    if (!be || !be->user_data) return false;
    win32_state_t *st = (win32_state_t *)be->user_data;

    /* Drain any pending Win32 messages */
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* Check for resize */
    if (st->resized) {
        st->resized = false;
        if (st->new_w != st->width || st->new_h != st->height) {
            win32_create_dib(st, st->new_w, st->new_h);
            if (be->canvas) {
                ui_canvas_free(be->canvas);
                be->canvas = ui_canvas_new_fb(st->new_w, st->new_h);
            }
            memset(out, 0, sizeof(*out));
            out->type = UI_EVENT_RESIZE;
            out->resize.w = st->new_w;
            out->resize.h = st->new_h;
            return true;
        }
    }

    /* Return queued events */
    if (st->event_head != st->event_tail) {
        *out = st->events[st->event_head];
        st->event_head = (st->event_head + 1) & 63;
        return true;
    }

    if (st->closed) {
        memset(out, 0, sizeof(*out));
        out->type = UI_EVENT_QUIT;
        return true;
    }

    /* No events: optionally sleep */
    if (timeout_ms > 0) {
        MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)timeout_ms, QS_ALLEVENTS);
    }
    return false;
}

ui_backend_t *ui_backend_win32_new(void) {
    ui_backend_t *be = calloc(1, sizeof(ui_backend_t));
    if (!be) return NULL;
    be->name = "win32";
    be->init = be_win32_init;
    be->shutdown = be_win32_shutdown;
    be->poll_event = be_win32_poll_event;
    be->present = be_win32_present;
    return be;
}

#else /* !_WIN32 */

ui_backend_t *ui_backend_win32_new(void) {
    return NULL;
}

#endif
