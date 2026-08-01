/*
 * test_soft.c -- host-side harness for the software rasterizer.
 *
 * Builds a small voxel scene, renders it, and writes the 320x240 index
 * buffer out as a PPM (expanded through the palette). This exists so the
 * rasterizer can be developed and eyeballed on the dev machine: the
 * device has no display we can screenshot and a cross-compile round trip
 * per change would make this port impractical.
 *
 * Also times a batch of frames. Host timings say nothing about PXA255
 * performance in absolute terms -- there is an FPU here and the cache is
 * an order of magnitude bigger -- but they are a valid RELATIVE measure
 * when optimising, and they catch algorithmic blowups early.
 *
 *   make test && ./test-soft out.ppm
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "soft.h"

/* ── the cube tables, transcribed from cube.c ───────────────────────── */

/* Face order: left(-x) right(+x) TOP(+y) BOTTOM(-y) front(-z) back(+z) */
static const int positions[6][4][3] = {
    {{-1, -1, -1}, {-1, -1, +1}, {-1, +1, -1}, {-1, +1, +1}},
    {{+1, -1, -1}, {+1, -1, +1}, {+1, +1, -1}, {+1, +1, +1}},
    {{-1, +1, -1}, {-1, +1, +1}, {+1, +1, -1}, {+1, +1, +1}},
    {{-1, -1, -1}, {-1, -1, +1}, {+1, -1, -1}, {+1, -1, +1}},
    {{-1, -1, -1}, {-1, +1, -1}, {+1, -1, -1}, {+1, +1, -1}},
    {{-1, -1, +1}, {-1, +1, +1}, {+1, -1, +1}, {+1, +1, +1}}
};
static const int uvs[6][4][2] = {
    {{0, 0}, {1, 0}, {0, 1}, {1, 1}},
    {{1, 0}, {0, 0}, {1, 1}, {0, 1}},
    {{0, 1}, {0, 0}, {1, 1}, {1, 0}},
    {{0, 0}, {0, 1}, {1, 0}, {1, 1}},
    {{0, 0}, {0, 1}, {1, 0}, {1, 1}},
    {{1, 0}, {1, 1}, {0, 0}, {0, 1}}
};
static const int indices[6][6] = {
    {0, 3, 2, 0, 1, 3},
    {0, 3, 1, 0, 2, 3},
    {0, 3, 2, 0, 1, 3},
    {0, 3, 1, 0, 2, 3},
    {0, 3, 2, 0, 1, 3},
    {0, 3, 1, 0, 2, 3}
};

/* w => (left, right, top, bottom, front, back) tiles, from item.c */
static const int block_tiles[][6] = {
    {0, 0, 0, 0, 0, 0},       /* 0 empty */
    {16, 16, 32, 0, 16, 16},  /* 1 grass */
    {1, 1, 1, 1, 1, 1},       /* 2 sand  */
    {2, 2, 2, 2, 2, 2},       /* 3 stone */
    {3, 3, 3, 3, 3, 3},       /* 4 brick */
    {20, 20, 36, 4, 20, 20},  /* 5 wood  */
};

/* ── scene ──────────────────────────────────────────────────────────── */

#define WORLD 24
#define MAX_VERTS (WORLD * WORLD * 8 * 6 * 6)

static unsigned char world[WORLD][16][WORLD];
static soft_vtx mesh[MAX_VERTS];
static int mesh_count;

static int solid(int x, int y, int z)
{
    if (x < 0 || x >= WORLD || y < 0 || y >= 16 || z < 0 || z >= WORLD)
        return 0;
    return world[x][y][z] != 0;
}

static void emit_face(int bx, int by, int bz, int face, int w)
{
    /*
     * u,v are whole texels and a tile is 16 wide, so the quad corners sit
     * at the tile's first and LAST texel (base+15) rather than base+16.
     * Spanning the full 16 would let the last sample of a span wrap into
     * the neighbouring tile and show as a seam; losing one texel of
     * stretch is invisible at this resolution.
     */
    int tile = block_tiles[w][face];
    int base_u = (tile % 16) * 16;
    int base_v = (tile / 16) * 16;
    int i;

    for (i = 0; i < 6; i++) {
        int c = indices[face][i];
        soft_vtx *v = &mesh[mesh_count++];

        v->x = (int16_t)(bx * 16 + positions[face][c][0] * 8);
        v->y = (int16_t)(by * 16 + positions[face][c][1] * 8);
        v->z = (int16_t)(bz * 16 + positions[face][c][2] * 8);
        v->u = (uint8_t)(base_u + uvs[face][c][0] * 15);
        v->v = (uint8_t)(base_v + uvs[face][c][1] * 15);
        v->ao_light = 0x00;         /* no ao, no block light */
        v->normal = (uint8_t)face;
    }
}

static void build_scene(void)
{
    static const int offs[6][3] = {
        {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, -1}, {0, 0, 1}
    };
    int x, y, z, face;

    memset(world, 0, sizeof(world));

    /* Rolling ground, so there are surfaces at grazing angles -- that is
     * where affine texture error would show up if the perspective
     * subdivision were wrong. */
    for (x = 0; x < WORLD; x++) {
        for (z = 0; z < WORLD; z++) {
            int h = 3 + (int)(2.0 * sin(x * 0.4) + 2.0 * cos(z * 0.35));
            if (h < 1) h = 1;
            for (y = 0; y <= h && y < 16; y++)
                world[x][y][z] = (y == h) ? 1 : (y > h - 2 ? 3 : 4);
        }
    }

    /* A tower and a wall, to give vertical faces and occlusion. */
    for (y = 0; y < 9; y++)
        world[6][y][6] = 5;
    for (x = 12; x < 20; x++)
        for (y = 0; y < 6; y++)
            world[x][y][14] = 2;

    mesh_count = 0;
    for (x = 0; x < WORLD; x++)
        for (y = 0; y < 16; y++)
            for (z = 0; z < WORLD; z++) {
                int w = world[x][y][z];
                if (!w)
                    continue;
                for (face = 0; face < 6; face++)
                    if (!solid(x + offs[face][0], y + offs[face][1],
                               z + offs[face][2]))
                        emit_face(x, y, z, face, w);
            }
}

