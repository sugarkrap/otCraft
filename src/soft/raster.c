/*
 * raster.c -- the software rasterizer.
 *
 * Scanline, z-buffered, perspective-corrected in 16-pixel subspans.
 * Output is 8-bit palette indices; lighting and fog are table lookups
 * rather than arithmetic, because on a PXA255 a dependent load is
 * cheaper than the multiply-and-clamp the original fragment shader did.
 *
 * Nothing in the inner loops uses floating point.
 */

#include <stdlib.h>
#include <string.h>

#include "soft.h"

/* ── buffers ────────────────────────────────────────────────────────── */

static uint8_t  colour_buf[SOFT_W * SOFT_H];

/*
 * Depth is 1/w (bigger = nearer), stored 16-bit to halve the per-pixel
 * traffic against a 32-bit buffer. DEPTH_SHIFT scales 16.16 1/w into
 * that range: with the near plane at W_NEAR below, 1/w tops out around
 * 8.0, which lands at 16384 after the shift and leaves headroom.
 */
typedef uint16_t depth_t;
#define DEPTH_SHIFT 5
#define DEPTH_MAX   0xffff

static depth_t depth_buf[SOFT_W * SOFT_H];

/* ── assets ─────────────────────────────────────────────────────────── */

static const uint8_t *g_palette;
static const uint8_t *g_colormap;   /* [64][256] */
static const uint8_t *g_atlas;      /* [256][256] */
static const uint8_t *g_font;       /* [128][128] coverage */
static const uint8_t *g_sky;        /* [64][64] */

/* ── render state ───────────────────────────────────────────────────── */

static soft_mat4 g_mvp;

/*
 * Resolved shade per (face, ao, light). Craft's vertex shader computed
 * diffuse from the face normal and its fragment shader folded in ao,
 * block light and daylight; all of that is view-independent, so it is
 * precomputed here whenever daylight moves and read as a byte per vertex.
 */
static uint8_t g_shade[7][16][16];

/* Blend toward the fog colour: fogmap[step][index] -> index. */
static uint8_t g_fogmap[SOFT_FOG_STEPS][256];
static uint8_t g_fog_index = 0;
static int     g_fog_enabled = 0;
static fx_t    g_fog_near = 0, g_fog_scale = 0;

static soft_stats g_stats;

/* Near plane. Deliberately close: the player can stand against a block
 * face and the camera sits inside the block grid. */
#define W_NEAR FX(0.125f)

/* ── init ───────────────────────────────────────────────────────────── */

