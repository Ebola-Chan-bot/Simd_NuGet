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
#include "Simd/SimdSynetQuantizeLinear.h"
#include "Simd/SimdSynetConvolution8iCommon.h"
#include "Simd/SimdSynet.h"
#include "Simd/SimdMath.h"
#include "Simd/SimdBase.h"
#include "Simd/SimdAvx512bw.h"
#include "Simd/SimdCpu.h"

namespace Simd
{
#if defined(SIMD_AVX512BW_ENABLE) && defined(SIMD_SYNET_ENABLE)
    namespace Avx512bw
    {
        using AlgParamV0 = SynetQuantizedConvolutionNhwcDepthwiseV0::AlgParam;
        using AlgParamV1 = SynetQuantizedConvolutionNhwcDepthwiseV1::AlgParam;
        using AlgParamV2 = SynetQuantizedConvolutionNhwcDepthwiseV2::AlgParam;

        //------------------------------------------------------------------------------------------------

        SIMD_INLINE __m512i LoadAs32i(const uint8_t* src, __mmask16 tail = -1)
        {
            return _mm512_cvtepu8_epi32(_mm_maskz_loadu_epi8(tail, src));
        }

        SIMD_INLINE __m512i LoadAs32i(const int8_t* src, __mmask16 tail = -1)
        {
            return _mm512_cvtepi8_epi32(_mm_maskz_loadu_epi8(tail, src));
        }

        SIMD_INLINE void Madd1(__m512i& i32, __m512i u8, __m512i i8)
        {
            i32 = _mm512_add_epi32(i32, _mm512_madd_epi16(u8, i8));
        }

        SIMD_INLINE void Madd2(__m512i& i32, __m512i u8, __m512i i8)
        {
            i32 = _mm512_add_epi32(i32, _mm512_madd_epi16(u8, i8));
        }

        //------------------------------------------------------------------------------------------------

        template <Term8iType term> SIMD_INLINE void Save1(uint8_t* dst, __m512i sum, const int32_t* bias, const float* norm, const __m512i& zero, size_t offset, __mmask16 tail = -1)
        {
            __m512i _bias = _mm512_maskz_loadu_epi32(tail, bias + offset);
            __m512 _norm = _mm512_maskz_loadu_ps(tail, norm + offset);
            QuntizedTerm8i<term>::template Save<0>(dst + offset, (int32_t*)NULL, sum, &_bias, &_norm, zero, tail);
        }

        template <Term8iType term> SIMD_INLINE void Save1(uint8_t* dst, __m512i sum, const __m512i& bias, const __m512& norm, const __m512i& zero, __mmask16 tail = -1)
        {
            QuntizedTerm8i<term>::template Save<0>(dst, (int32_t*)NULL, sum, &bias, &norm, zero, tail);
        }

        template <Term8iType term> SIMD_INLINE void Save2(uint8_t* dst0, uint8_t* dst1, __m512i sum0, __m512i sum1, const int32_t* bias, const float* norm, const __m512i& zero, size_t offset, __mmask16 tail = -1)
        {
            __m512i _bias = _mm512_loadu_si512((__m512i*)(bias + offset));
            __m512 _norm = _mm512_loadu_ps(norm + offset);
            QuntizedTerm8i<term>::template Save<0>(dst0 + offset, (int32_t*)NULL, sum0, &_bias, &_norm, zero, tail);
            QuntizedTerm8i<term>::template Save<0>(dst1 + offset, (int32_t*)NULL, sum1, &_bias, &_norm, zero, tail);
        }

        //------------------------------------------------------------------------------------------------

        template <Term8iType term> void QuantizedConvolutionNhwcDepthwiseV0_Default(const uint8_t* src, uint32_t srcZero,
            const ConvParam& p, const AlgParamV0& a, const int8_t* weight, const int32_t* bias, const float* norm, uint32_t dstZero, uint8_t* dst)
        {
            __m512i _srcZero = _mm512_set1_epi32(srcZero);
            __m512i _dstZero = _mm512_set1_epi32(dstZero);
            __m512i d00, d01, d02, d03, w0, w1, w2, w3;
            size_t size = p.group, sizeF = AlignLo(size, F), sizeF2 = AlignLo(size, F * 2), sizeF4 = AlignLo(size, F * 4);
            __mmask16 tail = TailMask16(size - sizeF);
            for (size_t dy = 0; dy < p.dstH; ++dy)
            {
                for (size_t dx = 0; dx < p.dstW; ++dx)
                {
                    size_t i = 0;
                    for (; i < sizeF4; i += F * 4)
                    {
                        d00 = _mm512_setzero_si512();
                        d01 = _mm512_setzero_si512();
                        d02 = _mm512_setzero_si512();
                        d03 = _mm512_setzero_si512();
                        for (size_t ky = 0; ky < p.kernelY; ++ky)
                        {
                            size_t sy = dy * p.strideY + ky * p.dilationY - p.padY;
                            for (size_t kx = 0; kx < p.kernelX; ++kx)
                            {
                                size_t sx = dx * p.strideX + kx * p.dilationX - p.padX;
                                size_t ow = (ky * p.kernelX + kx) * size + i;
                                w0 = LoadAs32i(weight + ow + 0 * F);
                                w1 = LoadAs32i(weight + ow + 1 * F);
                                w2 = LoadAs32i(weight + ow + 2 * F);
                                w3 = LoadAs32i(weight + ow + 3 * F);
                                if (sy < p.srcH && sx < p.srcW)
                                {
                                    size_t os = (sy * p.srcW + sx) * size + i;
                                    Madd1(d00, LoadAs32i(src + os + 0 * F), w0);
                                    Madd1(d01, LoadAs32i(src + os + 1 * F), w1);
                                    Madd1(d02, LoadAs32i(src + os + 2 * F), w2);
                                    Madd1(d03, LoadAs32i(src + os + 3 * F), w3);
                                }
                                else
                                {
                                    Madd1(d00, _srcZero, w0);
                                    Madd1(d01, _srcZero, w1);
                                    Madd1(d02, _srcZero, w2);
                                    Madd1(d03, _srcZero, w3);
                                }
                            }
                        }
                        Save1<term>(dst, d00, bias, norm, _dstZero, i + F * 0);
                        Save1<term>(dst, d01, bias, norm, _dstZero, i + F * 1);
                        Save1<term>(dst, d02, bias, norm, _dstZero, i + F * 2);
                        Save1<term>(dst, d03, bias, norm, _dstZero, i + F * 3);
                    }
                    for (; i < sizeF2; i += F * 2)
                    {
                        d00 = _mm512_setzero_si512();
                        d01 = _mm512_setzero_si512();
                        for (size_t ky = 0; ky < p.kernelY; ++ky)
                        {
                            size_t sy = dy * p.strideY + ky * p.dilationY - p.padY;
                            for (size_t kx = 0; kx < p.kernelX; ++kx)
                            {
                                size_t sx = dx * p.strideX + kx * p.dilationX - p.padX;
                                size_t ow = (ky * p.kernelX + kx) * size + i;
                                w0 = LoadAs32i(weight + ow + 0 * F);
                                w1 = LoadAs32i(weight + ow + 1 * F);
                                if (sy < p.srcH && sx < p.srcW)
                                {
                                    size_t os = (sy * p.srcW + sx) * size + i;
                                    Madd1(d00, LoadAs32i(src + os + 0 * F), w0);
                                    Madd1(d01, LoadAs32i(src + os + 1 * F), w1);
                                }
                                else
                                {
                                    Madd1(d00, _srcZero, w0);
                                    Madd1(d01, _srcZero, w1);
                                }
                            }
                        }
                        Save1<term>(dst, d00, bias, norm, _dstZero, i + F * 0);
                        Save1<term>(dst, d01, bias, norm, _dstZero, i + F * 1);
                    }
                    for (; i < sizeF; i += F)
                    {
                        d00 = _mm512_setzero_si512();
                        for (size_t ky = 0; ky < p.kernelY; ++ky)
                        {
                            size_t sy = dy * p.strideY + ky * p.dilationY - p.padY;
                            for (size_t kx = 0; kx < p.kernelX; ++kx)
                            {
                                size_t sx = dx * p.strideX + kx * p.dilationX - p.padX;
                                size_t ow = (ky * p.kernelX + kx) * size + i;
                                w0 = LoadAs32i(weight + ow + 0 * F);
                                if (sy < p.srcH && sx < p.srcW)
                                {
                                    size_t os = (sy * p.srcW + sx) * size + i;
                                    Madd1(d00, LoadAs32i(src + os + 0 * F), w0);
                                }
                                else
                                {
                                    Madd1(d00, _srcZero, w0);
                                }
                            }
                        }
                        Save1<term>(dst, d00, bias, norm, _dstZero, i + F * 0);
                    }
                    for (; i < size; i += F)
                    {
                        d00 = _mm512_setzero_si512();
                        for (size_t ky = 0; ky < p.kernelY; ++ky)
                        {
                            size_t sy = dy * p.strideY + ky * p.dilationY - p.padY;
                            for (size_t kx = 0; kx < p.kernelX; ++kx)
                            {
                                size_t sx = dx * p.strideX + kx * p.dilationX - p.padX;
                                size_t ow = (ky * p.kernelX + kx) * size + i;
                                w0 = LoadAs32i(weight + ow + 0 * F, tail);
                                if (sy < p.srcH && sx < p.srcW)
                                {
                                    size_t os = (sy * p.srcW + sx) * size + i;
                                    Madd1(d00, LoadAs32i(src + os + 0 * F, tail), w0);
                                }
                                else
                                {
                                    Madd1(d00, _srcZero, w0);
                                }
                            }
                        }
                        Save1<term>(dst, d00, bias, norm, _dstZero, i + F * 0, tail);
                    }
                    dst += p.dstC * a.srcE;
                }
            }
        }

        //------------------------------------------------------------------------------------------------

