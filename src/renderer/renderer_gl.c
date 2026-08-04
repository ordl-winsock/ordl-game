/*
 * src/renderer/renderer_gl.c — GPU-accelerated 2D sprite renderer
 *
 * Streams batched sprites through a dynamic VBO with per-texture draw calls.
 * Compatible with OpenGL ES 2.0 (baseline) and desktop GL 3.3+.
 */

#define _GNU_SOURCE
#include "forge/renderer_gl.h"
#include "forge/memory.h"
#include "forge/log.h"

#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------- */
/* GL types & constants                                                       */
/* -------------------------------------------------------------------------- */

typedef unsigned int GLbitfield;
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef void GLvoid;
typedef char GLchar;
typedef ptrdiff_t GLintptr;
typedef ptrdiff_t GLsizeiptr;
typedef unsigned char GLubyte;

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
#define GL_UNSIGNED_BYTE                0x1401
#define GL_UNSIGNED_SHORT               0x1403
#define GL_RGBA                         0x1908
#define GL_TRIANGLES                    0x0004
#define GL_COLOR_BUFFER_BIT             0x00004000
#define GL_STREAM_DRAW                  0x88E0
#define GL_STATIC_DRAW                  0x88E4
#define GL_LINEAR                       0x2601
#define GL_NEAREST                      0x2600
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_CLAMP_TO_EDGE                0x812F
#define GL_TEXTURE_WRAP_S               0x2802
#define GL_TEXTURE_WRAP_T               0x2803
#define GL_BLEND                        0x0BE2
#define GL_SRC_ALPHA                    0x0302
#define GL_ONE_MINUS_SRC_ALPHA          0x0303
#define GL_ONE                          1
#define GL_FUNC_ADD                     0x8006
#define GL_TEXTURE_MAX_ANISOTROPY_EXT   0x84FE
#define GL_UNPACK_ALIGNMENT             0x0CF5
#define GL_TEXTURE_BASE_LEVEL           0x813C
#define GL_TEXTURE_MAX_LEVEL            0x813D

/* -------------------------------------------------------------------------- */
/* Runtime GL function table                                                  */
/* -------------------------------------------------------------------------- */

