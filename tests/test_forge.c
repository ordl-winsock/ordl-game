/*
 * tests/test_forge.c — Comprehensive test suite
 */

#include "forge/core.h"
#include "forge/math.h"
#include "forge/memory.h"
#include "forge/time.h"
#include "forge/log.h"
#include "forge/serialize.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int s_passed = 0;
static int s_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN(name) do { printf("  [RUN]  %s\n", #name); test_##name(); } while (0)
#define ASSERT(cond) do { \
    if (cond) { s_passed++; } \
    else { s_failed++; printf("    [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)
#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NEAR(a, b, eps) ASSERT(fge_fabsf((a) - (b)) < (eps))

/* -------------------------------------------------------------------------- */
TEST(core_macros) {
    int arr[3] = {1,2,3};
    ASSERT_EQ(FGE_ARRAY_COUNT(arr), 3);
    ASSERT_EQ(FGE_MIN(3, 5), 3);
    ASSERT_EQ(FGE_MAX(3, 5), 5);
    ASSERT_EQ(FGE_CLAMP(10, 0, 5), 5);
    ASSERT_EQ(FGE_CLAMP(-1, 0, 5), 0);
}

TEST(core_hash) {
    uint32_t h = fge_hash_u32(12345);
    ASSERT(h != 0);
    uint64_t h64 = fge_hash_u64(0xDEADBEEFCAFEBABEull);
    ASSERT(h64 != 0);
}

TEST(core_rng) {
    fge_rng_t rng;
    fge_rng_seed(&rng, 12345);
    uint64_t a = fge_rng_u64(&rng);
    uint64_t b = fge_rng_u64(&rng);
    ASSERT(a != b);
    float f = fge_rng_f32(&rng);
    ASSERT(f >= 0.0f && f < 1.0f);
}

TEST(core_atomic) {
    fge_atomic_int_t val = FGE_ATOMIC_INIT(0);
    FGE_ATOMIC_ADD(&val, 5);
    ASSERT_EQ(FGE_ATOMIC_LOAD(&val), 5);
}

/* -------------------------------------------------------------------------- */
TEST(math_vec2) {
    fge_vec2_t a = fge_v2(3, 4);
    ASSERT_NEAR(fge_v2_len(a), 5.0f, 0.001f);
    fge_vec2_t n = fge_v2_norm(a);
    ASSERT_NEAR(fge_v2_len(n), 1.0f, 0.001f);
    ASSERT_NEAR(n.x, 0.6f, 0.001f);
    ASSERT_NEAR(n.y, 0.8f, 0.001f);
}

TEST(math_vec3) {
    fge_vec3_t a = fge_v3(1, 0, 0);
    fge_vec3_t b = fge_v3(0, 1, 0);
    fge_vec3_t c = fge_v3_cross(a, b);
    ASSERT_NEAR(c.x, 0.0f, 0.001f);
    ASSERT_NEAR(c.y, 0.0f, 0.001f);
    ASSERT_NEAR(c.z, 1.0f, 0.001f);
}

TEST(math_mat4) {
    fge_mat4_t I = fge_m4_identity();
    fge_mat4_t T = fge_m4_translate(fge_v3(1, 2, 3));
    fge_mat4_t R = fge_m4_mul(T, I);
    fge_vec4_t v = fge_m4_mulv(R, fge_v4(0, 0, 0, 1));
    ASSERT_NEAR(v.x, 1.0f, 0.001f);
    ASSERT_NEAR(v.y, 2.0f, 0.001f);
    ASSERT_NEAR(v.z, 3.0f, 0.001f);
}

TEST(math_quat) {
    fge_quat_t q = fge_quat_from_axis_angle(fge_v3_up(), FGE_PI_2);
    fge_vec3_t v = fge_quat_rotate_vec3(q, fge_v3(1, 0, 0));
    ASSERT_NEAR(v.x, 0.0f, 0.01f);
    ASSERT_NEAR(v.z, -1.0f, 0.01f);
}

TEST(math_aabb) {
    fge_aabb2_t b = fge_aabb2(fge_v2(0, 0), fge_v2(10, 10));
    ASSERT(fge_aabb2_contains(b, fge_v2(5, 5)));
    ASSERT(!fge_aabb2_contains(b, fge_v2(11, 5)));
}

/* -------------------------------------------------------------------------- */
TEST(memory_arena) {
    fge_arena_t a;
    ASSERT(fge_arena_init(&a, FGE_KIB(4), "test"));
    int *p = FGE_ARENA_ALLOC(&a, int);
    ASSERT(p != nullptr);
    *p = 42;
    ASSERT_EQ(*p, 42);
    fge_arena_reset(&a);
    fge_arena_free(&a);
}

TEST(memory_pool) {
    fge_pool_t p;
    ASSERT(fge_pool_init(&p, sizeof(int), alignof(int), 16, "test"));
    int *a = fge_pool_alloc(&p);
    int *b = fge_pool_alloc(&p);
    ASSERT(a != nullptr);
    ASSERT(b != nullptr);
    *a = 100;
    fge_pool_free_obj(&p, a);
    int *c = fge_pool_alloc(&p);
    ASSERT(c == a); /* reuse */
    fge_pool_free(&p);
}

/* -------------------------------------------------------------------------- */
TEST(time_clock) {
    fge_clock_t clk;
    fge_clock_init(&clk);
    uint64_t t1 = fge_clock_now(&clk);
    usleep(1000); /* 1ms */
    uint64_t t2 = fge_clock_now(&clk);
    ASSERT(t2 > t1);
}

TEST(time_timer) {
    fge_timer_t t = fge_timer_make(1.0, false);
    fge_timer_start(&t);
    bool done = fge_timer_tick(&t, 0.5);
    ASSERT(!done);
    done = fge_timer_tick(&t, 0.6);
    ASSERT(done);
}

/* -------------------------------------------------------------------------- */
TEST(serialize_basic) {
    uint8_t buf[256];
    fge_write_cursor_t w = fge_write_cursor(buf, sizeof(buf));
    fge_write_u8(&w, 0xAB);
    fge_write_u16(&w, 0x1234);
    fge_write_u32(&w, 0xDEADBEEFu);
    fge_write_u64(&w, 0xCAFEBABEDEADBEEFull);
    fge_write_f32(&w, 3.14159f);
    fge_write_varint(&w, 15000);

    fge_read_cursor_t r = fge_read_cursor(buf, w.pos);
    ASSERT_EQ(fge_read_u8(&r), 0xAB);
    ASSERT_EQ(fge_read_u16(&r), 0x1234);
    ASSERT_EQ(fge_read_u32(&r), 0xDEADBEEFu);
    ASSERT_EQ(fge_read_u64(&r), 0xCAFEBABEDEADBEEFull);
    ASSERT_NEAR(fge_read_f32(&r), 3.14159f, 0.001f);
    ASSERT_EQ(fge_read_varint(&r), 15000);
    ASSERT(!r.overflow);
}

/* -------------------------------------------------------------------------- */
TEST(log_basic) {
    /* Just verify it doesn't crash */
    fge_log_init(FGE_LOG_LEVEL_DEBUG);
    fge_log_add_sink_stdout(FGE_LOG_LEVEL_DEBUG);
    FGE_DEBUG(FGE_LOG_CAT_GENERAL, "Test debug message: %d", 42);
    FGE_INFO(FGE_LOG_CAT_GENERAL, "Test info message");
    FGE_WARN(FGE_LOG_CAT_MEMORY, "Test warn message");
    fge_log_flush();
    fge_log_shutdown();
    ASSERT(true);
}

/* -------------------------------------------------------------------------- */
int main(void) {
    printf("========================================\n");
    printf("FORGE Engine Test Suite\n");
    printf("========================================\n\n");

    RUN(core_macros);
    RUN(core_hash);
    RUN(core_rng);
    RUN(core_atomic);
    RUN(math_vec2);
    RUN(math_vec3);
    RUN(math_mat4);
    RUN(math_quat);
    RUN(math_aabb);
    RUN(memory_arena);
    RUN(memory_pool);
    RUN(time_clock);
    RUN(time_timer);
    RUN(serialize_basic);
    RUN(log_basic);

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", s_passed, s_failed);
    printf("========================================\n");

    return s_failed > 0 ? 1 : 0;
}
