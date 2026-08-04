/*
 * examples/hello_forge/main.c — First demo: window, logging, frame timing
 */

#include "forge/core.h"
#include "forge/math.h"
#include "forge/memory.h"
#include "forge/time.h"
#include "forge/log.h"
#include <stdio.h>
#include <unistd.h>

int main(void) {
    printf("FORGE Engine — Hello World Demo\n");
    printf("================================\n\n");

    /* Logging */
    fge_log_init(FGE_LOG_LEVEL_DEBUG);
    fge_log_add_sink_stdout(FGE_LOG_LEVEL_DEBUG);
    FGE_INFO(FGE_LOG_CAT_GENERAL, "Starting FORGE v0.1.0");

    /* Memory */
    fge_arena_t frame_arena;
    fge_arena_init(&frame_arena, FGE_MIB(1), "frame");
    FGE_INFO(FGE_LOG_CAT_MEMORY, "Frame arena: %zu bytes", fge_arena_remaining(&frame_arena));

    /* Clock & frame timing */
    fge_clock_t clock;
    fge_clock_init(&clock);
    fge_frame_time_t ft;
    fge_frame_time_init(&ft, 60.0);

    /* Profiler */
    fge_profiler_init();

    /* Simulate a few frames */
    for (int frame = 0; frame < 10; frame++) {
        FGE_PROFILE_SCOPE("frame");

        fge_frame_time_update(&ft, &clock);
        fge_scratch_reset();

        FGE_DEBUG(FGE_LOG_CAT_GENERAL, "Frame %d | dt=%.3fms | fps=%.1f | mem_used=%.1f%%",
                  (int)ft.tick, ft.dt * 1000.0, ft.fps,
                  fge_arena_utilization(&frame_arena) * 100.0f);

        /* Simulate some work */
        {
            FGE_PROFILE_SCOPE("simulate");
            usleep(1000); /* 1ms fake work */
        }

        /* Math test */
        {
            FGE_PROFILE_SCOPE("math");
            fge_vec2_t v = fge_v2(3, 4);
            float len = fge_v2_len(v);
            (void)len;
        }

        fge_arena_reset(&frame_arena);
    }

    /* Summary */
    fge_profiler_dump();
    fge_counter_dump_all();

    FGE_INFO(FGE_LOG_CAT_GENERAL, "Shutting down...");
    fge_arena_free(&frame_arena);
    fge_log_shutdown();

    printf("\nDemo complete.\n");
    return 0;
}
