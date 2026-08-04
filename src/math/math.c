/*
 * src/math/math.c — Mat4, Quat, and transform implementations
 */

#include "forge/math.h"

fge_mat4_t fge_m4_rotate_x(float angle) {
    float c = fge_cosf(angle), s = fge_sinf(angle);
    fge_mat4_t r = fge_m4_identity();
    r.m[5] = c;  r.m[6] = s;
    r.m[9] = -s; r.m[10] = c;
    return r;
}

fge_mat4_t fge_m4_rotate_y(float angle) {
    float c = fge_cosf(angle), s = fge_sinf(angle);
    fge_mat4_t r = fge_m4_identity();
    r.m[0] = c;  r.m[2] = -s;
    r.m[8] = s;  r.m[10] = c;
    return r;
}

fge_mat4_t fge_m4_rotate_z(float angle) {
    float c = fge_cosf(angle), s = fge_sinf(angle);
    fge_mat4_t r = fge_m4_identity();
    r.m[0] = c;  r.m[1] = s;
    r.m[4] = -s; r.m[5] = c;
    return r;
}

fge_mat4_t fge_m4_rotate_axis(fge_vec3_t axis, float angle) {
    fge_vec3_t a = fge_v3_norm(axis);
    float c = fge_cosf(angle), s = fge_sinf(angle);
    float t = 1.0f - c;
    fge_mat4_t r = {0};
    r.m[0] = t*a.x*a.x + c;     r.m[1] = t*a.x*a.y + s*a.z; r.m[2] = t*a.x*a.z - s*a.y;
    r.m[4] = t*a.x*a.y - s*a.z; r.m[5] = t*a.y*a.y + c;     r.m[6] = t*a.y*a.z + s*a.x;
    r.m[8] = t*a.x*a.z + s*a.y; r.m[9] = t*a.y*a.z - s*a.x; r.m[10] = t*a.z*a.z + c;
    r.m[15] = 1.0f;
    return r;
}

fge_mat4_t fge_m4_mul(fge_mat4_t a, fge_mat4_t b) {
    fge_mat4_t r = {0};
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r.m[i*4+j] = a.m[i*4+0]*b.m[0*4+j] + a.m[i*4+1]*b.m[1*4+j]
                       + a.m[i*4+2]*b.m[2*4+j] + a.m[i*4+3]*b.m[3*4+j];
        }
    }
    return r;
}

fge_mat4_t fge_m4_transpose(fge_mat4_t m) {
    fge_mat4_t r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i*4+j] = m.m[j*4+i];
    return r;
}

fge_mat4_t fge_m4_inverse(fge_mat4_t m) {
    float *a = m.m, inv[16], det;
    inv[0]  = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    inv[4]  = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    inv[8]  = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    inv[1]  = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    inv[5]  = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    inv[9]  = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    inv[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    inv[2]  = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
    inv[6]  = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
    inv[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
    inv[3]  = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
    inv[7]  = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11] - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
    inv[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10] + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];
    det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
    if (fge_fabsf(det) < FGE_EPSILON_F) return fge_m4_identity();
    det = 1.0f / det;
    fge_mat4_t r;
    for (int i = 0; i < 16; i++) r.m[i] = inv[i] * det;
    return r;
}

fge_mat4_t fge_m4_perspective(float fovy, float aspect, float near, float far) {
    float f = 1.0f / fge_tanf(fovy * 0.5f);
    fge_mat4_t r = {0};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (far + near) / (near - far);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * far * near) / (near - far);
    return r;
}

fge_mat4_t fge_m4_ortho(float left, float right, float bottom, float top, float near, float far) {
    fge_mat4_t r = fge_m4_identity();
    r.m[0] = 2.0f / (right - left);
    r.m[5] = 2.0f / (top - bottom);
    r.m[10] = -2.0f / (far - near);
    r.m[12] = -(right + left) / (right - left);
    r.m[13] = -(top + bottom) / (top - bottom);
    r.m[14] = -(far + near) / (far - near);
    return r;
}

fge_mat4_t fge_m4_look_at(fge_vec3_t eye, fge_vec3_t center, fge_vec3_t up) {
    fge_vec3_t f = fge_v3_norm(fge_v3_sub(center, eye));
    fge_vec3_t s = fge_v3_norm(fge_v3_cross(f, up));
    fge_vec3_t u = fge_v3_cross(s, f);
    fge_mat4_t r = fge_m4_identity();
    r.m[0] = s.x;  r.m[1] = u.x;  r.m[2] = -f.x;
    r.m[4] = s.y;  r.m[5] = u.y;  r.m[6] = -f.y;
    r.m[8] = s.z;  r.m[9] = u.z;  r.m[10] = -f.z;
    r.m[12] = -fge_v3_dot(s, eye);
    r.m[13] = -fge_v3_dot(u, eye);
    r.m[14] = fge_v3_dot(f, eye);
    return r;
}

fge_quat_t fge_quat_from_axis_angle(fge_vec3_t axis, float angle) {
    float half = angle * 0.5f;
    float s = fge_sinf(half);
    fge_vec3_t a = fge_v3_norm(axis);
    return fge_quat(a.x * s, a.y * s, a.z * s, fge_cosf(half));
}

fge_quat_t fge_quat_slerp(fge_quat_t a, fge_quat_t b, float t) {
    float dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    if (dot < 0.0f) { b = fge_quat(-b.x, -b.y, -b.z, -b.w); dot = -dot; }
    if (dot > 0.9995f) {
        fge_quat_t r = fge_quat(a.x + t*(b.x-a.x), a.y + t*(b.y-a.y), a.z + t*(b.z-a.z), a.w + t*(b.w-a.w));
        return fge_quat_norm(r);
    }
    float theta0 = fge_acosf(dot);
    float theta = theta0 * t;
    float st = fge_sinf(theta), st0 = fge_sinf(theta0);
    float s0 = fge_cosf(theta) - dot * st / st0;
    float s1 = st / st0;
    return fge_quat(a.x*s0 + b.x*s1, a.y*s0 + b.y*s1, a.z*s0 + b.z*s1, a.w*s0 + b.w*s1);
}

fge_mat4_t fge_quat_to_mat4(fge_quat_t q) {
    float xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z;
    float xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z;
    float wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
    fge_mat4_t r = fge_m4_identity();
    r.m[0] = 1 - 2*(yy+zz); r.m[1] = 2*(xy+wz);     r.m[2] = 2*(xz-wy);
    r.m[4] = 2*(xy-wz);     r.m[5] = 1 - 2*(xx+zz); r.m[6] = 2*(yz+wx);
    r.m[8] = 2*(xz+wy);     r.m[9] = 2*(yz-wx);     r.m[10] = 1 - 2*(xx+yy);
    return r;
}

fge_mat4_t fge_transform_to_mat4(fge_transform_t t) {
    fge_mat4_t T = fge_m4_translate(t.position);
    fge_mat4_t R = fge_quat_to_mat4(t.rotation);
    fge_mat4_t S = fge_m4_scale(t.scale);
    return fge_m4_mul(fge_m4_mul(T, R), S);
}
