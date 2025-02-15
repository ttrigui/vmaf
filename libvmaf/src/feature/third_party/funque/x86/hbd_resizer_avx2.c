/*   SPDX-License-Identifier: BSD-3-Clause
*   Copyright (C) 2022 Intel Corporation.
*/
/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <immintrin.h>

#include "resizer_avx2.h"

#define shift22_64b_signExt(a, r)\
{ \
    r = _mm256_add_epi64( _mm256_srli_epi64(a, 22) , _mm256_and_si256(a, _mm256_set1_epi64x(0xFFFFFC0000000000)));\
}

#define shift22_64b_signExt_128(a, r)\
{ \
    r = _mm_add_epi64( _mm_srli_epi64(a, 22) , _mm_and_si128(a, _mm_set1_epi64x(0xFFFFFC0000000000)));\
}

const int HBD_INTER_RESIZE_COEF_SCALE_avx2 = 2048;
static const int HBD_MAX_ESIZE_avx2 = 16;

#define CLIP3(X, MIN, MAX) ((X < MIN) ? MIN : (X > MAX) ? MAX \
                                                        : X)
#define MAX(LEFT, RIGHT) (LEFT > RIGHT ? LEFT : RIGHT)
#define MIN(LEFT, RIGHT) (LEFT < RIGHT ? LEFT : RIGHT)

// enabled by default for funque since resize factor is always 0.5, disabled otherwise
//#define OPTIMISED_COEFF 1

//#define USE_C_VRESIZE 0

#if !OPTIMISED_COEFF
static void interpolateCubic(float x, float *coeffs)
{
    const float A = -0.75f;

    coeffs[0] = ((A * (x + 1) - 5 * A) * (x + 1) + 8 * A) * (x + 1) - 4 * A;
    coeffs[1] = ((A + 2) * x - (A + 3)) * x * x + 1;
    coeffs[2] = ((A + 2) * (1 - x) - (A + 3)) * (1 - x) * (1 - x) + 1;
    coeffs[3] = 1.f - coeffs[0] - coeffs[1] - coeffs[2];
}
#endif

#if OPTIMISED_COEFF
void hbd_hresize_avx2(const unsigned short **src, int **dst, int count,
                 const short *alpha,
                 int swidth, int dwidth, int cn, int xmin, int xmax)
#else
void hbd_hresize_avx2(const unsigned short **src, int **dst, int count,
                 const int *xofs, const short *alpha,
                 int swidth, int dwidth, int cn, int xmin, int xmax)
