/*
 * ORDL UI — OpenGL ES 2.0 GPU compositor backend
 * Runtime-loaded EGL + GLESv2. Zero linked dependencies.
 *
 * Supports offscreen pbuffer rendering and windowed rendering via
 * eglCreateWindowSurface (X11, Wayland, or any EGLNativeWindowType).
 */

#include "forge/ui/ordl_ui.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* Runtime GL function loading                                                */
/* -------------------------------------------------------------------------- */

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

#define GL_GLES_PROTOTYPES 0  /* We load everything at runtime */

/* EGL types */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void *EGLNativeWindowType;
typedef void *EGLNativePixmapType;
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
#define EGL_PBUFFER_BIT     0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT  0x0004
#define EGL_RED_SIZE        0x3024
#define EGL_GREEN_SIZE      0x3023
#define EGL_BLUE_SIZE       0x3022
#define EGL_ALPHA_SIZE      0x3021
#define EGL_DEPTH_SIZE      0x3025
#define EGL_STENCIL_SIZE    0x3026
#define EGL_WIDTH           0x3057
#define EGL_HEIGHT          0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API   0x30A0
#define EGL_DRAW            0x3059
#define EGL_READ            0x305A
#define EGL_SWAP_BEHAVIOR   0x3093
#define EGL_BUFFER_PRESERVED 0x3094

/* GL types */
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
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
#define GL_ELEMENT_ARRAY_BUFFER         0x8893
#define GL_VERTEX_SHADER                0x8B31
#define GL_FRAGMENT_SHADER              0x8B30
#define GL_COMPILE_STATUS               0x8B81
#define GL_LINK_STATUS                  0x8B82
#define GL_INFO_LOG_LENGTH              0x8B84
#define GL_FLOAT                        0x1406
#define GL_UNSIGNED_SHORT               0x1403
#define GL_UNSIGNED_BYTE                0x1401
#define GL_RGBA                         0x1908
#define GL_TRIANGLES                    0x0004
#define GL_TRIANGLE_STRIP               0x0005
#define GL_COLOR_BUFFER_BIT             0x00004000
#define GL_STATIC_DRAW                  0x88E4
#define GL_LINEAR                       0x2601
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_CLAMP_TO_EDGE                0x812F
#define GL_TEXTURE_WRAP_S               0x2802
#define GL_TEXTURE_WRAP_T               0x2803

/* Runtime function pointers */
static struct {
    void *egl, *gles;

    /* EGL */
    EGLDisplay (*eglGetDisplay)(EGLNativeDisplayType);
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint*, EGLint*);
    EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
    EGLSurface (*eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*);
    EGLSurface (*eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*);
    EGLBoolean (*eglDestroySurface)(EGLDisplay, EGLSurface);
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
    EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext);
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*eglSwapBuffers)(EGLDisplay, EGLSurface);
    EGLBoolean (*eglBindAPI)(EGLenum);
    EGLBoolean (*eglSwapInterval)(EGLDisplay, EGLint);
    EGLBoolean (*eglTerminate)(EGLDisplay);
    void *(*eglGetProcAddress)(const char*);

    /* GLES 2.0 */
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
    void (*glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
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
    void (*glGetString)(GLenum);
} gl;

