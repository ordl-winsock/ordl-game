/*
 * src/platform/platform_win32.c — Windows platform layer
 *
 * Pure Win32 API, zero external dependencies.
 * No windows.h included — minimal types defined inline for self-containment.
 */

#ifdef _WIN32

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include "forge/platform.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Minimal Win32 types (avoid pulling in all of windows.h)                    */
/* -------------------------------------------------------------------------- */

typedef unsigned long DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef HANDLE HWND;
typedef HANDLE HINSTANCE;
typedef HANDLE HDC;
typedef HANDLE HBITMAP;
typedef HANDLE HICON;
typedef HANDLE HCURSOR;
typedef HANDLE HBRUSH;
typedef HANDLE HMENU;
typedef HANDLE HMODULE;
typedef long LONG;
typedef LONG LPARAM;
typedef unsigned long long UINT_PTR;
typedef UINT_PTR WPARAM;
typedef unsigned short WORD;
typedef WORD ATOM;

#ifndef CALLBACK
#define CALLBACK __stdcall
#endif
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

/* Messages */
#define WM_DESTROY          0x0002
#define WM_SIZE             0x0005
#define WM_SETFOCUS         0x0007
#define WM_KILLFOCUS        0x0008
#define WM_CLOSE            0x0010
#define WM_ERASEBKGND       0x0014
#define WM_KEYDOWN          0x0100
#define WM_KEYUP            0x0101
#define WM_CHAR             0x0102
#define WM_SYSKEYDOWN       0x0104
#define WM_SYSKEYUP         0x0105
#define WM_MOUSEMOVE        0x0200
#define WM_LBUTTONDOWN      0x0201
#define WM_LBUTTONUP        0x0202
#define WM_RBUTTONDOWN      0x0204
#define WM_RBUTTONUP        0x0205
#define WM_MBUTTONDOWN      0x0207
#define WM_MBUTTONUP        0x0208
#define WM_MOUSEWHEEL       0x020A
#define WM_XBUTTONDOWN      0x020B
#define WM_XBUTTONUP        0x020C

/* Window styles */
#define WS_OVERLAPPED       0x00000000L
#define WS_CAPTION          0x00C00000L
#define WS_SYSMENU          0x00080000L
#define WS_THICKFRAME       0x00040000L
#define WS_MINIMIZEBOX      0x00020000L
#define WS_MAXIMIZEBOX      0x00010000L
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_VISIBLE          0x10000000L
#define CW_USEDEFAULT       ((int)0x80000000)

/* Class styles */
#define CS_HREDRAW          0x0002
#define CS_VREDRAW          0x0001
#define CS_OWNDC            0x0020

/* Virtual-key codes */
#define VK_BACK             0x08
#define VK_TAB              0x09
#define VK_RETURN           0x0D
#define VK_SHIFT            0x10
#define VK_CONTROL          0x11
#define VK_MENU             0x12
#define VK_PAUSE            0x13
#define VK_CAPITAL          0x14
#define VK_ESCAPE           0x1B
#define VK_SPACE            0x20
#define VK_PRIOR            0x21
#define VK_NEXT             0x22
#define VK_END              0x23
#define VK_HOME             0x24
#define VK_LEFT             0x25
#define VK_UP               0x26
#define VK_RIGHT            0x27
#define VK_DOWN             0x28
#define VK_INSERT           0x2D
#define VK_DELETE           0x2E
#define VK_LWIN             0x5B
#define VK_RWIN             0x5C
#define VK_NUMPAD0          0x60
#define VK_NUMPAD9          0x69
#define VK_MULTIPLY         0x6A
#define VK_ADD              0x6B
#define VK_SUBTRACT         0x6D
#define VK_DECIMAL          0x6E
#define VK_DIVIDE           0x6F
#define VK_F1               0x70
#define VK_F12              0x7B
#define VK_LSHIFT           0xA0
#define VK_RSHIFT           0xA1
#define VK_LCONTROL         0xA2
#define VK_RCONTROL         0xA3
#define VK_LMENU            0xA4
#define VK_RMENU            0xA5
#define VK_XBUTTON1         0x05
#define VK_XBUTTON2         0x06