        template<Term8iType term> SIMD_INLINE void QuantizedConvolutionNhwcDepthwiseV0_3x3Edge(
            const uint8_t* src, const __m512i& srcZero, const ConvParam& p, const AlgParamV0& a, size_t dy, size_t dx,
            const int8_t* weight, const int32_t* bias, const float* norm, const __m512i& dstZero, uint8_t* dst)
        {
            __m512i d00, d01, d02, d03, w0, w1, w2, w3;
            size_t size = p.group;
            size_t sizeF = AlignLo(size, F), sizeDF = AlignLo(size, DF), sizeA = AlignLo(size, A);
            __mmask16 tail = TailMask16(size - sizeF);
            size_t i = 0;
            for (; i < sizeA; i += A)
            {
                d00 = _mm512_setzero_si512();
                d01 = _mm512_setzero_si512();
                d02 = _mm512_setzero_si512();
                d03 = _mm512_setzero_si512();
                for (size_t ky = 0; ky < 3; ++ky)
                {
                    size_t sy = dy * p.strideY + ky - p.padY;
                    for (size_t kx = 0; kx < 3; ++kx)
                    {
                        size_t sx = dx * p.strideX + kx - p.padX;
                        size_t ow = (ky * p.kernelX + kx) * size + i;
                        w0 = LoadAs32i(weight + ow + 0 * F);
                        w1 = LoadAs32i(weight + ow + 1 * F);
                        w2 = LoadAs32i(weight + ow + 2 * F);
                        w3 = LoadAs32i(weight + ow + 3 * F);
                        if (sy < p.srcH && sx < p.srcW)
                        {
                            size_t os = (sy * p.srcW + sx) * size + i;
                            Madd1(d00, LoadAs32i(src + os + 0 * F), w0);
                            Madd1(d01, LoadAs32i(src + os + 1 * F), w1);
                            Madd1(d02, LoadAs32i(src + os + 2 * F), w2);
                            Madd1(d03, LoadAs32i(src + os + 3 * F), w3);
                        }
                        else
                        {
                            Madd1(d00, srcZero, w0);
                            Madd1(d01, srcZero, w1);
                            Madd1(d02, srcZero, w2);
                            Madd1(d03, srcZero, w3);
                        }
                    }
                }
                Save1<term>(dst, d00, bias, norm, dstZero, i + F * 0);
                Save1<term>(dst, d01, bias, norm, dstZero, i + F * 1);
                Save1<term>(dst, d02, bias, norm, dstZero, i + F * 2);
                Save1<term>(dst, d03, bias, norm, dstZero, i + F * 3);
            }
            for (; i < sizeDF; i += DF)
            {
                d00 = _mm512_setzero_si512();
                d01 = _mm512_setzero_si512();
                for (size_t ky = 0; ky < 3; ++ky)
                {
                    size_t sy = dy * p.strideY + ky - p.padY;
                    for (size_t kx = 0; kx < 3; ++kx)
                    {
                        size_t sx = dx * p.strideX + kx - p.padX;
                        size_t ow = (ky * p.kernelX + kx) * size + i;
                        w0 = LoadAs32i(weight + ow + 0 * F);
                        w1 = LoadAs32i(weight + ow + 1 * F);
                        if (sy < p.srcH && sx < p.srcW)
                        {
                            size_t os = (sy * p.srcW + sx) * size + i;
                            Madd1(d00, LoadAs32i(src + os + 0 * F), w0);
                            Madd1(d01, LoadAs32i(src + os + 1 * F), w1);
                        }
                        else
                        {
                            Madd1(d00, srcZero, w0);
                            Madd1(d01, srcZero, w1);
                        }
                    }
                }
                Save1<term>(dst, d00, bias, norm, dstZero, i + F * 0);
                Save1<term>(dst, d01, bias, norm, dstZero, i + F * 1);
            }
            for (; i < sizeF; i += F)
            {
                d00 = _mm512_setzero_si512();
                for (size_t ky = 0; ky < 3; ++ky)
                {
                    size_t sy = dy * p.strideY + ky - p.padY;
                    for (size_t kx = 0; kx < 3; ++kx)
                    {
                        size_t sx = dx * p.strideX + kx - p.padX;
                        size_t ow = (ky * p.kernelX + kx) * size + i;
                        w0 = LoadAs32i(weight + ow + 0 * F);
                        if (sy < p.srcH && sx < p.srcW)
                        {
                            size_t os = (sy * p.srcW + sx) * size + i;
                            Madd1(d00, LoadAs32i(src + os + 0 * F), w0);
                        }
                        else
                        {
                            Madd1(d00, srcZero, w0);
                        }
                    }
                }
                Save1<term>(dst, d00, bias, norm, dstZero, i + F * 0);
            }
            for (; i < size; i += F)
            {
                d00 = _mm512_setzero_si512();
                for (size_t ky = 0; ky < 3; ++ky)
                {
                    size_t sy = dy * p.strideY + ky - p.padY;
                    for (size_t kx = 0; kx < 3; ++kx)
                    {
                        size_t sx = dx * p.strideX + kx - p.padX;
                        size_t ow = (ky * p.kernelX + kx) * size + i;
                        w0 = LoadAs32i(weight + ow + 0 * F, tail);
                        if (sy < p.srcH && sx < p.srcW)
                        {
                            size_t os = (sy * p.srcW + sx) * size + i;
                            Madd1(d00, LoadAs32i(src + os + 0 * F, tail), w0);
                        }
                        else
                        {
                            Madd1(d00, srcZero, w0);
                        }
                    }
                }
                Save1<term>(dst, d00, bias, norm, dstZero, i + F * 0, tail);
            }
        }

        template<Term8iType term> SIMD_INLINE void QuantizedConvolutionNhwcDepthwiseV0_3x3Main1(
            const uint8_t* src, const ConvParam& p, const AlgParamV0& a,
            const int8_t* weight, const int32_t* bias, const float* norm, const __m512i& dstZero, uint8_t* dst)
        {
            __m512i d00, d01, d02, d03;
            size_t srcC = p.srcC;
            size_t srcCF = AlignLo(srcC, F), srcCDF = AlignLo(srcC, DF), srcCA = AlignLo(srcC, A);
            __mmask16 tail = TailMask16(srcC - srcCF);
            size_t srcS = srcC * p.srcW;
            size_t c = 0;
            for (; c < srcCA; c += A)
            {
                d00 = _mm512_setzero_si512();
                d01 = _mm512_setzero_si512();
                d02 = _mm512_setzero_si512();
                d03 = _mm512_setzero_si512();
                for (size_t ky = 0; ky < 3; ++ky)
                {
                    const uint8_t* ps = src + ky * srcS + c;
                    const int8_t* pw = weight + ky * 3 * srcC + c;
                    for (size_t kx = 0; kx < 3; ++kx, ps += srcC, pw += srcC)
                    {
                        Madd1(d00, LoadAs32i(ps + 0 * F), LoadAs32i(pw + 0 * F));
                        Madd1(d01, LoadAs32i(ps + 1 * F), LoadAs32i(pw + 1 * F));
                        Madd1(d02, LoadAs32i(ps + 2 * F), LoadAs32i(pw + 2 * F));
                        Madd1(d03, LoadAs32i(ps + 3 * F), LoadAs32i(pw + 3 * F));
                    }
                }
                Save1<term>(dst, d00, bias, norm, dstZero, c + F * 0);
                Save1<term>(dst, d01, bias, norm, dstZero, c + F * 1);
                Save1<term>(dst, d02, bias, norm, dstZero, c + F * 2);
                Save1<term>(dst, d03, bias, norm, dstZero, c + F * 3);
            }
            for (; c < srcCDF; c += DF)
            {
                d00 = _mm512_setzero_si512();
                d01 = _mm512_setzero_si512();
                for (size_t ky = 0; ky < 3; ++ky)
                {
                    const uint8_t* ps = src + ky * srcS + c;
                    const int8_t* pw = weight + ky * 3 * srcC + c;
                    for (size_t kx = 0; kx < 3; ++kx, ps += srcC, pw += srcC)
                    {
                        Madd1(d00, LoadAs32i(ps + 0 * F), LoadAs32i(pw + 0 * F));
                        Madd1(d01, LoadAs32i(ps + 1 * F), LoadAs32i(pw + 1 * F));
                    }
                }
                Save1<term>(dst, d00, bias, norm, dstZero, c + F * 0);
                Save1<term>(dst, d01, bias, norm, dstZero, c + F * 1);
            }
            for (; c < srcC; c += F)
            {
                d00 = _mm512_setzero_si512();
                for (size_t ky = 0; ky < 3; ++ky)
                {
                    const uint8_t* ps = src + ky * srcS + c;
                    const int8_t* pw = weight + ky * 3 * srcC + c;
                    for (size_t kx = 0; kx < 3; ++kx, ps += srcC, pw += srcC)
                    {
                        Madd1(d00, LoadAs32i(ps + 0 * F, tail), LoadAs32i(pw + 0 * F, tail));
                    }
                }
                Save1<term>(dst, d00, bias, norm, dstZero, c, tail);
            }
        }