/* ── matrices (float on the host, and on the device too) ────────────── */

/*
 * Matrix setup stays in float deliberately. It runs a few dozen times a
 * frame, not a few hundred thousand, so even soft-float costs well under
 * a millisecond -- and it means Craft's existing matrix.c can be reused
 * unchanged, with soft_mat4_from_float() as the only bridge.
 */
static void mat_identity(float *m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat_mul(float *out, const float *a, const float *b)
{
    float t[16];
    int c, r, k;
    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++) {
            float s = 0;
            for (k = 0; k < 4; k++)
                s += a[k * 4 + r] * b[c * 4 + k];
            t[c * 4 + r] = s;
        }
    memcpy(out, t, sizeof(t));
}

static void mat_perspective(float *m, float fovy, float aspect,
                            float znear, float zfar)
{
    float f = 1.0f / tanf(fovy * (float)M_PI / 360.0f);
    memset(m, 0, 16 * sizeof(float));
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zfar + znear) / (znear - zfar);
    m[11] = -1.0f;
    m[14] = 2.0f * zfar * znear / (znear - zfar);
}

static void mat_lookat(float *m, const float *eye, const float *at)
{
    float up[3] = {0, 1, 0};
    float f[3], s[3], u[3], len;
    int i;

    for (i = 0; i < 3; i++) f[i] = at[i] - eye[i];
    len = sqrtf(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    for (i = 0; i < 3; i++) f[i] /= len;

    s[0] = f[1]*up[2] - f[2]*up[1];
    s[1] = f[2]*up[0] - f[0]*up[2];
    s[2] = f[0]*up[1] - f[1]*up[0];
    len = sqrtf(s[0]*s[0] + s[1]*s[1] + s[2]*s[2]);
    for (i = 0; i < 3; i++) s[i] /= len;

    u[0] = s[1]*f[2] - s[2]*f[1];
    u[1] = s[2]*f[0] - s[0]*f[2];
    u[2] = s[0]*f[1] - s[1]*f[0];

    mat_identity(m);
    m[0] = s[0]; m[4] = s[1]; m[8]  = s[2];
    m[1] = u[0]; m[5] = u[1]; m[9]  = u[2];
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
    m[12] = -(s[0]*eye[0] + s[1]*eye[1] + s[2]*eye[2]);
    m[13] = -(u[0]*eye[0] + u[1]*eye[1] + u[2]*eye[2]);
    m[14] =  (f[0]*eye[0] + f[1]*eye[1] + f[2]*eye[2]);
}

/* ── assets ─────────────────────────────────────────────────────────── */

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

/* ── main ───────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "out.ppm";
    int frames = argc > 2 ? atoi(argv[2]) : 1;

    uint8_t *palette  = slurp("assets/palette.bin", 768);
    uint8_t *colormap = slurp("assets/colormap.bin", 64 * 256);
    uint8_t *blocks   = slurp("assets/blocks.raw", 256 * 256);
    uint8_t *font     = slurp("assets/font.raw", SOFT_FONT_W * SOFT_FONT_H);
    uint8_t *sky      = slurp("assets/sky.raw", 64 * 64);

    float proj[16], view[16], mvp[16];
    float eye[3] = {-4.0f, 11.0f, -5.0f};
    float at[3]  = {12.0f, 3.0f, 12.0f};
    soft_mat4 m;
    struct timespec t0, t1;
    double elapsed;
    const soft_stats *st;
    int i, ink = 0, best = 1 << 30;

    /* Brightest palette entry, for the crosshair and HUD text. */
    for (i = 0; i < SOFT_KEY; i++) {
        int dr = 255 - palette[i * 3 + 0];
        int dg = 255 - palette[i * 3 + 1];
        int db = 255 - palette[i * 3 + 2];
        int d = dr * dr + dg * dg + db * db;
        if (d < best) { best = d; ink = i; }
    }

    if (soft_init(palette, colormap, blocks, font, sky) != 0) {
        fprintf(stderr, "soft_init failed\n");
        return 1;
    }

    build_scene();
    printf("scene: %d vertices, %d triangles\n", mesh_count, mesh_count / 3);

    mat_perspective(proj, 65.0f, (float)SOFT_W / SOFT_H, 0.125f, 128.0f);
    mat_lookat(view, eye, at);
    mat_mul(mvp, proj, view);
    soft_mat4_from_float(&m, mvp);

    soft_set_daylight(FX(1.0f));
    /* Fog to the horizon colour, so the scene edge fades instead of
     * ending. Distances are in blocks. */
    soft_set_fog(FX(12.0f), FX(34.0f), soft_sky_sample(FX(0.5f), FX(0.5f)));

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < frames; i++) {
        soft_begin_frame();
        soft_draw_sky(FX(0.5f), FX(0.0f));
        soft_set_matrix(&m);
        soft_draw_blocks(mesh, mesh_count, SOFT_OPAQUE);
        soft_end_frame();
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    /* Crosshair and a HUD line, exercising the 2D paths. */
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
