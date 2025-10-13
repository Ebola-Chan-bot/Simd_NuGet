/*
* Simd Library (http://ermig1979.github.io/Simd).
*
* Copyright (c) 2011-2025 Yermalayeu Ihar.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/
#include "Simd/SimdSynetQuantizedConvolution.h"
#include "Simd/SimdSynetQuantizedDepthwise.h"
#include "Simd/SimdSynetQuantizeLinear.h"
#include "Simd/SimdSynetConvolution8iCommon.h"
#include "Simd/SimdSynet.h"
#include "Simd/SimdMath.h"
#include "Simd/SimdBase.h"
#include "Simd/SimdSse41.h"
#include "Simd/SimdCpu.h"

namespace Simd
{
#if defined(SIMD_SSE41_ENABLE) && defined(SIMD_SYNET_ENABLE)
    namespace Sse41
    {
        using AlgParam = SynetQuantizedConvolutionNhwcDepthwiseV2::AlgParam;

        //------------------------------------------------------------------------------------------------

        SIMD_INLINE void Madd2(__m128i& i32, __m128i u8, __m128i i8)
        {
            i32 = _mm_add_epi32(i32, _mm_madd_epi16(u8, i8));
        }

        //------------------------------------------------------------------------------------------------

        static void QuantizedConvolutionNhwcDepthwiseV2_Preprocess(const uint8_t* src, const uint8_t* zero, const ConvParam& p, const AlgParam& a, size_t dyBeg, size_t dyEnd, int16_t* dst)
        {
            __m128i _zero = _mm_set1_epi16(zero[0]);
            size_t srcC = p.srcC, srcCF = Simd::AlignLo(p.srcC, a.F), byMask = a.bufH - 1;
            size_t byPad = p.kernelY - 1, srcR = p.srcW * p.srcC, bufR = a.bufW * a.bufC;
            size_t byBeg = dyBeg ? dyBeg * p.strideY + byPad : 0, byEnd = dyEnd * p.strideY + byPad;
            if (a.reorderType == 0)
            {
                size_t bxPad = p.padX * a.bufC * 2, bwPad = p.padW * a.bufC * 2;
                for (size_t by = byBeg; by < byEnd; by += 2)
                {
                    int16_t* pd = dst + (by & byMask) * bufR;
                    size_t sy = by - p.padY;
                    const uint8_t* ps0 = (sy + 0) < p.srcH ? src + (sy + 0) * srcR : zero;
                    const uint8_t* ps1 = (sy + 1) < p.srcH ? src + (sy + 1) * srcR : zero;
                    if (bxPad)
                    {
                        for (size_t i = 0; i < bxPad; i += DF)
                            _mm_storeu_si128((__m128i*)(pd + i), _zero);
                        pd += bxPad;
                    }
                    for (size_t sx = 0; sx < p.srcW; sx++)
                    {
                        size_t sc = 0;
                        for (; sc < srcC; sc += F, pd += DF)
                        {
                            __m128i s0 = _mm_cvtepu8_epi32(_mm_cvtsi32_si128(*(int32_t*)(ps0 + sc)));
                            __m128i s1 = _mm_cvtepu8_epi32(_mm_cvtsi32_si128(*(int32_t*)(ps1 + sc)));
                            _mm_storeu_si128((__m128i*)pd, _mm_or_si128(s0, _mm_slli_epi32(s1, 16)));
                        }
                        ps0 += p.srcC;
                        ps1 += p.srcC;
                    }
                    if (bwPad)
                    {
                        for (size_t i = 0; i < bwPad; i += DF)
                            _mm_storeu_si128((__m128i*)(pd + i), _zero);
                        pd += bwPad;
                    }
                }
            }
            else
            {
                size_t bW = a.bufW * 2, bC = a.bufC, xPad = p.padX * 2, wPad = p.padW * 2;
                for (size_t by = byBeg; by < byEnd; by += 2)
                {
                    int16_t* pd = dst + (by & byMask) * bufR;
                    size_t sy = by - p.padY;
                    const uint8_t* ps0 = (sy + 0) < p.srcH ? src + (sy + 0) * srcR : zero;
                    const uint8_t* ps1 = (sy + 1) < p.srcH ? src + (sy + 1) * srcR : zero;
                    if (xPad)
                    {
                        for (size_t x = 0; x < xPad; x += 2, pd += DF)
                            for (size_t c = 0; c < bC; c += F)
                                _mm_storeu_si128((__m128i*)(pd + c * bW), _zero);
                    }
                    for (size_t sx = 0; sx < p.srcW; sx++, pd += DF)
                    {
                        for (size_t sc = 0; sc < bC; sc += F)
                        {
                            __m128i s0 = _mm_cvtepu8_epi32(_mm_cvtsi32_si128(*(int32_t*)(ps0 + sc)));
                            __m128i s1 = _mm_cvtepu8_epi32(_mm_cvtsi32_si128(*(int32_t*)(ps1 + sc)));
                            _mm_storeu_si128((__m128i*)(pd + sc * bW), _mm_or_si128(s0, _mm_slli_epi32(s1, 16)));
                        }
                        ps0 += p.srcC;
                        ps1 += p.srcC;
                    }
                    if (wPad)
                    {
                        for (size_t x = 0; x < wPad; x += 2, pd += DF)
                            for (size_t c = 0; c < bC; c += F)
                                _mm_storeu_si128((__m128i*)(pd + c * bW), _zero);
                    }
                }
            }
        }

        //------------------------------------------------------------------------------------------------

        template <Term8iType term> void QuantizedConvolutionNhwcDepthwiseV2_AnyR0(const int16_t* src, const ConvParam& p, const AlgParam& a, size_t dyBeg, size_t dyEnd, const int16_t* weight,
            const int32_t* sBias, const float* sNorm, int32_t iZero, float iScale, const float* params, float dNorm, int32_t dZero, uint8_t* dst)
        {
            __m128i _zero = _mm_set1_epi32(dZero);
            __m128i d00, d01, d02, d03, d10, d11, d12, d13, w0, s0;
            size_t srcC = p.srcC, srcCF = AlignLo(srcC, F), srcCF4 = AlignLo(srcC, F * 4), kY = p.kernelY, kX = p.kernelX, sY = p.strideY, sX = p.strideX;
            size_t byMask = a.bufH - 1, bufC = a.bufC * 2, bufR = a.bufR, dstW2 = AlignLo(p.dstW, 2), dD = p.dstC * a.srcE, dX = sX * bufC;
            size_t dyEnd2 = dyBeg + (sY == 1 ? AlignLo(dyEnd - dyBeg, 2) : 0), sizeW = a.sizeW, dyD = p.dstW * dD;
            dst += dyBeg * p.dstW * p.dstC * a.srcE;
            size_t dy = dyBeg;
            for (; dy < dyEnd2; dy += 2)
            {
                size_t dx = 0;
                for (; dx < p.dstW; ++dx)
                {
                    const int16_t* ps0 = src + dx * sX * bufC;
                    size_t sc = 0;
                    for (; sc < srcCF4; sc += F * 4)
                    {
                        d00 = _mm_setzero_si128();
                        d01 = _mm_setzero_si128();
                        d02 = _mm_setzero_si128();
                        d03 = _mm_setzero_si128();
                        d10 = _mm_setzero_si128();
                        d11 = _mm_setzero_si128();
                        d12 = _mm_setzero_si128();
                        d13 = _mm_setzero_si128();
                        const int16_t* pw0 = weight + sc * 2, *pw1 = pw0 + sizeW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps = ps0 + (sy & byMask) * bufR + sc * 2;
                            for (size_t kx = 0; kx < kX; ++kx, ps += bufC, pw0 += bufC, pw1 += bufC)
                            {
                                s0 = _mm_loadu_si128((__m128i*)ps + 0);
                                Madd2(d00, s0, _mm_loadu_si128((__m128i*)pw0 + 0));
                                Madd2(d10, s0, _mm_loadu_si128((__m128i*)pw1 + 0));
                                s0 = _mm_loadu_si128((__m128i*)ps + 1);
                                Madd2(d01, s0, _mm_loadu_si128((__m128i*)pw0 + 1));
                                Madd2(d11, s0, _mm_loadu_si128((__m128i*)pw1 + 1));
                                s0 = _mm_loadu_si128((__m128i*)ps + 2);
                                Madd2(d02, s0, _mm_loadu_si128((__m128i*)pw0 + 2));
                                Madd2(d12, s0, _mm_loadu_si128((__m128i*)pw1 + 2));
                                s0 = _mm_loadu_si128((__m128i*)ps + 3);
                                Madd2(d03, s0, _mm_loadu_si128((__m128i*)pw0 + 3));
                                Madd2(d13, s0, _mm_loadu_si128((__m128i*)pw1 + 3));
                            }
                        }
                        Save2<term>(dst, dst + dyD, d00, d10, sBias, sNorm, _zero, sc + F * 0);
                        Save2<term>(dst, dst + dyD, d01, d11, sBias, sNorm, _zero, sc + F * 1);
                        Save2<term>(dst, dst + dyD, d02, d12, sBias, sNorm, _zero, sc + F * 2);
                        Save2<term>(dst, dst + dyD, d03, d13, sBias, sNorm, _zero, sc + F * 3);
                    }
                    for (; sc < srcCF; sc += F)
                    {
                        d00 = _mm_setzero_si128();
                        d10 = _mm_setzero_si128();
                        const int16_t* pw0 = weight + sc * 2, * pw1 = pw0 + sizeW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps = ps0 + (sy & byMask) * bufR + sc * 2;
                            for (size_t kx = 0; kx < kX; ++kx, ps += bufC, pw0 += bufC, pw1 += bufC)
                            {
                                s0 = _mm_loadu_si128((__m128i*)ps + 0);
                                Madd2(d00, s0, _mm_loadu_si128((__m128i*)pw0 + 0));
                                Madd2(d10, s0, _mm_loadu_si128((__m128i*)pw1 + 0));
                            }
                        }
                        Save2<term>(dst, dst + dyD, d00, d10, sBias, sNorm, _zero, sc + F * 0);
                    }
                    for (; sc < srcC; sc += F)
                    {
                        d00 = _mm_setzero_si128();
                        d10 = _mm_setzero_si128();
                        const int16_t* pw0 = weight + sc * 2, * pw1 = pw0 + sizeW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps = ps0 + (sy & byMask) * bufR + sc * 2;
                            for (size_t kx = 0; kx < kX; ++kx, ps += bufC, pw0 += bufC, pw1 += bufC)
                            {
                                s0 = _mm_loadu_si128((__m128i*)ps + 0);
                                Madd2(d00, s0, _mm_loadu_si128((__m128i*)pw0 + 0));
                                Madd2(d10, s0, _mm_loadu_si128((__m128i*)pw1 + 0));
                            }
                        }
                        Save2<term>(dst, dst + dyD, d00, d10, sBias, sNorm, _zero, sc + F * 0, srcC - srcCF);
                    }
                    dst += dD;
                }
            }
            for (; dy < dyEnd; ++dy)
            {
                size_t dx = 0;
                for (; dx < dstW2; dx += 2)
                {
                    const int16_t* ps00 = src + (dx + 0) * sX * bufC;
                    uint8_t* dst0 = dst, * dst1 = dst + dD;
                    size_t sc = 0;
                    for (; sc < srcCF4; sc += F * 4)
                    {
                        d00 = _mm_setzero_si128();
                        d01 = _mm_setzero_si128();
                        d02 = _mm_setzero_si128();
                        d03 = _mm_setzero_si128();
                        d10 = _mm_setzero_si128();
                        d11 = _mm_setzero_si128();
                        d12 = _mm_setzero_si128();
                        d13 = _mm_setzero_si128();
                        const int16_t* pw = weight + sc * 2;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps0 = ps00 + (sy & byMask) * bufR + sc * 2, * ps1 = ps0 + dX;
                            for (size_t kx = 0; kx < kX; ++kx, ps0 += bufC, ps1 += bufC, pw += bufC)
                            {
                                w0 = _mm_loadu_si128((__m128i*)pw + 0);
                                Madd2(d00, _mm_loadu_si128((__m128i*)ps0 + 0), w0);
                                Madd2(d10, _mm_loadu_si128((__m128i*)ps1 + 0), w0);
                                w0 = _mm_loadu_si128((__m128i*)pw + 1);
                                Madd2(d01, _mm_loadu_si128((__m128i*)ps0 + 1), w0);
                                Madd2(d11, _mm_loadu_si128((__m128i*)ps1 + 1), w0);
                                w0 = _mm_loadu_si128((__m128i*)pw + 2);
                                Madd2(d02, _mm_loadu_si128((__m128i*)ps0 + 2), w0);
                                Madd2(d12, _mm_loadu_si128((__m128i*)ps1 + 2), w0);
                                w0 = _mm_loadu_si128((__m128i*)pw + 3);
                                Madd2(d03, _mm_loadu_si128((__m128i*)ps0 + 3), w0);
                                Madd2(d13, _mm_loadu_si128((__m128i*)ps1 + 3), w0);
                            }
                        }
                        Save2<term>(dst, dst + dD, d00, d10, sBias, sNorm, _zero, sc + F * 0);
                        Save2<term>(dst, dst + dD, d01, d11, sBias, sNorm, _zero, sc + F * 1);
                        Save2<term>(dst, dst + dD, d02, d12, sBias, sNorm, _zero, sc + F * 2);
                        Save2<term>(dst, dst + dD, d03, d13, sBias, sNorm, _zero, sc + F * 3);
                    }
                    for (; sc < srcCF; sc += F)
                    {
                        d00 = _mm_setzero_si128();
                        d10 = _mm_setzero_si128();
                        const int16_t* pw = weight + sc * 2;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps0 = ps00 + (sy & byMask) * bufR + sc * 2, * ps1 = ps0 + dX;
                            for (size_t kx = 0; kx < kX; ++kx, ps0 += bufC, ps1 += bufC, pw += bufC)
                            {
                                w0 = _mm_loadu_si128((__m128i*)pw + 0);
                                Madd2(d00, _mm_loadu_si128((__m128i*)ps0 + 0), w0);
                                Madd2(d10, _mm_loadu_si128((__m128i*)ps1 + 0), w0);
                            }
                        }
                        Save2<term>(dst, dst + dD, d00, d10, sBias, sNorm, _zero, sc + F * 0);
                    }
                    for (; sc < srcC; sc += F)
                    {
                        d00 = _mm_setzero_si128();
                        d10 = _mm_setzero_si128();
                        const int16_t* pw = weight + sc * 2;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps0 = ps00 + (sy & byMask) * bufR + sc * 2, * ps1 = ps0 + dX;
                            for (size_t kx = 0; kx < kX; ++kx, ps0 += bufC, ps1 += bufC, pw += bufC)
                            {
                                w0 = _mm_loadu_si128((__m128i*)pw + 0);
                                Madd2(d00, _mm_loadu_si128((__m128i*)ps0 + 0), w0);
                                Madd2(d10, _mm_loadu_si128((__m128i*)ps1 + 0), w0);
                            }
                        }
                        Save2<term>(dst, dst + dD, d00, d10, sBias, sNorm, _zero, sc + F * 0, srcC - srcCF);
                    }
                    dst += 2 * dD;
                }
                for (; dx < p.dstW; ++dx)
                {
                    const int16_t* ps0 = src + dx * sX * bufC;
                    size_t sc = 0;
                    for (; sc < srcCF4; sc += F * 4)
                    {
                        d00 = _mm_setzero_si128();
                        d01 = _mm_setzero_si128();
                        d02 = _mm_setzero_si128();
                        d03 = _mm_setzero_si128();
                        const int16_t* pw = weight + sc * 2;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps = ps0 + (sy & byMask) * bufR + sc * 2;
                            for (size_t kx = 0; kx < kX; ++kx, ps += bufC, pw += bufC)
                            {
                                w0 = _mm_loadu_si128((__m128i*)pw + 0);
                                Madd2(d00, _mm_loadu_si128((__m128i*)ps + 0), w0);
                                w0 = _mm_loadu_si128((__m128i*)pw + 1);
                                Madd2(d01, _mm_loadu_si128((__m128i*)ps + 1), w0);
                                w0 = _mm_loadu_si128((__m128i*)pw + 2);
                                Madd2(d02, _mm_loadu_si128((__m128i*)ps + 2), w0);
                                w0 = _mm_loadu_si128((__m128i*)pw + 3);
                                Madd2(d03, _mm_loadu_si128((__m128i*)ps + 3), w0);
                            }
                        }
                        Save1<term>(dst, d00, sBias, sNorm, _zero, sc + F * 0);
                        Save1<term>(dst, d01, sBias, sNorm, _zero, sc + F * 1);
                        Save1<term>(dst, d02, sBias, sNorm, _zero, sc + F * 2);
                        Save1<term>(dst, d03, sBias, sNorm, _zero, sc + F * 3);
                    }
                    for (; sc < srcCF; sc += F)
                    {
                        d00 = _mm_setzero_si128();
                        const int16_t* pw = weight + sc * 2;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps = ps0 + (sy & byMask) * bufR + sc * 2;
                            for (size_t kx = 0; kx < kX; ++kx, ps += bufC, pw += bufC)
                            {
                                w0 = _mm_loadu_si128((__m128i*)pw);
                                Madd2(d00, _mm_loadu_si128((__m128i*)ps), w0);
                            }
                        }
                        Save1<term>(dst, d00, sBias, sNorm, _zero, sc);
                    }
                    for (; sc < srcC; sc += F)
                    {
                        d00 = _mm_setzero_si128();
                        const int16_t* pw = weight + sc * 2;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps = ps0 + (sy & byMask) * bufR + sc * 2;
                            for (size_t kx = 0; kx < kX; ++kx, ps += bufC, pw += bufC)
                            {
                                w0 = _mm_loadu_si128((__m128i*)pw);
                                Madd2(d00, _mm_loadu_si128((__m128i*)ps), w0);
                            }
                        }
                        Save1<term>(dst, d00, sBias, sNorm, _zero, sc, srcC - srcCF);
                    }
                    dst += dD;
                }
            }
        }

        //------------------------------------------------------------------------------------------------

        template <Term8iType term> void QuantizedConvolutionNhwcDepthwiseV2_AnyR1(const int16_t* src, const ConvParam& p, const AlgParam& a, size_t dyBeg, size_t dyEnd, const int16_t* weight,
            const int32_t* sBias, const float* sNorm, int32_t iZero, float iScale, const float* params, float dNorm, int32_t dZero, uint8_t* dst)
        {
            __m128 _norm;
            __m128i _zero = _mm_set1_epi32(dZero), _bias;
            __m128i d00, d10, d20, d30, d01, d11, d21, d31, w0, w1, s0;
            size_t srcC = p.srcC, srcCF = AlignLo(srcC, F), kY = p.kernelY, kX = p.kernelX, sY = p.strideY, sX = p.strideX, dX = sX * DF, dW = a.stepW;
            size_t byMask = a.bufH - 1, bW = a.bufW * 2, bufR = a.bufR, dstW2 = AlignLo(p.dstW, 2), dstW4 = AlignLo(p.dstW, 4), dD = p.dstC * a.srcE;
            size_t dyEnd2 = dyBeg + (sY == 1 ? AlignLo(dyEnd - dyBeg, 2) : 0), sizeW = a.sizeW, dyD = p.dstW * dD;
            dst += dyBeg * p.dstW * dD;
            size_t dy = dyBeg;
            for (; dy < dyEnd2; dy += 2)
            {
                size_t sc = 0, sy = dy * sY;
                for (; sc < srcCF; sc += F)
                {
                    uint8_t* pd0 = dst + sc, *pd1 = pd0 + dyD;
                    const int16_t* ps0 = src + sc * bW;
                    _bias = _mm_loadu_si128((__m128i*)(sBias + sc));
                    _norm = _mm_loadu_ps(sNorm + sc);
                    size_t dx = 0;
                    for (; dx < dstW4; dx += 4, ps0 += 4 * dX)
                    {
                        d00 = _mm_setzero_si128();
                        d10 = _mm_setzero_si128();
                        d20 = _mm_setzero_si128();
                        d30 = _mm_setzero_si128();
                        d01 = _mm_setzero_si128();
                        d11 = _mm_setzero_si128();
                        d21 = _mm_setzero_si128();
                        d31 = _mm_setzero_si128();
                        const int16_t* pw0 = weight + sc * dW, *pw1 = pw0 + sizeW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            const int16_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += DF, pw0 += DF, pw1 += DF)
                            {
                                w0 = _mm_loadu_si128((__m128i*)pw0);
                                w1 = _mm_loadu_si128((__m128i*)pw1);
                                s0 = _mm_loadu_si128((__m128i*)(ps + 0 * dX));
                                Madd2(d00, s0, w0);
                                Madd2(d01, s0, w1);
                                s0 = _mm_loadu_si128((__m128i*)(ps + 1 * dX));
                                Madd2(d10, s0, w0);
                                Madd2(d11, s0, w1);
                                s0 = _mm_loadu_si128((__m128i*)(ps + 2 * dX));
                                Madd2(d20, s0, w0);
                                Madd2(d21, s0, w1);
                                s0 = _mm_loadu_si128((__m128i*)(ps + 3 * dX));
                                Madd2(d30, s0, w0);
                                Madd2(d31, s0, w1);
                            }
                        }
                        Save1<term>(pd0 + 0 * dD, d00, _bias, _norm, _zero);
                        Save1<term>(pd0 + 1 * dD, d10, _bias, _norm, _zero);
                        Save1<term>(pd0 + 2 * dD, d20, _bias, _norm, _zero);
                        Save1<term>(pd0 + 3 * dD, d30, _bias, _norm, _zero);
                        Save1<term>(pd1 + 0 * dD, d01, _bias, _norm, _zero);
                        Save1<term>(pd1 + 1 * dD, d11, _bias, _norm, _zero);
                        Save1<term>(pd1 + 2 * dD, d21, _bias, _norm, _zero);
                        Save1<term>(pd1 + 3 * dD, d31, _bias, _norm, _zero);
                        pd0 += 4 * dD;
                        pd1 += 4 * dD;
                    }
                    for (; dx < dstW2; dx += 2, ps0 += 2 * dX)
                    {
                        d00 = _mm_setzero_si128();
                        d10 = _mm_setzero_si128();
                        d01 = _mm_setzero_si128();
                        d11 = _mm_setzero_si128();
                        const int16_t* pw0 = weight + sc * dW, *pw1 = pw0 + sizeW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            const int16_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += DF, pw0 += DF, pw1 += DF)
                            {
                                w0 = _mm_loadu_si128((__m128i*)pw0);
                                w1 = _mm_loadu_si128((__m128i*)pw1);
                                s0 = _mm_loadu_si128((__m128i*)(ps + 0 * dX));
                                Madd2(d00, s0, w0);
                                Madd2(d01, s0, w1);
                                s0 = _mm_loadu_si128((__m128i*)(ps + 1 * dX));
                                Madd2(d10, s0, w0);
                                Madd2(d11, s0, w1);
                            }
                        }
                        Save1<term>(pd0 + 0 * dD, d00, _bias, _norm, _zero);
                        Save1<term>(pd0 + 1 * dD, d10, _bias, _norm, _zero);
                        Save1<term>(pd1 + 0 * dD, d01, _bias, _norm, _zero);
                        Save1<term>(pd1 + 1 * dD, d11, _bias, _norm, _zero);
                        pd0 += 2 * dD;
                        pd1 += 2 * dD;
                    }
                    for (; dx < p.dstW; ++dx, ps0 += dX)
                    {
                        d00 = _mm_setzero_si128();
                        d01 = _mm_setzero_si128();
                        const int16_t* pw0 = weight + sc * dW, *pw1 = pw0 + sizeW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            const int16_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += DF, pw0 += DF, pw1 += DF)
                            {
                                w0 = _mm_loadu_si128((__m128i*)pw0);
                                w1 = _mm_loadu_si128((__m128i*)pw1);
                                s0 = _mm_loadu_si128((__m128i*)(ps + 0 * dX));
                                Madd2(d00, s0, w0);
                                Madd2(d01, s0, w1);
                            }
                        }
                        Save1<term>(pd0 + 0 * dD, d00, _bias, _norm, _zero);
                        Save1<term>(pd1 + 0 * dD, d01, _bias, _norm, _zero);
                        pd0 += dD;
                        pd1 += dD;
                    }
                }
                for (; sc < srcC; sc += F)
                {
                    uint8_t* pd0 = dst + sc, *pd1 = pd0 + dyD;
                    const int16_t* ps0 = src + sc * bW;
                    _bias = _mm_loadu_si128((__m128i*)(sBias + sc));
                    _norm = _mm_loadu_ps(sNorm + sc);
                    size_t dx = 0, tail = srcC - srcCF;
                    for (; dx < p.dstW; ++dx, ps0 += dX)
                    {
                        d00 = _mm_setzero_si128();
                        d01 = _mm_setzero_si128();
                        const int16_t* pw0 = weight + sc * dW, *pw1 = pw0 + sizeW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            const int16_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += DF, pw0 += DF, pw1 += DF)
                            {
                                w0 = _mm_loadu_si128((__m128i*)pw0);
                                w1 = _mm_loadu_si128((__m128i*)pw1);
                                s0 = _mm_loadu_si128((__m128i*)(ps + 0 * dX));
                                Madd2(d00, s0, w0);
                                Madd2(d01, s0, w1);
                            }
                        }
                        Save1<term>(pd0 + 0 * dD, d00, _bias, _norm, _zero, tail);
                        Save1<term>(pd1 + 0 * dD, d01, _bias, _norm, _zero, tail);
                        pd0 += dD;
                        pd1 += dD;
                    }
                }
                dst += p.dstW * 2 * dD;
            }
            for (; dy < dyEnd; ++dy)
            {
                size_t sc = 0, sy = dy * sY;
                for (; sc < srcCF; sc += F)
                {
                    uint8_t* pd = dst + sc;
                    const int16_t* ps0 = src + sc * bW;
                    _bias = _mm_loadu_si128((__m128i*)(sBias + sc));
                    _norm = _mm_loadu_ps(sNorm + sc);
                    size_t dx = 0;
                    for (; dx < dstW4; dx += 4, ps0 += 4 * dX)
                    {
                        d00 = _mm_setzero_si128();
                        d10 = _mm_setzero_si128();
                        d20 = _mm_setzero_si128();
                        d30 = _mm_setzero_si128();
                        const int16_t* pw = weight + sc * dW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            const int16_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += DF, pw += DF)
                            {
                                w0 = _mm_loadu_si128((__m128i*)pw);
                                Madd2(d00, _mm_loadu_si128((__m128i*)(ps + 0 * dX)), w0);
                                Madd2(d10, _mm_loadu_si128((__m128i*)(ps + 1 * dX)), w0);
                                Madd2(d20, _mm_loadu_si128((__m128i*)(ps + 2 * dX)), w0);
                                Madd2(d30, _mm_loadu_si128((__m128i*)(ps + 3 * dX)), w0);
                            }
                        }
                        Save1<term>(pd + 0 * dD, d00, _bias, _norm, _zero);
                        Save1<term>(pd + 1 * dD, d10, _bias, _norm, _zero);
                        Save1<term>(pd + 2 * dD, d20, _bias, _norm, _zero);
                        Save1<term>(pd + 3 * dD, d30, _bias, _norm, _zero);
                        pd += 4 * dD;
                    }
                    for (; dx < dstW2; dx += 2, ps0 += 2 * dX)
                    {
                        d00 = _mm_setzero_si128();
                        d10 = _mm_setzero_si128();
                        const int16_t* pw = weight + sc * dW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            const int16_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += DF, pw += DF)
                            {
                                w0 = _mm_loadu_si128((__m128i*)pw);
                                Madd2(d00, _mm_loadu_si128((__m128i*)(ps + 0 * dX)), w0);
                                Madd2(d10, _mm_loadu_si128((__m128i*)(ps + 1 * dX)), w0);
                            }
                        }
                        Save1<term>(pd + 0 * dD, d00, _bias, _norm, _zero);
                        Save1<term>(pd + 1 * dD, d10, _bias, _norm, _zero);
                        pd += 2 * dD;
                    }
                    for (; dx < p.dstW; ++dx, ps0 += dX)
                    {
                        d00 = _mm_setzero_si128();
                        const int16_t* pw = weight + sc * dW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            const int16_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += DF, pw += DF)
                            {
                                w0 = _mm_loadu_si128((__m128i*)pw);
                                Madd2(d00, _mm_loadu_si128((__m128i*)ps), w0);
                            }
                        }
                        Save1<term>(pd, d00, _bias, _norm, _zero);
                        pd += dD;
                    }
                }
                for (; sc < srcC; sc += F)
                {
                    uint8_t* pd = dst + sc;
                    const int16_t* ps0 = src + sc * bW;
                    _bias = _mm_loadu_si128((__m128i*)(sBias + sc));
                    _norm = _mm_loadu_ps(sNorm + sc);
                    size_t dx = 0, tail = srcC - srcCF;
                    for (; dx < p.dstW; ++dx, ps0 += dX)
                    {
                        d00 = _mm_setzero_si128();
                        const int16_t* pw = weight + sc * dW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            const int16_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += DF, pw += DF)
                            {
                                w0 = _mm_loadu_si128((__m128i*)pw);
                                Madd2(d00, _mm_loadu_si128((__m128i*)ps), w0);
                            }
                        }
                        Save1<term>(pd, d00, _bias, _norm, _zero, tail);
                        pd += dD;
                    }
                }
                dst += p.dstW * dD;
            }
        }

        //------------------------------------------------------------------------------------------------

        template <Term8iType term> void QuantizedConvolutionNhwcDepthwiseV2_3x3R1(const int16_t* src, const ConvParam& p, const AlgParam& a, size_t dyBeg, size_t dyEnd, const int16_t* weight,
            const int32_t* sBias, const float* sNorm, int32_t iZero, float iScale, const float* params, float dNorm, int32_t dZero, uint8_t* dst)
        {
            __m128 _norm;
            __m128i _zero = _mm_set1_epi32(dZero), _bias;
            __m128i d00, d10, w03, w14, w25, s0;
            size_t srcC = p.srcC, srcCF = AlignLo(srcC, F), sY = p.strideY, sX = p.strideX, dX = sX * DF, dW = a.stepW;
            size_t byMask = a.bufH - 1, bW = a.bufW * 2, bufR = a.bufW * a.bufC, dstW2 = sX == 1 ? AlignLo(p.dstW, 2) : 0, dD = p.dstC * a.srcE;
            size_t dyEnd2 = dyBeg + (sY == 1 ? AlignLo(dyEnd - dyBeg, 2) : 0), sizeW = a.sizeW, dyD = p.dstW * dD;
            dst += dyBeg * p.dstW * dD;
            size_t dy = dyBeg;
            for (; dy < dyEnd2; dy += 2)
            {
                __m128i d01, w36, w47, w58;
                size_t sc = 0, sy = dy * sY;
                for (; sc < srcC; sc += F)
                {
                    uint8_t* pd0 = dst + sc, *pd1 = pd0 + dyD;
                    const int16_t* ps0 = src + ((sy + 0) & byMask) * bufR + sc * bW;
                    const int16_t* ps2 = src + ((sy + 2) & byMask) * bufR + sc * bW;
                    const int16_t* pw0 = weight + sc * dW, *pw1 = pw0 + sizeW;
                    _bias = _mm_loadu_si128((__m128i*)(sBias + sc));
                    _norm = _mm_loadu_ps(sNorm + sc);
                    w03 = _mm_loadu_si128((__m128i*)pw0 + 0);
                    w14 = _mm_loadu_si128((__m128i*)pw0 + 1);
                    w25 = _mm_loadu_si128((__m128i*)pw0 + 2);
                    w36 = _mm_loadu_si128((__m128i*)pw1 + 3);
                    w47 = _mm_loadu_si128((__m128i*)pw1 + 4);
                    w58 = _mm_loadu_si128((__m128i*)pw1 + 5);
                    if (sc < srcCF)
                    {
                        size_t dx = 0;
                        for (; dx < p.dstW; ++dx, ps0 += dX, ps2 += dX)
                        {
                            d00 = _mm_setzero_si128();
                            d01 = _mm_setzero_si128();

                            s0 = _mm_loadu_si128((__m128i*)ps0 + 0);
                            Madd2(d00, s0, w03);
                            Madd2(d01, s0, _mm_slli_epi32(w03, 16));
                            s0 = _mm_loadu_si128((__m128i*)ps0 + 1);
                            Madd2(d00, s0, w14);
                            Madd2(d01, s0, _mm_slli_epi32(w14, 16));
                            s0 = _mm_loadu_si128((__m128i*)ps0 + 2);
                            Madd2(d00, s0, w25);
                            Madd2(d01, s0, _mm_slli_epi32(w25, 16));
                            s0 = _mm_loadu_si128((__m128i*)ps2 + 0);
                            Madd2(d00, s0, _mm_srli_epi32(w36, 16));
                            Madd2(d01, s0, w36);
                            s0 = _mm_loadu_si128((__m128i*)ps2 + 1);
                            Madd2(d00, s0, _mm_srli_epi32(w47, 16));
                            Madd2(d01, s0, w47);
                            s0 = _mm_loadu_si128((__m128i*)ps2 + 2);
                            Madd2(d00, s0, _mm_srli_epi32(w58, 16));
                            Madd2(d01, s0, w58);

                            Save1<term>(pd0, d00, _bias, _norm, _zero);
                            Save1<term>(pd1, d01, _bias, _norm, _zero);
                            pd0 += dD;
                            pd1 += dD;
                        }
                    }
                    else
                    {
                        size_t tail = srcC - srcCF;
                        for (size_t dx = 0; dx < p.dstW; ++dx, ps0 += dX, ps2 += dX)
                        {
                            d00 = _mm_setzero_si128();
                            d01 = _mm_setzero_si128();

                            s0 = _mm_loadu_si128((__m128i*)ps0 + 0);
                            Madd2(d00, s0, w03);
                            Madd2(d01, s0, _mm_slli_epi32(w03, 16));
                            s0 = _mm_loadu_si128((__m128i*)ps0 + 1);
                            Madd2(d00, s0, w14);
                            Madd2(d01, s0, _mm_slli_epi32(w14, 16));
                            s0 = _mm_loadu_si128((__m128i*)ps0 + 2);
                            Madd2(d00, s0, w25);
                            Madd2(d01, s0, _mm_slli_epi32(w25, 16));
                            s0 = _mm_loadu_si128((__m128i*)ps2 + 0);
                            Madd2(d00, s0, _mm_srli_epi32(w36, 16));
                            Madd2(d01, s0, w36);
                            s0 = _mm_loadu_si128((__m128i*)ps2 + 1);
                            Madd2(d00, s0, _mm_srli_epi32(w47, 16));
                            Madd2(d01, s0, w47);
                            s0 = _mm_loadu_si128((__m128i*)ps2 + 2);
                            Madd2(d00, s0, _mm_srli_epi32(w58, 16));
                            Madd2(d01, s0, w58);

                            Save1<term>(pd0, d00, _bias, _norm, _zero, tail);
                            Save1<term>(pd1, d01, _bias, _norm, _zero, tail);
                            pd0 += dD;
                            pd1 += dD;
                        }
                    }
                }
                dst += p.dstW * dD * 2;
            }
            for (; dy < dyEnd; ++dy)
            {
                __m128i w6, w7, w8;
                size_t sc = 0, sy = dy * sY;
                for (; sc < srcC; sc += F)
                {
                    uint8_t* pd = dst + sc;
                    const int16_t* ps0 = src + ((sy + 0) & byMask) * bufR + sc * bW;
                    const int16_t* ps2 = src + ((sy + 2) & byMask) * bufR + sc * bW;
                    const int16_t* pw = weight + sc * dW;
                    _bias = _mm_loadu_si128((__m128i*)(sBias + sc));
                    _norm = _mm_loadu_ps(sNorm + sc);
                    w03 = _mm_loadu_si128((__m128i*)pw + 0);
                    w14 = _mm_loadu_si128((__m128i*)pw + 1);
                    w25 = _mm_loadu_si128((__m128i*)pw + 2);
                    w6 = _mm_loadu_si128((__m128i*)pw + 3);
                    w7 = _mm_loadu_si128((__m128i*)pw + 4);
                    w8 = _mm_loadu_si128((__m128i*)pw + 5);
                    if (sc < srcCF)
                    {
                        size_t dx = 0;
                        for (; dx < dstW2; dx += 2, ps0 += QF, ps2 += QF)
                        {
                            d00 = _mm_setzero_si128();
                            d10 = _mm_setzero_si128();

                            s0 = _mm_loadu_si128((__m128i*)ps0 + 0);
                            Madd2(d00, s0, w03);
                            s0 = _mm_loadu_si128((__m128i*)ps0 + 1);
                            Madd2(d00, s0, w14);
                            Madd2(d10, s0, w03);
                            s0 = _mm_loadu_si128((__m128i*)ps0 + 2);
                            Madd2(d00, s0, w25);
                            Madd2(d10, s0, w14);
                            s0 = _mm_loadu_si128((__m128i*)ps0 + 3);
                            Madd2(d10, s0, w25);

                            s0 = _mm_loadu_si128((__m128i*)ps2 + 0);
                            Madd2(d00, s0, w6);
                            s0 = _mm_loadu_si128((__m128i*)ps2 + 1);
                            Madd2(d00, s0, w7);
                            Madd2(d10, s0, w6);
                            s0 = _mm_loadu_si128((__m128i*)ps2 + 2);
                            Madd2(d00, s0, w8);
                            Madd2(d10, s0, w7);
                            s0 = _mm_loadu_si128((__m128i*)ps2 + 3);
                            Madd2(d10, s0, w8);

                            Save1<term>(pd + 0 * dD, d00, _bias, _norm, _zero);
                            Save1<term>(pd + 1 * dD, d10, _bias, _norm, _zero);
                            pd += 2 * dD;
                        }
                        for (; dx < p.dstW; ++dx, ps0 += dX, ps2 += dX)
                        {
                            d00 = _mm_setzero_si128();

                            s0 = _mm_loadu_si128((__m128i*)ps0 + 0);
                            Madd2(d00, s0, w03);
                            s0 = _mm_loadu_si128((__m128i*)ps0 + 1);
                            Madd2(d00, s0, w14);
                            s0 = _mm_loadu_si128((__m128i*)ps0 + 2);
                            Madd2(d00, s0, w25);
                            s0 = _mm_loadu_si128((__m128i*)ps2 + 0);
                            Madd2(d00, s0, w6);
                            s0 = _mm_loadu_si128((__m128i*)ps2 + 1);
                            Madd2(d00, s0, w7);
                            s0 = _mm_loadu_si128((__m128i*)ps2 + 2);
                            Madd2(d00, s0, w8);

                            Save1<term>(pd, d00, _bias, _norm, _zero);
                            pd += dD;
                        }
                    }
                    else
                    {
                        size_t tail = srcC - srcCF;
                        for (size_t dx = 0; dx < p.dstW; ++dx, ps0 += dX, ps2 += dX)
                        {
                            d00 = _mm_setzero_si128();

                            s0 = _mm_loadu_si128((__m128i*)ps0 + 0);
                            Madd2(d00, s0, w03);
                            s0 = _mm_loadu_si128((__m128i*)ps0 + 1);
                            Madd2(d00, s0, w14);
                            s0 = _mm_loadu_si128((__m128i*)ps0 + 2);
                            Madd2(d00, s0, w25);
                            s0 = _mm_loadu_si128((__m128i*)ps2 + 0);
                            Madd2(d00, s0, w6);
                            s0 = _mm_loadu_si128((__m128i*)ps2 + 1);
                            Madd2(d00, s0, w7);
                            s0 = _mm_loadu_si128((__m128i*)ps2 + 2);
                            Madd2(d00, s0, w8);

                            Save1<term>(pd, d00, _bias, _norm, _zero, tail);
                            pd += dD;
                        }
                    }
                }
                dst += p.dstW * dD;
            }
        }

        //------------------------------------------------------------------------------------------------

        template <Term8iType term> void SetV2(const ConvParam& p, const AlgParam& a, SynetQuantizedConvolutionNhwcDepthwiseV2::ConvolutionPtr& convolution)
        {
            if (p.IsKernel(3) && p.IsDilation(1) && a.reorderType == 1)
                convolution = QuantizedConvolutionNhwcDepthwiseV2_3x3R1<term>;
            else
            {
                if (a.reorderType == 0)
                    convolution = QuantizedConvolutionNhwcDepthwiseV2_AnyR0<term>;
                else if (a.reorderType == 1)
                    convolution = QuantizedConvolutionNhwcDepthwiseV2_AnyR1<term>;
                else
                    assert(0);
            }
        }

        //------------------------------------------------------------------------------------------------

        SynetQuantizedConvolutionNhwcDepthwiseV2::SynetQuantizedConvolutionNhwcDepthwiseV2(const ConvParam& p)
            : Base::SynetQuantizedConvolutionNhwcDepthwiseV2(p)
        {
            SetAlgParam(F);
            _preprocess = QuantizedConvolutionNhwcDepthwiseV2_Preprocess;
            if (p.dstT == SimdTensorData8u)
                SetV2<Term8iLast8u>(p, _alg, _convolution);
            //else
            //    SetV2<Term8iLast32f>(p, _alg, _convolution);
        }
    }
#endif
}
