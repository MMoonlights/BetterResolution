// Resize inner loops. Included by resize_simd_generic.cpp and resize_simd_avx2.cpp,
// each compiled with different target flags. Define BR_SIMD_NS and BR_SIMD_AVX2 first.

#include "resize/resize_simd.hpp"


#if !defined(BR_RESIZE_SCALAR) && (defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#define BR_SIMD_SSE 1
#include <immintrin.h>
#else
#define BR_SIMD_SSE 0
#endif

#if BR_SIMD_AVX2
#define BR_FMA128(a, b, c) _mm_fmadd_ps((a), (b), (c))
#define BR_FMA256(a, b, c) _mm256_fmadd_ps((a), (b), (c))
#else
#define BR_FMA128(a, b, c) _mm_add_ps(_mm_mul_ps((a), (b)), (c))
#endif

namespace br::resize::BR_SIMD_NS {
namespace {

// Local helpers instead of std::min/max/clamp: template instantiations shared between
// translation units compiled with different -m flags could otherwise leak AVX code.
inline float mnf(float a, float b) { return b < a ? b : a; }
inline float mxf(float a, float b) { return a < b ? b : a; }
inline float clampf(float v, float lo, float hi) { return mnf(mxf(v, lo), hi); }

#if BR_SIMD_SSE
inline __m128 dot4(const float* p, const float* w, uint32_t T) {
    __m128 a0 = _mm_setzero_ps(), a1 = _mm_setzero_ps();
    uint32_t k = 0;
    for (; k + 2 <= T; k += 2) {
        a0 = BR_FMA128(_mm_set1_ps(w[k]), _mm_loadu_ps(p + 4 * k), a0);
        a1 = BR_FMA128(_mm_set1_ps(w[k + 1]), _mm_loadu_ps(p + 4 * k + 4), a1);
    }
    if (k < T) a0 = BR_FMA128(_mm_set1_ps(w[k]), _mm_loadu_ps(p + 4 * k), a0);
    return _mm_add_ps(a0, a1);
}
inline __m128 clamp_lobe4(__m128 acc, const float* p, uint32_t lb, uint32_t le) {
    __m128 mn = _mm_loadu_ps(p + 4 * lb), mx = mn;
    for (uint32_t j = lb + 1; j < le; ++j) {
        const __m128 v = _mm_loadu_ps(p + 4 * j);
        mn = _mm_min_ps(mn, v);
        mx = _mm_max_ps(mx, v);
    }
    return _mm_min_ps(_mm_max_ps(acc, mn), mx);
}
#endif

#if BR_SIMD_SSE
// Fully unrolled two-pixel kernel for a compile-time tap count.
template <uint32_t T>
void h4_fixed(const float* in, float* out, const AxisSampler& s, bool antiring) {
    const float* W = s.weights.data();
    const int32_t* st = s.start.data();
    const uint32_t n = s.out_size;
    uint32_t x = 0;
    for (; x + 2 <= n; x += 2) {
        const float* pa = in + 4 * static_cast<size_t>(st[x]);
        const float* pb = in + 4 * static_cast<size_t>(st[x + 1]);
        const float* wa = W + static_cast<size_t>(x) * T;
        const float* wb = wa + T;
        __m128 a0 = _mm_mul_ps(_mm_set1_ps(wa[0]), _mm_loadu_ps(pa));
        __m128 b0 = _mm_mul_ps(_mm_set1_ps(wb[0]), _mm_loadu_ps(pb));
        __m128 a1 = _mm_setzero_ps(), b1 = _mm_setzero_ps();
        for (uint32_t k = 1; k < T; k += 2) { // unrolled by the compiler (T is constant)
            a1 = BR_FMA128(_mm_set1_ps(wa[k]), _mm_loadu_ps(pa + 4 * k), a1);
            b1 = BR_FMA128(_mm_set1_ps(wb[k]), _mm_loadu_ps(pb + 4 * k), b1);
            if (k + 1 < T) {
                a0 = BR_FMA128(_mm_set1_ps(wa[k + 1]), _mm_loadu_ps(pa + 4 * k + 4), a0);
                b0 = BR_FMA128(_mm_set1_ps(wb[k + 1]), _mm_loadu_ps(pb + 4 * k + 4), b0);
            }
        }
        __m128 ra = _mm_add_ps(a0, a1), rb = _mm_add_ps(b0, b1);
        if (antiring) {
            ra = clamp_lobe4(ra, pa, s.lobe_begin[x], s.lobe_end[x]);
            rb = clamp_lobe4(rb, pb, s.lobe_begin[x + 1], s.lobe_end[x + 1]);
        }
        _mm_storeu_ps(out + 4 * static_cast<size_t>(x), ra);
        _mm_storeu_ps(out + 4 * static_cast<size_t>(x) + 4, rb);
    }
    for (; x < n; ++x) {
        const float* p = in + 4 * static_cast<size_t>(st[x]);
        __m128 acc = dot4(p, W + static_cast<size_t>(x) * T, T);
        if (antiring) acc = clamp_lobe4(acc, p, s.lobe_begin[x], s.lobe_end[x]);
        _mm_storeu_ps(out + 4 * static_cast<size_t>(x), acc);
    }
}
#endif

void h4(const float* in, float* out, const AxisSampler& s, bool antiring) {
    const uint32_t T = s.taps;
    const float* W = s.weights.data();
    const int32_t* st = s.start.data();
    const uint32_t n = s.out_size;
    uint32_t x = 0;
#if BR_SIMD_SSE
    switch (T) {
    case 2: return h4_fixed<2>(in, out, s, antiring);
    case 3: return h4_fixed<3>(in, out, s, antiring);
    case 4: return h4_fixed<4>(in, out, s, antiring);
    case 5: return h4_fixed<5>(in, out, s, antiring);
    case 6: return h4_fixed<6>(in, out, s, antiring);
    case 7: return h4_fixed<7>(in, out, s, antiring);
    case 8: return h4_fixed<8>(in, out, s, antiring);
    case 9: return h4_fixed<9>(in, out, s, antiring);
    case 10: return h4_fixed<10>(in, out, s, antiring);
    case 12: return h4_fixed<12>(in, out, s, antiring);
    default: break;
    }
    // Generic: two output pixels per iteration, four independent FMA chains.
    for (; x + 2 <= n; x += 2) {
        const float* pa = in + 4 * static_cast<size_t>(st[x]);
        const float* pb = in + 4 * static_cast<size_t>(st[x + 1]);
        const float* wa = W + static_cast<size_t>(x) * T;
        const float* wb = wa + T;
        __m128 a0 = _mm_setzero_ps(), a1 = _mm_setzero_ps(), b0 = _mm_setzero_ps(), b1 = _mm_setzero_ps();
        uint32_t k = 0;
        for (; k + 2 <= T; k += 2) {
            a0 = BR_FMA128(_mm_set1_ps(wa[k]), _mm_loadu_ps(pa + 4 * k), a0);
            b0 = BR_FMA128(_mm_set1_ps(wb[k]), _mm_loadu_ps(pb + 4 * k), b0);
            a1 = BR_FMA128(_mm_set1_ps(wa[k + 1]), _mm_loadu_ps(pa + 4 * k + 4), a1);
            b1 = BR_FMA128(_mm_set1_ps(wb[k + 1]), _mm_loadu_ps(pb + 4 * k + 4), b1);
        }
        if (k < T) {
            a0 = BR_FMA128(_mm_set1_ps(wa[k]), _mm_loadu_ps(pa + 4 * k), a0);
            b0 = BR_FMA128(_mm_set1_ps(wb[k]), _mm_loadu_ps(pb + 4 * k), b0);
        }
        __m128 ra = _mm_add_ps(a0, a1), rb = _mm_add_ps(b0, b1);
        if (antiring) {
            ra = clamp_lobe4(ra, pa, s.lobe_begin[x], s.lobe_end[x]);
            rb = clamp_lobe4(rb, pb, s.lobe_begin[x + 1], s.lobe_end[x + 1]);
        }
        _mm_storeu_ps(out + 4 * static_cast<size_t>(x), ra);
        _mm_storeu_ps(out + 4 * static_cast<size_t>(x) + 4, rb);
    }
    for (; x < n; ++x) {
        const float* p = in + 4 * static_cast<size_t>(st[x]);
        __m128 acc = dot4(p, W + static_cast<size_t>(x) * T, T);
        if (antiring) acc = clamp_lobe4(acc, p, s.lobe_begin[x], s.lobe_end[x]);
        _mm_storeu_ps(out + 4 * static_cast<size_t>(x), acc);
    }
#else
    for (; x < n; ++x) {
        const float* p = in + 4 * static_cast<size_t>(st[x]);
        const float* w = W + static_cast<size_t>(x) * T;
        float acc[4] = {0, 0, 0, 0};
        for (uint32_t k = 0; k < T; ++k)
            for (int l = 0; l < 4; ++l) acc[l] += w[k] * p[4 * k + l];
        if (antiring) {
            const uint32_t lb = s.lobe_begin[x], le = s.lobe_end[x];
            for (int l = 0; l < 4; ++l) {
                float mn = p[4 * lb + l], mx = mn;
                for (uint32_t j = lb + 1; j < le; ++j) { mn = mnf(mn, p[4 * j + l]); mx = mxf(mx, p[4 * j + l]); }
                acc[l] = clampf(acc[l], mn, mx);
            }
        }
        for (int l = 0; l < 4; ++l) out[4 * static_cast<size_t>(x) + l] = acc[l];
    }
#endif
}

void u8_to_f32(const uint8_t* src, float* dst, size_t n) {
    size_t i = 0;
    const float k = 1.0f / 255.0f;
#if BR_SIMD_AVX2
    const __m256 kv = _mm256_set1_ps(k);
    for (; i + 8 <= n; i += 8) {
        const __m128i b = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src + i));
        _mm256_storeu_ps(dst + i, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(b)), kv));
    }