#define WHEEL_DELTA         120
#define GET_WHEEL_DELTA_WPARAM(w) ((short)HIWORD(w))
#define GET_X_LPARAM(lp)        ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp)        ((int)(short)HIWORD(lp))

#ifndef LOWORD
#define LOWORD(l) ((WORD)(DWORD)(l))
#endif
#ifndef HIWORD
#define HIWORD(l) ((WORD)(((DWORD)(l) >> 16) & 0xFFFF))
#endif

/* Structs */
typedef struct {
    UINT_PTR style;
    void (CALLBACK *lpfnWndProc)(HWND, unsigned int, WPARAM, LPARAM);
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    const char *lpszMenuName;
    const char *lpszClassName;
    HICON hIconSm;
} WNDCLASSEXA;

typedef struct {
    LONG biSize;
    LONG biWidth;
    LONG biHeight;
    WORD biPlanes;
    WORD biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG biXPelsPerMeter;
    LONG biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BITMAPINFOHEADER;

typedef struct {
    BITMAPINFOHEADER bmiHeader;
    DWORD bmiColors[3];
} BITMAPINFO;

typedef struct {
    LONG left, top, right, bottom;
} RECT;

typedef struct {
    HWND hwnd;
    unsigned int message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
} MSG;

/* DIB color table constants */
#define BI_RGB 0

/* Function prototypes */
extern HINSTANCE WINAPI GetModuleHandleA(const char *);
extern ATOM WINAPI RegisterClassExA(const WNDCLASSEXA *);
extern HWND WINAPI CreateWindowExA(DWORD, const char *, const char *, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, void *);
extern BOOL WINAPI ShowWindow(HWND, int);
extern BOOL WINAPI UpdateWindow(HWND);
extern BOOL WINAPI PeekMessageA(MSG *, HWND, unsigned int, unsigned int, unsigned int);
extern BOOL WINAPI TranslateMessage(const MSG *);
extern LONG_PTR WINAPI DispatchMessageA(const MSG *);
extern BOOL WINAPI DestroyWindow(HWND);
extern HDC WINAPI GetDC(HWND);
extern int WINAPI ReleaseDC(HWND, HDC);
extern void WINAPI PostQuitMessage(int);
extern LONG_PTR WINAPI SetWindowLongPtrA(HWND, int, LONG_PTR);
extern LONG_PTR WINAPI GetWindowLongPtrA(HWND, int);
extern BOOL WINAPI GetClientRect(HWND, RECT *);
extern HCURSOR WINAPI LoadCursorA(HINSTANCE, const char *);
extern HICON WINAPI LoadIconA(HINSTANCE, const char *);
extern BOOL WINAPI SetCursorPos(int, int);
extern BOOL WINAPI ShowCursor(BOOL);
extern int WINAPI StretchDIBits(HDC, int, int, int, int, int, int, int, int, const void *, const BITMAPINFO *, unsigned int, unsigned long);
extern void WINAPI Sleep(DWORD);

#define GWLP_USERDATA   -21
#define SW_SHOW         5
#define PM_REMOVE       0x0001
#define IDC_ARROW       ((const char *)32512)
#define IDI_APPLICATION ((const char *)32512)
#define DIB_RGB_COLORS  0
#define SRCCOPY         0x00CC0020

/* -------------------------------------------------------------------------- */
/* Win32 backend state                                                        */
/* -------------------------------------------------------------------------- */

typedef struct {
    HWND hwnd;
    HDC hdc;
    int width, height;
    bool fullscreen;
    bool focused;
    bool cursor_visible;
    bool running;
    bool should_close;
    int mouse_x, mouse_y;
    int mouse_wheel_accum;
    fge_input_state_t *input;
} win32_backend_t;

static win32_backend_t *g_win32 = NULL;

/* -------------------------------------------------------------------------- */
/* VK -> fge_key_t mapping                                                    */
/* -------------------------------------------------------------------------- */

static fge_key_t vk_to_fge(int vk) {
    switch (vk) {
    case 'A': case 'a': return FGE_KEY_A;
    case 'B': case 'b': return FGE_KEY_B;
    case 'C': case 'c': return FGE_KEY_C;
    case 'D': case 'd': return FGE_KEY_D;
    case 'E': case 'e': return FGE_KEY_E;
    case 'F': case 'f': return FGE_KEY_F;
    case 'G': case 'g': return FGE_KEY_G;
    case 'H': case 'h': return FGE_KEY_H;
    case 'I': case 'i': return FGE_KEY_I;
    case 'J': case 'j': return FGE_KEY_J;
    case 'K': case 'k': return FGE_KEY_K;
    case 'L': case 'l': return FGE_KEY_L;
    case 'M': case 'm': return FGE_KEY_M;
    case 'N': case 'n': return FGE_KEY_N;
    case 'O': case 'o': return FGE_KEY_O;
    case 'P': case 'p': return FGE_KEY_P;
    case 'Q': case 'q': return FGE_KEY_Q;
    case 'R': case 'r': return FGE_KEY_R;
    case 'S': case 's': return FGE_KEY_S;
    case 'T': case 't': return FGE_KEY_T;
    case 'U': case 'u': return FGE_KEY_U;
    case 'V': case 'v': return FGE_KEY_V;
    case 'W': case 'w': return FGE_KEY_W;
    case 'X': case 'x': return FGE_KEY_X;
    case 'Y': case 'y': return FGE_KEY_Y;
    case 'Z': case 'z': return FGE_KEY_Z;
    case '0': return FGE_KEY_0;
    case '1': return FGE_KEY_1;
    case '2': return FGE_KEY_2;
    case '3': return FGE_KEY_3;
    case '4': return FGE_KEY_4;
    case '5': return FGE_KEY_5;
    case '6': return FGE_KEY_6;
    case '7': return FGE_KEY_7;
    case '8': return FGE_KEY_8;
    case '9': return FGE_KEY_9;
    case VK_SPACE:     return FGE_KEY_SPACE;
    case VK_RETURN:    return FGE_KEY_ENTER;
    case VK_TAB:       return FGE_KEY_TAB;
    case VK_BACK:      return FGE_KEY_BACKSPACE;
    case VK_ESCAPE:    return FGE_KEY_ESCAPE;
    case VK_DELETE:    return FGE_KEY_DELETE;
    case VK_UP:        return FGE_KEY_UP;
    case VK_DOWN:      return FGE_KEY_DOWN;
    case VK_LEFT:      return FGE_KEY_LEFT;
    case VK_RIGHT:     return FGE_KEY_RIGHT;
    case VK_HOME:      return FGE_KEY_HOME;
    case VK_END:       return FGE_KEY_END;
    case VK_PRIOR:     return FGE_KEY_PAGE_UP;
    case VK_NEXT:      return FGE_KEY_PAGE_DOWN;
    case VK_INSERT:    return FGE_KEY_INSERT;
    case VK_F1:        return FGE_KEY_F1;
    case VK_F2:        return FGE_KEY_F2;
    case VK_F3:        return FGE_KEY_F3;
    case VK_F4:        return FGE_KEY_F4;
    case VK_F5:        return FGE_KEY_F5;
    case VK_F6:        return FGE_KEY_F6;
    case VK_F7:        return FGE_KEY_F7;
    case VK_F8:        return FGE_KEY_F8;
    case VK_F9:        return FGE_KEY_F9;
    case VK_F10:       return FGE_KEY_F10;
    case VK_F11:       return FGE_KEY_F11;
    case VK_F12:       return FGE_KEY_F12;
    case VK_LSHIFT:    return FGE_KEY_LSHIFT;
    case VK_RSHIFT:    return FGE_KEY_RSHIFT;
    case VK_LCONTROL:  return FGE_KEY_LCTRL;
    case VK_RCONTROL:  return FGE_KEY_RCTRL;
    case VK_LMENU:     return FGE_KEY_LALT;
    case VK_RMENU:     return FGE_KEY_RALT;
    case VK_LWIN:      return FGE_KEY_LMETA;
    case VK_RWIN:      return FGE_KEY_RMETA;
    default:           return FGE_KEY_NONE;
    }
}

static fge_mouse_button_t win32_button_to_fge(int btn) {
    switch (btn) {
    case 0: return FGE_MOUSE_LEFT;
    case 1: return FGE_MOUSE_RIGHT;
    case 2: return FGE_MOUSE_MIDDLE;
    case 3: return FGE_MOUSE_X1;
    case 4: return FGE_MOUSE_X2;
    default: return FGE_MOUSE_NONE;
    }
}

/* -------------------------------------------------------------------------- */
/* Window procedure                                                           */
/* -------------------------------------------------------------------------- */

static void CALLBACK win32_wndproc(HWND hwnd, unsigned int msg, WPARAM wparam, LPARAM lparam) {
    win32_backend_t *w = g_win32;
    if (!w) {
        /* Fallback for messages before backend is set up */
        goto def_proc;
    }

    switch (msg) {
    case WM_SIZE: {
        RECT rc;
        if (GetClientRect(hwnd, &rc)) {
            w->width = (int)(rc.right - rc.left);
            w->height = (int)(rc.bottom - rc.top);
        }
        break;
    }
    case WM_SETFOCUS:
        w->focused = true;
        break;
    case WM_KILLFOCUS:
        w->focused = false;
        break;
    case WM_CLOSE:
        w->should_close = true;
        w->running = false;
        break;
    case WM_DESTROY:
        w->should_close = true;
        w->running = false;
        PostQuitMessage(0);
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int vk = (int)wparam;
        fge_key_t key = vk_to_fge(vk);
        if (key != FGE_KEY_NONE && (size_t)key < FGE_KEY_COUNT) {
            w->input->keys[key] = true;
        }
        break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int vk = (int)wparam;
        fge_key_t key = vk_to_fge(vk);
        if (key != FGE_KEY_NONE && (size_t)key < FGE_KEY_COUNT) {
            w->input->keys[key] = false;
        }
        break;
    }
    case WM_CHAR: {
        unsigned int c = (unsigned int)wparam;
        if (c < 32 || c == 127) break;
        if (w->input->text_input_len < (int)sizeof(w->input->text_input) - 4) {
            if (c < 0x80) {
                w->input->text_input[w->input->text_input_len++] = (char)c;
            } else if (c < 0x800) {
                w->input->text_input[w->input->text_input_len++] = (char)(0xC0 | (c >> 6));
                w->input->text_input[w->input->text_input_len++] = (char)(0x80 | (c & 0x3F));
            } else {
                w->input->text_input[w->input->text_input_len++] = (char)(0xE0 | (c >> 12));
                w->input->text_input[w->input->text_input_len++] = (char)(0x80 | ((c >> 6) & 0x3F));
                w->input->text_input[w->input->text_input_len++] = (char)(0x80 | (c & 0x3F));
            }
        }
        break;
    }
    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(lparam);
        int my = GET_Y_LPARAM(lparam);
        w->input->mouse_delta.x = (float)(mx - w->mouse_x);
        w->input->mouse_delta.y = (float)(my - w->mouse_y);
        w->input->mouse_pos.x = (float)mx;
        w->input->mouse_pos.y = (float)my;
        w->mouse_x = mx;
        w->mouse_y = my;
        break;
    }
    case WM_LBUTTONDOWN:
        w->input->mouse_buttons[FGE_MOUSE_LEFT] = true;
        break;
    case WM_LBUTTONUP:
        w->input->mouse_buttons[FGE_MOUSE_LEFT] = false;
        break;
    case WM_RBUTTONDOWN:
        w->input->mouse_buttons[FGE_MOUSE_RIGHT] = true;
        break;
    case WM_RBUTTONUP:
        w->input->mouse_buttons[FGE_MOUSE_RIGHT] = false;
        break;
    case WM_MBUTTONDOWN:
        w->input->mouse_buttons[FGE_MOUSE_MIDDLE] = true;
        break;
    case WM_MBUTTONUP:
        w->input->mouse_buttons[FGE_MOUSE_MIDDLE] = false;
        break;
    case WM_XBUTTONDOWN: {
        int btn = ((int)HIWORD(wparam) == 1) ? 3 : 4;
        w->input->mouse_buttons[btn] = true;
        break;
    }
    case WM_XBUTTONUP: {
        int btn = ((int)HIWORD(wparam) == 1) ? 3 : 4;
        w->input->mouse_buttons[btn] = false;
        break;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        w->mouse_wheel_accum += delta;
        while (w->mouse_wheel_accum >= WHEEL_DELTA) {
            w->input->mouse_scroll.y += 1.0f;
            w->mouse_wheel_accum -= WHEEL_DELTA;
        }
        while (w->mouse_wheel_accum <= -WHEEL_DELTA) {
            w->input->mouse_scroll.y -= 1.0f;
            w->mouse_wheel_accum += WHEEL_DELTA;
        }
        break;
    }
    default:
def_proc:
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* Event translation for platform_poll_event                                  */
/* -------------------------------------------------------------------------- */

static bool win32_translate_event(win32_backend_t *w, MSG *m, fge_event_t *out) {
    (void)w;
    memset(out, 0, sizeof(*out));
    switch (m->message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        out->type = FGE_EVENT_KEY_DOWN;
        out->key.key = vk_to_fge((int)m->wParam);
        out->key.repeat = ((int)m->lParam & (1 << 30)) != 0;
        return out->key.key != FGE_KEY_NONE;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        out->type = FGE_EVENT_KEY_UP;
        out->key.key = vk_to_fge((int)m->wParam);
        out->key.repeat = false;
        return out->key.key != FGE_KEY_NONE;
    }
    case WM_CHAR: {
        unsigned int c = (unsigned int)m->wParam;
        if (c < 32 || c == 127) return false;
        out->type = FGE_EVENT_TEXT_INPUT;
        int len = 0;
        if (c < 0x80) {
            out->text.text[len++] = (char)c;
        } else if (c < 0x800) {
            out->text.text[len++] = (char)(0xC0 | (c >> 6));
            out->text.text[len++] = (char)(0x80 | (c & 0x3F));
        } else {
            out->text.text[len++] = (char)(0xE0 | (c >> 12));
            out->text.text[len++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out->text.text[len++] = (char)(0x80 | (c & 0x3F));
        }
        out->text.text[len] = '\0';
        out->text.len = len;
        return true;
    }
    case WM_MOUSEMOVE: {
        out->type = FGE_EVENT_MOUSE_MOVE;
        int mx = GET_X_LPARAM(m->lParam);
        int my = GET_Y_LPARAM(m->lParam);
        out->mouse_move.pos.x = (float)mx;
        out->mouse_move.pos.y = (float)my;
        out->mouse_move.delta.x = (float)(mx - w->mouse_x);
        out->mouse_move.delta.y = (float)(my - w->mouse_y);
        w->mouse_x = mx;
        w->mouse_y = my;
        return true;
    }
    case WM_LBUTTONDOWN:
        out->type = FGE_EVENT_MOUSE_DOWN;
        out->mouse_button.button = FGE_MOUSE_LEFT;
        out->mouse_button.pos = w->input->mouse_pos;
        return true;
    case WM_LBUTTONUP:
        out->type = FGE_EVENT_MOUSE_UP;
        out->mouse_button.button = FGE_MOUSE_LEFT;
        out->mouse_button.pos = w->input->mouse_pos;
        return true;
    case WM_RBUTTONDOWN:
        out->type = FGE_EVENT_MOUSE_DOWN;
        out->mouse_button.button = FGE_MOUSE_RIGHT;
        out->mouse_button.pos = w->input->mouse_pos;
        return true;
    case WM_RBUTTONUP:
        out->type = FGE_EVENT_MOUSE_UP;
        out->mouse_button.button = FGE_MOUSE_RIGHT;
        out->mouse_button.pos = w->input->mouse_pos;
        return true;
    case WM_MBUTTONDOWN:
        out->type = FGE_EVENT_MOUSE_DOWN;
        out->mouse_button.button = FGE_MOUSE_MIDDLE;
        out->mouse_button.pos = w->input->mouse_pos;
        return true;
    case WM_MBUTTONUP:
        out->type = FGE_EVENT_MOUSE_UP;
        out->mouse_button.button = FGE_MOUSE_MIDDLE;
        out->mouse_button.pos = w->input->mouse_pos;
        return true;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(m->wParam);
        out->type = FGE_EVENT_MOUSE_SCROLL;
        out->mouse_scroll.scroll.x = 0.0f;
        out->mouse_scroll.scroll.y = (float)delta / (float)WHEEL_DELTA;
        return true;
    }
    case WM_SIZE: {
        out->type = FGE_EVENT_RESIZE;
        out->resize.width = LOWORD(m->lParam);
        out->resize.height = HIWORD(m->lParam);
        return true;
    }
    case WM_CLOSE:
        out->type = FGE_EVENT_CLOSE;
        return true;
    default:
        return false;
    }
}

