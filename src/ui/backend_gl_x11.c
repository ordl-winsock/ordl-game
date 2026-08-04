/*
 * ORDL UI — X11 + EGL windowed GPU compositor backend
 * Runtime-loaded EGL + GLESv2. Zero linked dependencies.
 *
 * Creates an X11 window via raw wire protocol, then sets up EGL
 * window surface for GPU-accelerated presentation.
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

/* -------------------------------------------------------------------------- */
/* X11 core protocol (minimal subset)                                         */
/* -------------------------------------------------------------------------- */

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

#define X_CreateWindow       1
#define X_DestroyWindow      4
#define X_MapWindow          8
#define X_InternAtom         16
#define X_ChangeProperty     18
#define X_CreateGC           55
#define X_ChangeGC           56
#define X_PutImage           72
#define X_CreateColormap     78
#define X_FreeColormap       79

#define CWBackPixel        (1L<<1)
#define CWBorderPixel      (1L<<3)
#define CWEventMask        (1L<<11)
#define CWColormap         (1L<<13)

#define KeyPressMask        (1L<<0)
#define KeyReleaseMask      (1L<<1)
#define ButtonPressMask     (1L<<2)
#define ButtonReleaseMask   (1L<<3)
#define PointerMotionMask   (1L<<6)
#define ExposureMask        (1L<<15)
#define StructureNotifyMask (1L<<17)
#define FocusChangeMask     (1L<<21)

#define X_Error              0
#define X_Reply              1
#define X_KeyPress           2
#define X_KeyRelease         3
#define X_ButtonPress        4
#define X_ButtonRelease      5
#define X_MotionNotify       6
#define X_Expose             12
#define X_FocusIn            9
#define X_FocusOut           10
#define X_ClientMessage      33

#define ZPixmap              2

/* -------------------------------------------------------------------------- */
/* EGL / GLES types (same as backend_gl.c)                                    */
/* -------------------------------------------------------------------------- */

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void *EGLNativeWindowType;
typedef void *EGLNativeDisplayType;
typedef int   EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;

#define EGL_FALSE 0
#define EGL_TRUE  1
#define EGL_NONE  0x3038
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NO_CONTEXT      ((EGLContext)0)
#define EGL_NO_DISPLAY      ((EGLDisplay)0)
#define EGL_NO_SURFACE      ((EGLSurface)0)
#define EGL_SURFACE_TYPE    0x3033
#define EGL_WINDOW_BIT      0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT  0x0004
#define EGL_RED_SIZE        0x3024
#define EGL_GREEN_SIZE      0x3023
#define EGL_BLUE_SIZE       0x3022
#define EGL_ALPHA_SIZE      0x3021
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API   0x30A0
#define EGL_DRAW            0x3059
#define EGL_READ            0x305A

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef float GLclampf;
typedef void GLvoid;
typedef char GLchar;
typedef ptrdiff_t GLintptr;
typedef ptrdiff_t GLsizeiptr;

#define GL_FALSE                        0
#define GL_TRUE                         1
#define GL_TEXTURE_2D                   0x0DE1
#define GL_TEXTURE0                     0x84C0
#define GL_ARRAY_BUFFER                 0x8892
#define GL_VERTEX_SHADER                0x8B31
#define GL_FRAGMENT_SHADER              0x8B30
#define GL_COMPILE_STATUS               0x8B81
#define GL_LINK_STATUS                  0x8B82
#define GL_INFO_LOG_LENGTH              0x8B84
#define GL_FLOAT                        0x1406
#define GL_UNSIGNED_BYTE                0x1401
#define GL_RGBA                         0x1908
#define GL_TRIANGLE_STRIP               0x0005
#define GL_COLOR_BUFFER_BIT             0x00004000
#define GL_STATIC_DRAW                  0x88E4
#define GL_LINEAR                       0x2601
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_CLAMP_TO_EDGE                0x812F
#define GL_TEXTURE_WRAP_S               0x2802
#define GL_TEXTURE_WRAP_T               0x2803

/* -------------------------------------------------------------------------- */
/* Runtime GL loader                                                          */
/* -------------------------------------------------------------------------- */

