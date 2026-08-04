/*
 * forge/core.h — Core types, macros, compiler features, atomic ops, spinlocks
 * Pure C23, zero external dependencies.
 *
 * Designed for real-time MMORPG server and client.
 */

#ifndef FORGE_CORE_H
#define FORGE_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

/* -------------------------------------------------------------------------- */
/* Compiler / Platform                                                        */
/* -------------------------------------------------------------------------- */

#if defined(__GNUC__) || defined(__clang__)
#  define FGE_LIKELY(x)      __builtin_expect(!!(x), 1)
#  define FGE_UNLIKELY(x)    __builtin_expect(!!(x), 0)
#  define FGE_INLINE         static inline __attribute__((always_inline))
#  define FGE_NOINLINE       __attribute__((noinline))
#  define FGE_NORETURN       __attribute__((noreturn))
#  define FGE_PACKED         __attribute__((packed))
#  define FGE_ALIGNED(x)     __attribute__((aligned(x)))
#  define FGE_PREFETCH(p, rw, loc) __builtin_prefetch((p), (rw), (loc))
#  define FGE_UNREACHABLE()  __builtin_unreachable()
#elif defined(_MSC_VER)
#  define FGE_LIKELY(x)      (x)
#  define FGE_UNLIKELY(x)    (x)
#  define FGE_INLINE         static inline __forceinline
#  define FGE_NOINLINE       __declspec(noinline)
#  define FGE_NORETURN       __declspec(noreturn)
#  define FGE_PACKED
#  define FGE_ALIGNED(x)     __declspec(align(x))
#  define FGE_PREFETCH(p, rw, loc) _mm_prefetch((const char*)(p), (loc))
#  define FGE_UNREACHABLE()  __assume(0)
#else
#  define FGE_LIKELY(x)      (x)
#  define FGE_UNLIKELY(x)    (x)
#  define FGE_INLINE         static inline
#  define FGE_NOINLINE
#  define FGE_NORETURN
#  define FGE_PACKED
#  define FGE_ALIGNED(x)
#  define FGE_PREFETCH(p, rw, loc) ((void)0)
#  define FGE_UNREACHABLE()  ((void)0)
#endif

#if defined(__linux__)
#  define FGE_PLATFORM_LINUX   1
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#  define FGE_PLATFORM_BSD     1
#elif defined(__APPLE__) && defined(__MACH__)
#  define FGE_PLATFORM_MACOS   1
#elif defined(_WIN32)
#  define FGE_PLATFORM_WINDOWS 1
#else
#  define FGE_PLATFORM_POSIX   1
#endif

#if defined(__x86_64__) || defined(_M_X64)
#  define FGE_ARCH_X64     1
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define FGE_ARCH_ARM64   1
#elif defined(__i386__) || defined(_M_IX86)
#  define FGE_ARCH_X86     1
#elif defined(__arm__) || defined(_M_ARM)
#  define FGE_ARCH_ARM32   1
#endif

/* -------------------------------------------------------------------------- */
/* Basics                                                                     */
/* -------------------------------------------------------------------------- */

#define FGE_ARRAY_COUNT(a)     (sizeof(a) / sizeof((a)[0]))
#define FGE_UNUSED(x)          ((void)(x))
#define FGE_KIB(n)             ((size_t)(n) * 1024)
#define FGE_MIB(n)             ((size_t)(n) * 1024 * 1024)
#define FGE_GIB(n)             ((size_t)(n) * 1024 * 1024 * 1024)

#define FGE_MIN(a, b)          ((a) < (b) ? (a) : (b))
#define FGE_MAX(a, b)          ((a) > (b) ? (a) : (b))
#define FGE_CLAMP(v, lo, hi)   (FGE_MAX((lo), FGE_MIN((v), (hi))))
#define FGE_LERP(a, b, t)      ((a) + ((b) - (a)) * (t))
#define FGE_ABS(x)             ((x) < 0 ? -(x) : (x))
#define FGE_SIGN(x)            ((x) > 0 ? 1 : ((x) < 0 ? -1 : 0))
#define FGE_SWAP(T, a, b)      do { T _tmp = (a); (a) = (b); (b) = _tmp; } while (0)

#define FGE_PI                 3.14159265358979323846f
#define FGE_PI_2               1.57079632679489661923f
#define FGE_TAU                6.28318530717958647693f
#define FGE_EPSILON_F          1.19209290e-07f
#define FGE_EPSILON_D          2.2204460492503131e-16

/* -------------------------------------------------------------------------- */
/* Assertions — always enabled, not just debug                                */
/* -------------------------------------------------------------------------- */

