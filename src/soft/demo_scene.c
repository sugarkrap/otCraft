/*
 * demo_scene.c -- the shared test scene (see demo_scene.h).
 *
 * The terrain is deliberately rolling rather than flat: grazing-angle
 * surfaces are where affine texture error shows up, so a flat floor
 * would hide exactly the defect the perspective subdivision exists to
 * prevent. The tower and wall add vertical faces and real occlusion for
 * the depth buffer to get wrong.
 */

#include <math.h>
#include <string.h>

#include "demo_scene.h"

/* ── cube tables, transcribed from cube.c ───────────────────────────── */

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
    {20, 20, 36, 4, 20, 20}   /* 5 wood  */
};

/* ── scene ──────────────────────────────────────────────────────────── */

#define WORLD  24
#define HEIGHT 16

/* Measured at 12,036 for this scene; sized with room to spare and
 * guarded below, because silently truncating a mesh would look like a
 * rasterizer bug rather than an allocation one. */
#define MAX_VERTS 65536

static unsigned char world[WORLD][HEIGHT][WORLD];
static soft_vtx mesh[MAX_VERTS];
static int mesh_count;

const soft_vtx *demo_mesh(void)  { return mesh; }
int demo_mesh_count(void)        { return mesh_count; }

static int solid(int x, int y, int z)
{
    if (x < 0 || x >= WORLD || y < 0 || y >= HEIGHT || z < 0 || z >= WORLD)
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

    if (mesh_count + 6 > MAX_VERTS)
        return;

    for (i = 0; i < 6; i++) {
        int c = indices[face][i];
        soft_vtx *v = &mesh[mesh_count++];

        v->x = (int16_t)(bx * 16 + positions[face][c][0] * 8);
        v->y = (int16_t)(by * 16 + positions[face][c][1] * 8);
        v->z = (int16_t)(bz * 16 + positions[face][c][2] * 8);
        v->u = (uint8_t)(base_u + uvs[face][c][0] * 15);
        v->v = (uint8_t)(base_v + uvs[face][c][1] * 15);
        v->ao_light = 0x00;
        v->normal = (uint8_t)face;
    }
}

void demo_build(void)
{
    static const int offs[6][3] = {
        {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, -1}, {0, 0, 1}
    };
    int x, y, z, face;

    memset(world, 0, sizeof(world));

    for (x = 0; x < WORLD; x++) {
        for (z = 0; z < WORLD; z++) {
            int h = 3 + (int)(2.0 * sin(x * 0.4) + 2.0 * cos(z * 0.35));
            if (h < 1) h = 1;
            for (y = 0; y <= h && y < HEIGHT; y++)
                world[x][y][z] = (y == h) ? 1 : (y > h - 2 ? 3 : 4);
        }
    }

    for (y = 0; y < 9; y++)
        world[6][y][6] = 5;
    for (x = 12; x < 20; x++)
        for (y = 0; y < 6; y++)
            world[x][y][14] = 2;

    mesh_count = 0;
    for (x = 0; x < WORLD; x++)
        for (y = 0; y < HEIGHT; y++)
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

/* ── matrices ───────────────────────────────────────────────────────── */

/*
 * Kept in float on purpose. This runs a handful of times per frame, not
 * hundreds of thousands, so even soft-float costs well under a
 * millisecond -- and it means Craft's existing matrix.c can be reused
 * unchanged, with soft_mat4_from_float() as the only bridge.
 */
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

    memset(m, 0, 16 * sizeof(float));
    m[15] = 1.0f;
    m[0] = s[0]; m[4] = s[1]; m[8]  = s[2];
    m[1] = u[0]; m[5] = u[1]; m[9]  = u[2];
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
    m[12] = -(s[0]*eye[0] + s[1]*eye[1] + s[2]*eye[2]);
    m[13] = -(u[0]*eye[0] + u[1]*eye[1] + u[2]*eye[2]);
    m[14] =  (f[0]*eye[0] + f[1]*eye[1] + f[2]*eye[2]);
}

void demo_camera(float t, soft_mat4 *out)
{
    /* t == 0 reproduces the reference viewpoint exactly. */
    static const float base_eye[3] = {-4.0f, 11.0f, -5.0f};
    static const float at[3] = {12.0f, 3.0f, 12.0f};

    float proj[16], view[16], mvp[16];
    float eye[3];
    float ang = t * 0.25f;
    float dx = base_eye[0] - at[0];
    float dz = base_eye[2] - at[2];
    float ca = cosf(ang), sa = sinf(ang);

    eye[0] = at[0] + dx * ca - dz * sa;
    eye[1] = base_eye[1];
    eye[2] = at[2] + dx * sa + dz * ca;

    mat_perspective(proj, 65.0f, (float)SOFT_W / SOFT_H, 0.125f, 128.0f);
    mat_lookat(view, eye, at);
    mat_mul(mvp, proj, view);
    soft_mat4_from_float(out, mvp);
}

void demo_camera_player(float t, soft_mat4 *out)
{
    /* Middle of the world, on top of whatever the terrain does there --
     * same expression demo_build() used, so the eye sits on the ground
     * rather than in it. */
    const float cx = 12.0f, cz = 12.0f;
    float ground = 3.0f + 2.0f * sinf(cx * 0.4f) + 2.0f * cosf(cz * 0.35f);
    float proj[16], view[16], mvp[16];
    float eye[3], at[3];
    float yaw = t * 0.6f;

    if (ground < 1.0f)
        ground = 1.0f;

    eye[0] = cx;
    eye[1] = ground + 1.7f;          /* eye height above the block */
    eye[2] = cz;

    /* Look level, turning on the spot, so every frame sweeps a different
     * slice of the world through the cull. */
    at[0] = eye[0] + cosf(yaw);
    at[1] = eye[1] - 0.15f;
    at[2] = eye[2] + sinf(yaw);

    mat_perspective(proj, 65.0f, (float)SOFT_W / SOFT_H, 0.125f, 128.0f);
    mat_lookat(view, eye, at);
    mat_mul(mvp, proj, view);
    soft_mat4_from_float(out, mvp);
}

int demo_ink(const uint8_t *palette)
{
    int i, ink = 0, best = 1 << 30;

    for (i = 0; i < SOFT_KEY; i++) {
        int dr = 255 - palette[i * 3 + 0];
        int dg = 255 - palette[i * 3 + 1];
        int db = 255 - palette[i * 3 + 2];
        int d = dr * dr + dg * dg + db * db;
        if (d < best) { best = d; ink = i; }
    }
    return ink;
}
