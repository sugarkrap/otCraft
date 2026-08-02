/*
 * test_soft.c -- host-side harness for the software rasterizer.
 *
 * Renders the shared demo scene and writes the 320x240 index buffer out
 * as a PPM (expanded through the palette). This exists so the rasterizer
 * can be developed and eyeballed on the dev machine: the device has no
 * display we can screenshot cheaply, and a cross-compile plus deploy per
 * change would make iterating on a renderer impractical.
 *
 * The frame it produces at t == 0 is the reference the on-device smoke
 * test is compared against -- same scene, same viewpoint.
 *
 * Also times a batch of frames. Host timings say nothing about PXA255
 * performance in absolute terms -- there is an FPU here and the cache is
 * an order of magnitude bigger -- but they are a valid RELATIVE measure
 * when optimising, and they catch algorithmic blowups early.
 *
 *   make -f Makefile.piko run
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "demo_scene.h"
#include "soft.h"

static uint8_t *slurp(const char *path, size_t want)
{
    FILE *fh = fopen(path, "rb");
    uint8_t *buf;
    size_t got;

    if (!fh) {
        fprintf(stderr, "cannot open %s -- run tools/mkassets.py first\n", path);
        exit(1);
    }
    buf = malloc(want);
    got = fread(buf, 1, want, fh);
    fclose(fh);
    if (got != want) {
        fprintf(stderr, "%s: expected %zu bytes, got %zu\n", path, want, got);
        exit(1);
    }
    return buf;
}

static void write_ppm(const char *path, const uint8_t *palette)
{
    const uint8_t *fb = soft_framebuffer();
    FILE *fh = fopen(path, "wb");
    int i;

    if (!fh) {
        perror(path);
        return;
    }
    fprintf(fh, "P6\n%d %d\n255\n", SOFT_W, SOFT_H);
    for (i = 0; i < SOFT_W * SOFT_H; i++)
        fwrite(palette + fb[i] * 3, 1, 3, fh);
    fclose(fh);
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "out.ppm";
    int frames = argc > 2 ? atoi(argv[2]) : 1;

    uint8_t *palette  = slurp("assets/palette.bin", 768);
    uint8_t *colormap = slurp("assets/colormap.bin", SOFT_LIGHT_LEVELS * 256);
    uint8_t *blocks   = slurp("assets/blocks.raw", SOFT_ATLAS_W * SOFT_ATLAS_W);
    uint8_t *font     = slurp("assets/font.raw", SOFT_FONT_W * SOFT_FONT_H);
    uint8_t *sky      = slurp("assets/sky.raw", SOFT_SKY_W * SOFT_SKY_W);

    soft_mat4 mvp;
    struct timespec t0, t1;
    double elapsed;
    const soft_stats *st;
    int i, ink;

    if (soft_init(palette, colormap, blocks, font, sky) != 0) {
        fprintf(stderr, "soft_init failed\n");
        return 1;
    }

    demo_build();
    ink = demo_ink(palette);
    printf("scene: %d vertices, %d triangles\n",
           demo_mesh_count(), demo_mesh_count() / 3);

    demo_camera(0.0f, &mvp);
    soft_set_daylight(FX(1.0f));
    soft_set_fog(FX(12.0f), FX(34.0f), soft_sky_sample(FX(0.5f), FX(0.5f)));

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < frames; i++) {
        soft_begin_frame();
        soft_draw_sky(FX(0.5f), FX(0.0f));
        soft_set_matrix(&mvp);
        soft_draw_blocks(demo_mesh(), demo_mesh_count(), SOFT_OPAQUE);
        soft_end_frame();
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    soft_draw_line2d(SOFT_W / 2 - 5, SOFT_H / 2, SOFT_W / 2 + 5, SOFT_H / 2, ink);
    soft_draw_line2d(SOFT_W / 2, SOFT_H / 2 - 5, SOFT_W / 2, SOFT_H / 2 + 5, ink);
    soft_draw_text(2, 2, 1, "otCraft", ink);

    st = soft_get_stats();
    printf("tris in %u, drawn %u, spans %u, pixels %u\n",
           st->tris_in, st->tris_drawn, st->spans, st->pixels);
    printf("%d frame(s) in %.3f s = %.1f fps (host, not the device)\n",
           frames, elapsed, frames / elapsed);
    printf("overdraw: %.2fx of a full screen\n",
           st->pixels / (double)(SOFT_W * SOFT_H));

    write_ppm(out, palette);
    printf("wrote %s\n", out);
    return 0;
}