#ifdef NDEBUG
#  define FGE_ASSERT(cond)          ((void)0)
#  define FGE_ASSERT_MSG(cond, msg) ((void)0)
#else
#  include <assert.h>
#  define FGE_ASSERT(cond)          assert(cond)
#  define FGE_ASSERT_MSG(cond, msg) assert((cond) && (msg))
#endif

#define FGE_STATIC_ASSERT(cond) static_assert(cond, #cond)

/* -------------------------------------------------------------------------- */
/* Result type                                                                */
/* -------------------------------------------------------------------------- */

typedef enum {
    FGE_OK = 0,
    FGE_ERR_NOMEM,
    FGE_ERR_INVALID,
    FGE_ERR_NOT_FOUND,
    FGE_ERR_IO,
    FGE_ERR_UNSUPPORTED,
    FGE_ERR_OVERFLOW,
    FGE_ERR_UNDERFLOW,
    FGE_ERR_TIMEOUT,
    FGE_ERR_NETWORK,
    FGE_ERR_DISCONNECTED,
    FGE_ERR_PROTOCOL,
    FGE_ERR_BUSY,
    FGE_ERR_CANCELLED,
    FGE_ERR_UNKNOWN,
} fge_result_t;

static inline const char *fge_result_str(fge_result_t r) {
    switch (r) {
        case FGE_OK:              return "OK";
        case FGE_ERR_NOMEM:       return "OutOfMemory";
        case FGE_ERR_INVALID:     return "InvalidArgument";
        case FGE_ERR_NOT_FOUND:   return "NotFound";
        case FGE_ERR_IO:          return "IOError";
        case FGE_ERR_UNSUPPORTED: return "Unsupported";
        case FGE_ERR_OVERFLOW:    return "Overflow";
        case FGE_ERR_UNDERFLOW:   return "Underflow";
        case FGE_ERR_TIMEOUT:     return "Timeout";
        case FGE_ERR_NETWORK:     return "NetworkError";
        case FGE_ERR_DISCONNECTED:return "Disconnected";
        case FGE_ERR_PROTOCOL:    return "ProtocolError";
        case FGE_ERR_BUSY:        return "Busy";
        case FGE_ERR_CANCELLED:   return "Cancelled";
        case FGE_ERR_UNKNOWN:     return "Unknown";
        default:                  return "?";
    }
}

/* -------------------------------------------------------------------------- */
/* Handle type                                                                */
/* -------------------------------------------------------------------------- */

typedef uint32_t fge_handle_t;
#define FGE_HANDLE_INVALID 0u

/* Handle = 16-bit generation + 16-bit index */
FGE_INLINE fge_handle_t fge_handle_make(uint16_t index, uint16_t generation) {
    return ((uint32_t)generation << 16) | (uint32_t)index;
}
FGE_INLINE uint16_t fge_handle_index(fge_handle_t h)      { return (uint16_t)(h & 0xFFFF); }
FGE_INLINE uint16_t fge_handle_generation(fge_handle_t h) { return (uint16_t)(h >> 16); }
FGE_INLINE bool fge_handle_valid(fge_handle_t h)          { return h != FGE_HANDLE_INVALID; }

/* -------------------------------------------------------------------------- */
/* Slice / Span types                                                         */
/* -------------------------------------------------------------------------- */

typedef struct { uint8_t  *data; size_t len; } fge_slice_u8_t;
typedef struct { uint16_t *data; size_t len; } fge_slice_u16_t;
typedef struct { uint32_t *data; size_t len; } fge_slice_u32_t;
typedef struct { uint64_t *data; size_t len; } fge_slice_u64_t;
typedef struct { float    *data; size_t len; } fge_slice_f32_t;

/* -------------------------------------------------------------------------- */
/* Hash utilities (fast, not crypto)                                          */
/* -------------------------------------------------------------------------- */

FGE_INLINE uint32_t fge_hash_u32(uint32_t x) {
    x = ((x >> 16) ^ x) * 0x119de1f3u;
    x = ((x >> 16) ^ x) * 0x119de1f3u;
    x = (x >> 16) ^ x;
    return x;
}

FGE_INLINE uint64_t fge_hash_u64(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    x = x ^ (x >> 31);
    return x;
}

FGE_INLINE uint32_t fge_hash_bytes(const uint8_t *data, size_t len) {
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h;
}

