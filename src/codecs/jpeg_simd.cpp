#include "codecs/jpeg_simd.hpp"
#if BR_SIMD_X86
namespace br::codec::jpeg_detail {
namespace {
// AAN сохраняет порядок скалярных операций; FMA и приближённые обратные значения не используются.
void aan4(__m128 (&p)[8]) noexcept {
    const __m128 t0 = _mm_add_ps(p[0],p[7]), t7 = _mm_sub_ps(p[0],p[7]);
    const __m128 t1 = _mm_add_ps(p[1],p[6]), t6 = _mm_sub_ps(p[1],p[6]);
    const __m128 t2 = _mm_add_ps(p[2],p[5]), t5 = _mm_sub_ps(p[2],p[5]);
    const __m128 t3 = _mm_add_ps(p[3],p[4]), t4 = _mm_sub_ps(p[3],p[4]);
    __m128 t10 = _mm_add_ps(t0,t3), t13 = _mm_sub_ps(t0,t3);
    __m128 t11 = _mm_add_ps(t1,t2), t12 = _mm_sub_ps(t1,t2);
    p[0] = _mm_add_ps(t10,t11); p[4] = _mm_sub_ps(t10,t11);
    const __m128 z1 = _mm_mul_ps(_mm_add_ps(t12,t13),_mm_set1_ps(0.707106781f));
    p[2] = _mm_add_ps(t13,z1); p[6] = _mm_sub_ps(t13,z1);
    t10 = _mm_add_ps(t4,t5); t11 = _mm_add_ps(t5,t6); t12 = _mm_add_ps(t6,t7);
    const __m128 z5 = _mm_mul_ps(_mm_sub_ps(t10,t12),_mm_set1_ps(0.382683433f));
    const __m128 z2 = _mm_add_ps(_mm_mul_ps(_mm_set1_ps(0.541196100f),t10),z5);
    const __m128 z4 = _mm_add_ps(_mm_mul_ps(_mm_set1_ps(1.306562965f),t12),z5);
    const __m128 z3 = _mm_mul_ps(t11,_mm_set1_ps(0.707106781f));
    const __m128 z11 = _mm_add_ps(t7,z3), z13 = _mm_sub_ps(t7,z3);
    p[5] = _mm_add_ps(z13,z2); p[3] = _mm_sub_ps(z13,z2);
    p[1] = _mm_add_ps(z11,z4); p[7] = _mm_sub_ps(z11,z4);
}
inline __m128i quant4(const float* p, const float* div) noexcept {
    const __m128 v = _mm_mul_ps(_mm_loadu_ps(p),_mm_loadu_ps(div));
    const __m128 sign = _mm_and_ps(v,_mm_set1_ps(-0.0f));
    return _mm_cvttps_epi32(_mm_add_ps(v,_mm_or_ps(_mm_set1_ps(0.5f),sign)));
}
template <int Channels, bool Bgr>
uint32_t convert(const uint8_t* src, uint32_t width, float* y, float* cb, float* cr) noexcept {
    constexpr int ro = Bgr ? 2 : 0, bo = Bgr ? 0 : 2;
    const __m128i mask = _mm_set1_epi32(255);
    uint32_t x = 0;
    for (; x + 4 <= width; x += 4, src += Channels * 4) {
        __m128 r, g, b;
        if constexpr (Channels == 4) {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
            r = _mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(v,8*ro),mask));
            g = _mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(v,8),mask));
            b = _mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(v,8*bo),mask));
        } else {
            r = _mm_cvtepi32_ps(_mm_setr_epi32(src[ro],src[3+ro],src[6+ro],src[9+ro]));
            g = _mm_cvtepi32_ps(_mm_setr_epi32(src[1],src[4],src[7],src[10]));
            b = _mm_cvtepi32_ps(_mm_setr_epi32(src[bo],src[3+bo],src[6+bo],src[9+bo]));
        }
        _mm_storeu_ps(y+x,_mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(0.299f),r),_mm_mul_ps(_mm_set1_ps(0.587f),g)),_mm_mul_ps(_mm_set1_ps(0.114f),b)));
        _mm_storeu_ps(cb+x,_mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(-0.168735892f),r),_mm_mul_ps(_mm_set1_ps(-0.331264108f),g)),_mm_mul_ps(_mm_set1_ps(0.5f),b)),_mm_set1_ps(128.0f)));
        _mm_storeu_ps(cr+x,_mm_add_ps(_mm_sub_ps(_mm_sub_ps(_mm_mul_ps(_mm_set1_ps(0.5f),r),_mm_mul_ps(_mm_set1_ps(0.418687589f),g)),_mm_mul_ps(_mm_set1_ps(0.081312411f),b)),_mm_set1_ps(128.0f)));
    }
    return x;
}
}

void quantize_block_sse2(const float* src, size_t stride, const float* div, int16_t* q) noexcept {
    alignas(16) float block[64];
    const __m128 bias = _mm_set1_ps(128.0f);
    for (size_t base = 0; base < 8; base += 4) {
        __m128 p[8];
        for (size_t k = 0; k < 4; ++k) {
            p[k] = _mm_sub_ps(_mm_loadu_ps(src+(base+k)*stride),bias);
            p[k+4] = _mm_sub_ps(_mm_loadu_ps(src+(base+k)*stride+4),bias);
        }
        _MM_TRANSPOSE4_PS(p[0],p[1],p[2],p[3]);
        _MM_TRANSPOSE4_PS(p[4],p[5],p[6],p[7]);
        aan4(p);
        _MM_TRANSPOSE4_PS(p[0],p[1],p[2],p[3]);
        _MM_TRANSPOSE4_PS(p[4],p[5],p[6],p[7]);
        for (size_t k = 0; k < 4; ++k) {
            _mm_store_ps(block+(base+k)*8,p[k]);
            _mm_store_ps(block+(base+k)*8+4,p[k+4]);
        }
    }
    for (size_t x = 0; x < 8; x += 4) {
        __m128 p[8];
        for (size_t k = 0; k < 8; ++k) p[k] = _mm_load_ps(block+k*8+x);
        aan4(p);
        for (size_t k = 0; k < 8; ++k) _mm_store_ps(block+k*8+x,p[k]);
    }
    for (size_t i = 0; i < 64; i += 8)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(q+i),_mm_packs_epi32(quant4(block+i,div+i),quant4(block+i+4,div+i+4)));
}
uint32_t ycbcr_sse2(const uint8_t* src, uint32_t width, uint32_t channels, bool bgr,
                   float* y, float* cb, float* cr) noexcept {
    if (channels == 4) return bgr ? convert<4,true>(src,width,y,cb,cr) : convert<4,false>(src,width,y,cb,cr);
    return bgr ? convert<3,true>(src,width,y,cb,cr) : convert<3,false>(src,width,y,cb,cr);
}
}
#endif