static bool gl_load(void) {
    gl.egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!gl.egl) gl.egl = dlopen("libEGL.so", RTLD_LAZY | RTLD_LOCAL);
    if (!gl.egl) return false;

    gl.gles = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!gl.gles) gl.gles = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_LOCAL);
    if (!gl.gles) { dlclose(gl.egl); gl.egl = NULL; return false; }

    #define LOAD_EGL(name) *(void **)(&gl.name) = dlsym(gl.egl, #name)
    #define LOAD_GLES(name) *(void **)(&gl.name) = dlsym(gl.gles, #name)

    LOAD_EGL(eglGetDisplay);
    LOAD_EGL(eglInitialize);
    LOAD_EGL(eglChooseConfig);
    LOAD_EGL(eglCreateWindowSurface);
    LOAD_EGL(eglCreatePbufferSurface);
    LOAD_EGL(eglDestroySurface);
    LOAD_EGL(eglCreateContext);
    LOAD_EGL(eglDestroyContext);
    LOAD_EGL(eglMakeCurrent);
    LOAD_EGL(eglSwapBuffers);
    LOAD_EGL(eglBindAPI);
    LOAD_EGL(eglSwapInterval);
    LOAD_EGL(eglTerminate);
    LOAD_EGL(eglGetProcAddress);

    LOAD_GLES(glViewport);
    LOAD_GLES(glClearColor);
    LOAD_GLES(glClear);
    LOAD_GLES(glGenTextures);
    LOAD_GLES(glBindTexture);
    LOAD_GLES(glTexImage2D);
    LOAD_GLES(glTexParameteri);
    LOAD_GLES(glGenBuffers);
    LOAD_GLES(glBindBuffer);
    LOAD_GLES(glBufferData);
    LOAD_GLES(glEnableVertexAttribArray);
    LOAD_GLES(glVertexAttribPointer);
    LOAD_GLES(glDrawArrays);
    LOAD_GLES(glActiveTexture);
    LOAD_GLES(glUniform1i);
    LOAD_GLES(glUniformMatrix4fv);
    LOAD_GLES(glUseProgram);
    LOAD_GLES(glCreateShader);
    LOAD_GLES(glShaderSource);
    LOAD_GLES(glCompileShader);
    LOAD_GLES(glGetShaderiv);
    LOAD_GLES(glGetShaderInfoLog);
    LOAD_GLES(glCreateProgram);
    LOAD_GLES(glAttachShader);
    LOAD_GLES(glLinkProgram);
    LOAD_GLES(glGetProgramiv);
    LOAD_GLES(glGetProgramInfoLog);
    LOAD_GLES(glGetUniformLocation);
    LOAD_GLES(glDeleteShader);
    LOAD_GLES(glDeleteProgram);
    LOAD_GLES(glDeleteTextures);
    LOAD_GLES(glDeleteBuffers);
    LOAD_GLES(glGetString);

    #undef LOAD_EGL
    #undef LOAD_GLES

    return gl.eglGetDisplay && gl.eglInitialize && gl.glViewport && gl.glCreateShader;
}

static void gl_unload(void) {
    if (gl.gles) { dlclose(gl.gles); gl.gles = NULL; }
    if (gl.egl)  { dlclose(gl.egl);  gl.egl = NULL; }
    memset(&gl, 0, sizeof(gl));
}

/* -------------------------------------------------------------------------- */
/* Shader helpers                                                             */
/* -------------------------------------------------------------------------- */