FGE_INLINE uint64_t fge_hash_bytes_64(const uint8_t *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/* -------------------------------------------------------------------------- */
/* PRNG (xoshiro256**) — fast, good statistical quality                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint64_t s[4];
} fge_rng_t;

FGE_INLINE void fge_rng_seed(fge_rng_t *rng, uint64_t seed) {
    uint64_t z = seed + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    rng->s[0] = z ^ (z >> 31);
    z = rng->s[0] + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    rng->s[1] = z ^ (z >> 31);
    z = rng->s[1] + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    rng->s[2] = z ^ (z >> 31);
    z = rng->s[2] + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    rng->s[3] = z ^ (z >> 31);
}

FGE_INLINE uint64_t fge_rng_u64(fge_rng_t *rng) {
    uint64_t result = ((rng->s[1] * 5) << 7) | ((rng->s[1] * 5) >> 57);
    uint64_t t = rng->s[1] << 17;
    rng->s[2] ^= rng->s[0];
    rng->s[3] ^= rng->s[1];
    rng->s[1] ^= rng->s[2];
    rng->s[0] ^= rng->s[3];
    rng->s[2] ^= t;
    rng->s[3] = (rng->s[3] << 45) | (rng->s[3] >> 19);
    return result;
}

FGE_INLINE uint32_t fge_rng_u32(fge_rng_t *rng) {
    return (uint32_t)fge_rng_u64(rng);
}

FGE_INLINE float fge_rng_f32(fge_rng_t *rng) {
    return (float)(fge_rng_u64(rng) >> 40) * (1.0f / 16777216.0f);
}

FGE_INLINE double fge_rng_f64(fge_rng_t *rng) {
    return (double)(fge_rng_u64(rng) >> 11) * (1.0 / 9007199254740992.0);
}

FGE_INLINE float fge_rng_range_f32(fge_rng_t *rng, float min, float max) {
    return min + fge_rng_f32(rng) * (max - min);
}

FGE_INLINE int fge_rng_range_i32(fge_rng_t *rng, int min, int max) {
    return min + (int)(fge_rng_u64(rng) % (uint64_t)(max - min + 1));
}

/* -------------------------------------------------------------------------- */
/* Atomic operations (C11 stdatomics)                                         */
/* -------------------------------------------------------------------------- */

#include <stdatomic.h>

typedef _Atomic int     fge_atomic_int_t;
typedef _Atomic unsigned int    fge_atomic_uint_t;
typedef _Atomic int64_t fge_atomic_i64_t;
typedef _Atomic uint64_t fge_atomic_u64_t;
typedef atomic_flag fge_atomic_flag_t;

#define FGE_ATOMIC_INIT(val)      (val)
#define FGE_ATOMIC_LOAD(ptr)      atomic_load_explicit((ptr), memory_order_relaxed)
#define FGE_ATOMIC_LOAD_ACQ(ptr)  atomic_load_explicit((ptr), memory_order_acquire)
#define FGE_ATOMIC_STORE(ptr, v)  atomic_store_explicit((ptr), (v), memory_order_relaxed)
#define FGE_ATOMIC_STORE_REL(ptr, v) atomic_store_explicit((ptr), (v), memory_order_release)
#define FGE_ATOMIC_ADD(ptr, v)    atomic_fetch_add_explicit((ptr), (v), memory_order_relaxed)
#define FGE_ATOMIC_SUB(ptr, v)    atomic_fetch_sub_explicit((ptr), (v), memory_order_relaxed)
#define FGE_ATOMIC_AND(ptr, v)    atomic_fetch_and_explicit((ptr), (v), memory_order_relaxed)
#define FGE_ATOMIC_OR(ptr, v)     atomic_fetch_or_explicit((ptr), (v), memory_order_relaxed)
#define FGE_ATOMIC_CAS(ptr, exp, des) atomic_compare_exchange_weak_explicit((ptr), (exp), (des), memory_order_relaxed, memory_order_relaxed)
#define FGE_ATOMIC_CAS_STRONG(ptr, exp, des) atomic_compare_exchange_strong_explicit((ptr), (exp), (des), memory_order_relaxed, memory_order_relaxed)
#define FGE_ATOMIC_FLAG_TEST_SET(ptr) atomic_flag_test_and_set_explicit((ptr), memory_order_relaxed)
#define FGE_ATOMIC_FLAG_CLEAR(ptr)    atomic_flag_clear_explicit((ptr), memory_order_relaxed)

/* Spinlock using atomic_flag */
typedef atomic_flag fge_spinlock_t;

FGE_INLINE void fge_spinlock_init(fge_spinlock_t *lock) {
    atomic_flag_clear_explicit(lock, memory_order_relaxed);
}

FGE_INLINE void fge_spinlock_lock(fge_spinlock_t *lock) {
    while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) {
        /* Spin with pause hint */
#if defined(FGE_ARCH_X64) || defined(FGE_ARCH_X86)
        __asm__ volatile("pause");
#endif
    }
}
FGE_INLINE void fge_spinlock_unlock(fge_spinlock_t *lock) {
    atomic_flag_clear_explicit(lock, memory_order_release);
}
FGE_INLINE bool fge_spinlock_trylock(fge_spinlock_t *lock) {
    return !atomic_flag_test_and_set_explicit(lock, memory_order_acquire);
}