#define GL_FUNC_LIST(X) \
    X(void,        glActiveTexture,      (GLenum)) \
    X(void,        glAttachShader,       (GLuint, GLuint)) \
    X(void,        glBindBuffer,         (GLenum, GLuint)) \
    X(void,        glBindTexture,        (GLenum, GLuint)) \
    X(void,        glBlendFunc,          (GLenum, GLenum)) \
    X(void,        glBufferData,         (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void,        glBufferSubData,      (GLenum, GLintptr, GLsizeiptr, const void *)) \
    X(void,        glClear,              (GLbitfield)) \
    X(void,        glClearColor,         (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void,        glCompileShader,      (GLuint)) \
    X(GLuint,      glCreateProgram,      (void)) \
    X(GLuint,      glCreateShader,       (GLenum)) \
    X(void,        glDeleteBuffers,      (GLsizei, const GLuint *)) \
    X(void,        glDeleteProgram,      (GLuint)) \
    X(void,        glDeleteShader,       (GLuint)) \
    X(void,        glDeleteTextures,     (GLsizei, const GLuint *)) \
    X(void,        glDisable,            (GLenum)) \
    X(void,        glDisableVertexAttribArray, (GLuint)) \
    X(void,        glDrawElements,       (GLenum, GLsizei, GLenum, const void *)) \
    X(void,        glEnable,             (GLenum)) \
    X(void,        glEnableVertexAttribArray, (GLuint)) \
    X(void,        glGenBuffers,         (GLsizei, GLuint *)) \
    X(void,        glGenTextures,        (GLsizei, GLuint *)) \
    X(GLint,       glGetAttribLocation,  (GLuint, const GLchar *)) \
    X(void,        glGetProgramInfoLog,  (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void,        glGetProgramiv,       (GLuint, GLenum, GLint *)) \
    X(void,        glGetShaderInfoLog,   (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void,        glGetShaderiv,        (GLuint, GLenum, GLint *)) \
    X(GLint,       glGetUniformLocation, (GLuint, const GLchar *)) \
    X(void,        glLinkProgram,        (GLuint)) \
    X(void,        glPixelStorei,        (GLenum, GLint)) \
    X(void,        glShaderSource,       (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void,        glTexImage2D,         (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void,        glTexParameteri,      (GLenum, GLenum, GLint)) \
    X(void,        glTexSubImage2D,      (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void,        glUniform1i,          (GLint, GLint)) \
    X(void,        glUniform2f,          (GLint, GLfloat, GLfloat)) \
    X(void,        glUniformMatrix4fv,   (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void,        glUseProgram,         (GLuint)) \
    X(void,        glVertexAttribPointer,(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void,        glViewport,           (GLint, GLint, GLsizei, GLsizei)) \
    X(void,        glBlendEquation,      (GLenum)) \
    X(void,        glGenerateMipmap,     (GLenum))

typedef struct {
    #define X(ret, name, args) ret (*name) args;
    GL_FUNC_LIST(X)
    #undef X
    bool loaded;
} gl_funcs_t;

static gl_funcs_t gl;

static bool gl_load(void) {
    if (gl.loaded) return true;
    void *lib = dlopen("libGLESv2.so.2", RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) lib = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) {
        FGE_ERROR(FGE_LOG_CAT_RENDERER, "Failed to load GL library");
        return false;
    }
    #define X(ret, name, args) \
        do { \
            *(void **)&gl.name = dlsym(lib, #name); \
            if (!gl.name) { \
                FGE_ERROR(FGE_LOG_CAT_RENDERER, "Missing GL function: %s", #name); \
                return false; \
            } \
        } while (0);
    GL_FUNC_LIST(X)
    #undef X
    gl.loaded = true;
    return true;
}

/* -------------------------------------------------------------------------- */
/* Shaders                                                                    */
/* -------------------------------------------------------------------------- */

static const char *vs_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "attribute vec4 a_color;\n"
    "uniform mat4 u_mvp;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    gl_Position = u_mvp * vec4(a_pos, 0.0, 1.0);\n"
    "    v_uv = a_uv;\n"
    "    v_color = a_color;\n"
    "}\n";

static const char *fs_src =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_tex, v_uv) * v_color;\n"
    "}\n";

static GLuint gl_compile_shader(GLenum type, const char *src) {
    GLuint s = gl.glCreateShader(type);
    gl.glShaderSource(s, 1, &src, NULL);
    gl.glCompileShader(s);
    GLint ok = 0;
    gl.glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl.glGetShaderInfoLog(s, sizeof(log), NULL, log);
        FGE_ERROR(FGE_LOG_CAT_RENDERER, "Shader compile failed: %s", log);
        gl.glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint gl_link_program(GLuint vs, GLuint fs) {
    GLuint p = gl.glCreateProgram();
    gl.glAttachShader(p, vs);
    gl.glAttachShader(p, fs);
    gl.glLinkProgram(p);
    GLint ok = 0;
    gl.glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl.glGetProgramInfoLog(p, sizeof(log), NULL, log);
        FGE_ERROR(FGE_LOG_CAT_RENDERER, "Program link failed: %s", log);
        gl.glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* -------------------------------------------------------------------------- */
/* Internal structures                                                        */
/* -------------------------------------------------------------------------- */

#define FGE_GL_MAX_SPRITES 8192
#define FGE_GL_VERTS_PER_SPRITE 4
#define FGE_GL_INDICES_PER_SPRITE 6
#define FGE_GL_MAX_VERTICES (FGE_GL_MAX_SPRITES * FGE_GL_VERTS_PER_SPRITE)
#define FGE_GL_MAX_INDICES  (FGE_GL_MAX_SPRITES * FGE_GL_INDICES_PER_SPRITE)

/* Interleaved vertex: pos(2), uv(2), color(4) */
typedef struct {
    float pos[2];
    float uv[2];
    uint8_t color[4];
} gl_vertex_t;

typedef struct {
    uint32_t tex_id;
    int layer;
    bool additive;
} gl_batch_key_t;

typedef struct {
    gl_batch_key_t key;
    uint32_t start_index;
    uint32_t index_count;
} gl_draw_call_t;

struct fge_gl_renderer {
    int width, height;

    /* GL objects */
    GLuint program;
    GLuint vbo, ibo;
    GLuint white_tex; /* 1x1 white for colored quads */

    /* Shader locations */
    GLint loc_mvp;
    GLint loc_tex;
    GLint loc_a_pos;
    GLint loc_a_uv;
    GLint loc_a_color;

    /* CPU-side batch buffers */
    gl_vertex_t *vertices;
    uint16_t *indices;
    uint32_t vertex_count;
    uint32_t index_count;

    /* Draw call list */
    gl_draw_call_t *draw_calls;
    uint32_t draw_call_count;
    uint32_t draw_call_cap;

    /* Stats */
    fge_gl_stats_t stats;

    /* Projection matrix (ortho, column-major) */
    float mvp[16];
};

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static void gl_ortho(float *m, float l, float r, float b, float t, float n, float f) {
    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / (r - l);
    m[5]  = 2.0f / (t - b);
    m[10] = -2.0f / (f - n);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -(f + n) / (f - n);
    m[15] = 1.0f;
}

static void gl_renderer_grow_draw_calls(fge_gl_renderer_t *r) {
    if (r->draw_call_count < r->draw_call_cap) return;
    uint32_t new_cap = r->draw_call_cap ? r->draw_call_cap * 2 : 256;
    gl_draw_call_t *nc = FGE_REALLOC(r->draw_calls, new_cap * sizeof(gl_draw_call_t));
    if (!nc) return;
    r->draw_calls = nc;
    r->draw_call_cap = new_cap;
}

static void gl_renderer_add_draw_call(fge_gl_renderer_t *r, const gl_batch_key_t *key,
                                       uint32_t start_index, uint32_t index_count) {
    if (index_count == 0) return;
    /* Merge with previous if same key */
    if (r->draw_call_count > 0) {
        gl_draw_call_t *prev = &r->draw_calls[r->draw_call_count - 1];
        if (prev->key.tex_id == key->tex_id &&
            prev->key.layer == key->layer &&
            prev->key.additive == key->additive) {
            prev->index_count += index_count;
            return;
        }
    }
    gl_renderer_grow_draw_calls(r);
    if (r->draw_call_count >= r->draw_call_cap) return;
    gl_draw_call_t *dc = &r->draw_calls[r->draw_call_count++];
    dc->key = *key;
    dc->start_index = start_index;
    dc->index_count = index_count;
}

static int sprite_key_cmp(const void *a, const void *b) {
    const gl_batch_key_t *ka = (const gl_batch_key_t *)a;
    const gl_batch_key_t *kb = (const gl_batch_key_t *)b;
    if (ka->layer != kb->layer) return (ka->layer < kb->layer) ? -1 : 1;
    if (ka->tex_id != kb->tex_id) return (ka->tex_id < kb->tex_id) ? -1 : 1;
    if (ka->additive != kb->additive) return (ka->additive ? 1 : -1);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

fge_gl_renderer_t *fge_gl_renderer_create(int width, int height) {
    if (!gl_load()) return NULL;

    fge_gl_renderer_t *r = FGE_CALLOC(1, sizeof(fge_gl_renderer_t));
    if (!r) return NULL;
    r->width = width;
    r->height = height;

    /* Compile shaders */
    GLuint vs = gl_compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = gl_compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) goto fail;
    r->program = gl_link_program(vs, fs);
    gl.glDeleteShader(vs);
    gl.glDeleteShader(fs);
    if (!r->program) goto fail;

    r->loc_mvp = gl.glGetUniformLocation(r->program, "u_mvp");
    r->loc_tex = gl.glGetUniformLocation(r->program, "u_tex");
    r->loc_a_pos = gl.glGetAttribLocation(r->program, "a_pos");
    r->loc_a_uv = gl.glGetAttribLocation(r->program, "a_uv");
    r->loc_a_color = gl.glGetAttribLocation(r->program, "a_color");

    /* Create VBO/IBO */
    gl.glGenBuffers(1, &r->vbo);
    gl.glGenBuffers(1, &r->ibo);
    gl.glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    gl.glBufferData(GL_ARRAY_BUFFER, FGE_GL_MAX_VERTICES * sizeof(gl_vertex_t), NULL, GL_STREAM_DRAW);
    gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ibo);
    gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER, FGE_GL_MAX_INDICES * sizeof(uint16_t), NULL, GL_STREAM_DRAW);

    /* CPU buffers */
    r->vertices = FGE_MALLOC(FGE_GL_MAX_VERTICES * sizeof(gl_vertex_t));
    r->indices = FGE_MALLOC(FGE_GL_MAX_INDICES * sizeof(uint16_t));
    if (!r->vertices || !r->indices) goto fail;

    /* 1x1 white texture */
    uint32_t white = 0xFFFFFFFF;
    r->white_tex = fge_gl_tex_create(r, 1, 1, &white);
    if (!r->white_tex) goto fail;

    /* Initial GL state */
    gl.glEnable(GL_BLEND);
    gl.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.glBlendEquation(GL_FUNC_ADD);
    gl.glViewport(0, 0, width, height);

    /* Build ortho projection: top-left origin, Y down, pixels */
    gl_ortho(r->mvp, 0.0f, (float)width, (float)height, 0.0f, -1.0f, 1.0f);

    FGE_INFO(FGE_LOG_CAT_RENDERER, "GL renderer created %dx%d", width, height);
    return r;

fail:
    fge_gl_renderer_destroy(r);
    return NULL;
}

void fge_gl_renderer_destroy(fge_gl_renderer_t *r) {
    if (!r) return;
    if (r->program) gl.glDeleteProgram(r->program);
    if (r->vbo) gl.glDeleteBuffers(1, &r->vbo);
    if (r->ibo) gl.glDeleteBuffers(1, &r->ibo);
    if (r->white_tex) gl.glDeleteTextures(1, &r->white_tex);
    FGE_FREE(r->vertices);
    FGE_FREE(r->indices);
    FGE_FREE(r->draw_calls);
    FGE_FREE(r);
}

void fge_gl_renderer_resize(fge_gl_renderer_t *r, int width, int height) {
    if (!r) return;
    r->width = width;
    r->height = height;
    gl.glViewport(0, 0, width, height);
    gl_ortho(r->mvp, 0.0f, (float)width, (float)height, 0.0f, -1.0f, 1.0f);
}

void fge_gl_renderer_begin(fge_gl_renderer_t *r, float clear_r, float clear_g,
                            float clear_b, float clear_a) {
    if (!r) return;
    gl.glClearColor(clear_r, clear_g, clear_b, clear_a);
    gl.glClear(GL_COLOR_BUFFER_BIT);
    r->vertex_count = 0;
    r->index_count = 0;
    r->draw_call_count = 0;
    memset(&r->stats, 0, sizeof(r->stats));
}

uint32_t fge_gl_tex_create(fge_gl_renderer_t *r, int width, int height,
                            const uint32_t *pixels) {
    if (!r || width <= 0 || height <= 0) return 0;
    GLuint tex;
    gl.glGenTextures(1, &tex);
    gl.glBindTexture(GL_TEXTURE_2D, tex);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, pixels);
    return (uint32_t)tex;
}

bool fge_gl_tex_update(fge_gl_renderer_t *r, uint32_t tex_id,
                       int x, int y, int w, int h, const uint32_t *pixels) {
    if (!r || !tex_id || !pixels) return false;
    gl.glBindTexture(GL_TEXTURE_2D, (GLuint)tex_id);
    gl.glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return true;
}

void fge_gl_tex_destroy(fge_gl_renderer_t *r, uint32_t tex_id) {
    if (!r || !tex_id) return;
    GLuint t = (GLuint)tex_id;
    gl.glDeleteTextures(1, &t);
}

void fge_gl_draw_sprite_ex(fge_gl_renderer_t *r,
                           uint32_t tex_id,
                           float x, float y, float w, float h,
                           float rotation, uint32_t color,
                           float u0, float v0, float u1, float v1,
                           int layer, bool additive) {
    if (!r || tex_id == 0) return;
    if (r->vertex_count + 4 > FGE_GL_MAX_VERTICES ||
        r->index_count + 6 > FGE_GL_MAX_INDICES) {
        /* Buffer full: flush implicitly by emitting draw call and resetting */
        /* For simplicity, we just drop extra sprites. A production renderer
         * would flush mid-frame. */
        return;
    }

    float hw = w * 0.5f, hh = h * 0.5f;
    float c = rotation != 0.0f ? fge_cosf(rotation) : 1.0f;
    float s = rotation != 0.0f ? fge_sinf(rotation) : 0.0f;

    /* 4 corners: top-left, top-right, bottom-right, bottom-left */
    fge_vec2_t corners[4] = {
        {-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}
    };
    float uvs[4][2] = {
        {u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}
    };

    uint32_t base = r->vertex_count;
    for (int i = 0; i < 4; i++) {
        float rx = corners[i].x * c - corners[i].y * s;
        float ry = corners[i].x * s + corners[i].y * c;
        gl_vertex_t *v = &r->vertices[r->vertex_count++];
        v->pos[0] = x + rx;
        v->pos[1] = y + ry;
        v->uv[0] = uvs[i][0];
        v->uv[1] = uvs[i][1];
        v->color[0] = (uint8_t)(color & 0xFF);
        v->color[1] = (uint8_t)((color >> 8) & 0xFF);
        v->color[2] = (uint8_t)((color >> 16) & 0xFF);
        v->color[3] = (uint8_t)((color >> 24) & 0xFF);
    }

    uint16_t *idx = &r->indices[r->index_count];
    idx[0] = (uint16_t)(base + 0);
    idx[1] = (uint16_t)(base + 1);
    idx[2] = (uint16_t)(base + 2);
    idx[3] = (uint16_t)(base + 0);
    idx[4] = (uint16_t)(base + 2);
    idx[5] = (uint16_t)(base + 3);
    r->index_count += 6;
    r->stats.sprites_batched++;

    /* Record draw call key */
    gl_batch_key_t key = { tex_id, layer, additive };
    gl_renderer_add_draw_call(r, &key, r->index_count - 6, 6);
}

void fge_gl_draw_quad(fge_gl_renderer_t *r, float x, float y, float w, float h,
                       uint32_t color) {
    if (!r) return;
    fge_gl_draw_sprite_ex(r, r->white_tex, x, y, w, h, 0.0f, color,
                          0.0f, 0.0f, 1.0f, 1.0f, 0, false);
}

void fge_gl_renderer_end(fge_gl_renderer_t *r) {
    if (!r || r->index_count == 0) return;

    /* Sort draw calls by layer/texture/additive */
    qsort(r->draw_calls, r->draw_call_count, sizeof(gl_draw_call_t), sprite_key_cmp);

    /* Upload vertex/index data */
    gl.glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    gl.glBufferData(GL_ARRAY_BUFFER, FGE_GL_MAX_VERTICES * sizeof(gl_vertex_t), NULL, GL_STREAM_DRAW);
    gl.glBufferSubData(GL_ARRAY_BUFFER, 0, r->vertex_count * sizeof(gl_vertex_t), r->vertices);

    gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ibo);
    gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER, FGE_GL_MAX_INDICES * sizeof(uint16_t), NULL, GL_STREAM_DRAW);
    gl.glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, r->index_count * sizeof(uint16_t), r->indices);

    r->stats.vertices_uploaded = r->vertex_count;

    /* Set up shader */
    gl.glUseProgram(r->program);
    gl.glUniformMatrix4fv(r->loc_mvp, 1, GL_FALSE, r->mvp);
    gl.glUniform1i(r->loc_tex, 0);
    gl.glActiveTexture(GL_TEXTURE0);

    /* Vertex attributes */
    gl.glEnableVertexAttribArray((GLuint)r->loc_a_pos);
    gl.glEnableVertexAttribArray((GLuint)r->loc_a_uv);
    gl.glEnableVertexAttribArray((GLuint)r->loc_a_color);
    gl.glVertexAttribPointer((GLuint)r->loc_a_pos, 2, GL_FLOAT, GL_FALSE,
                              sizeof(gl_vertex_t), (const void *)offsetof(gl_vertex_t, pos));
    gl.glVertexAttribPointer((GLuint)r->loc_a_uv, 2, GL_FLOAT, GL_FALSE,
                              sizeof(gl_vertex_t), (const void *)offsetof(gl_vertex_t, uv));
    gl.glVertexAttribPointer((GLuint)r->loc_a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                              sizeof(gl_vertex_t), (const void *)offsetof(gl_vertex_t, color));

    /* Execute draw calls */
    uint32_t current_tex = 0;
    bool current_additive = false;
    for (uint32_t i = 0; i < r->draw_call_count; i++) {
        const gl_draw_call_t *dc = &r->draw_calls[i];
        if (dc->key.tex_id != current_tex) {
            gl.glBindTexture(GL_TEXTURE_2D, (GLuint)dc->key.tex_id);
            current_tex = dc->key.tex_id;
            r->stats.texture_binds++;
        }
        if (dc->key.additive != current_additive) {
            gl.glBlendFunc(GL_SRC_ALPHA, dc->key.additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
            current_additive = dc->key.additive;
        }
        gl.glDrawElements(GL_TRIANGLES, (GLsizei)dc->index_count, GL_UNSIGNED_SHORT,
                          (const void *)(uintptr_t)(dc->start_index * sizeof(uint16_t)));
        r->stats.draw_calls++;
    }

    /* Restore default blend */
    if (current_additive) {
        gl.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    gl.glDisableVertexAttribArray((GLuint)r->loc_a_pos);
    gl.glDisableVertexAttribArray((GLuint)r->loc_a_uv);
    gl.glDisableVertexAttribArray((GLuint)r->loc_a_color);
}

const fge_gl_stats_t *fge_gl_renderer_stats(const fge_gl_renderer_t *r) {
    return r ? &r->stats : NULL;
}
