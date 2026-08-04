/*
 * forge/serialize.h — Fast binary serialization for network packets
 *
 * Features:
 *   - Zero-copy where possible
 *   - Little-endian encoding (native on x86/ARM)
 *   - Bounds checking on all operations
 *   - Varint encoding for compact integers
 *
 * Pure C23, zero external dependencies.
 */

#ifndef FORGE_SERIALIZE_H
#define FORGE_SERIALIZE_H

#include "forge/core.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Byte order                                                                 */
/* -------------------------------------------------------------------------- */

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#  define FGE_LITTLE_ENDIAN 1
#else
#  define FGE_LITTLE_ENDIAN 0
#endif

FGE_INLINE uint16_t fge_bswap_u16(uint16_t x) {
    return (x >> 8) | (x << 8);
}
FGE_INLINE uint32_t fge_bswap_u32(uint32_t x) {
    return ((x >> 24) & 0xFF) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) | (x << 24);
}
FGE_INLINE uint64_t fge_bswap_u64(uint64_t x) {
    return ((x >> 56) & 0xFFULL) | ((x >> 40) & 0xFF00ULL) | ((x >> 24) & 0xFF0000ULL) |
           ((x >> 8) & 0xFF000000ULL) | ((x << 8) & 0xFF00000000ULL) |
           ((x << 24) & 0xFF0000000000ULL) | ((x << 40) & 0xFF000000000000ULL) | (x << 56);
}

#if FGE_LITTLE_ENDIAN
#  define fge_le_u16(x) (x)
#  define fge_le_u32(x) (x)
#  define fge_le_u64(x) (x)
#else
#  define fge_le_u16(x) fge_bswap_u16(x)
#  define fge_le_u32(x) fge_bswap_u32(x)
#  define fge_le_u64(x) fge_bswap_u64(x)
#endif

/* -------------------------------------------------------------------------- */
/* Write cursor                                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t   pos;
    size_t   size;
    bool     overflow;
} fge_write_cursor_t;

FGE_INLINE fge_write_cursor_t fge_write_cursor(uint8_t *data, size_t size) {
    return (fge_write_cursor_t){data, 0, size, false};
}

FGE_INLINE bool fge_write_check(fge_write_cursor_t *c, size_t need) {
    if (c->pos + need > c->size) { c->overflow = true; return false; }
    return true;
}

FGE_INLINE void fge_write_u8(fge_write_cursor_t *c, uint8_t v) {
    if (fge_write_check(c, 1)) c->data[c->pos++] = v;
}
FGE_INLINE void fge_write_u16(fge_write_cursor_t *c, uint16_t v) {
    if (fge_write_check(c, 2)) { uint16_t le = fge_le_u16(v); memcpy(c->data + c->pos, &le, 2); c->pos += 2; }
}
FGE_INLINE void fge_write_u32(fge_write_cursor_t *c, uint32_t v) {
    if (fge_write_check(c, 4)) { uint32_t le = fge_le_u32(v); memcpy(c->data + c->pos, &le, 4); c->pos += 4; }
}
FGE_INLINE void fge_write_u64(fge_write_cursor_t *c, uint64_t v) {
    if (fge_write_check(c, 8)) { uint64_t le = fge_le_u64(v); memcpy(c->data + c->pos, &le, 8); c->pos += 8; }
}
FGE_INLINE void fge_write_f32(fge_write_cursor_t *c, float v) {
    union { float f; uint32_t i; } u = {v};
    fge_write_u32(c, u.i);
}
FGE_INLINE void fge_write_f64(fge_write_cursor_t *c, double v) {
    union { double f; uint64_t i; } u = {v};
    fge_write_u64(c, u.i);
}
FGE_INLINE void fge_write_bytes(fge_write_cursor_t *c, const uint8_t *src, size_t len) {
    if (fge_write_check(c, len)) { memcpy(c->data + c->pos, src, len); c->pos += len; }
}
FGE_INLINE void fge_write_str(fge_write_cursor_t *c, const char *s) {
    size_t len = s ? strlen(s) : 0;
    fge_write_u32(c, (uint32_t)len);
    fge_write_bytes(c, (const uint8_t *)s, len);
}

/* Varint: 7 bits per byte, MSB indicates continuation */
FGE_INLINE void fge_write_varint(fge_write_cursor_t *c, uint64_t v) {
    while (v >= 0x80) {
        fge_write_u8(c, (uint8_t)(v | 0x80));
        v >>= 7;
    }
    fge_write_u8(c, (uint8_t)v);
}