#elif BR_SIMD_SSE
    const __m128 kv = _mm_set1_ps(k);
    const __m128i zero = _mm_setzero_si128();
    for (; i + 16 <= n; i += 16) {
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        const __m128i lo = _mm_unpacklo_epi8(b, zero), hi = _mm_unpackhi_epi8(b, zero);
        _mm_storeu_ps(dst + i, _mm_mul_ps(_mm_cvtepi32_ps(_mm_unpacklo_epi16(lo, zero)), kv));
        _mm_storeu_ps(dst + i + 4, _mm_mul_ps(_mm_cvtepi32_ps(_mm_unpackhi_epi16(lo, zero)), kv));
        _mm_storeu_ps(dst + i + 8, _mm_mul_ps(_mm_cvtepi32_ps(_mm_unpacklo_epi16(hi, zero)), kv));
        _mm_storeu_ps(dst + i + 12, _mm_mul_ps(_mm_cvtepi32_ps(_mm_unpackhi_epi16(hi, zero)), kv));
    }
#endif
    for (; i < n; ++i) dst[i] = static_cast<float>(src[i]) * k;
}

void f32_to_u8(const float* src, uint8_t* dst, size_t n) {
    size_t i = 0;
#if BR_SIMD_SSE
    const __m128 scale = _mm_set1_ps(255.0f), zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
    for (; i + 16 <= n; i += 16) {
        __m128i q[4];
        for (int j = 0; j < 4; ++j) {
            const __m128 v = _mm_min_ps(_mm_max_ps(_mm_loadu_ps(src + i + 4 * j), zero), one); // NaN -> 0
            q[j] = _mm_cvtps_epi32(_mm_mul_ps(v, scale));
        }
        const __m128i a = _mm_packs_epi32(q[0], q[1]), b = _mm_packs_epi32(q[2], q[3]);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), _mm_packus_epi16(a, b));
    }