static struct {
    void *egl, *gles;
    EGLDisplay (*eglGetDisplay)(EGLNativeDisplayType);
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint*, EGLint*);
    EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
    EGLSurface (*eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*);
    EGLBoolean (*eglDestroySurface)(EGLDisplay, EGLSurface);
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
    EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext);
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*eglSwapBuffers)(EGLDisplay, EGLSurface);
    EGLBoolean (*eglBindAPI)(EGLenum);
    EGLBoolean (*eglSwapInterval)(EGLDisplay, EGLint);
    EGLBoolean (*eglTerminate)(EGLDisplay);
    EGLBoolean (*eglGetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint*);
    EGLint (*eglGetError)(void);
    void (*glViewport)(GLint, GLint, GLsizei, GLsizei);
    void (*glClearColor)(GLclampf, GLclampf, GLclampf, GLclampf);
    void (*glClear)(GLenum);
    void (*glGenTextures)(GLsizei, GLuint*);
    void (*glBindTexture)(GLenum, GLuint);
    void (*glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*);
    void (*glTexParameteri)(GLenum, GLenum, GLint);
    void (*glGenBuffers)(GLsizei, GLuint*);
    void (*glBindBuffer)(GLenum, GLuint);
    void (*glBufferData)(GLenum, GLsizeiptr, const GLvoid*, GLenum);
    void (*glEnableVertexAttribArray)(GLuint);
    void (*glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid*);
    void (*glDrawArrays)(GLenum, GLint, GLsizei);
    void (*glActiveTexture)(GLenum);
    void (*glUniform1i)(GLint, GLint);
    void (*glUseProgram)(GLuint);
    GLuint (*glCreateShader)(GLenum);
    void (*glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
    void (*glCompileShader)(GLuint);
    void (*glGetShaderiv)(GLuint, GLenum, GLint*);
    void (*glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    GLuint (*glCreateProgram)(void);
    void (*glAttachShader)(GLuint, GLuint);
    void (*glLinkProgram)(GLuint);
    void (*glGetProgramiv)(GLuint, GLenum, GLint*);
    void (*glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    GLint (*glGetUniformLocation)(GLuint, const GLchar*);
    void (*glDeleteShader)(GLuint);
    void (*glDeleteProgram)(GLuint);
    void (*glDeleteTextures)(GLsizei, const GLuint*);
    void (*glDeleteBuffers)(GLsizei, const GLuint*);
} glx;

static bool glx_load(void) {
    glx.egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!glx.egl) glx.egl = dlopen("libEGL.so", RTLD_LAZY | RTLD_LOCAL);
    if (!glx.egl) return false;
    glx.gles = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!glx.gles) glx.gles = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_LOCAL);
    if (!glx.gles) { dlclose(glx.egl); glx.egl = NULL; return false; }

    #define LOAD_EGL(n) *(void **)(&glx.n) = dlsym(glx.egl, #n)
    #define LOAD_GLES(n) *(void **)(&glx.n) = dlsym(glx.gles, #n)
    LOAD_EGL(eglGetDisplay); LOAD_EGL(eglInitialize); LOAD_EGL(eglChooseConfig);
    LOAD_EGL(eglCreateWindowSurface); LOAD_EGL(eglDestroySurface);
    LOAD_EGL(eglCreateContext); LOAD_EGL(eglDestroyContext);
    LOAD_EGL(eglMakeCurrent); LOAD_EGL(eglSwapBuffers); LOAD_EGL(eglBindAPI);
    LOAD_EGL(eglSwapInterval); LOAD_EGL(eglTerminate);
    LOAD_EGL(eglGetConfigAttrib); LOAD_EGL(eglGetError);
    LOAD_GLES(glViewport); LOAD_GLES(glClearColor); LOAD_GLES(glClear);
    LOAD_GLES(glGenTextures); LOAD_GLES(glBindTexture); LOAD_GLES(glTexImage2D);
    LOAD_GLES(glTexParameteri); LOAD_GLES(glGenBuffers); LOAD_GLES(glBindBuffer);
    LOAD_GLES(glBufferData); LOAD_GLES(glEnableVertexAttribArray); LOAD_GLES(glVertexAttribPointer);
    LOAD_GLES(glDrawArrays); LOAD_GLES(glActiveTexture); LOAD_GLES(glUniform1i);
    LOAD_GLES(glUseProgram); LOAD_GLES(glCreateShader); LOAD_GLES(glShaderSource);
    LOAD_GLES(glCompileShader); LOAD_GLES(glGetShaderiv); LOAD_GLES(glGetShaderInfoLog);
    LOAD_GLES(glCreateProgram); LOAD_GLES(glAttachShader); LOAD_GLES(glLinkProgram);
    LOAD_GLES(glGetProgramiv); LOAD_GLES(glGetProgramInfoLog); LOAD_GLES(glGetUniformLocation);
    LOAD_GLES(glDeleteShader); LOAD_GLES(glDeleteProgram); LOAD_GLES(glDeleteTextures);
    LOAD_GLES(glDeleteBuffers);
    #undef LOAD_EGL
    #undef LOAD_GLES
    return glx.eglGetDisplay && glx.eglInitialize && glx.glViewport && glx.glCreateShader;
}

