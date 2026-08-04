/*
 * ORDL UI — Wayland + EGL windowed GPU compositor backend
 * Runtime-loaded libwayland-client + libwayland-egl + EGL + GLESv2.
 * Zero linked dependencies.
 *
 * Uses wl_shell (deprecated but universally supported) for window
 * toplevel, avoiding the need for xdg-shell protocol code generation.
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/poll.h>

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

/* -------------------------------------------------------------------------- */
/* libwayland-client types & function pointers                                */
/* -------------------------------------------------------------------------- */

struct wl_proxy;       struct wl_display;     struct wl_registry;
struct wl_compositor;  struct wl_surface;     struct wl_shell;
struct wl_shell_surface; struct wl_seat;      struct wl_keyboard;
struct wl_pointer;
struct wl_interface;

struct wl_registry_listener {
    void (*global)(void *data, struct wl_registry *registry,
                   uint32_t name, const char *interface, uint32_t version);
    void (*global_remove)(void *data, struct wl_registry *registry, uint32_t name);
};

struct wl_shell_surface_listener {
    void (*ping)(void *data, struct wl_shell_surface *shell_surface, uint32_t serial);
    void (*configure)(void *data, struct wl_shell_surface *shell_surface,
                      uint32_t edges, int32_t width, int32_t height);
    void (*popup_done)(void *data, struct wl_shell_surface *shell_surface);
};

static struct {
    void *handle;
    struct wl_display *(*display_connect)(const char *name);
    void (*display_disconnect)(struct wl_display *display);
    int  (*display_get_fd)(struct wl_display *display);
    int  (*display_dispatch)(struct wl_display *display);
    int  (*display_roundtrip)(struct wl_display *display);
    int  (*display_flush)(struct wl_display *display);
    struct wl_registry *(*display_get_registry)(struct wl_display *display);
    uint32_t (*proxy_get_id)(struct wl_proxy *proxy);
    int  (*proxy_add_listener)(struct wl_proxy *proxy,
                               void (**implementation)(void), void *data);
    void (*proxy_destroy)(struct wl_proxy *proxy);
    void (*proxy_marshal)(struct wl_proxy *proxy, uint32_t opcode, ...);
    struct wl_proxy *(*proxy_create)(struct wl_proxy *factory,
                                     const struct wl_interface *interface);
    const struct wl_interface *if_compositor;
    const struct wl_interface *if_shell;
    const struct wl_interface *if_shell_surface;
    const struct wl_interface *if_surface;
    const struct wl_interface *if_registry;
    const struct wl_interface *if_seat;
} wl;

static bool wl_load(void) {
    wl.handle = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!wl.handle) wl.handle = dlopen("libwayland-client.so", RTLD_LAZY | RTLD_LOCAL);
    if (!wl.handle) return false;

    #define LOAD(n) *(void **)(&wl.n) = dlsym(wl.handle, #n)
    LOAD(display_connect); LOAD(display_disconnect); LOAD(display_get_fd);
    LOAD(display_dispatch); LOAD(display_roundtrip); LOAD(display_flush);
    LOAD(display_get_registry); LOAD(proxy_get_id); LOAD(proxy_add_listener);
    LOAD(proxy_destroy); LOAD(proxy_marshal); LOAD(proxy_create);
    #undef LOAD

    wl.if_compositor      = (const struct wl_interface *)dlsym(wl.handle, "wl_compositor_interface");
    wl.if_shell           = (const struct wl_interface *)dlsym(wl.handle, "wl_shell_interface");
    wl.if_shell_surface   = (const struct wl_interface *)dlsym(wl.handle, "wl_shell_surface_interface");
    wl.if_surface         = (const struct wl_interface *)dlsym(wl.handle, "wl_surface_interface");
    wl.if_registry        = (const struct wl_interface *)dlsym(wl.handle, "wl_registry_interface");
    wl.if_seat            = (const struct wl_interface *)dlsym(wl.handle, "wl_seat_interface");

    return wl.display_connect && wl.display_get_registry && wl.if_compositor && wl.if_shell;
}

static void wl_unload(void) {
    if (wl.handle) { dlclose(wl.handle); wl.handle = NULL; }
    memset(&wl, 0, sizeof(wl));
}

/* -------------------------------------------------------------------------- */
/* libwayland-egl function pointers                                           */
/* -------------------------------------------------------------------------- */