/* ZigZag encoding for signed varints */
FGE_INLINE void fge_write_svarint(fge_write_cursor_t *c, int64_t v) {
    fge_write_varint(c, (uint64_t)((v << 1) ^ (v >> 63)));
}

/* -------------------------------------------------------------------------- */
/* Read cursor                                                                */
/* -------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *data;
    size_t         pos;
    size_t         size;
    bool           overflow;
} fge_read_cursor_t;

FGE_INLINE fge_read_cursor_t fge_read_cursor(const uint8_t *data, size_t size) {
    return (fge_read_cursor_t){data, 0, size, false};
}

FGE_INLINE bool fge_read_check(fge_read_cursor_t *c, size_t need) {
    if (c->pos + need > c->size) { c->overflow = true; return false; }
    return true;
}

FGE_INLINE uint8_t  fge_read_u8(fge_read_cursor_t *c) {
    if (!fge_read_check(c, 1)) return 0;
    return c->data[c->pos++];
}
FGE_INLINE uint16_t fge_read_u16(fge_read_cursor_t *c) {
    if (!fge_read_check(c, 2)) return 0;
    uint16_t v; memcpy(&v, c->data + c->pos, 2); c->pos += 2;
    return fge_le_u16(v);
}
FGE_INLINE uint32_t fge_read_u32(fge_read_cursor_t *c) {
    if (!fge_read_check(c, 4)) return 0;
    uint32_t v; memcpy(&v, c->data + c->pos, 4); c->pos += 4;
    return fge_le_u32(v);
}
FGE_INLINE uint64_t fge_read_u64(fge_read_cursor_t *c) {
    if (!fge_read_check(c, 8)) return 0;
    uint64_t v; memcpy(&v, c->data + c->pos, 8); c->pos += 8;
    return fge_le_u64(v);
}
FGE_INLINE float fge_read_f32(fge_read_cursor_t *c) {
    union { uint32_t i; float f; } u = {fge_read_u32(c)};
    return u.f;
}
FGE_INLINE double fge_read_f64(fge_read_cursor_t *c) {
    union { uint64_t i; double f; } u = {fge_read_u64(c)};
    return u.f;
}
FGE_INLINE void fge_read_bytes(fge_read_cursor_t *c, uint8_t *dst, size_t len) {
    if (fge_read_check(c, len)) { memcpy(dst, c->data + c->pos, len); c->pos += len; }
}
FGE_INLINE const char *fge_read_str(fge_read_cursor_t *c, fge_arena_t *arena) {
    uint32_t len = fge_read_u32(c);
    if (c->overflow || len == 0) return "";
    if (!fge_read_check(c, len)) return "";
    char *s = fge_arena_alloc(arena, len + 1);
    if (s) { memcpy(s, c->data + c->pos, len); s[len] = '\0'; }
    c->pos += len;
    return s ? s : "";
}

FGE_INLINE uint64_t fge_read_varint(fge_read_cursor_t *c) {
    uint64_t v = 0;
    int shift = 0;
    while (true) {
        uint8_t b = fge_read_u8(c);
        if (c->overflow) return 0;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift >= 64) { c->overflow = true; return 0; }
    }
    return v;
}

FGE_INLINE int64_t fge_read_svarint(fge_read_cursor_t *c) {
    uint64_t v = fge_read_varint(c);
    return (int64_t)((v >> 1) ^ -(int64_t)(v & 1));
}

#endif /* FORGE_SERIALIZE_H */