static void glx_unload(void) {
    if (glx.gles) { dlclose(glx.gles); glx.gles = NULL; }
    if (glx.egl)  { dlclose(glx.egl);  glx.egl = NULL; }
    memset(&glx, 0, sizeof(glx));
}

/* -------------------------------------------------------------------------- */
/* Shader helpers                                                             */
/* -------------------------------------------------------------------------- */

static GLuint glx_compile_shader(GLenum type, const char *src) {
    GLuint s = glx.glCreateShader(type);
    if (!s) return 0;
    GLint len = (GLint)strlen(src);
    glx.glShaderSource(s, 1, &src, &len);
    glx.glCompileShader(s);
    GLint status = 0;
    glx.glGetShaderiv(s, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512]; GLsizei n;
        glx.glGetShaderInfoLog(s, sizeof(log), &n, log);
        fprintf(stderr, "[GLX] shader compile error: %s\n", log);
        glx.glDeleteShader(s); return 0;
    }
    return s;
}

static GLuint glx_link_program(GLuint vs, GLuint fs) {
    GLuint p = glx.glCreateProgram();
    if (!p) return 0;
    glx.glAttachShader(p, vs); glx.glAttachShader(p, fs);
    glx.glLinkProgram(p);
    GLint status = 0;
    glx.glGetProgramiv(p, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512]; GLsizei n;
        glx.glGetProgramInfoLog(p, sizeof(log), &n, log);
        fprintf(stderr, "[GLX] program link error: %s\n", log);
        glx.glDeleteProgram(p); return 0;
    }
    return p;
}

static const char *glx_vs_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; }\n";

static const char *glx_fs_src =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

static const GLfloat glx_quad_verts[] = {
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
     1.0f,  1.0f,  1.0f, 1.0f,
};

/* -------------------------------------------------------------------------- */
/* X11 wire helpers                                                           */
/* -------------------------------------------------------------------------- */

