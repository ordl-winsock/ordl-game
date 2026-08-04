/*
 * ORDL UI — Cross-platform clipboard
 * Pure C23, zero external dependencies.
 *
 * Linux: xclip / xsel via popen, or raw X11 selection (via backend_x11.c)
 * Windows: OpenClipboard / SetClipboardData / GetClipboardData
 * macOS: pbcopy / pbpaste via popen (stub for now)
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------- */
/* POSIX fallback: try xclip, xsel, wl-copy, wl-paste                         */
/* -------------------------------------------------------------------------- */

#if !defined(_WIN32)

static char *clipboard_read_posix(void) {
    /* Try wl-paste first (Wayland) */
    FILE *f = popen("wl-paste --no-newline 2>/dev/null", "r");
    if (!f) f = popen("xclip -selection clipboard -o 2>/dev/null", "r");
    if (!f) f = popen("xsel --clipboard --output 2>/dev/null", "r");
    if (!f) return NULL;

    char *buf = malloc(8192);
    if (!buf) { pclose(f); return NULL; }
    size_t cap = 8192, len = 0;
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        if (len + n + 1 > cap) {
            cap *= 2;
            while (cap < len + n + 1) cap *= 2;
            char *nbuf = realloc(buf, cap);
            if (!nbuf) { free(buf); pclose(f); return NULL; }
            buf = nbuf;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    pclose(f);
    if (buf) buf[len] = '\0';
    return buf;
}

static bool clipboard_write_posix(const char *text) {
    if (!text) return false;
    FILE *f = popen("wl-copy 2>/dev/null", "w");
    if (!f) f = popen("xclip -selection clipboard 2>/dev/null", "w");
    if (!f) f = popen("xsel --clipboard --input 2>/dev/null", "w");
    if (!f) return false;
    fputs(text, f);
    pclose(f);
    return true;
}

char *ui_clipboard_read(void) {
    return clipboard_read_posix();
}

bool ui_clipboard_write(const char *text) {
    return clipboard_write_posix(text);
}

#endif /* !defined(_WIN32) */

/* -------------------------------------------------------------------------- */
/* Windows implementation                                                     */
/* -------------------------------------------------------------------------- */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

char *ui_clipboard_read(void) {
    if (!OpenClipboard(NULL)) return NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) { CloseClipboard(); return NULL; }
    wchar_t *wstr = GlobalLock(h);
    if (!wstr) { CloseClipboard(); return NULL; }

    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    char *utf8 = malloc((size_t)len);
    if (utf8) {
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8, len, NULL, NULL);
    }
    GlobalUnlock(h);
    CloseClipboard();
    return utf8;
}

bool ui_clipboard_write(const char *text) {
    if (!text) return false;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (wlen <= 0) return false;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (size_t)wlen * sizeof(wchar_t));
    if (!h) return false;
    wchar_t *wstr = GlobalLock(h);
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wstr, wlen);
    GlobalUnlock(h);

    if (!OpenClipboard(NULL)) { GlobalFree(h); return false; }
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, h);
    CloseClipboard();
    return true;
}

#endif /* _WIN32 */