/* -------------------------------------------------------------------------- */
/* Fast math helpers                                                          */
/* -------------------------------------------------------------------------- */

FGE_INLINE float fge_deg_to_rad(float deg) { return deg * (FGE_PI / 180.0f); }
FGE_INLINE float fge_rad_to_deg(float rad) { return rad * (180.0f / FGE_PI); }

#if defined(__GNUC__) || defined(__clang__)
FGE_INLINE float fge_sqrtf(float x) { return __builtin_sqrtf(x); }
FGE_INLINE float fge_fabsf(float x) { return __builtin_fabsf(x); }
FGE_INLINE float fge_floorf(float x) { return __builtin_floorf(x); }
FGE_INLINE float fge_ceilf(float x) { return __builtin_ceilf(x); }
FGE_INLINE float fge_roundf(float x) { return __builtin_roundf(x); }
FGE_INLINE float fge_sinf(float x) { return __builtin_sinf(x); }
FGE_INLINE float fge_cosf(float x) { return __builtin_cosf(x); }
FGE_INLINE float fge_acosf(float x) { return __builtin_acosf(x); }
FGE_INLINE float fge_tanf(float x) { return __builtin_tanf(x); }
FGE_INLINE float fge_atan2f(float y, float x) { return __builtin_atan2f(y, x); }
FGE_INLINE float fge_expf(float x) { return __builtin_expf(x); }
FGE_INLINE float fge_logf(float x) { return __builtin_logf(x); }
FGE_INLINE float fge_powf(float x, float y) { return __builtin_powf(x, y); }
FGE_INLINE float fge_fmodf(float x, float y) { return __builtin_fmodf(x, y); }
#else
#include <math.h>
FGE_INLINE float fge_sqrtf(float x) { return sqrtf(x); }
FGE_INLINE float fge_fabsf(float x) { return fabsf(x); }
FGE_INLINE float fge_floorf(float x) { return floorf(x); }
FGE_INLINE float fge_ceilf(float x) { return ceilf(x); }
FGE_INLINE float fge_roundf(float x) { return roundf(x); }
FGE_INLINE float fge_sinf(float x) { return sinf(x); }
FGE_INLINE float fge_cosf(float x) { return cosf(x); }
FGE_INLINE float fge_acosf(float x) { return acosf(x); }
FGE_INLINE float fge_tanf(float x) { return tanf(x); }
FGE_INLINE float fge_atan2f(float y, float x) { return atan2f(y, x); }
FGE_INLINE float fge_expf(float x) { return expf(x); }
FGE_INLINE float fge_logf(float x) { return logf(x); }
FGE_INLINE float fge_powf(float x, float y) { return powf(x, y); }
FGE_INLINE float fge_fmodf(float x, float y) { return fmodf(x, y); }
#endif

FGE_INLINE float fge_inv_sqrtf(float x) {
    /* Fast inverse sqrt with one Newton iteration */
    union { float f; uint32_t i; } u = {x};
    u.i = 0x5f375a86u - (u.i >> 1);
    u.f *= 1.5f - 0.5f * x * u.f * u.f;
    return u.f;
}

FGE_INLINE float fge_fast_sqrtf(float x) {
    return x * fge_inv_sqrtf(x);
}

/* Integer pow2 helpers */
FGE_INLINE bool fge_ispow2_u32(uint32_t x) { return x && !(x & (x - 1)); }
FGE_INLINE bool fge_ispow2_u64(uint64_t x) { return x && !(x & (x - 1)); }
FGE_INLINE uint32_t fge_next_pow2_u32(uint32_t x) {
    x--; x |= x >> 1; x |= x >> 2; x |= x >> 4; x |= x >> 8; x |= x >> 16; x++;
    return x;
}
FGE_INLINE uint64_t fge_next_pow2_u64(uint64_t x) {
    x--; x |= x >> 1; x |= x >> 2; x |= x >> 4; x |= x >> 8; x |= x >> 16; x |= x >> 32; x++;
    return x;
}

/* -------------------------------------------------------------------------- */
/* Ring buffer macros (power-of-2 size required)                              */
/* -------------------------------------------------------------------------- */

#define FGE_RING_MASK(cap)        ((cap) - 1)
#define FGE_RING_PUSH(head, cap)  (((head) + 1) & FGE_RING_MASK(cap))
#define FGE_RING_POP(tail, cap)   (((tail) + 1) & FGE_RING_MASK(cap))
#define FGE_RING_COUNT(head, tail, cap) (((head) - (tail)) & FGE_RING_MASK(cap))
#define FGE_RING_EMPTY(head, tail) ((head) == (tail))
#define FGE_RING_FULL(head, tail, cap) (FGE_RING_COUNT(head, tail, cap) == ((cap) - 1))

#endif /* FORGE_CORE_H */