static int palette_nearest(int r, int g, int b)
{
    int best = 0, best_d = 1 << 30, i;

    /* Index SOFT_KEY is the transparency key, never a colour match. */
    for (i = 0; i < SOFT_KEY; i++) {
        int dr = r - g_palette[i * 3 + 0];
        int dg = g - g_palette[i * 3 + 1];
        int db = b - g_palette[i * 3 + 2];
        int d = dr * dr + dg * dg + db * db;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

int soft_init(const uint8_t *palette, const uint8_t *colormap,
              const uint8_t *blocks, const uint8_t *font,
              const uint8_t *sky)
{
    if (!palette || !colormap || !blocks || !font || !sky)
        return -1;

    g_palette  = palette;
    g_colormap = colormap;
    g_atlas    = blocks;
    g_font     = font;
    g_sky      = sky;

    soft_mat4_identity(&g_mvp);
    soft_set_daylight(FX(1.0f));
    return 0;
}

void soft_shutdown(void)
{
    g_palette = g_colormap = g_atlas = g_font = g_sky = NULL;
}

uint8_t *soft_framebuffer(void)
{
    return colour_buf;
}

/* ── lighting ───────────────────────────────────────────────────────── */

/*
 * Per-face diffuse, as integer percentages. The original shader took
 * dot(normal, normalize(vec3(-1,1,-1))), which for the six axis-aligned
 * cube normals yields only three distinct values -- so it is a table,
 * not a dot product.
 *
 * Face order is cube.c's, which is {left, right, TOP, BOTTOM, front,
 * back} -- top comes before bottom, not the -y/+y order the axis names
 * would suggest. Getting this pair backwards lights the underside of the
 * world and shadows the ground, which looks like a broken normal rather
 * than a swapped table, so it is worth stating.
 */
static const uint8_t face_diffuse[7] = {
    58,   /* 0  -x  left,   lit side */
    16,   /* 1  +x  right,  shadow   */
    100,  /* 2  +y  TOP,    fully lit */
    16,   /* 3  -y  BOTTOM, shadow   */
    58,   /* 4  -z  front,  lit side */
    16,   /* 5  +z  back,   shadow   */
    100   /* 6      unlit / self-lit geometry */
};

void soft_set_daylight(fx_t daylight)
{
    int face, ao, light;
    int day = fx_to_int(fx_mul(daylight, FX(100.0f)));

    if (day < 0) day = 0;
    if (day > 100) day = 100;

    for (face = 0; face < 7; face++) {
        for (light = 0; light < 16; light++) {
            /* Block light is emissive: it survives nightfall. */
            int emissive = light * 100 / 15;
            int ambient = day * 30 / 100 + 20;
            int direct = (day * 30 / 100 + 20) * face_diffuse[face] / 100;
            int lit = ambient + direct;

            if (lit < emissive)
                lit = emissive;
            if (lit > 100)
                lit = 100;

            for (ao = 0; ao < 16; ao++) {
                /* ao 0 = fully open, 15 = fully occluded; the original
                 * mapped this to a 0.3..1.0 multiplier. */
                int occl = 100 - (ao * 70 / 15);
                int v = lit * occl / 100;
                int level;

                if (v < emissive)
                    v = emissive;
                level = v * (SOFT_LIGHT_LEVELS - 1) / 100;
                if (level < 0) level = 0;
                if (level > SOFT_LIGHT_LEVELS - 1) level = SOFT_LIGHT_LEVELS - 1;
                g_shade[face][ao][light] = (uint8_t)level;
            }
        }
    }
}

void soft_set_fog(fx_t near, fx_t far, uint8_t sky_index)
{
    int step, i;
    int fr, fg, fb;

    if (far <= near) {
        g_fog_enabled = 0;
        return;
    }

    g_fog_near = near;
    g_fog_scale = fx_div(fx_from_int(SOFT_FOG_STEPS - 1), far - near);
    g_fog_enabled = 1;

    if (sky_index == g_fog_index)
        return;                     /* LUT already matches this colour */
    g_fog_index = sky_index;

    fr = g_palette[sky_index * 3 + 0];
    fg = g_palette[sky_index * 3 + 1];
    fb = g_palette[sky_index * 3 + 2];

    for (step = 0; step < SOFT_FOG_STEPS; step++) {
        int t = step * 255 / (SOFT_FOG_STEPS - 1);
        for (i = 0; i < 256; i++) {
            int r, g, b;
            if (i == SOFT_KEY) {
                g_fogmap[step][i] = SOFT_KEY;
                continue;
            }
            r = (g_palette[i * 3 + 0] * (255 - t) + fr * t) / 255;
            g = (g_palette[i * 3 + 1] * (255 - t) + fg * t) / 255;
            b = (g_palette[i * 3 + 2] * (255 - t) + fb * t) / 255;
            g_fogmap[step][i] = (uint8_t)palette_nearest(r, g, b);
        }
    }
}

/* ── frame ──────────────────────────────────────────────────────────── */

void soft_clear(uint8_t index)
{
    memset(colour_buf, index, sizeof(colour_buf));
    memset(depth_buf, 0, sizeof(depth_buf));   /* 0 = infinitely far */
}

void soft_begin_frame(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
}

void soft_end_frame(void)
{
}

void soft_set_matrix(const soft_mat4 *mvp)
{
    g_mvp = *mvp;
}

const soft_stats *soft_get_stats(void)
{
    return &g_stats;
}

/* ── clip / project ─────────────────────────────────────────────────── */

typedef struct {
    fx_t x, y, z, w;    /* clip space */
    fx_t u, v;          /* atlas texels, 16.16 */
    fx_t light;         /* 0..63, 16.16 */
} clipv;

typedef struct {
    fx_t sx, sy;        /* screen pixels, 16.16 */
    fx_t invw;
    fx_t uow, vow;      /* u/w, v/w -- linear in screen space */
    fx_t light;
} screenv;

#define CLIP_NEAR_BIT  1
#define CLIP_LEFT_BIT  2
#define CLIP_RIGHT_BIT 4
#define CLIP_BOT_BIT   8
#define CLIP_TOP_BIT   16

static int outcode(const clipv *v)
{
    int code = 0;
    if (v->w < W_NEAR)  code |= CLIP_NEAR_BIT;
    if (v->x < -v->w)   code |= CLIP_LEFT_BIT;
    if (v->x >  v->w)   code |= CLIP_RIGHT_BIT;
    if (v->y < -v->w)   code |= CLIP_BOT_BIT;
    if (v->y >  v->w)   code |= CLIP_TOP_BIT;
    return code;
}

static void clip_lerp(clipv *out, const clipv *a, const clipv *b, fx_t t)
{
    out->x     = a->x     + fx_mul(b->x     - a->x,     t);
    out->y     = a->y     + fx_mul(b->y     - a->y,     t);
    out->z     = a->z     + fx_mul(b->z     - a->z,     t);
    out->w     = a->w     + fx_mul(b->w     - a->w,     t);
    out->u     = a->u     + fx_mul(b->u     - a->u,     t);
    out->v     = a->v     + fx_mul(b->v     - a->v,     t);
    out->light = a->light + fx_mul(b->light - a->light, t);
}

/* Signed distance to a clip plane; positive is inside. */
static fx_t plane_dist(const clipv *v, int plane)
{
    switch (plane) {
    case CLIP_NEAR_BIT:  return v->w - W_NEAR;
    case CLIP_LEFT_BIT:  return v->x + v->w;
    case CLIP_RIGHT_BIT: return v->w - v->x;
    case CLIP_BOT_BIT:   return v->y + v->w;
    default:             return v->w - v->y;
    }
}

/* Sutherland-Hodgman against a single plane. */
static int clip_plane(const clipv *in, int n, clipv *out, int plane)
{
    int i, count = 0;

    for (i = 0; i < n; i++) {
        const clipv *a = &in[i];
        const clipv *b = &in[(i + 1) % n];
        fx_t da = plane_dist(a, plane);
        fx_t db = plane_dist(b, plane);

        if (da >= 0)
            out[count++] = *a;
        if ((da >= 0) != (db >= 0)) {
            fx_t denom = da - db;
            if (denom != 0)
                clip_lerp(&out[count++], a, b, fx_div(da, denom));
        }
    }
    return count;
}

static void project(screenv *s, const clipv *c)
{
    fx_t invw = fx_div(FX_ONE, c->w);

    /* NDC -1..1 mapped to the viewport. */
    s->sx = FX(SOFT_W / 2.0f) + fx_mul(fx_mul(c->x, invw), FX(SOFT_W / 2.0f));
    s->sy = FX(SOFT_H / 2.0f) - fx_mul(fx_mul(c->y, invw), FX(SOFT_H / 2.0f));
    s->invw = invw;
    s->uow = fx_mul(c->u, invw);
    s->vow = fx_mul(c->v, invw);
    s->light = c->light;
}

/* ── span fill ──────────────────────────────────────────────────────── */

/*
 * One scanline segment. Texture coordinates are perspective-correct at
 * every 16th pixel and affine between -- the classic tradeoff, and the
 * right one here because a divide costs ~40 cycles on a core with no
 * divider while the error over 16 pixels of a 16x16 block face is
 * invisible.
 */
static void draw_span(int y, int x0, int x1,
                      fx_t invw, fx_t uow, fx_t vow, fx_t light,
                      fx_t d_invw, fx_t d_uow, fx_t d_vow, fx_t d_light,
                      int keyed)
{
    uint8_t *dst;
    depth_t *zb;
    fx_t u, v;
    int len, x;

    if (x1 <= x0)
        return;

    /* Scissor, advancing the interpolants over the clipped-off part. */
    if (x0 < 0) {
        int skip = -x0;
        invw  += d_invw  * skip;
        uow   += d_uow   * skip;
        vow   += d_vow   * skip;
        light += d_light * skip;
        x0 = 0;
    }
    if (x1 > SOFT_W)
        x1 = SOFT_W;
    if (x1 <= x0)
        return;

    dst = colour_buf + y * SOFT_W;
    zb  = depth_buf + y * SOFT_W;
    len = x1 - x0;
    x = x0;

    g_stats.spans++;

    u = invw > 0 ? fx_div(uow, invw) : 0;
    v = invw > 0 ? fx_div(vow, invw) : 0;

    while (len > 0) {
        int n = len > 16 ? 16 : len;
        fx_t n_invw = invw + d_invw * n;
        fx_t nu, nv, du, dv;
        int i;

        if (n_invw > 0) {
            nu = fx_div(uow + d_uow * n, n_invw);
            nv = fx_div(vow + d_vow * n, n_invw);
        } else {
            nu = u;
            nv = v;
        }

        if (n == 16) {
            du = (nu - u) >> 4;
            dv = (nv - v) >> 4;
        } else {
            du = (nu - u) / n;
            dv = (nv - v) / n;
        }

        for (i = 0; i < n; i++, x++) {
            depth_t z = (depth_t)(invw >> DEPTH_SHIFT);
            if (z > zb[x]) {
                uint8_t texel = g_atlas[((v >> 16) & 0xff) * SOFT_ATLAS_W
                                        + ((u >> 16) & 0xff)];
                if (!keyed || texel != SOFT_KEY) {
                    int level = light >> 16;
                    uint8_t c;

                    if (level < 0) level = 0;
                    if (level > SOFT_LIGHT_LEVELS - 1)
                        level = SOFT_LIGHT_LEVELS - 1;

                    c = g_colormap[level * 256 + texel];

                    if (g_fog_enabled) {
                        /* Fog by depth: w = 1/invw, but the LUT only
                         * needs a step index, so compare against
                         * precomputed reciprocals of the band edges. */
                        fx_t dist = invw > 0 ? fx_div(FX_ONE, invw) : 0;
                        int step = fx_to_int(fx_mul(dist - g_fog_near,
                                                    g_fog_scale));
                        if (step > 0) {
                            if (step > SOFT_FOG_STEPS - 1)
                                step = SOFT_FOG_STEPS - 1;
                            c = g_fogmap[step][c];
                        }
                    }

                    zb[x] = z;
                    dst[x] = c;
                    g_stats.pixels++;
                }
            }
            invw  += d_invw;
            u     += du;
            v     += dv;
            light += d_light;
        }

        uow += d_uow * n;
        vow += d_vow * n;
        u = nu;
        v = nv;
        len -= n;
    }
}

/* ── triangle setup ─────────────────────────────────────────────────── */

static void raster_tri(screenv *a, screenv *b, screenv *c, int keyed)
{
    screenv *tmp;
    int64_t area64;
    fx_t area, inv_area;
    fx_t d_invw_dx, d_uow_dx, d_vow_dx, d_light_dx;
    fx_t y01, y02, y12, x01, x02;
    int y, ytop, ybot, ymid;

    /* Signed area, and with it the backface test. Craft's meshes are
     * counter-clockwise front-facing. */
    area64 = ((int64_t)(b->sx - a->sx) * (int64_t)(c->sy - a->sy)
            - (int64_t)(c->sx - a->sx) * (int64_t)(b->sy - a->sy)) >> FX_BITS;

    if (area64 == 0)
        return;
    if (area64 > 0)
        return;                     /* back-facing */

    area = (fx_t)area64;
    inv_area = fx_div(FX_ONE, area);

#define GRAD(field) \
    fx_mul(fx_mul(b->field - a->field, c->sy - a->sy) \
         - fx_mul(c->field - a->field, b->sy - a->sy), inv_area)

    d_invw_dx  = GRAD(invw);
    d_uow_dx   = GRAD(uow);
    d_vow_dx   = GRAD(vow);
    d_light_dx = GRAD(light);
#undef GRAD

    /* Sort by y: a is top. */
    if (a->sy > b->sy) { tmp = a; a = b; b = tmp; }
    if (b->sy > c->sy) { tmp = b; b = c; c = tmp; }
    if (a->sy > b->sy) { tmp = a; a = b; b = tmp; }

    ytop = fx_to_int(a->sy + FX(0.5f));
    ymid = fx_to_int(b->sy + FX(0.5f));
    ybot = fx_to_int(c->sy + FX(0.5f));

    if (ybot <= 0 || ytop >= SOFT_H || ytop == ybot)
        return;

    g_stats.tris_drawn++;

    y01 = b->sy - a->sy;
    y02 = c->sy - a->sy;
    y12 = c->sy - b->sy;
    x01 = b->sx - a->sx;
    x02 = c->sx - a->sx;

    {
        fx_t dxdy_long = y02 != 0 ? fx_div(x02, y02) : 0;
        fx_t dxdy_top  = y01 != 0 ? fx_div(x01, y01) : 0;
        fx_t dxdy_bot  = y12 != 0 ? fx_div(c->sx - b->sx, y12) : 0;

        /* Interpolants along the long edge, so the span start is exact. */
        fx_t d_invw_dy  = y02 != 0 ? fx_div(c->invw  - a->invw,  y02) : 0;
        fx_t d_uow_dy   = y02 != 0 ? fx_div(c->uow   - a->uow,   y02) : 0;
        fx_t d_vow_dy   = y02 != 0 ? fx_div(c->vow   - a->vow,   y02) : 0;
        fx_t d_light_dy = y02 != 0 ? fx_div(c->light - a->light, y02) : 0;

        for (y = ytop < 0 ? 0 : ytop; y < (ybot > SOFT_H ? SOFT_H : ybot); y++) {
            fx_t fy = fx_from_int(y) + FX(0.5f) - a->sy;
            fx_t xl = a->sx + fx_mul(dxdy_long, fy);
            fx_t xr;
            fx_t invw, uow, vow, light;
            int ix0, ix1;
            fx_t sub;

            if (y < ymid)
                xr = a->sx + fx_mul(dxdy_top, fy);
            else
                xr = b->sx + fx_mul(dxdy_bot,
                                    fx_from_int(y) + FX(0.5f) - b->sy);

            if (xl > xr) {
                fx_t t = xl; xl = xr; xr = t;
            }

            /* Values on the long edge at this scanline, then stepped
             * horizontally to the actual span start. */
            invw  = a->invw  + fx_mul(d_invw_dy,  fy);
            uow   = a->uow   + fx_mul(d_uow_dy,   fy);
            vow   = a->vow   + fx_mul(d_vow_dy,   fy);
            light = a->light + fx_mul(d_light_dy, fy);

            ix0 = fx_to_int(xl + FX(0.5f));
            ix1 = fx_to_int(xr + FX(0.5f));

            sub = fx_from_int(ix0) + FX(0.5f)
                - (a->sx + fx_mul(dxdy_long, fy));
            invw  += fx_mul(d_invw_dx,  sub);
            uow   += fx_mul(d_uow_dx,   sub);
            vow   += fx_mul(d_vow_dx,   sub);
            light += fx_mul(d_light_dx, sub);

            draw_span(y, ix0, ix1, invw, uow, vow, light,
                      d_invw_dx, d_uow_dx, d_vow_dx, d_light_dx, keyed);
        }
    }
}

/* ── vertex entry ───────────────────────────────────────────────────── */

static void transform(clipv *out, const soft_vtx *v)
{
    /* 1/16-block units -> 16.16 */
    fx_t px = (fx_t)v->x << (FX_BITS - SOFT_VTX_POS_SHIFT);
    fx_t py = (fx_t)v->y << (FX_BITS - SOFT_VTX_POS_SHIFT);
    fx_t pz = (fx_t)v->z << (FX_BITS - SOFT_VTX_POS_SHIFT);
    const fx_t *m = g_mvp.m;
    int normal = v->normal <= SOFT_NORMAL_UNLIT ? v->normal : SOFT_NORMAL_UNLIT;

    out->x = fx_mul(m[0], px) + fx_mul(m[4], py) + fx_mul(m[8],  pz) + m[12];
    out->y = fx_mul(m[1], px) + fx_mul(m[5], py) + fx_mul(m[9],  pz) + m[13];
    out->z = fx_mul(m[2], px) + fx_mul(m[6], py) + fx_mul(m[10], pz) + m[14];
    out->w = fx_mul(m[3], px) + fx_mul(m[7], py) + fx_mul(m[11], pz) + m[15];

    out->u = fx_from_int(v->u);
    out->v = fx_from_int(v->v);
    out->light = fx_from_int(
        g_shade[normal][v->ao_light >> 4][v->ao_light & 0x0f]);
}

void soft_draw_blocks(const soft_vtx *verts, int count, int flags)
{
    int i;
    int keyed = (flags & SOFT_KEYED) != 0;

    for (i = 0; i + 2 < count; i += 3) {
        clipv poly[8], tmp[8];
        screenv sv[8];
        int n, j, code0, code1, code2, all, any;

        g_stats.tris_in++;

        transform(&poly[0], &verts[i + 0]);
        transform(&poly[1], &verts[i + 1]);
        transform(&poly[2], &verts[i + 2]);

        code0 = outcode(&poly[0]);
        code1 = outcode(&poly[1]);
        code2 = outcode(&poly[2]);

        all = code0 & code1 & code2;
        if (all)
            continue;               /* wholly outside one plane */

        n = 3;
        any = code0 | code1 | code2;
        if (any) {
            static const int planes[5] = {
                CLIP_NEAR_BIT, CLIP_LEFT_BIT, CLIP_RIGHT_BIT,
                CLIP_BOT_BIT, CLIP_TOP_BIT
            };
            for (j = 0; j < 5 && n >= 3; j++) {
                if (!(any & planes[j]))
                    continue;
                n = clip_plane(poly, n, tmp, planes[j]);
                if (n < 3)
                    break;
                memcpy(poly, tmp, n * sizeof(clipv));
            }
            if (n < 3)
                continue;
            if (n > 8)
                n = 8;
        }

        for (j = 0; j < n; j++)
            project(&sv[j], &poly[j]);

        /* Fan-triangulate the clipped polygon. */
        for (j = 1; j + 1 < n; j++)
            raster_tri(&sv[0], &sv[j], &sv[j + 1], keyed);
    }
}

/* ── sky ────────────────────────────────────────────────────────────── */

uint8_t soft_sky_sample(fx_t time_of_day, fx_t height)
{
    int x = fx_to_int(fx_mul(time_of_day, fx_from_int(SOFT_SKY_W)));
    int y = fx_to_int(fx_mul(height, fx_from_int(SOFT_SKY_W)));

    if (x < 0) x = 0;
    if (x >= SOFT_SKY_W) x = SOFT_SKY_W - 1;
    if (y < 0) y = 0;
    if (y >= SOFT_SKY_W) y = SOFT_SKY_W - 1;
    return g_sky[y * SOFT_SKY_W + x];
}

/*
 * The original drew a textured sphere. At 320x240 that is a lot of
 * triangles to express what is, on screen, a vertical gradient -- so
 * this fills scanlines directly from the same sky texture, mapping
 * screen y through the camera pitch.
 */
void soft_draw_sky(fx_t time_of_day, fx_t pitch)
{
    int y;

    for (y = 0; y < SOFT_H; y++) {
        /* Screen y -> the shader's fog_height, 0 (down) .. 1 (up),
         * shifted by where the camera is looking. */
        fx_t frac = fx_div(fx_from_int(SOFT_H - y), fx_from_int(SOFT_H));
        fx_t h = fx_mul(frac, FX(0.5f)) + FX(0.25f)
               + fx_mul(pitch, FX(0.35f));
        uint8_t index;

        if (h < 0) h = 0;
        if (h > FX_ONE) h = FX_ONE;

        index = soft_sky_sample(time_of_day, h);
        memset(colour_buf + y * SOFT_W, index, SOFT_W);
    }
    memset(depth_buf, 0, sizeof(depth_buf));
}

/* ── 2D helpers ─────────────────────────────────────────────────────── */

void soft_draw_rect(int x, int y, int w, int h, uint8_t index)
{
    int j;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SOFT_W) w = SOFT_W - x;
    if (y + h > SOFT_H) h = SOFT_H - y;
    if (w <= 0 || h <= 0)
        return;

    for (j = 0; j < h; j++)
        memset(colour_buf + (y + j) * SOFT_W + x, index, w);
}

void soft_draw_line2d(int x0, int y0, int x1, int y1, uint8_t index)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        if (x0 >= 0 && x0 < SOFT_W && y0 >= 0 && y0 < SOFT_H)
            colour_buf[y0 * SOFT_W + x0] = index;
        if (x0 == x1 && y0 == y1)
            break;
        {
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

void soft_draw_line3d(fx_t x0, fx_t y0, fx_t z0,
                      fx_t x1, fx_t y1, fx_t z1, uint8_t index)
{
    const fx_t *m = g_mvp.m;
    clipv a, b;
    screenv sa, sb;

#define XFORM(dst, px, py, pz)                                          \
    do {                                                                \
        dst.x = fx_mul(m[0], px) + fx_mul(m[4], py) + fx_mul(m[8],  pz) + m[12]; \
        dst.y = fx_mul(m[1], px) + fx_mul(m[5], py) + fx_mul(m[9],  pz) + m[13]; \
        dst.z = fx_mul(m[2], px) + fx_mul(m[6], py) + fx_mul(m[10], pz) + m[14]; \
        dst.w = fx_mul(m[3], px) + fx_mul(m[7], py) + fx_mul(m[11], pz) + m[15]; \
        dst.u = dst.v = dst.light = 0;                                  \
    } while (0)

    XFORM(a, x0, y0, z0);
    XFORM(b, x1, y1, z1);
#undef XFORM

    /* Near-clip only; the 2D drawer scissors the rest. */
    if (a.w < W_NEAR && b.w < W_NEAR)
        return;
    if (a.w < W_NEAR) {
        clipv c;
        clip_lerp(&c, &a, &b, fx_div(W_NEAR - a.w, b.w - a.w));
        a = c;
    } else if (b.w < W_NEAR) {
        clipv c;
        clip_lerp(&c, &b, &a, fx_div(W_NEAR - b.w, a.w - b.w));
        b = c;
    }

    project(&sa, &a);
    project(&sb, &b);

    soft_draw_line2d(fx_to_int(sa.sx), fx_to_int(sa.sy),
                     fx_to_int(sb.sx), fx_to_int(sb.sy), index);
}

/*
 * Text is a coverage mask, not a palette image: font.raw holds alpha
 * only, and glyphs are drawn as one solid colour. That keeps the font
 * out of the palette entirely and makes the blend a threshold test
 * instead of an alpha multiply.
 */
void soft_draw_text(int x, int y, int scale, const char *text, uint8_t index)
{
    const int glyph = SOFT_FONT_GLYPH;
    int i;

    if (scale < 1)
        scale = 1;

    for (i = 0; text[i]; i++) {
        int ch = (unsigned char)text[i];
        int cell, gx, gy, px, py;

        if (ch < SOFT_FONT_FIRST || ch > SOFT_FONT_LAST) {
            x += glyph * scale;     /* unmapped: advance, draw nothing */
            continue;
        }
        cell = ch - SOFT_FONT_FIRST;
        gx = (cell % SOFT_FONT_COLS) * glyph;
        gy = (cell / SOFT_FONT_COLS) * glyph;

        for (py = 0; py < glyph; py++) {
            for (px = 0; px < glyph; px++) {
                int cover = g_font[(gy + py) * SOFT_FONT_W + gx + px];
                int dx, dy;

                if (cover < 128)
                    continue;
                for (dy = 0; dy < scale; dy++) {
                    int sy = y + py * scale + dy;
                    if (sy < 0 || sy >= SOFT_H)
                        continue;
                    for (dx = 0; dx < scale; dx++) {
                        int sx = x + px * scale + dx;
                        if (sx < 0 || sx >= SOFT_W)
                            continue;
                        colour_buf[sy * SOFT_W + sx] = index;
                    }
                }
            }
        }
        x += glyph * scale;
    }
}