        template<Term8iType term> SIMD_INLINE void QuantizedConvolutionNhwcDepthwiseV0_3x3Main2(
            const uint8_t* src, const ConvParam& p, const AlgParamV0& a,
            const int8_t* weight, const int32_t* bias, const float* norm, const __m512i& dstZero, uint8_t* dst)
        {
            __m512i d00, d01, d02, d03, d10, d11, d12, d13, w0;
            size_t srcC = p.srcC;
            size_t srcCF = AlignLo(srcC, F), srcCDF = AlignLo(srcC, DF), srcCA = AlignLo(srcC, A);
            __mmask16 tail = TailMask16(srcC - srcCF);
            size_t srcS = srcC * p.srcW;
            size_t srcX = srcC * p.strideX;
            size_t c = 0;
            for (; c < srcCA; c += A)
            {
                d00 = _mm512_setzero_si512();
                d01 = _mm512_setzero_si512();
                d02 = _mm512_setzero_si512();
                d03 = _mm512_setzero_si512();
                d10 = _mm512_setzero_si512();
                d11 = _mm512_setzero_si512();
                d12 = _mm512_setzero_si512();
                d13 = _mm512_setzero_si512();
                for (size_t ky = 0; ky < 3; ++ky)
                {
                    const uint8_t* ps = src + ky * srcS + c;
                    const int8_t* pw = weight + ky * 3 * srcC + c;
                    for (size_t kx = 0; kx < 3; ++kx, ps += srcC, pw += srcC)
                    {
                        w0 = LoadAs32i(pw + 0 * F);
                        Madd1(d00, LoadAs32i(ps + 0 * F + 0 * srcX), w0);
                        Madd1(d10, LoadAs32i(ps + 0 * F + 1 * srcX), w0);
                        w0 = LoadAs32i(pw + 1 * F);
                        Madd1(d01, LoadAs32i(ps + 1 * F + 0 * srcX), w0);
                        Madd1(d11, LoadAs32i(ps + 1 * F + 1 * srcX), w0);
                        w0 = LoadAs32i(pw + 2 * F);
                        Madd1(d02, LoadAs32i(ps + 2 * F + 0 * srcX), w0);
                        Madd1(d12, LoadAs32i(ps + 2 * F + 1 * srcX), w0);
                        w0 = LoadAs32i(pw + 3 * F);
                        Madd1(d03, LoadAs32i(ps + 3 * F + 0 * srcX), w0);
                        Madd1(d13, LoadAs32i(ps + 3 * F + 1 * srcX), w0);
                    }
                }
                Save1<term>(dst, d00, bias, norm, dstZero, c + F * 0);
                Save1<term>(dst, d01, bias, norm, dstZero, c + F * 1);
                Save1<term>(dst, d02, bias, norm, dstZero, c + F * 2);
                Save1<term>(dst, d03, bias, norm, dstZero, c + F * 3);
                Save1<term>(dst + srcC, d10, bias, norm, dstZero, c + F * 0);
                Save1<term>(dst + srcC, d11, bias, norm, dstZero, c + F * 1);
                Save1<term>(dst + srcC, d12, bias, norm, dstZero, c + F * 2);
                Save1<term>(dst + srcC, d13, bias, norm, dstZero, c + F * 3);
            }
            for (; c < srcCDF; c += DF)
            {
                d00 = _mm512_setzero_si512();
                d01 = _mm512_setzero_si512();
                d10 = _mm512_setzero_si512();
                d11 = _mm512_setzero_si512();
                for (size_t ky = 0; ky < 3; ++ky)
                {
                    const uint8_t* ps = src + ky * srcS + c;
                    const int8_t* pw = weight + ky * 3 * srcC + c;
                    for (size_t kx = 0; kx < 3; ++kx, ps += srcC, pw += srcC)
                    {
                        w0 = LoadAs32i(pw + 0 * F);
                        Madd1(d00, LoadAs32i(ps + 0 * F + 0 * srcX), w0);
                        Madd1(d10, LoadAs32i(ps + 0 * F + 1 * srcX), w0);
                        w0 = LoadAs32i(pw + 1 * F);
                        Madd1(d01, LoadAs32i(ps + 1 * F + 0 * srcX), w0);
                        Madd1(d11, LoadAs32i(ps + 1 * F + 1 * srcX), w0);
                    }
                }
                Save1<term>(dst, d00, bias, norm, dstZero, c + F * 0);
                Save1<term>(dst, d01, bias, norm, dstZero, c + F * 1);
                Save1<term>(dst + srcC, d10, bias, norm, dstZero, c + F * 0);
                Save1<term>(dst + srcC, d11, bias, norm, dstZero, c + F * 1);
            }
            for (; c < srcCF; c += F)
            {
                d00 = _mm512_setzero_si512();
                d10 = _mm512_setzero_si512();
                for (size_t ky = 0; ky < 3; ++ky)
                {
                    const uint8_t* ps = src + ky * srcS + c;
                    const int8_t* pw = weight + ky * 3 * srcC + c;
                    for (size_t kx = 0; kx < 3; ++kx, ps += srcC, pw += srcC)
                    {
                        w0 = LoadAs32i(pw + 0 * F);
                        Madd1(d00, LoadAs32i(ps + 0 * F + 0 * srcX), w0);
                        Madd1(d10, LoadAs32i(ps + 0 * F + 1 * srcX), w0);
                    }
                }
                Save1<term>(dst, d00, bias, norm, dstZero, c);
                Save1<term>(dst + srcC, d10, bias, norm, dstZero, c);
            }
            for (; c < srcC; c += F)
            {
                d00 = _mm512_setzero_si512();
                d10 = _mm512_setzero_si512();
                for (size_t ky = 0; ky < 3; ++ky)
                {
                    const uint8_t* ps = src + ky * srcS + c;
                    const int8_t* pw = weight + ky * 3 * srcC + c;
                    for (size_t kx = 0; kx < 3; ++kx, ps += srcC, pw += srcC)
                    {
                        w0 = LoadAs32i(pw + 0 * F, tail);
                        Madd1(d00, LoadAs32i(ps + 0 * F + 0 * srcX, tail), w0);
                        Madd1(d10, LoadAs32i(ps + 0 * F + 1 * srcX, tail), w0);
                    }
                }
                Save1<term>(dst, d00, bias, norm, dstZero, c, tail);
                Save1<term>(dst + srcC, d10, bias, norm, dstZero, c, tail);
            }
        }

        template<Term8iType term> void QuantizedConvolutionNhwcDepthwiseV0_3x3(const uint8_t* src, uint32_t srcZero,
            const ConvParam& p, const AlgParamV0& a, const int8_t* weight, const int32_t* bias, const float* norm, uint32_t dstZero, uint8_t* dst)
        {
            __m512i _srcZero = _mm512_set1_epi32(srcZero);
            __m512i _dstZero = _mm512_set1_epi32(dstZero);
            size_t srcS = p.srcC * p.srcW;
            size_t srcX = p.srcC * p.strideX;
            size_t dstH = p.dstH - p.padH;
            size_t dstW = p.dstW - p.padW;
            size_t dstC = p.dstC * a.dstE;
            size_t dstW2 = AlignLo(dstW - p.padX, 2) + p.padX;
            size_t dy = 0;
            for (; dy < p.padY; ++dy)
                for (size_t dx = 0; dx < p.dstW; ++dx)
                    QuantizedConvolutionNhwcDepthwiseV0_3x3Edge<term>(src, _srcZero, p, a, dy, dx, weight, bias, norm, _dstZero, dst), dst += dstC;
            for (; dy < dstH; ++dy)
            {
                size_t dx = 0;
                for (; dx < p.padX; ++dx)
                    QuantizedConvolutionNhwcDepthwiseV0_3x3Edge<term>(src, _srcZero, p, a, dy, dx, weight, bias, norm, _dstZero, dst), dst += dstC;
                size_t offset = ((dy * p.strideY - p.padY) * p.srcW + dx * p.strideX - p.padX) * p.srcC;
                for (; dx < dstW2; dx += 2)
                    QuantizedConvolutionNhwcDepthwiseV0_3x3Main2<term>(src + offset, p, a, weight, bias, norm, _dstZero, dst), dst += dstC * 2, offset += srcX * 2;
                for (; dx < dstW; dx += 1)
                    QuantizedConvolutionNhwcDepthwiseV0_3x3Main1<term>(src + offset, p, a, weight, bias, norm, _dstZero, dst), dst += dstC, offset += srcX;
                for (; dx < p.dstW; ++dx)
                    QuantizedConvolutionNhwcDepthwiseV0_3x3Edge<term>(src, _srcZero, p, a, dy, dx, weight, bias, norm, _dstZero, dst), dst += dstC;
            }
            for (; dy < p.dstH; ++dy)
                for (size_t dx = 0; dx < p.dstW; ++dx)
                    QuantizedConvolutionNhwcDepthwiseV0_3x3Edge<term>(src, _srcZero, p, a, dy, dx, weight, bias, norm, _dstZero, dst), dst += dstC;
        }

        //------------------------------------------------------------------------------------------------

        template <Term8iType term> void SetV0(const ConvParam& p, SynetQuantizedConvolutionNhwcDepthwiseV0::ConvolutionPtr& convolution)
        {
            if (p.IsKernel(3) && p.IsDilation(1))
                convolution = QuantizedConvolutionNhwcDepthwiseV0_3x3<term>;
            else
                convolution = QuantizedConvolutionNhwcDepthwiseV0_Default<term>;
        }

        //------------------------------------------------------------------------------------------------

        SynetQuantizedConvolutionNhwcDepthwiseV0::SynetQuantizedConvolutionNhwcDepthwiseV0(const ConvParam& p)
            : Avx2::SynetQuantizedConvolutionNhwcDepthwiseV0(p)
        {
            if (p.dstT == SimdTensorData8u)
                SetV0<Term8iLast8u>(p, _convolution);
            //else
            //    SetV0<Term8iLast32f>(p, _convolution);
        }

        //------------------------------------------------------------------------------------------------

        static void QuantizedConvolutionNhwcDepthwiseV1_Preprocess(const uint8_t* src, uint8_t zero, const ConvParam& p, const AlgParamV1& a, size_t dyBeg, size_t dyEnd, int32_t* dst)
        {
            __m512i _zero = _mm512_set1_epi32(zero);
            size_t srcC = p.srcC, srcCF = Simd::AlignLo(p.srcC, a.F), byMask = a.bufH - 1;
            __mmask16 tail = TailMask16(srcC - srcCF);
            size_t byPad = p.kernelY - 1, srcR = p.srcW * p.srcC, bufR = a.bufW * a.bufC;
            size_t byBeg = dyBeg ? dyBeg * p.strideY + byPad : 0, byEnd = dyEnd * p.strideY + byPad;
            if (a.reorderType == 0)
            {
                size_t bxPad = p.padX * a.bufC, bwPad = p.padW * a.bufC;
                for (size_t by = byBeg; by < byEnd; ++by)
                {
                    int32_t* pd = dst + (by & byMask) * bufR;
                    size_t sy = by - p.padY;
                    if (sy < p.srcH)
                    {
                        const uint8_t* ps = src + sy * srcR;
                        if (bxPad)
                        {
                            for (size_t i = 0; i < bxPad; i += F)
                                _mm512_storeu_si512((__m512i*)(pd + i), _zero);
                            pd += bxPad;
                        }
                        for (size_t sx = 0; sx < p.srcW; sx++)
                        {
                            size_t sc = 0;
                            for (; sc < srcCF; sc += F)
                                _mm512_storeu_si512((__m512i*)(pd + sc), _mm512_cvtepu8_epi32(_mm_loadu_si128((__m128i*)(ps + sc))));
                            if(tail)
                                _mm512_storeu_si512((__m512i*)(pd + sc), _mm512_cvtepu8_epi32(_mm_maskz_loadu_epi8(tail, ps + sc)));
                            ps += p.srcC;
                            pd += a.bufC;
                        }
                        if (bwPad)
                        {
                            for (size_t i = 0; i < bwPad; i += F)
                                _mm512_storeu_si512((__m512i*)(pd + i), _zero);
                            pd += bwPad;
                        }
                    }
                    else
                    {
                        for (size_t i = 0; i < bufR; i += F)
                            _mm512_storeu_si512((__m512i*)(pd + i), _zero);
                    }
                }
            }
            else
            {
                size_t bW = a.bufW, bC = a.bufC, xPad = p.padX, wPad = p.padW;
                for (size_t by = byBeg; by < byEnd; ++by)
                {
                    int32_t* pd = dst + (by & byMask) * bufR;
                    size_t sy = by - p.padY;
                    if (sy < p.srcH)
                    {
                        const uint8_t* ps = src + sy * srcR;
                        if (xPad)
                        {
                            for (size_t x = 0; x < xPad; x += 1, pd += a.F)
                                for (size_t c = 0; c < bC; c += a.F)
                                    _mm512_storeu_si512((__m512i*)(pd + c * bW), _zero);
                        }
                        for (size_t sx = 0; sx < p.srcW; sx++, pd += a.F)
                        {
                            size_t sc = 0;
                            for (; sc < srcCF; sc += F)
                                _mm512_storeu_si512((__m512i*)(pd + sc * bW), _mm512_cvtepu8_epi32(_mm_loadu_si128((__m128i*)(ps + sc))));
                            for (; sc < srcC; sc += F)
                                _mm512_storeu_si512((__m512i*)(pd + sc * bW), _mm512_cvtepu8_epi32(_mm_maskz_loadu_epi8(tail, ps + sc)));
                            ps += p.srcC;
                        }
                        if (wPad)
                        {
                            for (size_t x = 0; x < wPad; x += 1, pd += a.F)
                                for (size_t c = 0; c < bC; c += a.F)
                                    _mm512_storeu_si512((__m512i*)(pd + c * bW), _zero);
                        }
                    }
                    else
                    {

                        for (size_t i = 0; i < bufR; i += F)
                            _mm512_storeu_si512((__m512i*)(pd + i), _zero);
                    }
                }
            }
        }

