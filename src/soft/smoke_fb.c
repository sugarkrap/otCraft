/*
 * smoke_fb.c -- on-device smoke test for the rasterizer and plat_fb.
 *
 * Renders the shared demo scene straight to /dev/fb0 on the Zaurus and
 * reports what it measured. This is the first thing that puts plat_fb.c
 * on real hardware, and it exists to answer three questions the host
 * harness cannot:
 *
 *   1. Does the framebuffer path work at all -- geometry, 16bpp, the 2x
 *      blit, and the page flip?
 *   2. What frame rate does a PXA255 actually reach? Every performance
 *      number in docs/PORTING.md so far is an estimate.
 *   3. Do the keyboard and the touchscreen produce the events the input
 *      model assumes?
 *
 * It ALWAYS exits on its own after a timeout. That is not a nicety: this
 * board is shared, it has no serial console, and a program holding the
 * VT in graphics mode after an SSH drop would look like a hung device.
 *
 *   otcraft-smoke [seconds] [assets-dir]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demo_scene.h"
#include "plat.h"
#include "soft.h"

#define DEFAULT_SECONDS 20
#define DEFAULT_ASSETS  "/usr/share/otcraft"

static uint8_t *slurp(const char *dir, const char *name, size_t want)
{
    char path[512];
    FILE *fh;
    uint8_t *buf;
    size_t got;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    fh = fopen(path, "rb");
    if (!fh) {
        fprintf(stderr, "smoke: cannot open %s\n", path);
        return NULL;
    }
    buf = malloc(want);
    if (!buf) {
        fclose(fh);
        return NULL;
    }
    got = fread(buf, 1, want, fh);
    fclose(fh);
    if (got != want) {
        fprintf(stderr, "smoke: %s: expected %lu bytes, got %lu\n",
                path, (unsigned long)want, (unsigned long)got);
        free(buf);
        return NULL;
    }
    return buf;
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : DEFAULT_SECONDS;
    const char *dir = argc > 2 ? argv[2] : DEFAULT_ASSETS;

    uint8_t *palette, *colormap, *blocks, *font, *sky;
    soft_mat4 mvp;
    double start, last_report;
    long frames = 0, frames_since = 0;
    int running = 1, ink;
    int look_x = 0, look_y = 0, last_key = -1;
    int actions = 0, keys_seen = 0;
    double best_fps = 0, worst_fps = 1e9;
    /*
     * Per-phase timing. The first device run came in at ~2.7 fps against
     * an estimate of 10-15, and the pixel count (~36k, under half a
     * screen) rules out raw fill rate -- so the cost has to be measured
     * per phase rather than reasoned about. Splitting sky / raster / HUD
     * / present is the cheapest way to find out which one is lying.
     */
    double t_sky = 0, t_raster = 0, t_hud = 0, t_present = 0;
    char hud[96];

    if (seconds <= 0)
        seconds = DEFAULT_SECONDS;

    palette  = slurp(dir, "palette.bin", 768);
    colormap = slurp(dir, "colormap.bin", SOFT_LIGHT_LEVELS * 256);
    blocks   = slurp(dir, "blocks.raw", SOFT_ATLAS_W * SOFT_ATLAS_W);
    font     = slurp(dir, "font.raw", SOFT_FONT_W * SOFT_FONT_H);
    sky      = slurp(dir, "sky.raw", SOFT_SKY_W * SOFT_SKY_W);

    if (!palette || !colormap || !blocks || !font || !sky) {
        fprintf(stderr, "smoke: assets missing under %s\n", dir);
        return 1;
    }

    if (soft_init(palette, colormap, blocks, font, sky) != 0) {
        fprintf(stderr, "smoke: soft_init failed\n");
        return 1;
    }

    demo_build();
    ink = demo_ink(palette);
    printf("smoke: scene %d triangles\n", demo_mesh_count() / 3);

    if (plat_init(palette) != 0) {
        fprintf(stderr, "smoke: plat_init failed\n");
        return 1;
    }

    soft_set_daylight(FX(1.0f));
    soft_set_fog(FX(12.0f), FX(34.0f), soft_sky_sample(FX(0.5f), FX(0.5f)));

    start = plat_time();
    last_report = start;

    while (running) {
        double now = plat_time();
        double elapsed = now - start;
        plat_event ev;
        int dx, dy;

        if (elapsed >= seconds)
            break;

        plat_poll();
        while (plat_poll_event(&ev)) {
            switch (ev.type) {
            case PLAT_EV_KEY_DOWN:
                last_key = ev.value;
                keys_seen++;
                /* Cancel is this keyboard's only Escape. */
                if (ev.value == PLAT_KEY_ESCAPE)
                    running = 0;
                break;
            case PLAT_EV_ACTION:
                actions++;
                break;
            case PLAT_EV_QUIT:
                running = 0;
                break;
            default:
                break;
            }
        }

        plat_get_look_delta(&dx, &dy);
        look_x += dx;
        look_y += dy;

        double ta, tb;

        demo_camera((float)elapsed, &mvp);

        soft_begin_frame();

        ta = plat_time();
        soft_draw_sky(FX(0.5f), FX(0.0f));
        tb = plat_time();
        t_sky += tb - ta;

        soft_set_matrix(&mvp);
        soft_draw_blocks(demo_mesh(), demo_mesh_count(), SOFT_OPAQUE);
        ta = plat_time();
        t_raster += ta - tb;

        soft_end_frame();

        /*
         * The HUD is the point of the on-device run as much as the scene
         * is: it puts the frame rate and the live input state into the
         * same fbgrab, so one screenshot answers all three questions.
         */
        {
            double span = now - last_report;
            double fps = span > 0 ? frames_since / span : 0;

            if (span >= 1.0) {
                if (fps > best_fps) best_fps = fps;
                if (fps < worst_fps) worst_fps = fps;
                last_report = now;
                frames_since = 0;
            }
            snprintf(hud, sizeof(hud), "%2d FPS  TRI %d",
                     (int)(fps + 0.5), demo_mesh_count() / 3);
            soft_draw_text(2, 2, 1, hud, ink);

            snprintf(hud, sizeof(hud), "PEN %d,%d  KEY %d  ACT %d",
                     look_x, look_y, last_key, actions);
            soft_draw_text(2, 12, 1, hud, ink);

            snprintf(hud, sizeof(hud), "%ds LEFT  CANCEL=QUIT",
                     (int)(seconds - elapsed));
            soft_draw_text(2, SOFT_H - 10, 1, hud, ink);
        }

        soft_draw_line2d(SOFT_W / 2 - 5, SOFT_H / 2,
                         SOFT_W / 2 + 5, SOFT_H / 2, ink);
        soft_draw_line2d(SOFT_W / 2, SOFT_H / 2 - 5,
                         SOFT_W / 2, SOFT_H / 2 + 5, ink);

        tb = plat_time();
        t_hud += tb - ta;

        plat_present(soft_framebuffer());
        t_present += plat_time() - tb;

        frames++;
        frames_since++;
    }

    {
        double total = plat_time() - start;
        const soft_stats *st = soft_get_stats();

        plat_shutdown();

        printf("smoke: %ld frames in %.1fs = %.2f fps average\n",
               frames, total, total > 0 ? frames / total : 0.0);
        if (best_fps > 0)
            printf("smoke: fps range %.2f .. %.2f\n", worst_fps, best_fps);
        printf("smoke: last frame -- tris in %u, drawn %u, spans %u, pixels %u\n",
               st->tris_in, st->tris_drawn, st->spans, st->pixels);
        printf("smoke: input -- %d key events, %d actions, pen total %d,%d\n",
               keys_seen, actions, look_x, look_y);

        if (frames > 0) {
            double acc = t_sky + t_raster + t_hud + t_present;
            printf("smoke: per frame (ms) -- sky %.1f  raster %.1f  "
                   "hud %.1f  present %.1f  accounted %.1f of %.1f\n",
                   1000 * t_sky / frames, 1000 * t_raster / frames,
                   1000 * t_hud / frames, 1000 * t_present / frames,
                   1000 * acc / frames, 1000 * total / frames);
        }
    }

    return 0;
}