static GLuint gl_compile_shader(GLenum type, const char *src) {
    GLuint s = gl.glCreateShader(type);
    if (!s) return 0;
    GLint len = (GLint)strlen(src);
    gl.glShaderSource(s, 1, &src, &len);
    gl.glCompileShader(s);
    GLint status = 0;
    gl.glGetShaderiv(s, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        GLsizei n;
        gl.glGetShaderInfoLog(s, sizeof(log), &n, log);
        fprintf(stderr, "[GL] shader compile error: %s\n", log);
        gl.glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint gl_link_program(GLuint vs, GLuint fs) {
    GLuint p = gl.glCreateProgram();
    if (!p) return 0;
    gl.glAttachShader(p, vs);
    gl.glAttachShader(p, fs);
    gl.glLinkProgram(p);
    GLint status = 0;
    gl.glGetProgramiv(p, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512];
        GLsizei n;
        gl.glGetProgramInfoLog(p, sizeof(log), &n, log);
        fprintf(stderr, "[GL] program link error: %s\n", log);
        gl.glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* Vertex shader: full-screen quad with UVs */
static const char *vs_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "}\n";

/* Fragment shader: sample from texture */
static const char *fs_src =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(u_tex, v_uv);\n"
    "}\n";

/* -------------------------------------------------------------------------- */
/* GL backend state                                                           */
/* -------------------------------------------------------------------------- */

typedef enum {
    GL_SURFACE_NONE,
    GL_SURFACE_X11,
    GL_SURFACE_DRM,
    GL_SURFACE_WINDOWED,
} gl_surface_type_t;

typedef struct {
    gl_surface_type_t surface_type;
    EGLDisplay dpy;
    EGLSurface surf;
    EGLContext ctx;
    EGLConfig config;
    GLuint program;
    GLuint tex;
    GLuint vbo;
    GLint u_tex_loc;
    int width, height;
    uint8_t *readback_buf; /* For DRM pbuffer readback */
} gl_state_t;

/* Fullscreen quad: 2 triangles, clip-space positions + UVs */
static const GLfloat quad_verts[] = {
    /* pos      uv */
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
     1.0f,  1.0f,  1.0f, 1.0f,
};

/* -------------------------------------------------------------------------- */
/* Generic GL init (windowed — eglCreateWindowSurface)                        */
/* -------------------------------------------------------------------------- */

static bool gl_init_windowed(gl_state_t *gs, void *native_window, int w, int h) {
    gs->surface_type = GL_SURFACE_WINDOWED;
    gs->dpy = gl.eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (gs->dpy == EGL_NO_DISPLAY) return false;

    EGLint maj, min;
    if (!gl.eglInitialize(gs->dpy, &maj, &min)) return false;
    if (!gl.eglBindAPI(EGL_OPENGL_ES_API)) return false;

    EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint nconfig;
    if (!gl.eglChooseConfig(gs->dpy, attribs, &config, 1, &nconfig) || nconfig < 1) return false;
    gs->config = config;

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    gs->ctx = gl.eglCreateContext(gs->dpy, config, EGL_NO_CONTEXT, ctx_attribs);
    if (gs->ctx == EGL_NO_CONTEXT) return false;

    gs->surf = gl.eglCreateWindowSurface(gs->dpy, config, (EGLNativeWindowType)native_window, NULL);
    if (gs->surf == EGL_NO_SURFACE) return false;

    if (!gl.eglMakeCurrent(gs->dpy, gs->surf, gs->surf, gs->ctx)) return false;

    /* Compile shaders */
    GLuint vs = gl_compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = gl_compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return false;
    gs->program = gl_link_program(vs, fs);
    gl.glDeleteShader(vs);
    gl.glDeleteShader(fs);
    if (!gs->program) return false;

    gs->u_tex_loc = gl.glGetUniformLocation(gs->program, "u_tex");

    /* Upload quad */
    gl.glGenBuffers(1, &gs->vbo);
    gl.glBindBuffer(GL_ARRAY_BUFFER, gs->vbo);
    gl.glBufferData(GL_ARRAY_BUFFER, sizeof(quad_verts), quad_verts, GL_STATIC_DRAW);

    /* Create texture */
    gl.glGenTextures(1, &gs->tex);
    gl.glBindTexture(GL_TEXTURE_2D, gs->tex);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    gs->width = w;
    gs->height = h;
    /* No readback buffer needed for windowed mode */

    gl.glViewport(0, 0, w, h);
    gl.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    return true;
}

/* -------------------------------------------------------------------------- */
/* Generic GL init (pbuffer offscreen)                                        */
/* -------------------------------------------------------------------------- */

static bool gl_init_offscreen(gl_state_t *gs, int w, int h) {
    gs->dpy = gl.eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (gs->dpy == EGL_NO_DISPLAY) return false;

    EGLint maj, min;
    if (!gl.eglInitialize(gs->dpy, &maj, &min)) return false;
    if (!gl.eglBindAPI(EGL_OPENGL_ES_API)) return false;

    EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint nconfig;
    if (!gl.eglChooseConfig(gs->dpy, attribs, &config, 1, &nconfig) || nconfig < 1) return false;
    gs->config = config;

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    gs->ctx = gl.eglCreateContext(gs->dpy, config, EGL_NO_CONTEXT, ctx_attribs);
    if (gs->ctx == EGL_NO_CONTEXT) return false;

    EGLint pb_attribs[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    gs->surf = gl.eglCreatePbufferSurface(gs->dpy, config, pb_attribs);
    if (gs->surf == EGL_NO_SURFACE) return false;

    if (!gl.eglMakeCurrent(gs->dpy, gs->surf, gs->surf, gs->ctx)) return false;

    /* Compile shaders */
    GLuint vs = gl_compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = gl_compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return false;
    gs->program = gl_link_program(vs, fs);
    gl.glDeleteShader(vs);
    gl.glDeleteShader(fs);
    if (!gs->program) return false;

    gs->u_tex_loc = gl.glGetUniformLocation(gs->program, "u_tex");

    /* Upload quad */
    gl.glGenBuffers(1, &gs->vbo);
    gl.glBindBuffer(GL_ARRAY_BUFFER, gs->vbo);
    gl.glBufferData(GL_ARRAY_BUFFER, sizeof(quad_verts), quad_verts, GL_STATIC_DRAW);

    /* Create texture */
    gl.glGenTextures(1, &gs->tex);
    gl.glBindTexture(GL_TEXTURE_2D, gs->tex);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    gs->width = w;
    gs->height = h;
    if (w <= 0 || h <= 0 || (size_t)w > SIZE_MAX / 4 || (size_t)w * 4 > SIZE_MAX / (size_t)h) return false;
    gs->readback_buf = malloc((size_t)w * (size_t)h * 4);
    if (!gs->readback_buf) return false;

    gl.glViewport(0, 0, w, h);
    gl.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    return true;
}

static void gl_shutdown(gl_state_t *gs) {
    if (!gs) return;
    if (gs->program) gl.glDeleteProgram(gs->program);
    if (gs->tex) gl.glDeleteTextures(1, &gs->tex);
    if (gs->vbo) gl.glDeleteBuffers(1, &gs->vbo);
    if (gs->surf != EGL_NO_SURFACE) gl.eglDestroySurface(gs->dpy, gs->surf);
    if (gs->ctx != EGL_NO_CONTEXT) gl.eglDestroyContext(gs->dpy, gs->ctx);
    if (gs->dpy != EGL_NO_DISPLAY) gl.eglTerminate(gs->dpy);
    free(gs->readback_buf);
    memset(gs, 0, sizeof(*gs));
}

static void gl_present(gl_state_t *gs, const uint32_t *rgba, int w, int h) {
    if (!gs || !rgba) return;

    gl.glBindTexture(GL_TEXTURE_2D, gs->tex);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    gl.glClear(GL_COLOR_BUFFER_BIT);
    gl.glUseProgram(gs->program);

    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, gs->tex);
    gl.glUniform1i(gs->u_tex_loc, 0);

    gl.glBindBuffer(GL_ARRAY_BUFFER, gs->vbo);
    gl.glEnableVertexAttribArray(0);
    gl.glEnableVertexAttribArray(1);
    gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const GLvoid *)0);
    gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const GLvoid *)(2 * sizeof(GLfloat)));

    gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    gl.eglSwapBuffers(gs->dpy, gs->surf);
}

