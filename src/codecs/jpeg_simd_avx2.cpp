#include "codecs/jpeg_simd.hpp"
#if BR_SIMD_X86
namespace br::codec::jpeg_detail {
namespace {
// AAN сохраняет порядок скалярных операций и квантования; FMA и приближённые обратные значения не используются.
BR_TARGET_AVX2 void aan8(__m256 (&p)[8]) noexcept {
    const __m256 t0 = _mm256_add_ps(p[0],p[7]), t7 = _mm256_sub_ps(p[0],p[7]);
    const __m256 t1 = _mm256_add_ps(p[1],p[6]), t6 = _mm256_sub_ps(p[1],p[6]);
    const __m256 t2 = _mm256_add_ps(p[2],p[5]), t5 = _mm256_sub_ps(p[2],p[5]);
    const __m256 t3 = _mm256_add_ps(p[3],p[4]), t4 = _mm256_sub_ps(p[3],p[4]);
    __m256 t10 = _mm256_add_ps(t0,t3), t13 = _mm256_sub_ps(t0,t3);
    __m256 t11 = _mm256_add_ps(t1,t2), t12 = _mm256_sub_ps(t1,t2);
    p[0] = _mm256_add_ps(t10,t11); p[4] = _mm256_sub_ps(t10,t11);
    const __m256 z1 = _mm256_mul_ps(_mm256_add_ps(t12,t13),_mm256_set1_ps(0.707106781f));
    p[2] = _mm256_add_ps(t13,z1); p[6] = _mm256_sub_ps(t13,z1);
    t10 = _mm256_add_ps(t4,t5); t11 = _mm256_add_ps(t5,t6); t12 = _mm256_add_ps(t6,t7);
    const __m256 z5 = _mm256_mul_ps(_mm256_sub_ps(t10,t12),_mm256_set1_ps(0.382683433f));
    const __m256 z2 = _mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(0.541196100f),t10),z5);
    const __m256 z4 = _mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(1.306562965f),t12),z5);
    const __m256 z3 = _mm256_mul_ps(t11,_mm256_set1_ps(0.707106781f));
    const __m256 z11 = _mm256_add_ps(t7,z3), z13 = _mm256_sub_ps(t7,z3);
    p[5] = _mm256_add_ps(z13,z2); p[3] = _mm256_sub_ps(z13,z2);
    p[1] = _mm256_add_ps(z11,z4); p[7] = _mm256_sub_ps(z11,z4);
}

BR_TARGET_AVX2 void transpose8(__m256 (&p)[8]) noexcept {
    const __m256 a0 = _mm256_unpacklo_ps(p[0],p[1]), a1 = _mm256_unpackhi_ps(p[0],p[1]);
    const __m256 a2 = _mm256_unpacklo_ps(p[2],p[3]), a3 = _mm256_unpackhi_ps(p[2],p[3]);
    const __m256 a4 = _mm256_unpacklo_ps(p[4],p[5]), a5 = _mm256_unpackhi_ps(p[4],p[5]);
    const __m256 a6 = _mm256_unpacklo_ps(p[6],p[7]), a7 = _mm256_unpackhi_ps(p[6],p[7]);
    const __m256 b0 = _mm256_shuffle_ps(a0,a2,0x44), b1 = _mm256_shuffle_ps(a0,a2,0xee);
    const __m256 b2 = _mm256_shuffle_ps(a1,a3,0x44), b3 = _mm256_shuffle_ps(a1,a3,0xee);
    const __m256 b4 = _mm256_shuffle_ps(a4,a6,0x44), b5 = _mm256_shuffle_ps(a4,a6,0xee);
    const __m256 b6 = _mm256_shuffle_ps(a5,a7,0x44), b7 = _mm256_shuffle_ps(a5,a7,0xee);
    p[0] = _mm256_permute2f128_ps(b0,b4,0x20); p[4] = _mm256_permute2f128_ps(b0,b4,0x31);
    p[1] = _mm256_permute2f128_ps(b1,b5,0x20); p[5] = _mm256_permute2f128_ps(b1,b5,0x31);
    p[2] = _mm256_permute2f128_ps(b2,b6,0x20); p[6] = _mm256_permute2f128_ps(b2,b6,0x31);
    p[3] = _mm256_permute2f128_ps(b3,b7,0x20); p[7] = _mm256_permute2f128_ps(b3,b7,0x31);
}