        //------------------------------------------------------------------------------------------------

        template <Term8iType term> void QuantizedConvolutionNhwcDepthwiseV1_AnyR0(const int32_t* src, const ConvParam& p, const AlgParamV1& a,
            const int32_t* weight, const int32_t* bias, const float* norm, size_t dyBeg, size_t dyEnd, uint32_t zero, uint8_t* dst)
        {
            __m512i _zero = _mm512_set1_epi32(zero);
            __m512i d00, d01, d02, d03, d10, d11, d12, d13, w0;
            size_t srcC = p.srcC, srcCF = AlignLo(srcC, F), srcCF4 = AlignLo(srcC, F * 4), kY = p.kernelY, kX = p.kernelX, sY = p.strideY, sX = p.strideX;
            size_t byMask = a.bufH - 1, bufC = a.bufC, bufR = a.bufW * a.bufC, dstW2 = AlignLo(p.dstW, 2), dD = p.dstC * a.srcE, dX = sX * bufC;
            __mmask16 tail = TailMask16(srcC - srcCF);
            dst += dyBeg * p.dstW * p.dstC * a.srcE;
            for (size_t dy = dyBeg; dy < dyEnd; ++dy)
            {
                size_t dx = 0;
                for (; dx < dstW2; dx += 2)
                {
                    const int32_t* ps00 = src + (dx + 0) * sX * bufC;
                    uint8_t* dst0 = dst, * dst1 = dst + dD;
                    size_t sc = 0;
                    for (; sc < srcCF4; sc += F * 4)
                    {
                        d00 = _mm512_setzero_si512();
                        d01 = _mm512_setzero_si512();
                        d02 = _mm512_setzero_si512();
                        d03 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();
                        d11 = _mm512_setzero_si512();
                        d12 = _mm512_setzero_si512();
                        d13 = _mm512_setzero_si512();
                        const int32_t* pw = weight + sc;
                        for (size_t ky = 0; ky < kY; ++ky)
                        {
                            size_t sy = dy * sY + ky;
                            const int32_t* ps0 = ps00 + (sy & byMask) * bufR + sc, * ps1 = ps0 + dX;
                            for (size_t kx = 0; kx < kX; ++kx, ps0 += bufC, ps1 += bufC, pw += bufC)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw + 0);
                                Madd1(d00, _mm512_loadu_si512((__m512i*)ps0 + 0), w0);
                                Madd1(d10, _mm512_loadu_si512((__m512i*)ps1 + 0), w0);
                                w0 = _mm512_loadu_si512((__m512i*)pw + 1);
                                Madd1(d01, _mm512_loadu_si512((__m512i*)ps0 + 1), w0);
                                Madd1(d11, _mm512_loadu_si512((__m512i*)ps1 + 1), w0);
                                w0 = _mm512_loadu_si512((__m512i*)pw + 2);
                                Madd1(d02, _mm512_loadu_si512((__m512i*)ps0 + 2), w0);
                                Madd1(d12, _mm512_loadu_si512((__m512i*)ps1 + 2), w0);
                                w0 = _mm512_loadu_si512((__m512i*)pw + 3);
                                Madd1(d03, _mm512_loadu_si512((__m512i*)ps0 + 3), w0);
                                Madd1(d13, _mm512_loadu_si512((__m512i*)ps1 + 3), w0);
                            }
                        }
                        Save2<term>(dst, dst + dD, d00, d10, bias, norm, _zero, sc + F * 0);
                        Save2<term>(dst, dst + dD, d01, d11, bias, norm, _zero, sc + F * 1);
                        Save2<term>(dst, dst + dD, d02, d12, bias, norm, _zero, sc + F * 2);
                        Save2<term>(dst, dst + dD, d03, d13, bias, norm, _zero, sc + F * 3);
                    }
                    for (; sc < srcCF; sc += F)
                    {
                        d00 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();
                        const int32_t* pw = weight + sc;
                        for (size_t ky = 0; ky < kY; ++ky)
                        {
                            size_t sy = dy * sY + ky;
                            const int32_t* ps0 = ps00 + (sy & byMask) * bufR + sc, * ps1 = ps0 + dX;
                            for (size_t kx = 0; kx < kX; ++kx, ps0 += bufC, ps1 += bufC, pw += bufC)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw + 0);
                                Madd1(d00, _mm512_loadu_si512((__m512i*)ps0 + 0), w0);
                                Madd1(d10, _mm512_loadu_si512((__m512i*)ps1 + 0), w0);
                            }
                        }
                        Save2<term>(dst, dst + dD, d00, d10, bias, norm, _zero, sc + F * 0);
                    }
                    for (; sc < srcC; sc += F)
                    {
                        d00 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();
                        const int32_t* pw = weight + sc;
                        for (size_t ky = 0; ky < kY; ++ky)
                        {
                            size_t sy = dy * sY + ky;
                            const int32_t* ps0 = ps00 + (sy & byMask) * bufR + sc, * ps1 = ps0 + dX;
                            for (size_t kx = 0; kx < kX; ++kx, ps0 += bufC, ps1 += bufC, pw += bufC)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw + 0);
                                Madd1(d00, _mm512_loadu_si512((__m512i*)ps0 + 0), w0);
                                Madd1(d10, _mm512_loadu_si512((__m512i*)ps1 + 0), w0);
                            }
                        }
                        Save2<term>(dst, dst + dD, d00, d10, bias, norm, _zero, sc + F * 0, tail);
                    }
                    dst += 2 * dD;
                }
                for (; dx < p.dstW; ++dx)
                {
                    const int32_t* ps0 = src + dx * sX * bufC;
                    size_t sc = 0;
                    for (; sc < srcCF4; sc += F * 4)
                    {
                        d00 = _mm512_setzero_si512();
                        d01 = _mm512_setzero_si512();
                        d02 = _mm512_setzero_si512();
                        d03 = _mm512_setzero_si512();
                        const int32_t* pw = weight + sc;
                        for (size_t ky = 0; ky < kY; ++ky)
                        {
                            size_t sy = dy * sY + ky;
                            const int32_t* ps = ps0 + (sy & byMask) * bufR + sc;
                            for (size_t kx = 0; kx < kX; ++kx, ps += bufC, pw += bufC)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw + 0);
                                Madd1(d00, _mm512_loadu_si512((__m512i*)ps + 0), w0);
                                w0 = _mm512_loadu_si512((__m512i*)pw + 1);
                                Madd1(d01, _mm512_loadu_si512((__m512i*)ps + 1), w0);
                                w0 = _mm512_loadu_si512((__m512i*)pw + 2);
                                Madd1(d02, _mm512_loadu_si512((__m512i*)ps + 2), w0);
                                w0 = _mm512_loadu_si512((__m512i*)pw + 3);
                                Madd1(d03, _mm512_loadu_si512((__m512i*)ps + 3), w0);
                            }
                        }
                        Save1<term>(dst, d00, bias, norm, _zero, sc + F * 0);
                        Save1<term>(dst, d01, bias, norm, _zero, sc + F * 1);
                        Save1<term>(dst, d02, bias, norm, _zero, sc + F * 2);
                        Save1<term>(dst, d03, bias, norm, _zero, sc + F * 3);
                    }
                    for (; sc < srcCF; sc += F)
                    {
                        d00 = _mm512_setzero_si512();
                        const int32_t* pw = weight + sc;
                        for (size_t ky = 0; ky < kY; ++ky)
                        {
                            size_t sy = dy * sY + ky;
                            const int32_t* ps = ps0 + (sy & byMask) * bufR + sc;
                            for (size_t kx = 0; kx < kX; ++kx, ps += bufC, pw += bufC)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw);
                                Madd1(d00, _mm512_loadu_si512((__m512i*)ps), w0);
                            }
                        }
                        Save1<term>(dst, d00, bias, norm, _zero, sc);
                    }
                    for (; sc < srcC; sc += F)
                    {
                        d00 = _mm512_setzero_si512();
                        const int32_t* pw = weight + sc;
                        for (size_t ky = 0; ky < kY; ++ky)
                        {
                            size_t sy = dy * sY + ky;
                            const int32_t* ps = ps0 + (sy & byMask) * bufR + sc;
                            for (size_t kx = 0; kx < kX; ++kx, ps += bufC, pw += bufC)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw);
                                Madd1(d00, _mm512_loadu_si512((__m512i*)ps), w0);
                            }
                        }
                        Save1<term>(dst, d00, bias, norm, _zero, sc, tail);
                    }
                    dst += dD;
                }
            }
        }

        //------------------------------------------------------------------------------------------------

        template <Term8iType term> void QuantizedConvolutionNhwcDepthwiseV1_AnyR1(const int32_t* src, const ConvParam& p, const AlgParamV1& a,
            const int32_t* weight, const int32_t* bias, const float* norm, size_t dyBeg, size_t dyEnd, uint32_t zero, uint8_t* dst)
        {
            __m512 _norm;
            __m512i _zero = _mm512_set1_epi32(zero), _bias;
            __m512i d00, d10, d20, d30, w0;
            size_t srcC = p.srcC, srcCF = AlignLo(srcC, F), kY = p.kernelY, kX = p.kernelX, sY = p.strideY, sX = p.strideX, dX = sX * F, dW = kY * kX;
            size_t byMask = a.bufH - 1, bW = a.bufW, bufR = a.bufW * a.bufC, dstW2 = AlignLo(p.dstW, 2), dstW4 = AlignLo(p.dstW, 4), dD = p.dstC * a.srcE;
            dst += dyBeg * p.dstW * dD;
            for (size_t dy = dyBeg; dy < dyEnd; ++dy)
            {
                size_t sy = dy * sY;
                for (size_t sc = 0; sc < srcC; sc += F)
                {
                    uint8_t* pd = dst + sc;
                    const int32_t* ps0 = src + sc * bW;
                    _bias = _mm512_loadu_si512((__m512i*)(bias + sc));
                    _norm = _mm512_loadu_ps(norm + sc);
                    __mmask16 tail = TailMask16(srcC - sc);
                    size_t dx = 0;
                    for (; dx < dstW4; dx += 4, ps0 += 4 * dX)
                    {
                        d00 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();
                        d20 = _mm512_setzero_si512();
                        d30 = _mm512_setzero_si512();
                        const int32_t* pw = weight + sc * dW;
                        for (size_t ky = 0; ky < kY; ++ky)
                        {
                            const int32_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += F, pw += F)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw);
                                Madd1(d00, _mm512_loadu_si512((__m512i*)(ps + 0 * dX)), w0);
                                Madd1(d10, _mm512_loadu_si512((__m512i*)(ps + 1 * dX)), w0);
                                Madd1(d20, _mm512_loadu_si512((__m512i*)(ps + 2 * dX)), w0);
                                Madd1(d30, _mm512_loadu_si512((__m512i*)(ps + 3 * dX)), w0);
                            }
                        }
                        Save1<term>(pd + 0 * dD, d00, _bias, _norm, _zero, tail);
                        Save1<term>(pd + 1 * dD, d10, _bias, _norm, _zero, tail);
                        Save1<term>(pd + 2 * dD, d20, _bias, _norm, _zero, tail);
                        Save1<term>(pd + 3 * dD, d30, _bias, _norm, _zero, tail);
                        pd += 4 * dD;
                    }
                    for (; dx < dstW2; dx += 2, ps0 += 2 * dX)
                    {
                        d00 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();
                        const int32_t* pw = weight + sc * dW;
                        for (size_t ky = 0; ky < kY; ++ky)
                        {
                            const int32_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += F, pw += F)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw);
                                Madd1(d00, _mm512_loadu_si512((__m512i*)(ps + 0 * dX)), w0);
                                Madd1(d10, _mm512_loadu_si512((__m512i*)(ps + 1 * dX)), w0);
                            }
                        }
                        Save1<term>(pd + 0 * dD, d00, _bias, _norm, _zero, tail);
                        Save1<term>(pd + 1 * dD, d10, _bias, _norm, _zero, tail);
                        pd += 2 * dD;
                    }
                    for (; dx < p.dstW; ++dx, ps0 += dX)
                    {
                        d00 = _mm512_setzero_si512();
                        const int32_t* pw = weight + sc * dW;
                        for (size_t ky = 0; ky < kY; ++ky)
                        {
                            const int32_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += F, pw += F)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw);
                                Madd1(d00, _mm512_loadu_si512((__m512i*)ps), w0);
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

        template <Term8iType term> void QuantizedConvolutionNhwcDepthwiseV1_3x3R1(const int32_t* src, const ConvParam& p, const AlgParamV1& a,
            const int32_t* weight, const int32_t* bias, const float* norm, size_t dyBeg, size_t dyEnd, uint32_t zero, uint8_t* dst)
        {
            __m512 _norm;
            __m512i _zero = _mm512_set1_epi32(zero), _bias;
            __m512i d00, d10, w0, w1, w2, w3, w4, w5, w6, w7, w8, s0;
            size_t srcC = p.srcC, srcCF = AlignLo(srcC, F), sY = p.strideY, sX = p.strideX, dX = sX * F, dW = 9;
            size_t byMask = a.bufH - 1, bW = a.bufW, bufR = a.bufW * a.bufC, dD = p.dstC * a.srcE;
            size_t dstW2 = sX == 1 ? AlignLo(p.dstW, 2) : 0;

            dst += dyBeg * p.dstW * dD;
            for (size_t dy = dyBeg; dy < dyEnd; ++dy)
            {
                size_t sc = 0, sy = dy * sY;
                for (; sc < srcC; sc += F)
                {
                    uint8_t* pd = dst + sc;
                    const int32_t* ps0 = src + ((sy + 0) & byMask) * bufR + sc * bW;
                    const int32_t* ps1 = src + ((sy + 1) & byMask) * bufR + sc * bW;
                    const int32_t* ps2 = src + ((sy + 2) & byMask) * bufR + sc * bW;
                    const int32_t* pw = weight + sc * dW;
                    _bias = _mm512_loadu_si512((__m512i*)(bias + sc));
                    _norm = _mm512_loadu_ps(norm + sc);
                    w0 = _mm512_loadu_si512((__m512i*)pw + 0);
                    w1 = _mm512_loadu_si512((__m512i*)pw + 1);
                    w2 = _mm512_loadu_si512((__m512i*)pw + 2);
                    w3 = _mm512_loadu_si512((__m512i*)pw + 3);
                    w4 = _mm512_loadu_si512((__m512i*)pw + 4);
                    w5 = _mm512_loadu_si512((__m512i*)pw + 5);
                    w6 = _mm512_loadu_si512((__m512i*)pw + 6);
                    w7 = _mm512_loadu_si512((__m512i*)pw + 7);
                    w8 = _mm512_loadu_si512((__m512i*)pw + 8);
                    __mmask16 tail = TailMask16(srcC - sc);                    
                    size_t dx = 0;
                    for (; dx < dstW2; dx += 2, ps0 += DF, ps1 += DF, ps2 += DF)
                    {
                        d00 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();

                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 0);
                        Madd1(d00, s0, w0);
                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 1);
                        Madd1(d00, s0, w1);
                        Madd1(d10, s0, w0);
                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 2);
                        Madd1(d00, s0, w2);
                        Madd1(d10, s0, w1);
                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 3);
                        Madd1(d10, s0, w2);

                        s0 = _mm512_loadu_si512((__m512i*)ps1 + 0);
                        Madd1(d00, s0, w3);
                        s0 = _mm512_loadu_si512((__m512i*)ps1 + 1);
                        Madd1(d00, s0, w4);
                        Madd1(d10, s0, w3);
                        s0 = _mm512_loadu_si512((__m512i*)ps1 + 2);
                        Madd1(d00, s0, w5);
                        Madd1(d10, s0, w4);
                        s0 = _mm512_loadu_si512((__m512i*)ps1 + 3);
                        Madd1(d10, s0, w5);

                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 0);
                        Madd1(d00, s0, w6);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 1);
                        Madd1(d00, s0, w7);
                        Madd1(d10, s0, w6);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 2);
                        Madd1(d00, s0, w8);
                        Madd1(d10, s0, w7);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 3);
                        Madd1(d10, s0, w8);

                        Save1<term>(pd + 0 * dD, d00, _bias, _norm, _zero, tail);
                        Save1<term>(pd + 1 * dD, d10, _bias, _norm, _zero, tail);
                        pd += 2 * dD;
                    }
                    for (; dx < p.dstW; ++dx, ps0 += dX, ps1 += dX, ps2 += dX)
                    {
                        d00 = _mm512_setzero_si512();

                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 0);
                        Madd1(d00, s0, w0);
                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 1);
                        Madd1(d00, s0, w1);
                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 2);
                        Madd1(d00, s0, w2);
                        s0 = _mm512_loadu_si512((__m512i*)ps1 + 0);
                        Madd1(d00, s0, w3);
                        s0 = _mm512_loadu_si512((__m512i*)ps1 + 1);
                        Madd1(d00, s0, w4);
                        s0 = _mm512_loadu_si512((__m512i*)ps1 + 2);
                        Madd1(d00, s0, w5);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 0);
                        Madd1(d00, s0, w6);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 1);
                        Madd1(d00, s0, w7);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 2);
                        Madd1(d00, s0, w8);

                        Save1<term>(pd, d00, _bias, _norm, _zero, tail);
                        pd += dD;
                    }
                }
                dst += p.dstW * dD;
            }
        }

        //------------------------------------------------------------------------------------------------

        template <Term8iType term> void SetV1(const ConvParam& p, const AlgParamV1& a, SynetQuantizedConvolutionNhwcDepthwiseV1::ConvolutionPtr& convolution)
        {
            if (p.IsKernel(3) && p.IsDilation(1) && a.reorderType == 1)
            {
                convolution = QuantizedConvolutionNhwcDepthwiseV1_3x3R1<term>;
            }
            else
            {
                if (a.reorderType == 0)
                    convolution = QuantizedConvolutionNhwcDepthwiseV1_AnyR0<term>;
                else if (a.reorderType == 1)
                    convolution = QuantizedConvolutionNhwcDepthwiseV1_AnyR1<term>;
                else
                    assert(0);
            }
        }

        //------------------------------------------------------------------------------------------------

        SynetQuantizedConvolutionNhwcDepthwiseV1::SynetQuantizedConvolutionNhwcDepthwiseV1(const ConvParam& p)
            : Avx2::SynetQuantizedConvolutionNhwcDepthwiseV1(p)
        {
            SetAlgParam(F);
            _preprocess = QuantizedConvolutionNhwcDepthwiseV1_Preprocess;
            if (p.dstT == SimdTensorData8u)
                SetV1<Term8iLast8u>(p, _alg, _convolution);
            //else
            //    SetV0<Term8iLast32f>(p, _alg, _convolution);
        }

        //------------------------------------------------------------------------------------------------

        static void QuantizedConvolutionNhwcDepthwiseV2_Preprocess(const uint8_t* src, const uint8_t* zero, const ConvParam& p, const AlgParamV2& a, size_t dyBeg, size_t dyEnd, int16_t* dst)
        {
            __m512i _zero = _mm512_set1_epi16(zero[0]);
            size_t srcC = p.srcC, srcCF = Simd::AlignLo(p.srcC, a.F), byMask = a.bufH - 1;
            __mmask16 tail = TailMask16(srcC - srcCF);
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
                            _mm512_storeu_si512((__m512i*)(pd + i), _zero);
                        pd += bxPad;
                    }
                    for (size_t sx = 0; sx < p.srcW; sx++)
                    {
                        size_t sc = 0;
                        for (; sc < srcCF; sc += F, pd += DF)
                        {
                            __m512i s0 = _mm512_cvtepu8_epi32(_mm_loadu_si128((__m128i*)(ps0 + sc)));
                            __m512i s1 = _mm512_cvtepu8_epi32(_mm_loadu_si128((__m128i*)(ps1 + sc)));
                            _mm512_storeu_si512((__m512i*)pd, _mm512_or_si512(s0, _mm512_slli_epi32(s1, 16)));
                        }
                        for (; sc < srcC; sc += F, pd += DF)
                        {
                            __m512i s0 = _mm512_cvtepu8_epi32(_mm_maskz_loadu_epi8(tail, ps0 + sc));
                            __m512i s1 = _mm512_cvtepu8_epi32(_mm_maskz_loadu_epi8(tail, ps1 + sc));
                            _mm512_storeu_si512((__m512i*)pd, _mm512_or_si512(s0, _mm512_slli_epi32(s1, 16)));
                        }
                        ps0 += p.srcC;
                        ps1 += p.srcC;
                    }
                    if (bwPad)
                    {
                        for (size_t i = 0; i < bwPad; i += DF)
                            _mm512_storeu_si512((__m512i*)(pd + i), _zero);
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
                                _mm512_storeu_si512((__m512i*)(pd + c * bW), _zero);
                    }
                    for (size_t sx = 0; sx < p.srcW; sx++, pd += DF)
                    {
                        size_t sc = 0;
                        for (; sc < srcCF; sc += F)
                        {
                            __m512i s0 = _mm512_cvtepu8_epi32(_mm_loadu_si128((__m128i*)(ps0 + sc)));
                            __m512i s1 = _mm512_cvtepu8_epi32(_mm_loadu_si128((__m128i*)(ps1 + sc)));
                            _mm512_storeu_si512((__m512i*)(pd + sc * bW), _mm512_or_si512(s0, _mm512_slli_epi32(s1, 16)));
                        }
                        for (; sc < srcC; sc += F)
                        {
                            __m512i s0 = _mm512_cvtepu8_epi32(_mm_maskz_loadu_epi8(tail, ps0 + sc));
                            __m512i s1 = _mm512_cvtepu8_epi32(_mm_maskz_loadu_epi8(tail, ps1 + sc));
                            _mm512_storeu_si512((__m512i*)(pd + sc * bW), _mm512_or_si512(s0, _mm512_slli_epi32(s1, 16)));
                        }
                        ps0 += p.srcC;
                        ps1 += p.srcC;
                    }
                    if (wPad)
                    {
                        for (size_t x = 0; x < wPad; x += 2, pd += DF)
                            for (size_t c = 0; c < bC; c += F)
                                _mm512_storeu_si512((__m512i*)(pd + c * bW), _zero);
                    }
                }
            }
        }

        //------------------------------------------------------------------------------------------------

        template <Term8iType term> void QuantizedConvolutionNhwcDepthwiseV2_AnyR0(const int16_t* src, const ConvParam& p, const AlgParamV2& a,
            const int16_t* weight, const int32_t* bias, const float* norm, size_t dyBeg, size_t dyEnd, uint32_t zero, uint8_t* dst)
        {
            __m512i _zero = _mm512_set1_epi32(zero);
            __m512i d00, d01, d02, d03, d10, d11, d12, d13, w0, s0;
            size_t srcC = p.srcC, srcCF = AlignLo(srcC, F), srcCF4 = AlignLo(srcC, F * 4), kY = p.kernelY, kX = p.kernelX, sY = p.strideY, sX = p.strideX;
            size_t byMask = a.bufH - 1, bufC = a.bufC * 2, bufR = a.bufR, dstW2 = AlignLo(p.dstW, 2), dD = p.dstC * a.srcE, dX = sX * bufC;
            size_t dyEnd2 = dyBeg + (sY == 1 ? AlignLo(dyEnd - dyBeg, 2) : 0), sizeW = a.sizeW, dyD = p.dstW * dD;
            __mmask16 tail = TailMask16(srcC - srcCF);
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
                        d00 = _mm512_setzero_si512();
                        d01 = _mm512_setzero_si512();
                        d02 = _mm512_setzero_si512();
                        d03 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();
                        d11 = _mm512_setzero_si512();
                        d12 = _mm512_setzero_si512();
                        d13 = _mm512_setzero_si512();
                        const int16_t* pw0 = weight + sc * 2, * pw1 = pw0 + sizeW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps = ps0 + (sy & byMask) * bufR + sc * 2;
                            for (size_t kx = 0; kx < kX; ++kx, ps += bufC, pw0 += bufC, pw1 += bufC)
                            {
                                s0 = _mm512_loadu_si512((__m512i*)ps + 0);
                                Madd2(d00, s0, _mm512_loadu_si512((__m512i*)pw0 + 0));
                                Madd2(d10, s0, _mm512_loadu_si512((__m512i*)pw1 + 0));
                                s0 = _mm512_loadu_si512((__m512i*)ps + 1);
                                Madd2(d01, s0, _mm512_loadu_si512((__m512i*)pw0 + 1));
                                Madd2(d11, s0, _mm512_loadu_si512((__m512i*)pw1 + 1));
                                s0 = _mm512_loadu_si512((__m512i*)ps + 2);
                                Madd2(d02, s0, _mm512_loadu_si512((__m512i*)pw0 + 2));
                                Madd2(d12, s0, _mm512_loadu_si512((__m512i*)pw1 + 2));
                                s0 = _mm512_loadu_si512((__m512i*)ps + 3);
                                Madd2(d03, s0, _mm512_loadu_si512((__m512i*)pw0 + 3));
                                Madd2(d13, s0, _mm512_loadu_si512((__m512i*)pw1 + 3));
                            }
                        }
                        Save2<term>(dst, dst + dyD, d00, d10, bias, norm, _zero, sc + F * 0);
                        Save2<term>(dst, dst + dyD, d01, d11, bias, norm, _zero, sc + F * 1);
                        Save2<term>(dst, dst + dyD, d02, d12, bias, norm, _zero, sc + F * 2);
                        Save2<term>(dst, dst + dyD, d03, d13, bias, norm, _zero, sc + F * 3);
                    }
                    for (; sc < srcCF; sc += F)
                    {
                        d00 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();
                        const int16_t* pw0 = weight + sc * 2, * pw1 = pw0 + sizeW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps = ps0 + (sy & byMask) * bufR + sc * 2;
                            for (size_t kx = 0; kx < kX; ++kx, ps += bufC, pw0 += bufC, pw1 += bufC)
                            {
                                s0 = _mm512_loadu_si512((__m512i*)ps + 0);
                                Madd2(d00, s0, _mm512_loadu_si512((__m512i*)pw0 + 0));
                                Madd2(d10, s0, _mm512_loadu_si512((__m512i*)pw1 + 0));
                            }
                        }
                        Save2<term>(dst, dst + dyD, d00, d10, bias, norm, _zero, sc + F * 0);
                    }
                    for (; sc < srcC; sc += F)
                    {
                        d00 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();
                        const int16_t* pw0 = weight + sc * 2, * pw1 = pw0 + sizeW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps = ps0 + (sy & byMask) * bufR + sc * 2;
                            for (size_t kx = 0; kx < kX; ++kx, ps += bufC, pw0 += bufC, pw1 += bufC)
                            {
                                s0 = _mm512_loadu_si512((__m512i*)ps + 0);
                                Madd2(d00, s0, _mm512_loadu_si512((__m512i*)pw0 + 0));
                                Madd2(d10, s0, _mm512_loadu_si512((__m512i*)pw1 + 0));
                            }
                        }
                        Save2<term>(dst, dst + dyD, d00, d10, bias, norm, _zero, sc + F * 0, tail);
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
                        d00 = _mm512_setzero_si512();
                        d01 = _mm512_setzero_si512();
                        d02 = _mm512_setzero_si512();
                        d03 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();
                        d11 = _mm512_setzero_si512();
                        d12 = _mm512_setzero_si512();
                        d13 = _mm512_setzero_si512();
                        const int16_t* pw = weight + sc * 2;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps0 = ps00 + (sy & byMask) * bufR + sc * 2, * ps1 = ps0 + dX;
                            for (size_t kx = 0; kx < kX; ++kx, ps0 += bufC, ps1 += bufC, pw += bufC)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw + 0);
                                Madd2(d00, _mm512_loadu_si512((__m512i*)ps0 + 0), w0);
                                Madd2(d10, _mm512_loadu_si512((__m512i*)ps1 + 0), w0);
                                w0 = _mm512_loadu_si512((__m512i*)pw + 1);
                                Madd2(d01, _mm512_loadu_si512((__m512i*)ps0 + 1), w0);
                                Madd2(d11, _mm512_loadu_si512((__m512i*)ps1 + 1), w0);
                                w0 = _mm512_loadu_si512((__m512i*)pw + 2);
                                Madd2(d02, _mm512_loadu_si512((__m512i*)ps0 + 2), w0);
                                Madd2(d12, _mm512_loadu_si512((__m512i*)ps1 + 2), w0);
                                w0 = _mm512_loadu_si512((__m512i*)pw + 3);
                                Madd2(d03, _mm512_loadu_si512((__m512i*)ps0 + 3), w0);
                                Madd2(d13, _mm512_loadu_si512((__m512i*)ps1 + 3), w0);
                            }
                        }
                        Save2<term>(dst, dst + dD, d00, d10, bias, norm, _zero, sc + F * 0);
                        Save2<term>(dst, dst + dD, d01, d11, bias, norm, _zero, sc + F * 1);
                        Save2<term>(dst, dst + dD, d02, d12, bias, norm, _zero, sc + F * 2);
                        Save2<term>(dst, dst + dD, d03, d13, bias, norm, _zero, sc + F * 3);
                    }
                    for (; sc < srcCF; sc += F)
                    {
                        d00 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();
                        const int16_t* pw = weight + sc * 2;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps0 = ps00 + (sy & byMask) * bufR + sc * 2, * ps1 = ps0 + dX;
                            for (size_t kx = 0; kx < kX; ++kx, ps0 += bufC, ps1 += bufC, pw += bufC)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw + 0);
                                Madd2(d00, _mm512_loadu_si512((__m512i*)ps0 + 0), w0);
                                Madd2(d10, _mm512_loadu_si512((__m512i*)ps1 + 0), w0);
                            }
                        }
                        Save2<term>(dst, dst + dD, d00, d10, bias, norm, _zero, sc + F * 0);
                    }
                    for (; sc < srcC; sc += F)
                    {
                        d00 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();
                        const int16_t* pw = weight + sc * 2;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps0 = ps00 + (sy & byMask) * bufR + sc * 2, * ps1 = ps0 + dX;
                            for (size_t kx = 0; kx < kX; ++kx, ps0 += bufC, ps1 += bufC, pw += bufC)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw + 0);
                                Madd2(d00, _mm512_loadu_si512((__m512i*)ps0 + 0), w0);
                                Madd2(d10, _mm512_loadu_si512((__m512i*)ps1 + 0), w0);
                            }
                        }
                        Save2<term>(dst, dst + dD, d00, d10, bias, norm, _zero, sc + F * 0, tail);
                    }
                    dst += 2 * dD;
                }
                for (; dx < p.dstW; ++dx)
                {
                    const int16_t* ps0 = src + dx * sX * bufC;
                    size_t sc = 0;
                    for (; sc < srcCF4; sc += F * 4)
                    {
                        d00 = _mm512_setzero_si512();
                        d01 = _mm512_setzero_si512();
                        d02 = _mm512_setzero_si512();
                        d03 = _mm512_setzero_si512();
                        const int16_t* pw = weight + sc * 2;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps = ps0 + (sy & byMask) * bufR + sc * 2;
                            for (size_t kx = 0; kx < kX; ++kx, ps += bufC, pw += bufC)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw + 0);
                                Madd2(d00, _mm512_loadu_si512((__m512i*)ps + 0), w0);
                                w0 = _mm512_loadu_si512((__m512i*)pw + 1);
                                Madd2(d01, _mm512_loadu_si512((__m512i*)ps + 1), w0);
                                w0 = _mm512_loadu_si512((__m512i*)pw + 2);
                                Madd2(d02, _mm512_loadu_si512((__m512i*)ps + 2), w0);
                                w0 = _mm512_loadu_si512((__m512i*)pw + 3);
                                Madd2(d03, _mm512_loadu_si512((__m512i*)ps + 3), w0);
                            }
                        }
                        Save1<term>(dst, d00, bias, norm, _zero, sc + F * 0);
                        Save1<term>(dst, d01, bias, norm, _zero, sc + F * 1);
                        Save1<term>(dst, d02, bias, norm, _zero, sc + F * 2);
                        Save1<term>(dst, d03, bias, norm, _zero, sc + F * 3);
                    }
                    for (; sc < srcCF; sc += F)
                    {
                        d00 = _mm512_setzero_si512();
                        const int16_t* pw = weight + sc * 2;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps = ps0 + (sy & byMask) * bufR + sc * 2;
                            for (size_t kx = 0; kx < kX; ++kx, ps += bufC, pw += bufC)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw);
                                Madd2(d00, _mm512_loadu_si512((__m512i*)ps), w0);
                            }
                        }
                        Save1<term>(dst, d00, bias, norm, _zero, sc);
                    }
                    for (; sc < srcC; sc += F)
                    {
                        d00 = _mm512_setzero_si512();
                        const int16_t* pw = weight + sc * 2;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            size_t sy = dy * sY + ky;
                            const int16_t* ps = ps0 + (sy & byMask) * bufR + sc * 2;
                            for (size_t kx = 0; kx < kX; ++kx, ps += bufC, pw += bufC)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw);
                                Madd2(d00, _mm512_loadu_si512((__m512i*)ps), w0);
                            }
                        }
                        Save1<term>(dst, d00, bias, norm, _zero, sc, tail);
                    }
                    dst += dD;
                }
            }
        }

        //------------------------------------------------------------------------------------------------

        template <Term8iType term> void QuantizedConvolutionNhwcDepthwiseV2_AnyR1(const int16_t* src, const ConvParam& p, const AlgParamV2& a,
            const int16_t* weight, const int32_t* bias, const float* norm, size_t dyBeg, size_t dyEnd, uint32_t zero, uint8_t* dst)
        {
            __m512 _norm;
            __m512i _zero = _mm512_set1_epi32(zero), _bias;
            __m512i d00, d10, d20, d30, d01, d11, d21, d31, w0, w1, s0;
            size_t srcC = p.srcC, srcCF = AlignLo(srcC, F), kY = p.kernelY, kX = p.kernelX, sY = p.strideY, sX = p.strideX, dX = sX * DF, dW = a.stepW;
            size_t byMask = a.bufH - 1, bW = a.bufW * 2, bufR = a.bufR, dstW2 = AlignLo(p.dstW, 2), dstW4 = AlignLo(p.dstW, 4), dD = p.dstC * a.srcE;
            size_t dyEnd2 = dyBeg + (sY == 1 ? AlignLo(dyEnd - dyBeg, 2) : 0), sizeW = a.sizeW, dyD = p.dstW * dD;
            dst += dyBeg * p.dstW * dD;
            size_t dy = dyBeg;
            for (; dy < dyEnd2; dy += 2)
            {
                size_t sy = dy * sY;
                for (size_t sc = 0; sc < srcC; sc += F)
                {
                    uint8_t* pd0 = dst + sc, * pd1 = pd0 + dyD;
                    const int16_t* ps0 = src + sc * bW;
                    _bias = _mm512_loadu_si512((__m512i*)(bias + sc));
                    _norm = _mm512_loadu_ps(norm + sc);
                    __mmask16 tail = TailMask16(srcC - sc);
                    size_t dx = 0;
                    for (; dx < dstW4; dx += 4, ps0 += 4 * dX)
                    {
                        d00 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();
                        d20 = _mm512_setzero_si512();
                        d30 = _mm512_setzero_si512();
                        d01 = _mm512_setzero_si512();
                        d11 = _mm512_setzero_si512();
                        d21 = _mm512_setzero_si512();
                        d31 = _mm512_setzero_si512();
                        const int16_t* pw0 = weight + sc * dW, * pw1 = pw0 + sizeW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            const int16_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += DF, pw0 += DF, pw1 += DF)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw0);
                                w1 = _mm512_loadu_si512((__m512i*)pw1);
                                s0 = _mm512_loadu_si512((__m512i*)(ps + 0 * dX));
                                Madd2(d00, s0, w0);
                                Madd2(d01, s0, w1);
                                s0 = _mm512_loadu_si512((__m512i*)(ps + 1 * dX));
                                Madd2(d10, s0, w0);
                                Madd2(d11, s0, w1);
                                s0 = _mm512_loadu_si512((__m512i*)(ps + 2 * dX));
                                Madd2(d20, s0, w0);
                                Madd2(d21, s0, w1);
                                s0 = _mm512_loadu_si512((__m512i*)(ps + 3 * dX));
                                Madd2(d30, s0, w0);
                                Madd2(d31, s0, w1);
                            }
                        }
                        Save1<term>(pd0 + 0 * dD, d00, _bias, _norm, _zero, tail);
                        Save1<term>(pd0 + 1 * dD, d10, _bias, _norm, _zero, tail);
                        Save1<term>(pd0 + 2 * dD, d20, _bias, _norm, _zero, tail);
                        Save1<term>(pd0 + 3 * dD, d30, _bias, _norm, _zero, tail);
                        Save1<term>(pd1 + 0 * dD, d01, _bias, _norm, _zero, tail);
                        Save1<term>(pd1 + 1 * dD, d11, _bias, _norm, _zero, tail);
                        Save1<term>(pd1 + 2 * dD, d21, _bias, _norm, _zero, tail);
                        Save1<term>(pd1 + 3 * dD, d31, _bias, _norm, _zero, tail);
                        pd0 += 4 * dD;
                        pd1 += 4 * dD;
                    }
                    for (; dx < dstW2; dx += 2, ps0 += 2 * dX)
                    {
                        d00 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();
                        d01 = _mm512_setzero_si512();
                        d11 = _mm512_setzero_si512();
                        const int16_t* pw0 = weight + sc * dW, * pw1 = pw0 + sizeW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            const int16_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += DF, pw0 += DF, pw1 += DF)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw0);
                                w1 = _mm512_loadu_si512((__m512i*)pw1);
                                s0 = _mm512_loadu_si512((__m512i*)(ps + 0 * dX));
                                Madd2(d00, s0, w0);
                                Madd2(d01, s0, w1);
                                s0 = _mm512_loadu_si512((__m512i*)(ps + 1 * dX));
                                Madd2(d10, s0, w0);
                                Madd2(d11, s0, w1);
                            }
                        }
                        Save1<term>(pd0 + 0 * dD, d00, _bias, _norm, _zero, tail);
                        Save1<term>(pd0 + 1 * dD, d10, _bias, _norm, _zero, tail);
                        Save1<term>(pd1 + 0 * dD, d01, _bias, _norm, _zero, tail);
                        Save1<term>(pd1 + 1 * dD, d11, _bias, _norm, _zero, tail);
                        pd0 += 2 * dD;
                        pd1 += 2 * dD;
                    }
                    for (; dx < p.dstW; ++dx, ps0 += dX)
                    {
                        d00 = _mm512_setzero_si512();
                        d01 = _mm512_setzero_si512();
                        const int16_t* pw0 = weight + sc * dW, * pw1 = pw0 + sizeW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            const int16_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += DF, pw0 += DF, pw1 += DF)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw0);
                                w1 = _mm512_loadu_si512((__m512i*)pw1);
                                s0 = _mm512_loadu_si512((__m512i*)(ps + 0 * dX));
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
                size_t sy = dy * sY;
                for (size_t sc = 0; sc < srcC; sc += F)
                {
                    uint8_t* pd = dst + sc;
                    const int16_t* ps0 = src + sc * bW;
                    _bias = _mm512_loadu_si512((__m512i*)(bias + sc));
                    _norm = _mm512_loadu_ps(norm + sc);
                    __mmask16 tail = TailMask16(srcC - sc);
                    size_t dx = 0;
                    for (; dx < dstW4; dx += 4, ps0 += 4 * dX)
                    {
                        d00 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();
                        d20 = _mm512_setzero_si512();
                        d30 = _mm512_setzero_si512();
                        const int16_t* pw = weight + sc * dW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            const int16_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += DF, pw += DF)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw);
                                Madd2(d00, _mm512_loadu_si512((__m512i*)(ps + 0 * dX)), w0);
                                Madd2(d10, _mm512_loadu_si512((__m512i*)(ps + 1 * dX)), w0);
                                Madd2(d20, _mm512_loadu_si512((__m512i*)(ps + 2 * dX)), w0);
                                Madd2(d30, _mm512_loadu_si512((__m512i*)(ps + 3 * dX)), w0);
                            }
                        }
                        Save1<term>(pd + 0 * dD, d00, _bias, _norm, _zero, tail);
                        Save1<term>(pd + 1 * dD, d10, _bias, _norm, _zero, tail);
                        Save1<term>(pd + 2 * dD, d20, _bias, _norm, _zero, tail);
                        Save1<term>(pd + 3 * dD, d30, _bias, _norm, _zero, tail);
                        pd += 4 * dD;
                    }
                    for (; dx < dstW2; dx += 2, ps0 += 2 * dX)
                    {
                        d00 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();
                        const int16_t* pw = weight + sc * dW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            const int16_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += DF, pw += DF)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw);
                                Madd2(d00, _mm512_loadu_si512((__m512i*)(ps + 0 * dX)), w0);
                                Madd2(d10, _mm512_loadu_si512((__m512i*)(ps + 1 * dX)), w0);
                            }
                        }
                        Save1<term>(pd + 0 * dD, d00, _bias, _norm, _zero, tail);
                        Save1<term>(pd + 1 * dD, d10, _bias, _norm, _zero, tail);
                        pd += 2 * dD;
                    }
                    for (; dx < p.dstW; ++dx, ps0 += dX)
                    {
                        d00 = _mm512_setzero_si512();
                        const int16_t* pw = weight + sc * dW;
                        for (size_t ky = 0; ky < kY; ky += 2)
                        {
                            const int16_t* ps = ps0 + ((sy + ky) & byMask) * bufR;
                            for (size_t kx = 0; kx < kX; ++kx, ps += DF, pw += DF)
                            {
                                w0 = _mm512_loadu_si512((__m512i*)pw);
                                Madd2(d00, _mm512_loadu_si512((__m512i*)ps), w0);
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

        template <Term8iType term> void QuantizedConvolutionNhwcDepthwiseV2_3x3R1(const int16_t* src, const ConvParam& p, const AlgParamV2& a,
            const int16_t* weight, const int32_t* bias, const float* norm, size_t dyBeg, size_t dyEnd, uint32_t zero, uint8_t* dst)
        {
            __m512 _norm;
            __m512i _zero = _mm512_set1_epi32(zero), _bias;
            __m512i d00, d10, w03, w14, w25, w6, w7, w8, s0;
            size_t srcC = p.srcC, srcCF = AlignLo(srcC, F), sY = p.strideY, sX = p.strideX, dX = sX * DF, dW = a.stepW;
            size_t byMask = a.bufH - 1, bW = a.bufW * 2, bufR = a.bufW * a.bufC, dstW2 = sX == 1 ? AlignLo(p.dstW, 2) : 0, dD = p.dstC * a.srcE;
            size_t dyEnd2 = dyBeg + (sY == 1 ? AlignLo(dyEnd - dyBeg, 2) : 0), sizeW = a.sizeW, dyD = p.dstW * dD;
            dst += dyBeg * p.dstW * dD;
            size_t dy = dyBeg;
            for (; dy < dyEnd2; dy += 2)
            {
                __m512i d01, d11, w0, w1, w2, w36, w47, w58;
                size_t sy = dy * sY;
                for (size_t sc = 0; sc < srcC; sc += F)
                {
                    uint8_t* pd0 = dst + sc, * pd1 = pd0 + dyD;
                    const int16_t* ps0 = src + ((sy + 0) & byMask) * bufR + sc * bW;
                    const int16_t* ps2 = src + ((sy + 2) & byMask) * bufR + sc * bW;
                    const int16_t* pw0 = weight + sc * dW, * pw1 = pw0 + sizeW;
                    _bias = _mm512_loadu_si512((__m512i*)(bias + sc));
                    _norm = _mm512_loadu_ps(norm + sc);
                    __mmask16 tail = TailMask16(srcC - sc);
                    w03 = _mm512_loadu_si512((__m512i*)pw0 + 0);
                    w14 = _mm512_loadu_si512((__m512i*)pw0 + 1);
                    w25 = _mm512_loadu_si512((__m512i*)pw0 + 2);
                    w6 = _mm512_loadu_si512((__m512i*)pw0 + 3);
                    w7 = _mm512_loadu_si512((__m512i*)pw0 + 4);
                    w8 = _mm512_loadu_si512((__m512i*)pw0 + 5);
                    w0 = _mm512_loadu_si512((__m512i*)pw1 + 0);
                    w1 = _mm512_loadu_si512((__m512i*)pw1 + 1);
                    w2 = _mm512_loadu_si512((__m512i*)pw1 + 2);
                    w36 = _mm512_loadu_si512((__m512i*)pw1 + 3);
                    w47 = _mm512_loadu_si512((__m512i*)pw1 + 4);
                    w58 = _mm512_loadu_si512((__m512i*)pw1 + 5);

                    size_t dx = 0;
#if 1
                    for (; dx < dstW2; dx += 2, ps0 += QF, ps2 += QF)
                    {
                        d00 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();
                        d01 = _mm512_setzero_si512();
                        d11 = _mm512_setzero_si512();

                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 0);
                        Madd1(d00, s0, w03);
                        Madd1(d01, s0, w0);
                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 1);
                        Madd1(d00, s0, w14);
                        Madd1(d10, s0, w03);
                        Madd1(d01, s0, w1);
                        Madd1(d11, s0, w0);
                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 2);
                        Madd1(d00, s0, w25);
                        Madd1(d10, s0, w14);
                        Madd1(d01, s0, w2);
                        Madd1(d11, s0, w1);
                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 3);
                        Madd1(d10, s0, w25);
                        Madd1(d11, s0, w2);

                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 0);
                        Madd1(d00, s0, w6);
                        Madd1(d01, s0, w36);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 1);
                        Madd1(d00, s0, w7);
                        Madd1(d10, s0, w6);
                        Madd1(d01, s0, w47);
                        Madd1(d11, s0, w36);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 2);
                        Madd1(d00, s0, w8);
                        Madd1(d10, s0, w7);
                        Madd1(d01, s0, w58);
                        Madd1(d11, s0, w47);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 3);
                        Madd1(d10, s0, w8);
                        Madd1(d11, s0, w58);

                        Save1<term>(pd0 + 0 * dD, d00, _bias, _norm, _zero, tail);
                        Save1<term>(pd0 + 1 * dD, d10, _bias, _norm, _zero, tail);
                        Save1<term>(pd1 + 0 * dD, d01, _bias, _norm, _zero, tail);
                        Save1<term>(pd1 + 1 * dD, d11, _bias, _norm, _zero, tail);
                        pd0 += 2 * dD;
                        pd1 += 2 * dD;
                    }
