/*
 * soft.h -- software rasterizer for otCraft on the Zaurus.
 *
 * Replaces the OpenGL layer. Everything here is integer: the PXA255 has
 * no FPU, so a float multiply is a libgcc call costing tens of cycles.
 * See docs/PORTING.md for the reasoning behind the 8-bit paletted
 * pipeline.
 *
 * The renderer draws into a 320x240 buffer of PALETTE INDICES, one byte
 * per pixel. The platform layer is responsible for getting that onto the
 * panel (2x pixel-doubled through an RGB565 LUT, on this hardware).
 */

#ifndef SOFT_SOFT_H
#define SOFT_SOFT_H

#include <stdint.h>

/* ── geometry of the render target ──────────────────────────────────── */

#define SOFT_W 320
#define SOFT_H 240

/* Palette index reserved as the transparency key. Matches the magenta
 * `discard` in Craft's original fragment shader, and tools/mkassets.py
 * writes it into the atlas for every transparent texel. */
#define SOFT_KEY 255

#define SOFT_LIGHT_LEVELS 64   /* rows in colormap.bin */
#define SOFT_FOG_STEPS    16   /* rows in the runtime fog LUT */

#define SOFT_ATLAS_W 256       /* blocks.raw is 256x256 */
#define SOFT_SKY_W   64        /* sky.raw is 64x64 */

/* font.raw is a 16x6 grid of 8x8 coverage masks holding chars 32..127;
 * character c is at cell (c - SOFT_FONT_FIRST). */
#define SOFT_FONT_W     128
#define SOFT_FONT_H     48
#define SOFT_FONT_GLYPH 8
#define SOFT_FONT_COLS  16
#define SOFT_FONT_FIRST 32
#define SOFT_FONT_LAST  127

/* ── fixed point ────────────────────────────────────────────────────── */

/*
 * 16.16 throughout. ARMv5TE has SMULL (32x32 -> 64 in one instruction),
 * so the 64-bit intermediate below costs about two cycles rather than a
 * helper call -- which is the entire reason this is affordable and float
 * is not.
 */
typedef int32_t fx_t;

#define FX_BITS 16
#define FX_ONE  (1 << FX_BITS)
#define FX(f)   ((fx_t)((f) * (float)FX_ONE))

#define fx_mul(a, b) ((fx_t)(((int64_t)(a) * (int64_t)(b)) >> FX_BITS))
#define fx_from_int(i) ((fx_t)((i) << FX_BITS))
#define fx_to_int(a)   ((int)((a) >> FX_BITS))

fx_t fx_div(fx_t a, fx_t b);

/* Column-major 4x4, same convention as the GL matrices Craft already
 * builds in matrix.c, so the porting there is a transcription. */
typedef struct { fx_t m[16]; } soft_mat4;

void soft_mat4_identity(soft_mat4 *out);
void soft_mat4_mul(soft_mat4 *out, const soft_mat4 *a, const soft_mat4 *b);
void soft_mat4_from_float(soft_mat4 *out, const float *src);
void soft_mat4_translate(soft_mat4 *out, fx_t x, fx_t y, fx_t z);

/* ── vertex format ──────────────────────────────────────────────────── */

/*
 * 10 bytes, against the 40 (ten floats) the GL path used. That 4x cut is
 * not cosmetic: chunk meshes are the largest allocation in the game and
 * this machine has 64 MB total.
 *
 * Positions are CHUNK-RELATIVE in 1/16-block units, so a chunk's whole
 * mesh fits comfortably in int16 and keeps precision for the 45-degree
 * plant quads. The chunk's world origin is folded into the matrix
 * instead, once per chunk rather than once per vertex.
 *
 * Lighting is stored as its INPUTS (ao, light, which face) rather than a
 * final shade, because daylight changes continuously and the mesh is
 * regenerated only when blocks change. Resolving it per-vertex per-frame
 * through soft_set_daylight()'s table is far cheaper than remeshing the
 * world every time the sun moves.
 */