/* -------------------------------------------------------------------------- */
/* vtable implementations                                                     */
/* -------------------------------------------------------------------------- */

static bool win32_init(fge_platform_t *p, const char *title, int w, int h, bool fullscreen) {
    (void)fullscreen;
    win32_backend_t *wb = FGE_CALLOC(1, sizeof(win32_backend_t));
    if (!wb) return false;

    g_win32 = wb;
    wb->width = w;
    wb->height = h;
    wb->cursor_visible = true;
    wb->running = true;
    wb->input = &p->input;

    HINSTANCE hinst = GetModuleHandleA(NULL);

    WNDCLASSEXA wc = {0};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = win32_wndproc;
    wc.hInstance = hinst;
    wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = "FORGEWindow";
    wc.hIconSm = LoadIconA(NULL, IDI_APPLICATION);

    if (!RegisterClassExA(&wc)) {
        FGE_FREE(wb);
        g_win32 = NULL;
        return false;
    }

    RECT rc = {0, 0, w, h};
    DWORD style = WS_OVERLAPPEDWINDOW;
    /* Adjust rect so client area is exactly w x h */
    /* We'd need AdjustWindowRect here but it requires user32.dll import */
    /* For simplicity, create window with style size and let it be slightly larger */
    (void)rc; (void)style;

    HWND hwnd = CreateWindowExA(
        0, "FORGEWindow", title ? title : "FORGE",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, w, h,
        NULL, NULL, hinst, NULL
    );
    if (!hwnd) {
        FGE_FREE(wb);
        g_win32 = NULL;
        return false;
    }

    wb->hwnd = hwnd;
    wb->hdc = GetDC(hwnd);

    /* Get actual client size */
    RECT client;
    if (GetClientRect(hwnd, &client)) {
        wb->width = (int)(client.right - client.left);
        wb->height = (int)(client.bottom - client.top);
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    p->native_window = hwnd;
    p->native_display = wb->hdc;
    p->width = wb->width;
    p->height = wb->height;
    p->running = true;
    p->focused = true;

    FGE_INFO(FGE_LOG_CAT_PLATFORM, "Win32 platform initialized: %dx%d", wb->width, wb->height);
    return true;
}

static void win32_shutdown(fge_platform_t *p) {
    if (!p) return;
    win32_backend_t *wb = g_win32;
    if (!wb) return;
    if (wb->hdc && wb->hwnd) ReleaseDC(wb->hwnd, wb->hdc);
    if (wb->hwnd) DestroyWindow(wb->hwnd);
    FGE_FREE(wb);
    g_win32 = NULL;
    p->native_window = NULL;
    p->native_display = NULL;
}

static bool win32_poll_event(fge_platform_t *p, fge_event_t *out) {
    (void)p;
    win32_backend_t *wb = g_win32;
    if (!wb) return false;

    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
        if (win32_translate_event(wb, &m, out)) {
            return true;
        }
    }
    return false;
}