#endif
    for (; i < n; ++i) {
        const float v = src[i];
        dst[i] = !(v > 0.0f) ? 0 : v >= 1.0f ? 255 : static_cast<uint8_t>(v * 255.0f + 0.5f);
    }
}

void h1(const float* in, float* out, const AxisSampler& s, bool antiring) {
    const uint32_t T = s.taps;
    const float* W = s.weights.data();
    const int32_t* st = s.start.data();
    for (uint32_t x = 0; x < s.out_size; ++x) {
        const float* p = in + st[x];
        const float* w = W + static_cast<size_t>(x) * T;
        float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
        uint32_t k = 0;
        for (; k + 4 <= T; k += 4) {
            a0 += w[k] * p[k];
            a1 += w[k + 1] * p[k + 1];
            a2 += w[k + 2] * p[k + 2];
            a3 += w[k + 3] * p[k + 3];
        }
        for (; k < T; ++k) a0 += w[k] * p[k];
        float acc = (a0 + a1) + (a2 + a3);
        if (antiring) {
            const uint32_t lb = s.lobe_begin[x], le = s.lobe_end[x];
            float mn = p[lb], mx = mn;
            for (uint32_t j = lb + 1; j < le; ++j) { mn = mnf(mn, p[j]); mx = mxf(mx, p[j]); }
            acc = clampf(acc, mn, mx);
        }
        out[x] = acc;
    }
}