typedef struct {
    int16_t x, y, z;     /* chunk-relative, 1/16 block units */
    uint8_t u, v;        /* texel in the 256x256 atlas */
    uint8_t ao_light;    /* ao in the high nibble, block light in the low */
    uint8_t normal;      /* 0..5 face direction; 6 = unlit (sky, UI) */
} soft_vtx;

#define SOFT_VTX_POS_SHIFT 4          /* 1/16 block */

#define SOFT_NORMAL_UNLIT 6

/* Draw flags for soft_draw_blocks(). */
#define SOFT_OPAQUE 0        /* no transparency test -- the fast inner loop */
#define SOFT_KEYED  1        /* test each texel against SOFT_KEY (plants, glass) */

/* ── lifecycle ──────────────────────────────────────────────────────── */

/*
 * Takes ownership of nothing; the caller keeps the asset buffers alive.
 * All five come from tools/mkassets.py.
 */
int  soft_init(const uint8_t *palette, const uint8_t *colormap,
               const uint8_t *blocks, const uint8_t *font,
               const uint8_t *sky);
void soft_shutdown(void);

/* The 320x240 index buffer the platform layer blits. */
uint8_t *soft_framebuffer(void);

void soft_begin_frame(void);
void soft_end_frame(void);

/* ── render state ───────────────────────────────────────────────────── */

/* Model-view-projection for subsequent draws. */
void soft_set_matrix(const soft_mat4 *mvp);

/*
 * Rebuilds the per-face shade table. Cheap (1,536 entries) but not free,
 * so callers should only call it when daylight has actually moved a
 * visible amount, not every frame.
 */
void soft_set_daylight(fx_t daylight);

/*
 * Fog. `near`/`far` are view-space distances in 16.16 block units;
 * beyond `far` geometry is fully replaced by `sky_index`. This is what
 * makes a 3-chunk render radius look like weather instead of like the
 * world ending, so it is load-bearing rather than decorative.
 *
 * Rebuilding the LUT is a 4,096-entry palette search, so it is done here
 * and only when the fog colour changes -- not per frame.
 */
void soft_set_fog(fx_t near, fx_t far, uint8_t sky_index);

/* Samples sky.raw the way the original sky shader did. */
uint8_t soft_sky_sample(fx_t time_of_day, fx_t height);

/* ── drawing ────────────────────────────────────────────────────────── */

/* Fills the colour buffer and resets the depth buffer. */
void soft_clear(uint8_t index);

/* Horizon gradient, replacing the sky sphere -- a full sphere mesh costs
 * triangles for something that is a vertical gradient at this resolution. */
void soft_draw_sky(fx_t time_of_day, fx_t pitch);

/* The hot path. `count` is a vertex count and must be a multiple of 3. */
void soft_draw_blocks(const soft_vtx *verts, int count, int flags);

/* 3D line in world space, for the block-selection wireframe. */
void soft_draw_line3d(fx_t x0, fx_t y0, fx_t z0,
                      fx_t x1, fx_t y1, fx_t z1, uint8_t index);

/* Screen-space helpers: crosshair, HUD, chat. */
void soft_draw_line2d(int x0, int y0, int x1, int y1, uint8_t index);
void soft_draw_text(int x, int y, int scale, const char *text, uint8_t index);
void soft_draw_rect(int x, int y, int w, int h, uint8_t index);

/* ── diagnostics ────────────────────────────────────────────────────── */

typedef struct {
    unsigned tris_in;        /* submitted */
    unsigned tris_fogged;    /* skipped: entirely beyond the fog far plane */
    unsigned tris_drawn;     /* survived cull + clip */
    unsigned spans;
    unsigned pixels;         /* depth-test passes */
} soft_stats;

const soft_stats *soft_get_stats(void);

#endif /* SOFT_SOFT_H */
