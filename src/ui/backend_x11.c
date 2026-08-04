/*
 * ORDL UI — Raw X11 wire protocol backend
 * Pure C23, zero external dependencies, cross-platform.
 *
 * Implements the X11 client protocol directly over a Unix domain socket
 * (or TCP).  No libX11.  Works on any POSIX system with an X server.
 */

#include "forge/ui/ordl_ui.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* X11 core protocol definitions (sufficient for a basic client)              */
/* -------------------------------------------------------------------------- */

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||      \
    defined(__NetBSD__)

#define X_TCP_PORT 6000

typedef struct {
  uint8_t byteOrder;
  uint8_t pad;
  uint16_t majorVersion;
  uint16_t minorVersion;
  uint16_t nbytesAuthProto;
  uint16_t nbytesAuthString;
  uint16_t pad2;
} xConnClientPrefix;

typedef struct {
  uint8_t success;
  uint8_t pad1;
  uint16_t majorVersion;
  uint16_t minorVersion;
  uint16_t length; /* additional data length in 4-byte units */
} xConnSetupPrefix;

/* Request opcodes */
#define X_CreateWindow 1
#define X_ChangeWindowAttributes 2
#define X_GetWindowAttributes 3
#define X_DestroyWindow 4
#define X_MapWindow 8
#define X_UnmapWindow 10
#define X_ConfigureWindow 12
#define X_GetGeometry 14
#define X_QueryTree 15
#define X_InternAtom 16
#define X_ChangeProperty 18
#define X_DeleteProperty 19
#define X_GetProperty 20
#define X_CreateGC 55
#define X_ChangeGC 56
#define X_CopyArea 62
#define X_PolyFillRectangle 70
#define X_PutImage 72
#define X_CreatePixmap 53
#define X_FreePixmap 54
#define X_QueryExtension 98
#define X_ListExtensions 99
#define X_Bell 104
#define X_SendEvent 25
#define X_GrabKeyboard 31
#define X_UngrabKeyboard 32
#define X_GrabPointer 28
#define X_UngrabPointer 29
#define X_SetInputFocus 42
#define X_CreateColormap 78
#define X_FreeColormap 79

/* Event codes */
#define X_Error 0
#define X_Reply 1
#define X_KeyPress 2
#define X_KeyRelease 3
#define X_ButtonPress 4
#define X_ButtonRelease 5
#define X_MotionNotify 6
#define X_EnterNotify 7
#define X_LeaveNotify 8
#define X_FocusIn 9
#define X_FocusOut 10
#define X_KeymapNotify 11
#define X_Expose 12
#define X_GraphicsExpose 13
#define X_NoExpose 14
#define X_VisibilityNotify 15
#define X_CreateNotify 16
#define X_DestroyNotify 17
#define X_UnmapNotify 18
#define X_MapNotify 19
#define X_MapRequest 20
#define X_ReparentNotify 21
#define X_ConfigureNotify 22
#define X_ConfigureRequest 23
#define X_GravityNotify 24
#define X_ResizeRequest 25
#define X_CirculateNotify 26
#define X_CirculateRequest 27
#define X_PropertyNotify 28
#define X_SelectionClear 29
#define X_SelectionRequest 30
#define X_SelectionNotify 31
#define X_ColormapNotify 32
#define X_ClientMessage 33
#define X_MappingNotify 34

/* Masks for ChangeWindowAttributes */
#define CWBackPixmap (1L << 0)
#define CWBackPixel (1L << 1)
#define CWBorderPixmap (1L << 2)
#define CWBorderPixel (1L << 3)
#define CWBitGravity (1L << 4)
#define CWWinGravity (1L << 5)
#define CWBackingStore (1L << 6)
#define CWBackingPlanes (1L << 7)
#define CWBackingPixel (1L << 8)
#define CWOverrideRedirect (1L << 9)
#define CWSaveUnder (1L << 10)
#define CWEventMask (1L << 11)
#define CWDontPropagate (1L << 12)
#define CWColormap (1L << 13)
#define CWCursor (1L << 14)

/* Event masks */
#define KeyPressMask (1L << 0)
#define KeyReleaseMask (1L << 1)
#define ButtonPressMask (1L << 2)
#define ButtonReleaseMask (1L << 3)
#define EnterWindowMask (1L << 4)
#define LeaveWindowMask (1L << 5)
#define PointerMotionMask (1L << 6)
#define ExposureMask (1L << 15)
#define StructureNotifyMask (1L << 17)
#define SubstructureNotifyMask (1L << 19)
#define FocusChangeMask (1L << 21)
#define PropertyChangeMask (1L << 22)