#endif
{
    __m256i coef0_256 = _mm256_set_epi32(alpha[1], alpha[0], alpha[1], alpha[0], alpha[1], alpha[0], alpha[1], alpha[0]);
    __m256i coef2_256 = _mm256_set_epi32(alpha[3], alpha[2], alpha[3], alpha[2], alpha[3], alpha[2], alpha[3], alpha[2]);
    __m256i zero_256 = _mm256_setzero_si256();

    int xmax_16 = xmax - (xmax % 16);
    int xmax_8 = xmax - (xmax % 8);
    int xmax_4 = xmax - (xmax % 4);
    for (int k = 0; k < count; k++)
    {
        const unsigned short *S = src[k];
        int *D = dst[k];
        int dx = 0, limit = xmin;
        for (;;)
        {
#if OPTIMISED_COEFF
            for (; dx < limit; dx++)
            {
                int j;
                int sx = (dx * 2) - cn;
#else
            for (; dx < limit; dx++, alpha += 4)
            {
                int j;
                int sx = xofs[dx] - cn;
#endif
                int v = 0;
                for (j = 0; j < 4; j++)
                {
                    int sxj = sx + j * cn;
                    if ((unsigned)sxj >= (unsigned)swidth)
                    {
                        while (sxj < 0)
                            sxj += cn;
                        while (sxj >= swidth)
                            sxj -= cn;
                    }
                    v += S[sxj] * alpha[j];
                }
                D[dx] = v;
            }
            if (limit == dwidth)
                break;
#if OPTIMISED_COEFF
            for (; dx < xmax_16; dx+=16)
            {
                int sx = dx * 2;
#else
            for (; dx < xmax; dx++, alpha += 4)
            {
                int sx = xofs[dx]; // sx - 2, 4, 6, 8....
#endif
                __m256i val0_0 = _mm256_loadu_si256((__m256i*)(S + sx - 1));
                __m256i val2_0 = _mm256_loadu_si256((__m256i*)(S + sx + 1));
                __m256i val0_16 = _mm256_loadu_si256((__m256i*)(S + sx + 15));
                __m256i val2_16 = _mm256_loadu_si256((__m256i*)(S + sx + 17));

                __m256i val0_0_lo = _mm256_unpacklo_epi16(val0_0, zero_256);
                __m256i val0_0_hi = _mm256_unpackhi_epi16(val0_0, zero_256);
                __m256i val2_0_lo = _mm256_unpacklo_epi16(val2_0, zero_256);
                __m256i val2_0_hi = _mm256_unpackhi_epi16(val2_0, zero_256);
                __m256i val0_16_lo = _mm256_unpacklo_epi16(val0_16, zero_256);
                __m256i val0_16_hi = _mm256_unpackhi_epi16(val0_16, zero_256);
                __m256i val2_16_lo = _mm256_unpacklo_epi16(val2_16, zero_256);
                __m256i val2_16_hi = _mm256_unpackhi_epi16(val2_16, zero_256);

                __m256i mul0_0_lo = _mm256_mullo_epi32(val0_0_lo, coef0_256);
                __m256i mul0_0_hi = _mm256_mullo_epi32(val0_0_hi, coef0_256);
                __m256i mul2_0_lo = _mm256_mullo_epi32(val2_0_lo, coef2_256);
                __m256i mul2_0_hi = _mm256_mullo_epi32(val2_0_hi, coef2_256);
                __m256i mul0_16_lo = _mm256_mullo_epi32(val0_16_lo, coef0_256);
                __m256i mul0_16_hi = _mm256_mullo_epi32(val0_16_hi, coef0_256);
                __m256i mul2_16_lo = _mm256_mullo_epi32(val2_16_lo, coef2_256);
                __m256i mul2_16_hi = _mm256_mullo_epi32(val2_16_hi, coef2_256);

                __m256i hadd0_0 = _mm256_hadd_epi32(mul0_0_lo, mul0_0_hi);
                __m256i hadd2_0 = _mm256_hadd_epi32(mul2_0_lo, mul2_0_hi);
                __m256i hadd0_16 = _mm256_hadd_epi32(mul0_16_lo, mul0_16_hi);
                __m256i hadd2_16 = _mm256_hadd_epi32(mul2_16_lo, mul2_16_hi);

                __m256i res_0 = _mm256_add_epi32(hadd0_0, hadd2_0);
                __m256i res_16 = _mm256_add_epi32(hadd0_16, hadd2_16);

                _mm256_storeu_si256((__m256i*)(D + dx), res_0);
                _mm256_storeu_si256((__m256i*)(D + dx + 8), res_16);

            }
            for (; dx < xmax_8; dx+=8)
            {
                int sx = dx * 2;

                __m256i val0_0 = _mm256_loadu_si256((__m256i*)(S + sx - 1));
                __m256i val2_0 = _mm256_loadu_si256((__m256i*)(S + sx + 1));

                __m256i val0_0_lo = _mm256_unpacklo_epi16(val0_0, zero_256);
                __m256i val0_0_hi = _mm256_unpackhi_epi16(val0_0, zero_256);
                __m256i val2_0_lo = _mm256_unpacklo_epi16(val2_0, zero_256);
                __m256i val2_0_hi = _mm256_unpackhi_epi16(val2_0, zero_256);

                __m256i mul0_0_lo = _mm256_mullo_epi32(val0_0_lo, coef0_256);
                __m256i mul0_0_hi = _mm256_mullo_epi32(val0_0_hi, coef0_256);
                __m256i mul2_0_lo = _mm256_mullo_epi32(val2_0_lo, coef2_256);
                __m256i mul2_0_hi = _mm256_mullo_epi32(val2_0_hi, coef2_256);

                __m256i hadd0_0 = _mm256_hadd_epi32(mul0_0_lo, mul0_0_hi);
                __m256i hadd2_0 = _mm256_hadd_epi32(mul2_0_lo, mul2_0_hi);

                __m256i res_0 = _mm256_add_epi32(hadd0_0, hadd2_0);

                _mm256_storeu_si256((__m256i*)(D + dx), res_0);
            }
            for (; dx < xmax_4; dx+=4)
            {
                int sx = dx * 2;

                __m256i val0_0 = _mm256_cvtepu16_epi32(_mm_loadu_si128((__m128i*)(S + sx - 1)));
                __m256i val2_0 = _mm256_cvtepu16_epi32(_mm_loadu_si128((__m128i*)(S + sx + 1)));
                __m256i mul0_0 = _mm256_mullo_epi32(val0_0, coef0_256);
                __m256i mul2_0 = _mm256_mullo_epi32(val2_0, coef2_256);

                __m256i hadd0 = _mm256_hadd_epi32(mul0_0, mul0_0);
                __m256i hadd2 = _mm256_hadd_epi32(mul2_0, mul2_0);

                __m256i res_0 = _mm256_add_epi32(hadd0, hadd2);
                res_0 = _mm256_permutevar8x32_epi32(res_0, _mm256_set_epi32(0, 0, 0, 0, 5, 4, 1, 0));

                _mm_storeu_si128 ((__m128i*)(D + dx), _mm256_castsi256_si128(res_0));                
            }

            for (; dx < xmax; dx++)
            {
                int sx = dx * 2;
                D[dx] = S[sx - 1] * alpha[0] + S[sx] * alpha[1] + S[sx + 1] * alpha[2] + S[sx + 2] * alpha[3];
            }
            limit = dwidth;
        }
#if !OPTIMISED_COEFF
        alpha -= dwidth * 4;
#endif
    }
}

unsigned short hbd_castOp_avx2(int64_t val, int bitdepth)
{
    int bits = 22;
    int SHIFT = bits;
    int DELTA = (1 << (bits - 1));
    return CLIP3((val + DELTA) >> SHIFT, 0, ((1 << bitdepth) - 1));
}

static int hbd_clip_avx2(int x, int a, int b)
{
    return x >= a ? (x < b ? x : b - 1) : a;
}

void hbd_vresize_avx2(const int **src, unsigned short *dst, const short *beta, int width, int bitdepth)
{
    int b0 = beta[0], b1 = beta[1], b2 = beta[2], b3 = beta[3];
    const int *S0 = src[0], *S1 = src[1], *S2 = src[2], *S3 = src[3];

    int bits = 22;

    __m256i delta_256 = _mm256_set1_epi64x(1 << (bits - 1));
    __m256i max_char_256 = _mm256_set1_epi64x(((1 << bitdepth) - 1));
    __m256i coef0_256 = _mm256_set1_epi32(beta[0]);
    __m256i coef1_256 = _mm256_set1_epi32(beta[1]);
    __m256i perm_256 = _mm256_set_epi32(7, 5, 3, 1, 6, 4, 2, 0);
    __m256i zero_256 = _mm256_setzero_si256();

    __m128i max_char_128 = _mm_set1_epi64x(((1 << bitdepth) - 1));
    __m128i delta_128 = _mm_set1_epi64x(1 << (bits - 1));
    __m128i coef0_128 = _mm_set1_epi32(beta[0]);
    __m128i coef1_128 = _mm_set1_epi32(beta[1]);
    __m128i zero_128 = _mm_setzero_si128();

    int width_16 = width - (width % 16);
    int width_8 = width - (width % 8);
    int width_4 = width - (width % 4);
    int x = 0;
    for (; x < width_16; x+=16)
    {
        __m256i src0_0 = _mm256_loadu_si256((__m256i*)(S0 + x));
        __m256i src1_0 = _mm256_loadu_si256((__m256i*)(S1 + x));
        __m256i src2_0 = _mm256_loadu_si256((__m256i*)(S2 + x));
        __m256i src3_0 = _mm256_loadu_si256((__m256i*)(S3 + x));

        __m256i src0_8 = _mm256_loadu_si256((__m256i*)(S0 + x + 8));
        __m256i src1_8 = _mm256_loadu_si256((__m256i*)(S1 + x + 8));
        __m256i src2_8 = _mm256_loadu_si256((__m256i*)(S2 + x + 8));
        __m256i src3_8 = _mm256_loadu_si256((__m256i*)(S3 + x + 8));

        __m256i mul0_0 = _mm256_mul_epi32(src0_0, coef0_256);
        __m256i mul1_0 = _mm256_mul_epi32(src1_0, coef1_256);
        __m256i mul2_0 = _mm256_mul_epi32(src2_0, coef1_256);
        __m256i mul3_0 = _mm256_mul_epi32(src3_0, coef0_256);

        __m256i mul0_4 = _mm256_mul_epi32(_mm256_srli_si256(src0_0, 4), coef0_256);
        __m256i mul1_4 = _mm256_mul_epi32(_mm256_srli_si256(src1_0, 4), coef1_256);
        __m256i mul2_4 = _mm256_mul_epi32(_mm256_srli_si256(src2_0, 4), coef1_256);
        __m256i mul3_4 = _mm256_mul_epi32(_mm256_srli_si256(src3_0, 4), coef0_256);

        __m256i mul0_8 = _mm256_mul_epi32(src0_8, coef0_256);
        __m256i mul1_8 = _mm256_mul_epi32(src1_8, coef1_256);
        __m256i mul2_8 = _mm256_mul_epi32(src2_8, coef1_256);
        __m256i mul3_8 = _mm256_mul_epi32(src3_8, coef0_256);

        __m256i mul0_12 = _mm256_mul_epi32(_mm256_srli_si256(src0_8, 4), coef0_256);
        __m256i mul1_12 = _mm256_mul_epi32(_mm256_srli_si256(src1_8, 4), coef1_256);
        __m256i mul2_12 = _mm256_mul_epi32(_mm256_srli_si256(src2_8, 4), coef1_256);
        __m256i mul3_12 = _mm256_mul_epi32(_mm256_srli_si256(src3_8, 4), coef0_256);

        __m256i accum_01_0 = _mm256_add_epi64(mul0_0, mul1_0);
        __m256i accum_23_0 = _mm256_add_epi64(mul2_0, mul3_0);
        __m256i accum_01_4 = _mm256_add_epi64(mul0_4, mul1_4);
        __m256i accum_23_4 = _mm256_add_epi64(mul2_4, mul3_4);
        __m256i accum_01_8 = _mm256_add_epi64(mul0_8, mul1_8);
        __m256i accum_23_8 = _mm256_add_epi64(mul2_8, mul3_8);
        __m256i accum_01_12 = _mm256_add_epi64(mul0_12, mul1_12);
        __m256i accum_23_12 = _mm256_add_epi64(mul2_12, mul3_12);

        __m256i accum_0123_0 = _mm256_add_epi64(accum_01_0, accum_23_0);
        __m256i accum_0123_4 = _mm256_add_epi64(accum_01_4, accum_23_4);
        __m256i accum_0123_8 = _mm256_add_epi64(accum_01_8, accum_23_8);
        __m256i accum_0123_12 = _mm256_add_epi64(accum_01_12, accum_23_12);

        accum_0123_0 = _mm256_add_epi64(accum_0123_0, delta_256);
        accum_0123_4 = _mm256_add_epi64(accum_0123_4, delta_256);
        accum_0123_8 = _mm256_add_epi64(accum_0123_8, delta_256);
        accum_0123_12 = _mm256_add_epi64(accum_0123_12, delta_256);

        shift22_64b_signExt(accum_0123_0, accum_0123_0);
        shift22_64b_signExt(accum_0123_4, accum_0123_4);
        shift22_64b_signExt(accum_0123_8, accum_0123_8);
        shift22_64b_signExt(accum_0123_12, accum_0123_12);

        accum_0123_0 = _mm256_max_epi32(accum_0123_0, zero_256);
        accum_0123_4 = _mm256_max_epi32(accum_0123_4, zero_256);
        accum_0123_8 = _mm256_max_epi32(accum_0123_8, zero_256);
        accum_0123_12 = _mm256_max_epi32(accum_0123_12, zero_256);

        accum_0123_0 = _mm256_min_epi32(accum_0123_0, max_char_256);
        accum_0123_4 = _mm256_min_epi32(accum_0123_4, max_char_256);
        accum_0123_8 = _mm256_min_epi32(accum_0123_8, max_char_256);
        accum_0123_12 = _mm256_min_epi32(accum_0123_12, max_char_256);

        accum_0123_0 = _mm256_or_si256(accum_0123_0, _mm256_slli_epi32(accum_0123_4, 16));
        accum_0123_8 = _mm256_or_si256(accum_0123_8, _mm256_slli_epi32(accum_0123_12, 16));
        accum_0123_0 = _mm256_or_si256(accum_0123_0, _mm256_slli_epi64(accum_0123_8, 32));

        accum_0123_0 = _mm256_permutevar8x32_epi32(accum_0123_0, perm_256);

        _mm256_storeu_si256((__m256i*)(dst + x), accum_0123_0);
    }
    for (; x < width_8; x+=8)
    {
        __m256i src0_0 = _mm256_loadu_si256((__m256i*)(S0 + x));
        __m256i src1_0 = _mm256_loadu_si256((__m256i*)(S1 + x));
        __m256i src2_0 = _mm256_loadu_si256((__m256i*)(S2 + x));
        __m256i src3_0 = _mm256_loadu_si256((__m256i*)(S3 + x));

        __m256i mul0_0 = _mm256_mul_epi32(src0_0, coef0_256);
        __m256i mul1_0 = _mm256_mul_epi32(src1_0, coef1_256);
        __m256i mul2_0 = _mm256_mul_epi32(src2_0, coef1_256);
        __m256i mul3_0 = _mm256_mul_epi32(src3_0, coef0_256);

        __m256i mul0_4 = _mm256_mul_epi32(_mm256_srli_si256(src0_0, 4), coef0_256);
        __m256i mul1_4 = _mm256_mul_epi32(_mm256_srli_si256(src1_0, 4), coef1_256);
        __m256i mul2_4 = _mm256_mul_epi32(_mm256_srli_si256(src2_0, 4), coef1_256);
        __m256i mul3_4 = _mm256_mul_epi32(_mm256_srli_si256(src3_0, 4), coef0_256);

        __m256i accum_01_0 = _mm256_add_epi64(mul0_0, mul1_0);
        __m256i accum_23_0 = _mm256_add_epi64(mul2_0, mul3_0);
        __m256i accum_01_4 = _mm256_add_epi64(mul0_4, mul1_4);
        __m256i accum_23_4 = _mm256_add_epi64(mul2_4, mul3_4);
        __m256i accum_0123_0 = _mm256_add_epi64(accum_01_0, accum_23_0);
        __m256i accum_0123_4 = _mm256_add_epi64(accum_01_4, accum_23_4);

        accum_0123_0 = _mm256_add_epi64(accum_0123_0, delta_256);
        accum_0123_4 = _mm256_add_epi64(accum_0123_4, delta_256);

        shift22_64b_signExt(accum_0123_0, accum_0123_0);
        shift22_64b_signExt(accum_0123_4, accum_0123_4);

        accum_0123_0 = _mm256_max_epi32(accum_0123_0, zero_256);
        accum_0123_4 = _mm256_max_epi32(accum_0123_4, zero_256);

        accum_0123_0 = _mm256_min_epi32(accum_0123_0, max_char_256);
        accum_0123_4 = _mm256_min_epi32(accum_0123_4, max_char_256);

        accum_0123_0 = _mm256_or_si256(accum_0123_0, _mm256_slli_epi32(accum_0123_4, 16));
        __m128i accum = _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(accum_0123_0, perm_256));

        _mm_storeu_si128((__m128i*)(dst + x), accum);
    }
    for (; x < width_4; x+=4)
    {
        __m128i src0_0 = _mm_loadu_si128((__m128i*)(S0 + x));
        __m128i src1_0 = _mm_loadu_si128((__m128i*)(S1 + x));
        __m128i src2_0 = _mm_loadu_si128((__m128i*)(S2 + x));
        __m128i src3_0 = _mm_loadu_si128((__m128i*)(S3 + x));

        __m128i mul0_0 = _mm_mul_epi32(src0_0, coef0_128);
        __m128i mul1_0 = _mm_mul_epi32(src1_0, coef1_128);
        __m128i mul2_0 = _mm_mul_epi32(src2_0, coef1_128);
        __m128i mul3_0 = _mm_mul_epi32(src3_0, coef0_128);

        __m128i mul0_4 = _mm_mul_epi32(_mm_srli_si128(src0_0, 4), coef0_128);
        __m128i mul1_4 = _mm_mul_epi32(_mm_srli_si128(src1_0, 4), coef1_128);
        __m128i mul2_4 = _mm_mul_epi32(_mm_srli_si128(src2_0, 4), coef1_128);
        __m128i mul3_4 = _mm_mul_epi32(_mm_srli_si128(src3_0, 4), coef0_128);

        __m128i accum_01_0 = _mm_add_epi64(mul0_0, mul1_0);
        __m128i accum_23_0 = _mm_add_epi64(mul2_0, mul3_0);
        __m128i accum_01_4 = _mm_add_epi64(mul0_4, mul1_4);
        __m128i accum_23_4 = _mm_add_epi64(mul2_4, mul3_4);
        __m128i accum_0123_0 = _mm_add_epi64(accum_01_0, accum_23_0);
        __m128i accum_0123_4 = _mm_add_epi64(accum_01_4, accum_23_4);
        accum_0123_0 = _mm_add_epi64(accum_0123_0, delta_128);
        accum_0123_4 = _mm_add_epi64(accum_0123_4, delta_128);

        shift22_64b_signExt_128(accum_0123_0, accum_0123_0);
        shift22_64b_signExt_128(accum_0123_4, accum_0123_4);
       
        accum_0123_0 = _mm_max_epi32(accum_0123_0, zero_128);
        accum_0123_4 = _mm_max_epi32(accum_0123_4, zero_128);

        accum_0123_0 = _mm_min_epi32(accum_0123_0, max_char_128);
        accum_0123_4 = _mm_min_epi32(accum_0123_4, max_char_128);

        accum_0123_0 = _mm_or_si128(accum_0123_0, _mm_slli_epi32(accum_0123_4, 16));
        accum_0123_0 = _mm_or_si128(accum_0123_0, _mm_srli_si128(accum_0123_0, 4));

        _mm_storel_epi64((__m128i*)(dst + x), accum_0123_0);
    }
    for (; x < width; x++)
        dst[x] = hbd_castOp_avx2((int64_t)S0[x] * b0 + (int64_t)S1[x] * b1 + (int64_t)S2[x] * b2 + (int64_t)S3[x] * b3, bitdepth);
}

#if OPTIMISED_COEFF
void hbd_step_avx2(const unsigned short *_src, unsigned short *_dst, const short *_alpha, const short *_beta, int iwidth, int iheight, int dwidth, int channels, int ksize, int start, int end, int xmin, int xmax, int bitdepth)
#else
void hbd_step_avx2(const unsigned short *_src, unsigned short *_dst, const int *xofs, const int *yofs, const short *_alpha, const short *_beta, int iwidth, int iheight, int dwidth, int dheight, int channels, int ksize, int start, int end, int xmin, int xmax, int bitdepth)
#endif
{
    int dy, cn = channels;

    int bufstep = (int)((dwidth + 16 - 1) & -16);
    int *_buffer = (int *)malloc(bufstep * ksize * sizeof(int));
    if (_buffer == NULL)
    {
        printf("resizer: malloc fails\n");
        return;
    }
    const unsigned short *srows[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int *rows[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int prev_sy[HBD_MAX_ESIZE_avx2];

    for (int k = 0; k < ksize; k++)
    {
        prev_sy[k] = -1;
        rows[k] = _buffer + bufstep * k;
    }

#if !OPTIMISED_COEFF
    const short *beta = _beta + ksize * start;
#endif

#if OPTIMISED_COEFF
    for (dy = start; dy < end; dy++)
    {
        int sy0 = dy * 2;
#else
    for (dy = start; dy < end; dy++, beta += ksize)
    {
        int sy0 = yofs[dy];
#endif
        int k0 = ksize, k1 = 0, ksize2 = ksize / 2;

        for (int k = 0; k < ksize; k++)
        {
            int sy = hbd_clip_avx2(sy0 - ksize2 + 1 + k, 0, iheight);
            for (k1 = MAX(k1, k); k1 < ksize; k1++)
            {
                if (k1 < HBD_MAX_ESIZE_avx2 && sy == prev_sy[k1]) // if the sy-th row has been computed already, reuse it.
                {
                    if (k1 > k)
                        memcpy(rows[k], rows[k1], bufstep * sizeof(rows[0][0]));
                    break;
                }
            }
            if (k1 == ksize)
                k0 = MIN(k0, k); // remember the first row that needs to be computed
            srows[k] = _src + (sy * iwidth);
            prev_sy[k] = sy;
        }



#if OPTIMISED_COEFF
        if (k0 < ksize)
        {
            hbd_hresize_avx2((srows + k0), (rows + k0), ksize - k0, _alpha,
                        iwidth, dwidth, cn, xmin, xmax);
        }
        hbd_vresize_avx2((const int **)rows, (_dst + dwidth * dy), _beta, dwidth, bitdepth);
#else
        if (k0 < ksize)
        {
            hbd_hresize_avx2((srows + k0), (rows + k0), ksize - k0, xofs, _alpha,
                        iwidth, dwidth, cn, xmin, xmax);
        }
        hbd_vresize_avx2((const int **)rows, (_dst + dwidth * dy), beta, dwidth, bitdepth);
#endif
    }
    free(_buffer);
}
/*
void hbd_resize_avx2(const unsigned short *_src, unsigned short *_dst, int iwidth, int iheight, int dwidth, int dheight, int bitdepth)
{
    // int depth = 0;
    int cn = 1;
    double inv_scale_x = (double)dwidth / iwidth;

    int ksize = 4, ksize2;
    ksize2 = ksize / 2;

    int xmin = 0, xmax = dwidth;

#if OPTIMISED_COEFF
    const short ibeta[] = {-192, 1216, 1216, -192};
    const short ialpha[] = {-192, 1216, 1216, -192};
    double scale_x = 1. / inv_scale_x;
    float fx;
    int sx;

    for (int dx = 0; dx < dwidth; dx++)
    {
        fx = (float)((dx + 0.5) * scale_x - 0.5);
        sx = (int)floor(fx);
        fx -= sx;

        if (sx < ksize2 - 1)
        {
            xmin = dx + 1;
        }

        if (sx + ksize2 >= iwidth)
        {
            xmax = MIN(xmax, dx);
        }
    }
    hbd_step_avx2(_src, _dst, ialpha, ibeta, iwidth, iheight, dwidth, cn, ksize, 0, dheight, xmin, xmax, bitdepth);

#else
    double inv_scale_y = (double)dheight / iheight;
    double scale_x = 1. / inv_scale_x, scale_y = 1. / inv_scale_y;
    width = dwidth * cn;

    int iscale_x = (int)scale_x;
    int iscale_y = (int)scale_y;

    int k, sx, sy, dx, dy;

    float fx, fy;

    unsigned short *_buffer = (unsigned short *)malloc((width + dheight) * (sizeof(int) + sizeof(float) * ksize));

    int *xofs = (int *)_buffer;
    int *yofs = xofs + width;
    float *alpha = (float *)(yofs + dheight);
    short *ialpha = (short *)alpha;
    float *beta = alpha + width * ksize;
    short *ibeta = ialpha + width * ksize;
    float cbuf[4] = {0};

    for (dx = 0; dx < dwidth; dx++)
    {
        fx = (float)((dx + 0.5) * scale_x - 0.5);
        sx = (int)floor(fx);
        fx -= sx;

        if (sx < ksize2 - 1)
        {
            xmin = dx + 1;
        }

        if (sx + ksize2 >= iwidth)
        {
            xmax = MIN(xmax, dx);
        }

        for (k = 0, sx *= cn; k < cn; k++)
            xofs[dx * cn + k] = sx + k;

        interpolateCubic(fx, cbuf);
        for (k = 0; k < ksize; k++)
            ialpha[dx * cn * ksize + k] = (short)(cbuf[k] * HBD_INTER_RESIZE_COEF_SCALE_avx2);
        for (; k < cn * ksize; k++)
            ialpha[dx * cn * ksize + k] = ialpha[dx * cn * ksize + k - ksize];
    }

    for (dy = 0; dy < dheight; dy++)
    {
        fy = (float)((dy + 0.5) * scale_y - 0.5);
        sy = (int)floor(fy);
        fy -= sy;

        yofs[dy] = sy;

        interpolateCubic(fy, cbuf);
        for (k = 0; k < ksize; k++)
            ibeta[dy * ksize + k] = (short)(cbuf[k] * HBD_INTER_RESIZE_COEF_SCALE_avx2);
    }
    hbd_step_avx2(_src, _dst, xofs, yofs, ialpha, ibeta, iwidth, iheight, dwidth, dheight, cn, ksize, 0, dheight, xmin, xmax, bitdepth);
#endif

}
*/