template <uint32_t Channels, bool Bgr>
BR_TARGET_AVX2 uint32_t convert8(const uint8_t* src, uint32_t width, float* y, float* cb, float* cr) noexcept {
    constexpr int ro = Bgr ? 2 : 0, bo = Bgr ? 0 : 2;
    const __m256i mask = _mm256_set1_epi32(255);
    const __m128i expand = _mm_setr_epi8(0,1,2,-1,3,4,5,-1,6,7,8,-1,9,10,11,-1);
    uint32_t x = 0;
    for (; x + 8 <= width; x += 8, src += Channels * 8) {
        __m256i v;
        if constexpr (Channels == 4) {
            v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
        } else {
            // Перекрывающиеся загрузки считывают ровно 24 байта.
            const __m128i a = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src)),expand);
            const __m128i b = _mm_shuffle_epi8(_mm_srli_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src+8)),4),expand);
            v = _mm256_set_m128i(b,a);
        }
        const __m256 r = _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(v,8*ro),mask));
        const __m256 g = _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(v,8),mask));
        const __m256 b = _mm256_cvtepi32_ps(_mm256_and_si256(_mm256_srli_epi32(v,8*bo),mask));
        _mm256_storeu_ps(y+x,_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(0.299f),r),_mm256_mul_ps(_mm256_set1_ps(0.587f),g)),_mm256_mul_ps(_mm256_set1_ps(0.114f),b)));
        _mm256_storeu_ps(cb+x,_mm256_add_ps(_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(-0.168735892f),r),_mm256_mul_ps(_mm256_set1_ps(-0.331264108f),g)),_mm256_mul_ps(_mm256_set1_ps(0.5f),b)),_mm256_set1_ps(128.0f)));
        _mm256_storeu_ps(cr+x,_mm256_add_ps(_mm256_sub_ps(_mm256_sub_ps(_mm256_mul_ps(_mm256_set1_ps(0.5f),r),_mm256_mul_ps(_mm256_set1_ps(0.418687589f),g)),_mm256_mul_ps(_mm256_set1_ps(0.081312411f),b)),_mm256_set1_ps(128.0f)));
    }
    return x;
}
}

BR_TARGET_AVX2 void quantize_block_avx2(const float* src, size_t stride, const float* div, int16_t* q) noexcept {
    __m256 p[8];
    for (size_t k = 0; k < 8; ++k) p[k] = _mm256_sub_ps(_mm256_loadu_ps(src+k*stride),_mm256_set1_ps(128.0f));
    transpose8(p);
    aan8(p);
    transpose8(p);
    aan8(p);
    for (size_t k = 0; k < 8; ++k) {
        const __m256 v = _mm256_mul_ps(p[k],_mm256_loadu_ps(div+k*8));
        const __m256 half = _mm256_or_ps(_mm256_set1_ps(0.5f),_mm256_and_ps(v,_mm256_set1_ps(-0.0f)));
        const __m256i i = _mm256_cvttps_epi32(_mm256_add_ps(v,half));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(q+k*8),_mm_packs_epi32(_mm256_castsi256_si128(i),_mm256_extracti128_si256(i,1)));
    }
}
BR_TARGET_AVX2 uint32_t ycbcr_avx2(const uint8_t* src, uint32_t width, uint32_t channels, bool bgr,
                                  float* y, float* cb, float* cr) noexcept {
    if (channels == 4) return bgr ? convert8<4,true>(src,width,y,cb,cr) : convert8<4,false>(src,width,y,cb,cr);
    return bgr ? convert8<3,true>(src,width,y,cb,cr) : convert8<3,false>(src,width,y,cb,cr);
}
}
#endif