/* Image formats */
#define XYBitmap 0
#define XYPixmap 1
#define ZPixmap 2

/* X11 state */
typedef struct {
  int fd;
  bool swapped; /* server byte order differs from client */

  uint32_t root;
  uint32_t root_visual;
  uint32_t white_pixel;
  uint32_t black_pixel;
  uint32_t resource_id_base;
  uint32_t resource_id_mask;
  uint8_t root_depth;

  uint32_t next_rid;
  uint32_t window;
  uint32_t gc;
  uint32_t pixmap;
  uint32_t cmap;

  int width, height;
  int depth;
  bool mapped;
  bool closed;

  uint8_t *img_data; /* local RGBA buffer */
  size_t img_size;

  uint32_t wm_delete_atom; /* WM_DELETE_WINDOW atom */

  uint8_t rx_buf[65536];
  size_t rx_len;
  uint16_t seq; /* request sequence number */
} x11_state_t;

/* -------------------------------------------------------------------------- */
/* Byte-order helpers                                                         */
/* -------------------------------------------------------------------------- */

static inline uint16_t bswap16(uint16_t v) {
  return (uint16_t)((v >> 8) | (v << 8));
}
static inline uint32_t bswap32(uint32_t v) {
  return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) |
         (v << 24);
}

static inline uint16_t x11_r16(x11_state_t *s, uint16_t v) {
  return s->swapped ? bswap16(v) : v;
}
static inline uint32_t x11_r32(x11_state_t *s, uint32_t v) {
  return s->swapped ? bswap32(v) : v;
}

static inline uint16_t x11_w16(x11_state_t *s, uint16_t v) {
  return s->swapped ? bswap16(v) : v;
}
static inline uint32_t x11_w32(x11_state_t *s, uint32_t v) {
  return s->swapped ? bswap32(v) : v;
}

/* -------------------------------------------------------------------------- */
/* Connection                                                                 */
/* -------------------------------------------------------------------------- */