void vpass(float* out, size_t n, const float* const* rows, const float* w, uint32_t T,
           uint32_t lb, uint32_t le, bool antiring) {
    size_t i = 0;
#if BR_SIMD_AVX2
    for (; i + 8 <= n; i += 8) {
        __m256 acc = _mm256_mul_ps(_mm256_set1_ps(w[0]), _mm256_loadu_ps(rows[0] + i));
        for (uint32_t k = 1; k < T; ++k) acc = BR_FMA256(_mm256_set1_ps(w[k]), _mm256_loadu_ps(rows[k] + i), acc);
        if (antiring) {
            __m256 mn = _mm256_loadu_ps(rows[lb] + i), mx = mn;
            for (uint32_t j = lb + 1; j < le; ++j) {
                const __m256 v = _mm256_loadu_ps(rows[j] + i);
                mn = _mm256_min_ps(mn, v);
                mx = _mm256_max_ps(mx, v);
            }
            acc = _mm256_min_ps(_mm256_max_ps(acc, mn), mx);
        }
        _mm256_storeu_ps(out + i, acc);
    }
#endif
#if BR_SIMD_SSE
    for (; i + 4 <= n; i += 4) {
        __m128 acc = _mm_mul_ps(_mm_set1_ps(w[0]), _mm_loadu_ps(rows[0] + i));
        for (uint32_t k = 1; k < T; ++k) acc = BR_FMA128(_mm_set1_ps(w[k]), _mm_loadu_ps(rows[k] + i), acc);
        if (antiring) {
            __m128 mn = _mm_loadu_ps(rows[lb] + i), mx = mn;
            for (uint32_t j = lb + 1; j < le; ++j) {
                const __m128 v = _mm_loadu_ps(rows[j] + i);
                mn = _mm_min_ps(mn, v);
                mx = _mm_max_ps(mx, v);
            }
            acc = _mm_min_ps(_mm_max_ps(acc, mn), mx);
        }
        _mm_storeu_ps(out + i, acc);
    }
#endif
    for (; i < n; ++i) {
        float acc = 0.0f;
        for (uint32_t k = 0; k < T; ++k) acc += w[k] * rows[k][i];
        if (antiring) {
            float mn = rows[lb][i], mx = mn;
            for (uint32_t j = lb + 1; j < le; ++j) { mn = mnf(mn, rows[j][i]); mx = mxf(mx, rows[j][i]); }
            acc = clampf(acc, mn, mx);
        }
        out[i] = acc;
    }
}

}

const SimdKernels& kernels() noexcept {
    static const SimdKernels k{BR_SIMD_AVX2 ? "avx2" : (BR_SIMD_SSE ? "sse2" : "scalar"), h4, h1, vpass, u8_to_f32, f32_to_u8};
    return k;
}

}

#undef BR_FMA128
#undef BR_FMA256
#undef BR_SIMD_SSE