struct wl_egl_window;

static struct {
    void *handle;
    struct wl_egl_window *(*create)(struct wl_surface *surface, int width, int height);
    void (*destroy)(struct wl_egl_window *window);
    void (*resize)(struct wl_egl_window *window, int width, int height, int dx, int dy);
} wle;

static bool wle_load(void) {
    wle.handle = dlopen("libwayland-egl.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!wle.handle) wle.handle = dlopen("libwayland-egl.so", RTLD_LAZY | RTLD_LOCAL);
    if (!wle.handle) return false;
    *(void **)(&wle.create)  = dlsym(wle.handle, "wl_egl_window_create");
    *(void **)(&wle.destroy) = dlsym(wle.handle, "wl_egl_window_destroy");
    *(void **)(&wle.resize)  = dlsym(wle.handle, "wl_egl_window_resize");
    return wle.create && wle.destroy;
}

static void wle_unload(void) {
    if (wle.handle) { dlclose(wle.handle); wle.handle = NULL; }
    memset(&wle, 0, sizeof(wle));
}

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
} glw;

static bool glw_load(void) {
    glw.egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!glw.egl) glw.egl = dlopen("libEGL.so", RTLD_LAZY | RTLD_LOCAL);
    if (!glw.egl) return false;
    glw.gles = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!glw.gles) glw.gles = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_LOCAL);
    if (!glw.gles) { dlclose(glw.egl); glw.egl = NULL; return false; }

    #define LOAD_EGL(n) *(void **)(&glw.n) = dlsym(glw.egl, #n)
    #define LOAD_GLES(n) *(void **)(&glw.n) = dlsym(glw.gles, #n)
    LOAD_EGL(eglGetDisplay); LOAD_EGL(eglInitialize); LOAD_EGL(eglChooseConfig);
    LOAD_EGL(eglCreateWindowSurface); LOAD_EGL(eglDestroySurface);
    LOAD_EGL(eglCreateContext); LOAD_EGL(eglDestroyContext);
    LOAD_EGL(eglMakeCurrent); LOAD_EGL(eglSwapBuffers); LOAD_EGL(eglBindAPI);
    LOAD_EGL(eglSwapInterval); LOAD_EGL(eglTerminate);
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
    return glw.eglGetDisplay && glw.eglInitialize && glw.glViewport && glw.glCreateShader;
}

static void glw_unload(void) {
    if (glw.gles) { dlclose(glw.gles); glw.gles = NULL; }
    if (glw.egl)  { dlclose(glw.egl);  glw.egl = NULL; }
    memset(&glw, 0, sizeof(glw));
}

/* -------------------------------------------------------------------------- */
/* Shader helpers                                                             */
/* -------------------------------------------------------------------------- */

static GLuint glw_compile_shader(GLenum type, const char *src) {
    GLuint s = glw.glCreateShader(type);
    if (!s) return 0;
    GLint len = (GLint)strlen(src);
    glw.glShaderSource(s, 1, &src, &len);
    glw.glCompileShader(s);
    GLint status = 0;
    glw.glGetShaderiv(s, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512]; GLsizei n;
        glw.glGetShaderInfoLog(s, sizeof(log), &n, log);
        fprintf(stderr, "[GLW] shader compile error: %s\n", log);
        glw.glDeleteShader(s); return 0;
    }
    return s;
}

static GLuint glw_link_program(GLuint vs, GLuint fs) {
    GLuint p = glw.glCreateProgram();
    if (!p) return 0;
    glw.glAttachShader(p, vs); glw.glAttachShader(p, fs);
    glw.glLinkProgram(p);
    GLint status = 0;
    glw.glGetProgramiv(p, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512]; GLsizei n;
        glw.glGetProgramInfoLog(p, sizeof(log), &n, log);
        fprintf(stderr, "[GLW] program link error: %s\n", log);
        glw.glDeleteProgram(p); return 0;
    }
    return p;
}

static const char *glw_vs_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; }\n";

static const char *glw_fs_src =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

static const GLfloat glw_quad_verts[] = {
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
     1.0f,  1.0f,  1.0f, 1.0f,
};

/* -------------------------------------------------------------------------- */
/* Combined state                                                             */
/* -------------------------------------------------------------------------- */