#endif
                    for (; dx < p.dstW; ++dx, ps0 += dX, ps2 += dX)
                    {
                        d00 = _mm512_setzero_si512();
                        d01 = _mm512_setzero_si512();

                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 0);
                        Madd1(d00, s0, w03);
                        Madd1(d01, s0, w0);
                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 1);
                        Madd1(d00, s0, w14);
                        Madd1(d01, s0, w1);
                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 2);
                        Madd1(d00, s0, w25);
                        Madd1(d01, s0, w2);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 0);
                        Madd1(d00, s0, w6);
                        Madd1(d01, s0, w36);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 1);
                        Madd1(d00, s0, w7);
                        Madd1(d01, s0, w47);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 2);
                        Madd1(d00, s0, w8);
                        Madd1(d01, s0, w58);

                        Save1<term>(pd0, d00, _bias, _norm, _zero, tail);
                        Save1<term>(pd1, d01, _bias, _norm, _zero, tail);
                        pd0 += dD;
                        pd1 += dD;
                    }
                }
                dst += p.dstW * dD * 2;
            }
            for (; dy < dyEnd; ++dy)
            {
                size_t sy = dy * sY;
                for (size_t sc = 0; sc < srcC; sc += F)
                {
                    uint8_t* pd = dst + sc;
                    const int16_t* ps0 = src + ((sy + 0) & byMask) * bufR + sc * bW;
                    const int16_t* ps2 = src + ((sy + 2) & byMask) * bufR + sc * bW;
                    const int16_t* pw = weight + sc * dW;
                    _bias = _mm512_loadu_si512((__m512i*)(bias + sc));
                    _norm = _mm512_loadu_ps(norm + sc);
                    __mmask16 tail = TailMask16(srcC - sc);
                    w03 = _mm512_loadu_si512((__m512i*)pw + 0);
                    w14 = _mm512_loadu_si512((__m512i*)pw + 1);
                    w25 = _mm512_loadu_si512((__m512i*)pw + 2);
                    w6 = _mm512_loadu_si512((__m512i*)pw + 3);
                    w7 = _mm512_loadu_si512((__m512i*)pw + 4);
                    w8 = _mm512_loadu_si512((__m512i*)pw + 5);

                    size_t dx = 0;
                    for (; dx < dstW2; dx += 2, ps0 += QF, ps2 += QF)
                    {
                        d00 = _mm512_setzero_si512();
                        d10 = _mm512_setzero_si512();

                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 0);
                        Madd1(d00, s0, w03);
                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 1);
                        Madd1(d00, s0, w14);
                        Madd1(d10, s0, w03);
                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 2);
                        Madd1(d00, s0, w25);
                        Madd1(d10, s0, w14);
                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 3);
                        Madd1(d10, s0, w25);

                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 0);
                        Madd1(d00, s0, w6);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 1);
                        Madd1(d00, s0, w7);
                        Madd1(d10, s0, w6);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 2);
                        Madd1(d00, s0, w8);
                        Madd1(d10, s0, w7);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 3);
                        Madd1(d10, s0, w8);

                        Save1<term>(pd + 0 * dD, d00, _bias, _norm, _zero, tail);
                        Save1<term>(pd + 1 * dD, d10, _bias, _norm, _zero, tail);
                        pd += 2 * dD;
                    }
                    for (; dx < p.dstW; ++dx, ps0 += dX, ps2 += dX)
                    {
                        d00 = _mm512_setzero_si512();

                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 0);
                        Madd1(d00, s0, w03);
                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 1);
                        Madd1(d00, s0, w14);
                        s0 = _mm512_loadu_si512((__m512i*)ps0 + 2);
                        Madd1(d00, s0, w25);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 0);
                        Madd1(d00, s0, w6);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 1);
                        Madd1(d00, s0, w7);
                        s0 = _mm512_loadu_si512((__m512i*)ps2 + 2);
                        Madd1(d00, s0, w8);

                        Save1<term>(pd, d00, _bias, _norm, _zero, tail);
                        pd += dD;
                    }
                }
                dst += p.dstW * dD;
            }
        }

        //------------------------------------------------------------------------------------------------

        template <Term8iType term> void SetV2(const ConvParam& p, const AlgParamV2& a, SynetQuantizedConvolutionNhwcDepthwiseV2::ConvolutionPtr& convolution)
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
            : Avx2::SynetQuantizedConvolutionNhwcDepthwiseV2(p)
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
