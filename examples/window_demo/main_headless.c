/* Headless test: renders one frame to a PPM file */
#include "forge/renderer.h"
#include <stdio.h>

int main(void) {
    fge_renderer_t r;
    if (!fge_renderer_init(&r, 400, 300)) {
        printf("FAIL: renderer init\n"); return 1;
    }
    fge_renderer_begin(&r, 0xFF101020);
    fge_draw_rect(fge_renderer_fb(&r), 50, 50, 100, 100, 0xFFFF0000);
    fge_draw_circle(fge_renderer_fb(&r), (fge_vec2_t){200, 150}, 40, 0xFF00FF00);
    fge_draw_text(fge_renderer_fb(&r), "FORGE", 160, 140, 0xFFFFFFFF, 2.0f);
    fge_renderer_end(&r);

    FILE *f = fopen("/tmp/forge_test.ppm", "wb");
    if (!f) { printf("FAIL: fopen\n"); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", r.fb.width, r.fb.height);
    for (int y = 0; y < r.fb.height; y++) {
        for (int x = 0; x < r.fb.width; x++) {
            uint32_t p = r.fb.pixels[y * r.fb.width + x];
            uint8_t rgb[3] = { (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    printf("OK: /tmp/forge_test.ppm written (%dx%d)\n", r.fb.width, r.fb.height);
    fge_renderer_shutdown(&r);
    return 0;
}