typedef struct {
    struct wl_display *display;
    struct wl_surface *surface;
    struct wl_shell_surface *shell_surface;
    struct wl_egl_window *egl_window;

    EGLDisplay dpy;
    EGLSurface surf;
    EGLContext ctx;
    EGLConfig config;
    GLuint program;
    GLuint tex;
    GLuint vbo;
    GLint u_tex_loc;
    int width, height;
    bool closed;
} glw_state_t;

/* Registry listener data */
typedef struct {
    struct wl_compositor *compositor;
    struct wl_shell *shell;
    struct wl_seat *seat;
    uint32_t compositor_name;
    uint32_t shell_name;
    uint32_t seat_name;
} registry_data_t;

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version) {
    (void)registry;
    registry_data_t *rd = (registry_data_t *)data;
    if (strcmp(interface, "wl_compositor") == 0) {
        rd->compositor_name = name;
        rd->compositor = (struct wl_compositor *)wl.proxy_create(
            (struct wl_proxy *)registry, wl.if_compositor);
        if (rd->compositor) {
            wl.proxy_marshal((struct wl_proxy *)registry, 0, name,
                             "wl_compositor", version < 4 ? version : 4,
                             wl.proxy_get_id((struct wl_proxy *)rd->compositor));
        }
    } else if (strcmp(interface, "wl_shell") == 0) {
        rd->shell_name = name;
        rd->shell = (struct wl_shell *)wl.proxy_create(
            (struct wl_proxy *)registry, wl.if_shell);
        if (rd->shell) {
            wl.proxy_marshal((struct wl_proxy *)registry, 0, name,
                             "wl_shell", version < 1 ? version : 1,
                             wl.proxy_get_id((struct wl_proxy *)rd->shell));
        }
    } else if (strcmp(interface, "wl_seat") == 0) {
        rd->seat_name = name;
        rd->seat = (struct wl_seat *)wl.proxy_create(
            (struct wl_proxy *)registry, wl.if_seat);
        if (rd->seat) {
            wl.proxy_marshal((struct wl_proxy *)registry, 0, name,
                             "wl_seat", version < 5 ? version : 5,
                             wl.proxy_get_id((struct wl_proxy *)rd->seat));
        }
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static void shell_surface_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial) {
    (void)data;
    wl.proxy_marshal((struct wl_proxy *)shell_surface, 0, serial);
}

static void shell_surface_configure(void *data, struct wl_shell_surface *shell_surface,
                                    uint32_t edges, int32_t width, int32_t height) {
    (void)shell_surface; (void)edges;
    glw_state_t *gs = (glw_state_t *)data;
    if (width > 0 && height > 0) {
        gs->width = width;
        gs->height = height;
        if (gs->egl_window) wle.resize(gs->egl_window, width, height, 0, 0);
        if (gs->dpy != EGL_NO_DISPLAY) glw.glViewport(0, 0, width, height);
    }
}

static void shell_surface_popup_done(void *data, struct wl_shell_surface *shell_surface) {
    (void)data; (void)shell_surface;
}

/* -------------------------------------------------------------------------- */
/* Backend vtable                                                             */
/* -------------------------------------------------------------------------- */

static bool be_gl_wayland_init(ui_backend_t *be, int w, int h) {
    if (!wl_load() || !wle_load() || !glw_load()) {
        wl_unload(); wle_unload(); glw_unload();
        return false;
    }

    glw_state_t *gs = calloc(1, sizeof(glw_state_t));
    if (!gs) { wl_unload(); wle_unload(); glw_unload(); return false; }
    gs->width = w; gs->height = h;

    gs->display = wl.display_connect(NULL);
    if (!gs->display) { free(gs); wl_unload(); wle_unload(); glw_unload(); return false; }

    struct wl_registry *registry = wl.display_get_registry(gs->display);
    if (!registry) {
        wl.display_disconnect(gs->display); free(gs);
        wl_unload(); wle_unload(); glw_unload(); return false;
    }

    registry_data_t rd = {0};
    struct wl_registry_listener reg_listener = { registry_global, registry_global_remove };
    wl.proxy_add_listener((struct wl_proxy *)registry,
                          (void (**)(void))&reg_listener, &rd);
    wl.display_roundtrip(gs->display);

    if (!rd.compositor || !rd.shell) {
        wl.proxy_destroy((struct wl_proxy *)registry);
        wl.display_disconnect(gs->display); free(gs);
        wl_unload(); wle_unload(); glw_unload(); return false;
    }

    /* Create surface */
    gs->surface = (struct wl_surface *)wl.proxy_create(
        (struct wl_proxy *)rd.compositor, wl.if_surface);
    if (!gs->surface) {
        wl.proxy_destroy((struct wl_proxy *)registry);
        wl.display_disconnect(gs->display); free(gs);
        wl_unload(); wle_unload(); glw_unload(); return false;
    }
    wl.proxy_marshal((struct wl_proxy *)rd.compositor, 0,
                     wl.proxy_get_id((struct wl_proxy *)gs->surface));

    /* Create shell surface */
    gs->shell_surface = (struct wl_shell_surface *)wl.proxy_create(
        (struct wl_proxy *)rd.shell, wl.if_shell_surface);
    if (!gs->shell_surface) {
        wl.proxy_destroy((struct wl_proxy *)gs->surface);
        wl.proxy_destroy((struct wl_proxy *)registry);
        wl.display_disconnect(gs->display); free(gs);
        wl_unload(); wle_unload(); glw_unload(); return false;
    }
    wl.proxy_marshal((struct wl_proxy *)rd.shell, 0,
                     wl.proxy_get_id((struct wl_proxy *)gs->surface),
                     wl.proxy_get_id((struct wl_proxy *)gs->shell_surface));

    struct wl_shell_surface_listener sh_listener = {
        shell_surface_ping, shell_surface_configure, shell_surface_popup_done
    };
    wl.proxy_add_listener((struct wl_proxy *)gs->shell_surface,
                          (void (**)(void))&sh_listener, gs);

    wl.proxy_marshal((struct wl_proxy *)gs->shell_surface, 1); /* set_toplevel = opcode 1 */
    wl.proxy_marshal((struct wl_proxy *)gs->shell_surface, 6, "ORDL UI"); /* set_title = opcode 6 */

    wl.display_roundtrip(gs->display);
    wl.proxy_destroy((struct wl_proxy *)registry);

    /* EGL setup */
    gs->dpy = glw.eglGetDisplay((EGLNativeDisplayType)gs->display);
    if (gs->dpy == EGL_NO_DISPLAY) {
        wl.proxy_destroy((struct wl_proxy *)gs->shell_surface);
        wl.proxy_destroy((struct wl_proxy *)gs->surface);
        wl.display_disconnect(gs->display); free(gs);
        wl_unload(); wle_unload(); glw_unload(); return false;
    }

    EGLint maj, min;
    if (!glw.eglInitialize(gs->dpy, &maj, &min)) {
        glw.eglTerminate(gs->dpy);
        wl.proxy_destroy((struct wl_proxy *)gs->shell_surface);
        wl.proxy_destroy((struct wl_proxy *)gs->surface);
        wl.display_disconnect(gs->display); free(gs);
        wl_unload(); wle_unload(); glw_unload(); return false;
    }
    if (!glw.eglBindAPI(EGL_OPENGL_ES_API)) {
        glw.eglTerminate(gs->dpy);
        wl.proxy_destroy((struct wl_proxy *)gs->shell_surface);
        wl.proxy_destroy((struct wl_proxy *)gs->surface);
        wl.display_disconnect(gs->display); free(gs);
        wl_unload(); wle_unload(); glw_unload(); return false;
    }

    EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint nconfig;
    if (!glw.eglChooseConfig(gs->dpy, attribs, &gs->config, 1, &nconfig) || nconfig < 1) {
        glw.eglTerminate(gs->dpy);
        wl.proxy_destroy((struct wl_proxy *)gs->shell_surface);
        wl.proxy_destroy((struct wl_proxy *)gs->surface);
        wl.display_disconnect(gs->display); free(gs);
        wl_unload(); wle_unload(); glw_unload(); return false;
    }

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    gs->ctx = glw.eglCreateContext(gs->dpy, gs->config, EGL_NO_CONTEXT, ctx_attribs);
    if (gs->ctx == EGL_NO_CONTEXT) {
        glw.eglTerminate(gs->dpy);
        wl.proxy_destroy((struct wl_proxy *)gs->shell_surface);
        wl.proxy_destroy((struct wl_proxy *)gs->surface);
        wl.display_disconnect(gs->display); free(gs);
        wl_unload(); wle_unload(); glw_unload(); return false;
    }

    gs->egl_window = wle.create(gs->surface, w, h);
    if (!gs->egl_window) {
        glw.eglDestroyContext(gs->dpy, gs->ctx); glw.eglTerminate(gs->dpy);
        wl.proxy_destroy((struct wl_proxy *)gs->shell_surface);
        wl.proxy_destroy((struct wl_proxy *)gs->surface);
        wl.display_disconnect(gs->display); free(gs);
        wl_unload(); wle_unload(); glw_unload(); return false;
    }

    gs->surf = glw.eglCreateWindowSurface(gs->dpy, gs->config,
                                          (EGLNativeWindowType)gs->egl_window, NULL);
    if (gs->surf == EGL_NO_SURFACE) {
        wle.destroy(gs->egl_window);
        glw.eglDestroyContext(gs->dpy, gs->ctx); glw.eglTerminate(gs->dpy);
        wl.proxy_destroy((struct wl_proxy *)gs->shell_surface);
        wl.proxy_destroy((struct wl_proxy *)gs->surface);
        wl.display_disconnect(gs->display); free(gs);
        wl_unload(); wle_unload(); glw_unload(); return false;
    }

    if (!glw.eglMakeCurrent(gs->dpy, gs->surf, gs->surf, gs->ctx)) {
        glw.eglDestroySurface(gs->dpy, gs->surf); wle.destroy(gs->egl_window);
        glw.eglDestroyContext(gs->dpy, gs->ctx); glw.eglTerminate(gs->dpy);
        wl.proxy_destroy((struct wl_proxy *)gs->shell_surface);
        wl.proxy_destroy((struct wl_proxy *)gs->surface);
        wl.display_disconnect(gs->display); free(gs);
        wl_unload(); wle_unload(); glw_unload(); return false;
    }

    GLuint vs = glw_compile_shader(GL_VERTEX_SHADER, glw_vs_src);
    GLuint fs = glw_compile_shader(GL_FRAGMENT_SHADER, glw_fs_src);
    if (!vs || !fs) {
        glw.eglMakeCurrent(gs->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        glw.eglDestroySurface(gs->dpy, gs->surf); wle.destroy(gs->egl_window);
        glw.eglDestroyContext(gs->dpy, gs->ctx); glw.eglTerminate(gs->dpy);
        wl.proxy_destroy((struct wl_proxy *)gs->shell_surface);
        wl.proxy_destroy((struct wl_proxy *)gs->surface);
        wl.display_disconnect(gs->display); free(gs);
        wl_unload(); wle_unload(); glw_unload(); return false;
    }
    gs->program = glw_link_program(vs, fs);
    glw.glDeleteShader(vs); glw.glDeleteShader(fs);
    if (!gs->program) {
        glw.eglMakeCurrent(gs->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        glw.eglDestroySurface(gs->dpy, gs->surf); wle.destroy(gs->egl_window);
        glw.eglDestroyContext(gs->dpy, gs->ctx); glw.eglTerminate(gs->dpy);
        wl.proxy_destroy((struct wl_proxy *)gs->shell_surface);
        wl.proxy_destroy((struct wl_proxy *)gs->surface);
        wl.display_disconnect(gs->display); free(gs);
        wl_unload(); wle_unload(); glw_unload(); return false;
    }
    gs->u_tex_loc = glw.glGetUniformLocation(gs->program, "u_tex");

    glw.glGenBuffers(1, &gs->vbo);
    glw.glBindBuffer(GL_ARRAY_BUFFER, gs->vbo);
    glw.glBufferData(GL_ARRAY_BUFFER, sizeof(glw_quad_verts), glw_quad_verts, GL_STATIC_DRAW);

    glw.glGenTextures(1, &gs->tex);
    glw.glBindTexture(GL_TEXTURE_2D, gs->tex);
    glw.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glw.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glw.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glw.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glw.glViewport(0, 0, w, h);
    glw.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glw.eglSwapInterval(gs->dpy, 1);

    be->canvas = ui_canvas_new_fb(w, h);
    if (!be->canvas) {
        glw.eglMakeCurrent(gs->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        glw.eglDestroySurface(gs->dpy, gs->surf); wle.destroy(gs->egl_window);
        glw.eglDestroyContext(gs->dpy, gs->ctx); glw.eglTerminate(gs->dpy);
        wl.proxy_destroy((struct wl_proxy *)gs->shell_surface);
        wl.proxy_destroy((struct wl_proxy *)gs->surface);
        wl.display_disconnect(gs->display); free(gs);
        wl_unload(); wle_unload(); glw_unload(); return false;
    }
    be->user_data = gs;
    be->supports_mouse = true;
    be->supports_color = true;
    be->supports_unicode = false;
    be->max_colors = 0xFFFFFF;
    return true;
}

static void be_gl_wayland_shutdown(ui_backend_t *be) {
    if (!be) return;
    glw_state_t *gs = (glw_state_t *)be->user_data;
    if (gs) {
        if (gs->program) glw.glDeleteProgram(gs->program);
        if (gs->tex) glw.glDeleteTextures(1, &gs->tex);
        if (gs->vbo) glw.glDeleteBuffers(1, &gs->vbo);
        glw.eglMakeCurrent(gs->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (gs->surf != EGL_NO_SURFACE) glw.eglDestroySurface(gs->dpy, gs->surf);
        if (gs->egl_window) wle.destroy(gs->egl_window);
        if (gs->ctx != EGL_NO_CONTEXT) glw.eglDestroyContext(gs->dpy, gs->ctx);
        if (gs->dpy != EGL_NO_DISPLAY) glw.eglTerminate(gs->dpy);
        if (gs->shell_surface) wl.proxy_destroy((struct wl_proxy *)gs->shell_surface);
        if (gs->surface) wl.proxy_destroy((struct wl_proxy *)gs->surface);
        if (gs->display) wl.display_disconnect(gs->display);
        free(gs);
    }
    if (be->canvas) { ui_canvas_free(be->canvas); be->canvas = NULL; }
    glw_unload(); wle_unload(); wl_unload();
}

static void be_gl_wayland_present(ui_backend_t *be) {
    if (!be || !be->canvas || be->canvas->type != UI_CANVAS_FB) return;
    glw_state_t *gs = (glw_state_t *)be->user_data;
    if (!gs) return;
    int w = be->canvas->w, h = be->canvas->h;
    glw.glBindTexture(GL_TEXTURE_2D, gs->tex);
    glw.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, be->canvas->pixels);
    glw.glClear(GL_COLOR_BUFFER_BIT);
    glw.glUseProgram(gs->program);
    glw.glActiveTexture(GL_TEXTURE0);
    glw.glBindTexture(GL_TEXTURE_2D, gs->tex);
    glw.glUniform1i(gs->u_tex_loc, 0);
    glw.glBindBuffer(GL_ARRAY_BUFFER, gs->vbo);
    glw.glEnableVertexAttribArray(0);
    glw.glEnableVertexAttribArray(1);
    glw.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const GLvoid *)0);
    glw.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const GLvoid *)(2 * sizeof(GLfloat)));
    glw.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glw.eglSwapBuffers(gs->dpy, gs->surf);
}