static void win32_swap_buffers(fge_platform_t *p) {
    (void)p;
    /* Win32 presentation is handled by the renderer or the framebuffer blit.
     * If a framebuffer is attached to user_data, we could blit it here.
     * For now, this is a no-op — the renderer handles its own presentation.
     */
}

static void win32_set_title(fge_platform_t *p, const char *title) {
    win32_backend_t *wb = g_win32;
    if (!wb || !wb->hwnd || !title) return;
    /* SetWindowTextA not defined above; use SendMessage with WM_SETTEXT */
    /* For simplicity, this is a no-op in the minimal build */
    (void)title;
}

static void win32_set_vsync(fge_platform_t *p, bool enabled) {
    (void)p; (void)enabled;
    /* VSync on Win32 requires DWM or OpenGL/D3D context */
}

static void win32_show_cursor(fge_platform_t *p, bool show) {
    (void)p;
    win32_backend_t *wb = g_win32;
    if (!wb) return;
    if (wb->cursor_visible == show) return;
    wb->cursor_visible = show;
    ShowCursor(show ? TRUE : FALSE);
}

static void win32_grab_input(fge_platform_t *p, bool grab) {
    (void)p;
    win32_backend_t *wb = g_win32;
    if (!wb || !wb->hwnd) return;
    if (grab) {
        RECT rc;
        GetClientRect(wb->hwnd, &rc);
        /* ClipCursor not defined above; stub for now */
        (void)rc;
    }
}