/* -------------------------------------------------------------------------- */
/* Backend vtable                                                             */
/* -------------------------------------------------------------------------- */

static bool be_gl_windowed_init(ui_backend_t *be, int w, int h) {
    /* Native window handle is passed via be->user_data before init is called */
    void *native_window = be->user_data;
    be->user_data = NULL;

    if (!gl_load()) return false;
    gl_state_t *gs = calloc(1, sizeof(gl_state_t));
    if (!gs) return false;

    if (!gl_init_windowed(gs, native_window, w, h)) {
        free(gs);
        gl_unload();
        return false;
    }

    be->canvas = ui_canvas_new_fb(w, h);
    if (!be->canvas) {
        gl_shutdown(gs);
        free(gs);
        gl_unload();
        return false;
    }

    be->user_data = gs;
    be->supports_mouse = true;  /* Windowed GL assumes an input-capable backend */
    be->supports_color = true;
    be->supports_unicode = false;
    be->max_colors = 0xFFFFFF;
    return true;
}

static bool be_gl_init(ui_backend_t *be, int w, int h) {
    if (!gl_load()) return false;
    gl_state_t *gs = calloc(1, sizeof(gl_state_t));
    if (!gs) return false;

    if (!gl_init_offscreen(gs, w, h)) {
        free(gs);
        gl_unload();
        return false;
    }

    be->canvas = ui_canvas_new_fb(w, h);
    if (!be->canvas) {
        gl_shutdown(gs);
        free(gs);
        gl_unload();
        return false;
    }

    be->user_data = gs;
    be->supports_mouse = false; /* Offscreen GL doesn't handle input directly */
    be->supports_color = true;
    be->supports_unicode = false;
    be->max_colors = 0xFFFFFF;
    return true;
}

static void be_gl_shutdown(ui_backend_t *be) {
    if (!be) return;
    gl_state_t *gs = (gl_state_t *)be->user_data;
    if (gs) {
        gl_shutdown(gs);
        free(gs);
    }
    if (be->canvas) { ui_canvas_free(be->canvas); be->canvas = NULL; }
    gl_unload();
}

static void be_gl_present(ui_backend_t *be) {
    if (!be || !be->canvas || be->canvas->type != UI_CANVAS_FB) return;
    gl_state_t *gs = (gl_state_t *)be->user_data;
    if (!gs) return;
    gl_present(gs, be->canvas->pixels, be->canvas->w, be->canvas->h);
}

static bool be_gl_poll_event(ui_backend_t *be, ui_event_t *out, int timeout_ms) {
    (void)be; (void)timeout_ms;
    (void)out;
    return false; /* Offscreen GL has no direct input; use alongside another backend */
}

ui_backend_t *ui_backend_gl_new_with_window(void *native_window) {
    ui_backend_t *be = calloc(1, sizeof(ui_backend_t));
    if (!be) return NULL;
    be->name = "gl-windowed";
    /* Pass native window handle through user_data; init will pick it up */
    be->user_data = native_window;
    be->init = be_gl_windowed_init;
    be->shutdown = be_gl_shutdown;
    be->poll_event = be_gl_poll_event;
    be->present = be_gl_present;
    return be;
}

ui_backend_t *ui_backend_gl_new(void) {
    ui_backend_t *be = calloc(1, sizeof(ui_backend_t));
    if (!be) return NULL;
    be->name = "gl";
    be->init = be_gl_init;
    be->shutdown = be_gl_shutdown;
    be->poll_event = be_gl_poll_event;
    be->present = be_gl_present;
    return be;
}

#else /* non-Linux */

ui_backend_t *ui_backend_gl_new_with_window(void *native_window) {
    (void)native_window;
    return NULL;
}

ui_backend_t *ui_backend_gl_new(void) {
    return NULL;
}

#endif