static bool be_gl_wayland_poll_event(ui_backend_t *be, ui_event_t *out, int timeout_ms) {
    if (!be || !be->user_data || !out) return false;
    glw_state_t *gs = (glw_state_t *)be->user_data;
    if (gs->closed) { out->type = UI_EVENT_QUIT; return true; }

    int fd = wl.display_get_fd(gs->display);
    struct pollfd pfd = { fd, POLLIN, 0 };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return false;

    wl.display_dispatch(gs->display);

    /* After dispatch, check if compositor sent a close event via configure/close.
     * wl_shell doesn't have a direct close event; we rely on window manager signals.
     * For now, return false (no event) and let the user close via compositor.
     * A full keyboard/mouse implementation would require wl_seat listener setup. */
    (void)gs;
    return false;
}

ui_backend_t *ui_backend_gl_wayland_new(void) {
    ui_backend_t *be = calloc(1, sizeof(ui_backend_t));
    if (!be) return NULL;
    be->name = "gl-wayland";
    be->init = be_gl_wayland_init;
    be->shutdown = be_gl_wayland_shutdown;
    be->poll_event = be_gl_wayland_poll_event;
    be->present = be_gl_wayland_present;
    return be;
}

#else /* non-POSIX */

ui_backend_t *ui_backend_gl_wayland_new(void) {
    return NULL;
}

#endif
