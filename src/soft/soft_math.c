/*
 * soft_math.c -- 16.16 fixed-point matrix maths for the software renderer.
 *
 * Column-major, matching the GL convention Craft's matrix.c already uses,
 * so soft_mat4_from_float() can take those matrices verbatim.
 */

#include "soft.h"

fx_t fx_div(fx_t a, fx_t b)
{
    if (b == 0)
        return a < 0 ? (fx_t)0x80000000 : (fx_t)0x7fffffff;
    return (fx_t)(((int64_t)a << FX_BITS) / b);
}

void soft_mat4_identity(soft_mat4 *out)
{
    int i;
    for (i = 0; i < 16; i++)
        out->m[i] = 0;
    out->m[0] = out->m[5] = out->m[10] = out->m[15] = FX_ONE;
}

void soft_mat4_from_float(soft_mat4 *out, const float *src)
{
    int i;
    for (i = 0; i < 16; i++)
        out->m[i] = FX(src[i]);
}

void soft_mat4_mul(soft_mat4 *out, const soft_mat4 *a, const soft_mat4 *b)
{
    soft_mat4 tmp;
    int c, r, k;

    /* out = a * b, column-major: element (r,c) is m[c*4+r]. */
    for (c = 0; c < 4; c++) {
        for (r = 0; r < 4; r++) {
            int64_t sum = 0;
            for (k = 0; k < 4; k++)
                sum += (int64_t)a->m[k * 4 + r] * (int64_t)b->m[c * 4 + k];
            tmp.m[c * 4 + r] = (fx_t)(sum >> FX_BITS);
        }
    }
    *out = tmp;
}

void soft_mat4_translate(soft_mat4 *out, fx_t x, fx_t y, fx_t z)
{
    soft_mat4_identity(out);
    out->m[12] = x;
    out->m[13] = y;
    out->m[14] = z;
}