static bool x11_write_all(int fd, const void *data, size_t len) {
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

static bool x11_read_all(int fd, void *data, size_t len) {
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

static uint8_t x11_req[1024];
static size_t  x11_req_len;

static void x11_begin(uint8_t opcode, uint8_t data) {
    x11_req_len = 0;
    x11_req[x11_req_len++] = opcode;
    x11_req[x11_req_len++] = data;
    x11_req_len += 2;
}
static void x11_w(uint32_t v) {
    x11_req[x11_req_len++] = (uint8_t)(v >> 0);
    x11_req[x11_req_len++] = (uint8_t)(v >> 8);
    x11_req[x11_req_len++] = (uint8_t)(v >> 16);
    x11_req[x11_req_len++] = (uint8_t)(v >> 24);
}
static bool x11_send(int fd, uint16_t *seq) {
    uint16_t len_words = (uint16_t)((x11_req_len + 3) / 4);
    x11_req[2] = (uint8_t)(len_words >> 0);
    x11_req[3] = (uint8_t)(len_words >> 8);
    (*seq)++;
    return x11_write_all(fd, x11_req, len_words * 4);
}

/* -------------------------------------------------------------------------- */
/* Combined state                                                             */
/* -------------------------------------------------------------------------- */

typedef struct {
    /* X11 */
    int fd;
    uint32_t root;
    uint32_t white_pixel;
    uint32_t black_pixel;
    uint32_t resource_id_base;
    uint32_t resource_id_mask;
    uint8_t  root_depth;
    uint32_t root_visual;
    uint32_t next_rid;
    uint32_t window;
    uint32_t gc;
    uint32_t wm_delete_atom;
    uint16_t seq;
    int width, height;
    bool mapped;
    bool closed;
    uint8_t rx_buf[32];

    /* EGL / GL */
    EGLDisplay dpy;
    EGLSurface surf;
    EGLContext ctx;
    EGLConfig config;
    GLuint program;
    GLuint tex;
    GLuint vbo;
    GLint u_tex_loc;
} glx_state_t;

static uint32_t x11_new_id(glx_state_t *g) {
    uint32_t id = g->next_rid;
    g->next_rid++;
    return id;
}

static bool x11_connect(glx_state_t *g) {
    const char *disp = getenv("DISPLAY");
    if (!disp) disp = ":0";
    int display_num = 0;
    sscanf(disp, ":%d", &display_num);

    char path[64];
    snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", display_num);
    g->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (g->fd < 0) return false;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(g->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(g->fd); g->fd = -1; return false;
    }

    xConnClientPrefix req = {0};
    req.byteOrder = (*(uint8_t *)&(uint16_t){1}) ? 'l' : 'B';
    req.majorVersion = 11;
    req.minorVersion = 0;
    if (!x11_write_all(g->fd, &req, sizeof(req))) return false;

    xConnSetupPrefix rsp = {0};
    if (!x11_read_all(g->fd, &rsp, sizeof(rsp))) return false;
    if (rsp.success != 1) return false;
    uint16_t extra = rsp.length;
    size_t extra_bytes = extra * 4;
    if (extra_bytes > 8192) return false;
    uint8_t setup[8192];
    if (!x11_read_all(g->fd, setup, extra_bytes)) return false;

    g->resource_id_base = *(uint32_t *)(setup + 4);
    g->resource_id_mask = *(uint32_t *)(setup + 8);
    g->root = *(uint32_t *)(setup + 16);
    g->white_pixel = *(uint32_t *)(setup + 20);
    g->black_pixel = *(uint32_t *)(setup + 24);
    g->next_rid = g->resource_id_base;

    /* Compute screen data offset for visual ID and depth:
     * header(32) + vendor_padded + formats*8 */
    uint16_t vendor_len = *(uint16_t *)(setup + 16);
    uint8_t num_formats = setup[21];
    size_t screen_off = 32 + ((vendor_len + 3) & ~3) + ((size_t)num_formats * 8);
    if (screen_off + 40 <= extra_bytes) {
        g->root_visual = *(uint32_t *)(setup + screen_off + 32);
        g->root_depth = setup[screen_off + 38];
    } else {
        g->root_visual = 0;
        g->root_depth = 24;
    }
    return true;
}

static uint32_t x11_intern_atom(glx_state_t *g, const char *name) {
    uint16_t nlen = (uint16_t)strlen(name);
    x11_begin(X_InternAtom, 0);
    x11_w((uint32_t)nlen);
    size_t pad = (4 - (nlen & 3)) & 3;
    for (uint16_t i = 0; i < nlen; i++) x11_req[x11_req_len++] = (uint8_t)name[i];
    for (size_t i = 0; i < pad; i++) x11_req[x11_req_len++] = 0;
    x11_send(g->fd, &g->seq);
    uint8_t reply[32];
    if (!x11_read_all(g->fd, reply, 32)) return 0;
    if (reply[0] != X_Reply) return 0;
    return ((uint32_t)reply[8] << 0) | ((uint32_t)reply[9] << 8) |
           ((uint32_t)reply[10] << 16) | ((uint32_t)reply[11] << 24);
}

static bool x11_create_window(glx_state_t *g, int w, int h, uint32_t visual_id, uint8_t depth) {
    /* Create colormap for non-root visual */
    uint32_t cmap = g->root;
    if (visual_id != g->root_visual) {
        cmap = x11_new_id(g);
        x11_begin(X_CreateColormap, 0);
        x11_w(cmap); x11_w(g->root); x11_w(visual_id); x11_w(0);
        if (!x11_send(g->fd, &g->seq)) return false;
    }

    g->window = x11_new_id(g);
    x11_begin(X_CreateWindow, depth);
    x11_w(g->window);
    x11_w(g->root);
    x11_w(0); x11_w(0);
    x11_w((uint32_t)w); x11_w((uint32_t)h);
    x11_w(0); x11_w(1);
    x11_w(visual_id);
    x11_w(CWEventMask | CWBackPixel | CWColormap);
    x11_w(ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
          PointerMotionMask | StructureNotifyMask | FocusChangeMask);
    x11_w(g->black_pixel);
    x11_w(0); x11_w(0); x11_w(0); x11_w(0); x11_w(0); x11_w(0);
    x11_w(0); x11_w(0); x11_w(0); x11_w(cmap); x11_w(0);
    if (!x11_send(g->fd, &g->seq)) return false;

    g->gc = x11_new_id(g);
    x11_begin(X_CreateGC, 0);
    x11_w(g->gc); x11_w(g->window); x11_w(0);
    if (!x11_send(g->fd, &g->seq)) return false;

    /* WM_DELETE_WINDOW */
    g->wm_delete_atom = x11_intern_atom(g, "WM_DELETE_WINDOW");
    uint32_t wm_protocols = x11_intern_atom(g, "WM_PROTOCOLS");
    if (g->wm_delete_atom && wm_protocols) {
        x11_begin(X_ChangeProperty, 0);
        x11_w(g->window); x11_w(wm_protocols); x11_w(32); x11_w(1);
        x11_w(g->wm_delete_atom);
        x11_send(g->fd, &g->seq);
    }

    x11_begin(X_MapWindow, 0);
    x11_w(g->window);
    return x11_send(g->fd, &g->seq);
}

/* -------------------------------------------------------------------------- */
/* Backend vtable                                                             */
/* -------------------------------------------------------------------------- */

static bool be_gl_x11_init(ui_backend_t *be, int w, int h) {
    if (!glx_load()) return false;
    glx_state_t *gs = calloc(1, sizeof(glx_state_t));
    if (!gs) return false;
    gs->fd = -1;
    gs->width = w; gs->height = h;

    if (!x11_connect(gs)) { free(gs); glx_unload(); return false; }

    gs->dpy = glx.eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (gs->dpy == EGL_NO_DISPLAY) { close(gs->fd); free(gs); glx_unload(); return false; }

    EGLint maj, min;
    if (!glx.eglInitialize(gs->dpy, &maj, &min)) { close(gs->fd); free(gs); glx_unload(); return false; }
    if (!glx.eglBindAPI(EGL_OPENGL_ES_API)) { close(gs->fd); free(gs); glx_unload(); return false; }

    EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        0x302E /*EGL_NATIVE_VISUAL_ID*/, (EGLint)gs->root_visual,
        EGL_NONE
    };
    EGLint nconfig;
    EGLint visual_id = 0;
    if (!glx.eglChooseConfig(gs->dpy, attribs, &gs->config, 1, &nconfig) || nconfig < 1) {
        /* Fallback: unconstrained config */
        EGLint fallback[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE
        };
        if (!glx.eglChooseConfig(gs->dpy, fallback, &gs->config, 1, &nconfig) || nconfig < 1) {
            glx.eglTerminate(gs->dpy); close(gs->fd); free(gs); glx_unload(); return false;
        }
        if (!glx.eglGetConfigAttrib(gs->dpy, gs->config, 0x302E /*EGL_NATIVE_VISUAL_ID*/, &visual_id)) {
            glx.eglTerminate(gs->dpy); close(gs->fd); free(gs); glx_unload(); return false;
        }
    } else {
        visual_id = (EGLint)gs->root_visual;
    }

    uint8_t win_depth = (visual_id == (EGLint)gs->root_visual) ? gs->root_depth : 32;
    if (!x11_create_window(gs, w, h, (uint32_t)visual_id, win_depth)) { glx.eglTerminate(gs->dpy); close(gs->fd); free(gs); glx_unload(); return false; }

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    gs->ctx = glx.eglCreateContext(gs->dpy, gs->config, EGL_NO_CONTEXT, ctx_attribs);
    if (gs->ctx == EGL_NO_CONTEXT) {
        glx.eglTerminate(gs->dpy); close(gs->fd); free(gs); glx_unload(); return false;
    }

    gs->surf = glx.eglCreateWindowSurface(gs->dpy, gs->config, (EGLNativeWindowType)(uintptr_t)gs->window, NULL);
    if (gs->surf == EGL_NO_SURFACE) {
        glx.eglDestroyContext(gs->dpy, gs->ctx); glx.eglTerminate(gs->dpy); close(gs->fd); free(gs); glx_unload(); return false;
    }

    if (!glx.eglMakeCurrent(gs->dpy, gs->surf, gs->surf, gs->ctx)) {
        glx.eglDestroySurface(gs->dpy, gs->surf); glx.eglDestroyContext(gs->dpy, gs->ctx);
        glx.eglTerminate(gs->dpy); close(gs->fd); free(gs); glx_unload(); return false;
    }

    GLuint vs = glx_compile_shader(GL_VERTEX_SHADER, glx_vs_src);
    GLuint fs = glx_compile_shader(GL_FRAGMENT_SHADER, glx_fs_src);
    if (!vs || !fs) {
        glx.eglMakeCurrent(gs->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        glx.eglDestroySurface(gs->dpy, gs->surf); glx.eglDestroyContext(gs->dpy, gs->ctx);
        glx.eglTerminate(gs->dpy); close(gs->fd); free(gs); glx_unload(); return false;
    }
    gs->program = glx_link_program(vs, fs);
    glx.glDeleteShader(vs); glx.glDeleteShader(fs);
    if (!gs->program) {
        glx.eglMakeCurrent(gs->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        glx.eglDestroySurface(gs->dpy, gs->surf); glx.eglDestroyContext(gs->dpy, gs->ctx);
        glx.eglTerminate(gs->dpy); close(gs->fd); free(gs); glx_unload(); return false;
    }
    gs->u_tex_loc = glx.glGetUniformLocation(gs->program, "u_tex");

    glx.glGenBuffers(1, &gs->vbo);
    glx.glBindBuffer(GL_ARRAY_BUFFER, gs->vbo);
    glx.glBufferData(GL_ARRAY_BUFFER, sizeof(glx_quad_verts), glx_quad_verts, GL_STATIC_DRAW);

    glx.glGenTextures(1, &gs->tex);
    glx.glBindTexture(GL_TEXTURE_2D, gs->tex);
    glx.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glx.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glx.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glx.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glx.glViewport(0, 0, w, h);
    glx.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glx.eglSwapInterval(gs->dpy, 1);

    be->canvas = ui_canvas_new_fb(w, h);
    if (!be->canvas) {
        glx.eglMakeCurrent(gs->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        glx.eglDestroySurface(gs->dpy, gs->surf); glx.eglDestroyContext(gs->dpy, gs->ctx);
        glx.eglTerminate(gs->dpy); close(gs->fd); free(gs); glx_unload(); return false;
    }
    be->user_data = gs;
    be->supports_mouse = true;
    be->supports_color = true;
    be->supports_unicode = false;
    be->max_colors = 0xFFFFFF;
    return true;
}

static void be_gl_x11_shutdown(ui_backend_t *be) {
    if (!be) return;
    glx_state_t *gs = (glx_state_t *)be->user_data;
    if (gs) {
        if (gs->program) glx.glDeleteProgram(gs->program);
        if (gs->tex) glx.glDeleteTextures(1, &gs->tex);
        if (gs->vbo) glx.glDeleteBuffers(1, &gs->vbo);
        glx.eglMakeCurrent(gs->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (gs->surf != EGL_NO_SURFACE) glx.eglDestroySurface(gs->dpy, gs->surf);
        if (gs->ctx != EGL_NO_CONTEXT) glx.eglDestroyContext(gs->dpy, gs->ctx);
        if (gs->dpy != EGL_NO_DISPLAY) glx.eglTerminate(gs->dpy);
        if (gs->fd >= 0) close(gs->fd);
        free(gs);
    }
    if (be->canvas) { ui_canvas_free(be->canvas); be->canvas = NULL; }
    glx_unload();
}

static void be_gl_x11_present(ui_backend_t *be) {
    if (!be || !be->canvas || be->canvas->type != UI_CANVAS_FB) return;
    glx_state_t *gs = (glx_state_t *)be->user_data;
    if (!gs) return;
    int w = be->canvas->w, h = be->canvas->h;
    glx.glBindTexture(GL_TEXTURE_2D, gs->tex);
    glx.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, be->canvas->pixels);
    glx.glClear(GL_COLOR_BUFFER_BIT);
    glx.glUseProgram(gs->program);
    glx.glActiveTexture(GL_TEXTURE0);
    glx.glBindTexture(GL_TEXTURE_2D, gs->tex);
    glx.glUniform1i(gs->u_tex_loc, 0);
    glx.glBindBuffer(GL_ARRAY_BUFFER, gs->vbo);
    glx.glEnableVertexAttribArray(0);
    glx.glEnableVertexAttribArray(1);
    glx.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const GLvoid *)0);
    glx.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const GLvoid *)(2 * sizeof(GLfloat)));
    glx.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glx.eglSwapBuffers(gs->dpy, gs->surf);
}