static bool x11_write_all(int fd, const void *data, size_t len) {
  const uint8_t *p = data;
  size_t done = 0;
  while (done < len) {
    ssize_t n = write(fd, p + done, len - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    done += (size_t)n;
  }
  return true;
}

static bool x11_read_all(int fd, void *data, size_t len) {
  uint8_t *p = data;
  size_t done = 0;
  while (done < len) {
    ssize_t n = read(fd, p + done, len - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    done += (size_t)n;
  }
  return true;
}

static bool x11_connect_unix(x11_state_t *x, const char *path) {
  x->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (x->fd < 0)
    return false;
  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
  if (connect(x->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(x->fd);
    x->fd = -1;
    return false;
  }
  return true;
}

static bool x11_connect_tcp(x11_state_t *x, const char *host, int display) {
  x->fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (x->fd < 0)
    return false;
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)(X_TCP_PORT + display));
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    close(x->fd);
    x->fd = -1;
    return false;
  }
  if (connect(x->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(x->fd);
    x->fd = -1;
    return false;
  }
  return true;
}

static bool x11_handshake(x11_state_t *x) {
  xConnClientPrefix req = {0};
  req.byteOrder = (*(uint8_t *)&(uint16_t){1}) ? 'l' : 'B'; /* little or big */
  req.majorVersion = x11_w16(x, 11);
  req.minorVersion = x11_w16(x, 0);
  if (!x11_write_all(x->fd, &req, sizeof(req)))
    return false;

  xConnSetupPrefix rsp = {0};
  if (!x11_read_all(x->fd, &rsp, sizeof(rsp)))
    return false;
  if (rsp.success != 1)
    return false;

  uint16_t extra = x11_r16(x, rsp.length);
  size_t extra_bytes = extra * 4;
  if (extra_bytes > 8192)
    return false;
  uint8_t setup[8192];
  if (!x11_read_all(x->fd, setup, extra_bytes))
    return false;

  /* Parse setup */
  x->resource_id_base = x11_r32(x, *(uint32_t *)(setup + 4));
  x->resource_id_mask = x11_r32(x, *(uint32_t *)(setup + 8));

  /* Parse to find first screen (variable offset after vendor + formats) */
  uint16_t vendor_len = x11_r16(x, *(uint16_t *)(setup + 16));
  uint8_t formats_count = setup[21];
  size_t vendor_padded = ((vendor_len + 3) / 4) * 4;
  size_t screen_offset = 32 + vendor_padded + (formats_count * 8);
  if (screen_offset + 40 > extra_bytes)
    return false;

  uint8_t *screen = setup + screen_offset;
  x->root = x11_r32(x, *(uint32_t *)(screen + 0));
  x->white_pixel = x11_r32(x, *(uint32_t *)(screen + 8));
  x->black_pixel = x11_r32(x, *(uint32_t *)(screen + 12));
  x->root_visual = x11_r32(x, *(uint32_t *)(screen + 32));
  x->root_depth = screen[38];
  x->depth = x->root_depth;
  x->next_rid = x->resource_id_base;
  fprintf(stderr,
          "[X11] root=0x%x visual=0x%x depth=%u black=0x%x white=0x%x\n",
          (unsigned)x->root, (unsigned)x->root_visual, x->root_depth,
          (unsigned)x->black_pixel, (unsigned)x->white_pixel);
  return true;
}

static bool x11_connect(x11_state_t *x) {
  const char *disp = getenv("DISPLAY");
  if (!disp)
    disp = ":0";
  int display_num = 0;
  sscanf(disp, ":%d", &display_num);

  char path[64];
  snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", display_num);
  if (!x11_connect_unix(x, path)) {
    if (!x11_connect_tcp(x, "127.0.0.1", display_num))
      return false;
  }
  if (!x11_handshake(x)) {
    close(x->fd);
    x->fd = -1;
    return false;
  }
  return true;
}

/* -------------------------------------------------------------------------- */
/* Request builders                                                           */
/* -------------------------------------------------------------------------- */

static uint8_t x11_req[1024];
static size_t x11_req_len;

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

static bool x11_send(x11_state_t *x) {
  uint16_t len_words = (uint16_t)((x11_req_len + 3) / 4);
  x11_req[2] = (uint8_t)(len_words >> 0);
  x11_req[3] = (uint8_t)(len_words >> 8);
  x->seq++;
  return x11_write_all(x->fd, x11_req, len_words * 4);
}

static uint32_t x11_new_id(x11_state_t *x) {
  uint32_t id = x->next_rid;
  x->next_rid++;
  return id;
}

/* -------------------------------------------------------------------------- */
/* Atom interning                                                             */
/* -------------------------------------------------------------------------- */

static uint32_t x11_intern_atom(x11_state_t *x, const char *name,
                                bool only_if_exists) {
  uint16_t nlen = (uint16_t)strlen(name);
  if (nlen > 512)
    return 0; /* cap atom name to prevent x11_req overflow */
  x11_begin(X_InternAtom, only_if_exists ? 1 : 0);
  x11_w((uint32_t)nlen); /* name length */
  /* Pad to 4-byte boundary */
  size_t pad = (4 - (nlen & 3)) & 3;
  for (uint16_t i = 0; i < nlen; i++)
    x11_req[x11_req_len++] = (uint8_t)name[i];
  for (size_t i = 0; i < pad; i++)
    x11_req[x11_req_len++] = 0;
  x11_send(x);

  /* Read reply (32 bytes) */
  uint8_t reply[32];
  if (!x11_read_all(x->fd, reply, 32))
    return 0;
  if (reply[0] != X_Reply)
    return 0;
  uint32_t atom = ((uint32_t)reply[8] << 0) | ((uint32_t)reply[9] << 8) |
                  ((uint32_t)reply[10] << 16) | ((uint32_t)reply[11] << 24);
  return x11_r32(x, atom);
}

/* -------------------------------------------------------------------------- */
/* X11 resource creation                                                      */
/* -------------------------------------------------------------------------- */

static void x11_put16(uint16_t v) {
  x11_req[x11_req_len++] = (uint8_t)(v >> 0);
  x11_req[x11_req_len++] = (uint8_t)(v >> 8);
}

static bool x11_create_window(x11_state_t *x, int w, int h) {
  x->window = x11_new_id(x);
  x11_begin(X_CreateWindow, x->depth);
  x11_w(x->window);       /* wid */
  x11_w(x->root);         /* parent */
  x11_put16(0);           /* x */
  x11_put16(0);           /* y */
  x11_put16((uint16_t)w); /* width */
  x11_put16((uint16_t)h); /* height */
  x11_put16(0);           /* border_width */
  x11_put16(1);           /* class = InputOutput */
  x11_w(x->root_visual);  /* visual = root visual */
  x11_w(CWEventMask | CWBackPixel | CWColormap);
  x11_w(ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
        ButtonReleaseMask | PointerMotionMask | StructureNotifyMask |
        FocusChangeMask);
  x11_w(x->black_pixel); /* background pixel */
  x11_w(0);              /* colormap = CopyFromParent */
  if (!x11_send(x))
    return false;
  return true;
}

static bool x11_create_gc(x11_state_t *x) {
  x->gc = x11_new_id(x);
  x11_begin(X_CreateGC, 0);
  x11_w(x->gc);
  x11_w(x->window);
  x11_w(0); /* value mask */
  return x11_send(x);
}

static bool x11_map_window(x11_state_t *x) {
  x11_begin(X_MapWindow, 0);
  x11_w(x->window);
  if (!x11_send(x))
    return false;
  /* Request keyboard focus — best-effort, WM may override */
  x11_begin(X_SetInputFocus, 1); /* revert-to = PointerRoot */
  x11_w(x->window);
  x11_w(0);    /* time = CurrentTime */
  x11_send(x); /* ignore failure */
  return true;
}

/* -------------------------------------------------------------------------- */
/* Present: RGBA8888 → X11 ZPixmap                                            */
/* -------------------------------------------------------------------------- */

static bool x11_put_image_16bit(x11_state_t *x, int w, int h) {
  if (w <= 0 || h <= 0)
    return false;
  if ((size_t)w > SIZE_MAX / 2 || (size_t)w * 2 > SIZE_MAX / (size_t)h)
    return false;
  size_t pixels = (size_t)w * (size_t)h;
  size_t data_len = pixels * 2;
  size_t pad = (4 - (data_len & 3)) & 3;
  uint32_t len_words = (uint32_t)((6 + (data_len + pad + 3) / 4));
  if (len_words > 65535)
    return false;

  x11_begin(X_PutImage, ZPixmap);
  x11_w(x->pixmap ? x->pixmap : x->window);
  x11_w(x->gc);
  x11_put16((uint16_t)w);
  x11_put16((uint16_t)h);
  x11_put16(0);                               /* dst_x */
  x11_put16(0);                               /* dst_y */
  x11_req[x11_req_len++] = 0;                 /* left_pad */
  x11_req[x11_req_len++] = (uint8_t)x->depth; /* depth */
  x11_req[x11_req_len++] = 0;                 /* pad */
  x11_req[x11_req_len++] = 0;                 /* pad */
  if (!x11_write_all(x->fd, x11_req, 6 * 4))
    return false;

  /* Convert RGBA8888 → RGB565 */
  uint8_t *src = x->img_data;
  for (size_t i = 0; i < pixels; i++) {
    uint8_t r = src[i * 4 + 2];
    uint8_t g = src[i * 4 + 1];
    uint8_t b = src[i * 4 + 0];
    uint16_t px = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    uint8_t pxle[2] = {(uint8_t)(px >> 0), (uint8_t)(px >> 8)};
    if (!x11_write_all(x->fd, pxle, 2))
      return false;
  }
  if (pad) {
    uint8_t zero[4] = {0};
    if (!x11_write_all(x->fd, zero, pad))
      return false;
  }
  x->seq++;
  return true;
}

static bool x11_put_image(x11_state_t *x, int w, int h) {
  if (w <= 0 || h <= 0)
    return false;
  if (x->depth == 24 || x->depth == 32) {
    /* Split into horizontal strips to fit X11 65535-word request limit */
    int max_rows = 200; /* ~200 rows of 1280x4 = ~1MB per request, safe */
    if (max_rows > h)
      max_rows = h;
    for (int y0 = 0; y0 < h; y0 += max_rows) {
      int rows = (y0 + max_rows <= h) ? max_rows : (h - y0);
      size_t strip_len = (size_t)w * (size_t)rows * 4;
      size_t pad = (4 - (strip_len & 3)) & 3;
      uint32_t len_words = (uint32_t)((6 + (strip_len + pad + 3) / 4));
      if (len_words > 65535)
        return false;

      x11_begin(X_PutImage, ZPixmap);
      x11_w(x->pixmap ? x->pixmap : x->window);
      x11_w(x->gc);
      x11_put16((uint16_t)w);
      x11_put16((uint16_t)rows);
      x11_put16(0);                               /* dst_x */
      x11_put16((uint16_t)y0);                    /* dst_y */
      x11_req[x11_req_len++] = 0;                 /* left_pad */
      x11_req[x11_req_len++] = (uint8_t)x->depth; /* depth */
      x11_req[x11_req_len++] = 0;                 /* pad */
      x11_req[x11_req_len++] = 0;                 /* pad */
      if (!x11_write_all(x->fd, x11_req, 6 * 4))
        return false;
      if (!x11_write_all(x->fd, x->img_data + (size_t)y0 * (size_t)w * 4,
                         strip_len))
        return false;
      if (pad) {
        uint8_t zero[4] = {0};
        if (!x11_write_all(x->fd, zero, pad))
          return false;
      }
      x->seq++;
    }
    return true;
  } else if (x->depth == 16) {
    return x11_put_image_16bit(x, w, h);
  }
  /* 8-bit depth requires colormap setup — not supported in minimal impl */
  return false;
}

/* -------------------------------------------------------------------------- */
/* Event dispatch                                                             */
/* -------------------------------------------------------------------------- */

static bool x11_recv_event(x11_state_t *x, ui_event_t *out, int timeout_ms) {
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(x->fd, &rfds);
  struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  int ret = select(x->fd + 1, &rfds, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
  if (ret <= 0)
    return false;

  uint8_t ev[32];
  if (!x11_read_all(x->fd, ev, 32))
    return false;
  uint8_t code = ev[0] & 0x7F;

  memset(out, 0, sizeof(*out));
  out->timestamp_ns = 0;

  switch (code) {
  case X_Expose:
    /* Not generating a ui_event for expose; app will redraw */
    return false;
  case X_KeyPress: {
    out->type = UI_EVENT_KEY;
    uint8_t keycode = ev[1];
    /* Very rough keycode → ui_key_t mapping */
    switch (keycode) {
    case 36:
      out->key.key = UI_KEY_ENTER;
      break;
    case 23:
      out->key.key = UI_KEY_TAB;
      break;
    case 22:
      out->key.key = UI_KEY_BACKSPACE;
      break;
    case 119:
      out->key.key = UI_KEY_DELETE;
      break;
    case 9:
      out->key.key = UI_KEY_ESCAPE;
      break;
    case 65:
      out->key.key = UI_KEY_SPACE;
      break;
    case 111:
      out->key.key = UI_KEY_UP;
      break;
    case 116:
      out->key.key = UI_KEY_DOWN;
      break;
    case 113:
      out->key.key = UI_KEY_LEFT;
      break;
    case 114:
      out->key.key = UI_KEY_RIGHT;
      break;
    case 110:
      out->key.key = UI_KEY_HOME;
      break;
    case 115:
      out->key.key = UI_KEY_END;
      break;
    case 112:
      out->key.key = UI_KEY_PAGE_UP;
      break;
    case 117:
      out->key.key = UI_KEY_PAGE_DOWN;
      break;
    case 118:
      out->key.key = UI_KEY_INSERT;
      break;
    default:
      out->key.key = UI_KEY_NONE;
      break;
    }
    out->key.ctrl = (ev[28] & 4) != 0;
    out->key.alt = (ev[28] & 8) != 0;
    out->key.shift = (ev[28] & 1) != 0;
    return true;
  }
  case X_ButtonPress:
  case X_ButtonRelease: {
    out->type =
        (code == X_ButtonPress) ? UI_EVENT_MOUSE_PRESS : UI_EVENT_MOUSE_RELEASE;
    int16_t rx = (int16_t)(((uint16_t)ev[25] << 8) | ev[24]);
    int16_t ry = (int16_t)(((uint16_t)ev[27] << 8) | ev[26]);
    out->mouse.x = rx;
    out->mouse.y = ry;
    out->mouse.button = ev[1]; /* 1=left, 2=middle, 3=right */
    return true;
  }
  case X_MotionNotify: {
    out->type = UI_EVENT_MOUSE_MOVE;
    int16_t rx = (int16_t)(((uint16_t)ev[25] << 8) | ev[24]);
    int16_t ry = (int16_t)(((uint16_t)ev[27] << 8) | ev[26]);
    out->mouse.x = rx;
    out->mouse.y = ry;
    return true;
  }
  case X_ClientMessage: {
    uint32_t atom = ((uint32_t)ev[12] << 0) | ((uint32_t)ev[13] << 8) |
                    ((uint32_t)ev[14] << 16) | ((uint32_t)ev[15] << 24);
    /* WM_DELETE_WINDOW */
    if (x->wm_delete_atom && atom == x->wm_delete_atom) {
      out->type = UI_EVENT_QUIT;
      return true;
    }
    return false;
  }
  case X_ConfigureNotify: {
    uint16_t nw = ((uint16_t)ev[20] << 8) | ev[21];
    uint16_t nh = ((uint16_t)ev[22] << 8) | ev[23];
    if (nw != (uint16_t)x->width || nh != (uint16_t)x->height) {
      x->width = nw;
      x->height = nh;
      out->type = UI_EVENT_RESIZE;
      out->resize.w = nw;
      out->resize.h = nh;
      return true;
    }
    return false;
  }
  default:
    return false;
  }
}

/* -------------------------------------------------------------------------- */
/* Backend vtable                                                             */
/* -------------------------------------------------------------------------- */

static bool be_x11_init(ui_backend_t *be, int w, int h) {
  x11_state_t *x = calloc(1, sizeof(x11_state_t));
  if (!x)
    return false;
  x->fd = -1;
  x->width = w;
  x->height = h;

  if (!x11_connect(x)) {
    free(x);
    return false;
  }
  if (!x11_create_window(x, w, h)) {
    close(x->fd);
    free(x);
    return false;
  }
  if (!x11_create_gc(x)) {
    close(x->fd);
    free(x);
    return false;
  }
  if (!x11_map_window(x)) {
    close(x->fd);
    free(x);
    return false;
  }
  x->mapped = true;

  /* WM_HINTS: tell WM we want keyboard input */
  uint32_t wm_hints_atom = x11_intern_atom(x, "WM_HINTS", false);
  if (wm_hints_atom) {
    x11_begin(X_ChangeProperty, 0);
    x11_w(x->window);
    x11_w(wm_hints_atom);
    x11_w(wm_hints_atom);
    x11_w(32);
    x11_w(9);
    x11_w(0x03); /* flags: InputHint(1) | StateHint(2) */
    x11_w(1);    /* input: True */
    x11_w(1);    /* initial_state: NormalState */
    x11_w(0);
    x11_w(0);
    x11_w(0);
    x11_w(0);
    x11_w(0);
    x11_w(0);
    x11_send(x);
  }

  /* WM_NAME */
  uint32_t wm_name = x11_intern_atom(x, "WM_NAME", false);
  uint32_t string_atom = x11_intern_atom(x, "STRING", false);
  if (wm_name && string_atom) {
    const char *title = "FORGE";
    size_t tlen = strlen(title);
    x11_begin(X_ChangeProperty, 0);
    x11_w(x->window);
    x11_w(wm_name);
    x11_w(string_atom);
    x11_w(8);
    x11_w((uint32_t)tlen);
    for (size_t i = 0; i < tlen; i++)
      x11_req[x11_req_len++] = (uint8_t)title[i];
    size_t pad = (4 - (tlen & 3)) & 3;
    for (size_t i = 0; i < pad; i++)
      x11_req[x11_req_len++] = 0;
    x11_send(x);
  }

  /* Drain any pending X11 events (MapNotify, etc.) to keep connection in sync
   */
  {
    fd_set rfds;
    struct timeval tv = {0, 0};
    uint8_t drain[256];
    while (1) {
      FD_ZERO(&rfds);
      FD_SET(x->fd, &rfds);
      int ret = select(x->fd + 1, &rfds, NULL, NULL, &tv);
      if (ret <= 0)
        break;
      ssize_t n = read(x->fd, drain, sizeof(drain));
      if (n <= 0)
        break;
    }
  }

  /* Intern WM_DELETE_WINDOW and set WM_PROTOCOLS */
  x->wm_delete_atom = x11_intern_atom(x, "WM_DELETE_WINDOW", false);
  if (x->wm_delete_atom) {
    uint32_t wm_protocols = x11_intern_atom(x, "WM_PROTOCOLS", false);
    if (wm_protocols) {
      x11_begin(X_ChangeProperty, 0); /* mode = Replace */
      x11_w(x->window);
      x11_w(wm_protocols); /* property */
      x11_w(4);            /* type = XA_ATOM = 4 */
      x11_w(32);           /* format = 32 */
      x11_w(1);            /* length in format units */
      x11_w(x->wm_delete_atom);
      x11_send(x);
    }
  }

  /* Allocate local image buffer (RGBA8888) */
  if (w <= 0 || h <= 0 || (size_t)w > SIZE_MAX / 4 ||
      (size_t)w * 4 > SIZE_MAX / (size_t)h) {
    close(x->fd);
    free(x);
    return false;
  }
  x->img_size = (size_t)w * (size_t)h * 4;
  x->img_data = malloc(x->img_size);
  if (!x->img_data) {
    close(x->fd);
    free(x);
    return false;
  }

  be->canvas = ui_canvas_new_fb(w, h);
  if (!be->canvas) {
    close(x->fd);
    free(x->img_data);
    free(x);
    return false;
  }

  be->user_data = x;
  be->supports_mouse = true;
  be->supports_color = true;
  be->supports_unicode = false;
  be->max_colors = 0xFFFFFF;
  return true;
}

static void be_x11_shutdown(ui_backend_t *be) {
  if (!be)
    return;
  x11_state_t *x = (x11_state_t *)be->user_data;
  if (x) {
    if (x->fd >= 0) {
      x11_begin(X_DestroyWindow, 0);
      x11_w(x->window);
      x11_send(x);
      close(x->fd);
      x->fd = -1;
    }
    if (x->img_data) {
      free(x->img_data);
      x->img_data = NULL;
    }
    free(x);
    be->user_data = NULL;
  }
  if (be->canvas) {
    ui_canvas_free(be->canvas);
    be->canvas = NULL;
  }
}

static void be_x11_present(ui_backend_t *be) {
  static int dbg = 0;
  if (!be || !be->canvas || be->canvas->type != UI_CANVAS_FB)
    return;
  x11_state_t *x = (x11_state_t *)be->user_data;
  if (!x || !x->img_data)
    return;

  /* Copy canvas RGBA → x11 image buffer (test: no swizzle, direct copy) */
  const uint32_t *src = be->canvas->pixels;
  uint8_t *dst = x->img_data;
  memcpy(dst, src, (size_t)(x->width * x->height) * 4);
  if ((dbg++ & 0xFF) == 0) {
    fprintf(stderr,
            "[PRESENT] be=%p canvas=%p src[0]=0x%08x img[0..3]=%02x %02x %02x "
            "%02x\n",
            (void *)be, be ? (void *)be->canvas : NULL, src[0], dst[0], dst[1],
            dst[2], dst[3]);
    /* Check for pending X11 error events */
    fd_set rfds;
    struct timeval tv = {0, 0};
    FD_ZERO(&rfds);
    FD_SET(x->fd, &rfds);
    if (select(x->fd + 1, &rfds, NULL, NULL, &tv) > 0) {
      uint8_t ev[32];
      ssize_t n = read(x->fd, ev, 32);
      if (n >= 32 && ev[0] == 0) { /* X_Error */
        fprintf(stderr, "[X11 ERROR] code=%u seq=%u bad=%u major=%u minor=%u\n",
                ev[1], (ev[2] | (ev[3] << 8)),
                (ev[4] | (ev[5] << 8) | (ev[6] << 16) | (ev[7] << 24)), ev[8],
                ev[9]);
      } else if (n > 0) {
        fprintf(stderr, "[X11 EVENT] type=%u\n", ev[0] & 0x7F);
      }
    }
  }
  x11_put_image(x, x->width, x->height);
}

static bool be_x11_poll_event(ui_backend_t *be, ui_event_t *out,
                              int timeout_ms) {
  if (!be || !be->user_data)
    return false;
  x11_state_t *x = (x11_state_t *)be->user_data;
  if (x->closed) {
    out->type = UI_EVENT_QUIT;
    return true;
  }
  return x11_recv_event(x, out, timeout_ms);
}

ui_backend_t *ui_backend_x11_new(void) {
  ui_backend_t *be = calloc(1, sizeof(ui_backend_t));
  if (!be)
    return NULL;
  be->name = "x11";
  be->init = be_x11_init;
  be->shutdown = be_x11_shutdown;
  be->poll_event = be_x11_poll_event;
  be->present = be_x11_present;
  return be;
}

#else /* non-POSIX */

ui_backend_t *ui_backend_x11_new(void) { return NULL; }

#endif