static const char *win32_get_clipboard(fge_platform_t *p) {
    (void)p;
    return NULL; /* TODO: OpenClipboard/GetClipboardData */
}

static void win32_set_clipboard(fge_platform_t *p, const char *text) {
    (void)p; (void)text;
    /* TODO: OpenClipboard/SetClipboardData */
}

/* -------------------------------------------------------------------------- */
/* Platform API                                                               */
/* -------------------------------------------------------------------------- */

fge_platform_t *fge_platform_create(const char *title, int w, int h, bool fullscreen) {
    fge_platform_t *p = FGE_CALLOC(1, sizeof(fge_platform_t));
    if (!p) return NULL;

    p->events = FGE_CALLOC(256, sizeof(fge_event_t));
    p->event_capacity = 256;

    p->init = win32_init;
    p->shutdown = win32_shutdown;
    p->poll_event = win32_poll_event;
    p->swap_buffers = win32_swap_buffers;
    p->set_title = win32_set_title;
    p->set_vsync = win32_set_vsync;
    p->show_cursor = win32_show_cursor;
    p->grab_input = win32_grab_input;
    p->get_clipboard = win32_get_clipboard;
    p->set_clipboard = win32_set_clipboard;

    if (p->init(p, title, w, h, fullscreen)) return p;

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
    if (next == tail) return;
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

        double target_dt = 1.0 / 60.0;
        if (dt < target_dt) {
            Sleep((DWORD)((target_dt - dt) * 1000.0));
        }
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

#endif /* _WIN32 */