static bool be_gl_x11_poll_event(ui_backend_t *be, ui_event_t *out, int timeout_ms) {
    if (!be || !be->user_data || !out) return false;
    glx_state_t *gs = (glx_state_t *)be->user_data;
    if (gs->closed) { out->type = UI_EVENT_QUIT; return true; }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(gs->fd, &rfds);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int ret = select(gs->fd + 1, &rfds, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
    if (ret <= 0) return false;

    uint8_t ev[32];
    if (!x11_read_all(gs->fd, ev, 32)) return false;
    uint8_t code = ev[0] & 0x7F;
    memset(out, 0, sizeof(*out));

    switch (code) {
    case X_KeyPress: {
        out->type = UI_EVENT_KEY;
        uint8_t keycode = ev[1];
        switch (keycode) {
        case 36: out->key.key = UI_KEY_ENTER; break;
        case 23: out->key.key = UI_KEY_TAB; break;
        case 22: out->key.key = UI_KEY_BACKSPACE; break;
        case 119: out->key.key = UI_KEY_DELETE; break;
        case 9:  out->key.key = UI_KEY_ESCAPE; break;
        case 65: out->key.key = UI_KEY_SPACE; break;
        case 111: out->key.key = UI_KEY_UP; break;
        case 116: out->key.key = UI_KEY_DOWN; break;
        case 113: out->key.key = UI_KEY_LEFT; break;
        case 114: out->key.key = UI_KEY_RIGHT; break;
        case 110: out->key.key = UI_KEY_HOME; break;
        case 115: out->key.key = UI_KEY_END; break;
        case 112: out->key.key = UI_KEY_PAGE_UP; break;
        case 117: out->key.key = UI_KEY_PAGE_DOWN; break;
        case 118: out->key.key = UI_KEY_INSERT; break;
        default: out->key.key = UI_KEY_NONE; break;
        }
        out->key.ctrl = (ev[28] & 4) != 0;
        out->key.alt  = (ev[28] & 8) != 0;
        out->key.shift = (ev[28] & 1) != 0;
        return true;
    }
    case X_ButtonPress:
    case X_ButtonRelease: {
        out->type = (code == X_ButtonPress) ? UI_EVENT_MOUSE_PRESS : UI_EVENT_MOUSE_RELEASE;
        int16_t rx = (int16_t)(((uint16_t)ev[25] << 8) | ev[24]);
        int16_t ry = (int16_t)(((uint16_t)ev[27] << 8) | ev[26]);
        out->mouse.x = rx; out->mouse.y = ry;
        out->mouse.button = ev[1];
        return true;
    }
    case X_MotionNotify: {
        out->type = UI_EVENT_MOUSE_MOVE;
        int16_t rx = (int16_t)(((uint16_t)ev[25] << 8) | ev[24]);
        int16_t ry = (int16_t)(((uint16_t)ev[27] << 8) | ev[26]);
        out->mouse.x = rx; out->mouse.y = ry;
        return true;
    }
    case X_ClientMessage: {
        uint32_t atom = ((uint32_t)ev[12] << 0) | ((uint32_t)ev[13] << 8) |
                        ((uint32_t)ev[14] << 16) | ((uint32_t)ev[15] << 24);
        if (gs->wm_delete_atom && atom == gs->wm_delete_atom) {
            gs->closed = true;
            out->type = UI_EVENT_QUIT;
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

ui_backend_t *ui_backend_gl_x11_new(void) {
    ui_backend_t *be = calloc(1, sizeof(ui_backend_t));
    if (!be) return NULL;
    be->name = "gl-x11";
    be->init = be_gl_x11_init;
    be->shutdown = be_gl_x11_shutdown;
    be->poll_event = be_gl_x11_poll_event;
    be->present = be_gl_x11_present;
    return be;
}

#else /* non-POSIX */

ui_backend_t *ui_backend_gl_x11_new(void) {
    return NULL;
}

#endif
