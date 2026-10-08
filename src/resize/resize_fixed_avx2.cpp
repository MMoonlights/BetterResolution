#include "resize/resize_fixed.hpp"
#include <cstring>

#if BR_SIMD_X86
namespace br::resize::fixed {
namespace {
// Отдельный файл для конкретного набора инструкций: глобальный -mavx2 и FMA/BMI не требуются.
// Векторное чтение не выходит за число исходных пикселей и выборок.
template <uint32_t T>
BR_TARGET_AVX2 void h4_impl(const uint8_t* in, int16_t* out, const AxisSampler& s, const Axis& q) {
    const uint32_t taps = T ? T : s.taps;
    const uint32_t pairs = (taps + 1u) / 2u;
    const __m256i mask = _mm256_broadcastsi128_si256(_mm_setr_epi8(0,-1,4,-1,1,-1,5,-1,2,-1,6,-1,3,-1,7,-1));
    uint32_t x = 0;
    for (; x + 2 <= s.out_size; x += 2) {
        const uint8_t* pa = in + static_cast<size_t>(s.start[x]) * 4;
        const uint8_t* pb = in + static_cast<size_t>(s.start[x + 1]) * 4;
        const uint32_t* wa = q.pairs.data() + static_cast<size_t>(x) * pairs;
        const uint32_t* wb = wa + pairs;
        __m256i sum = _mm256_set1_epi32(128);
        uint32_t k = 0;
        for (; k + 2 <= taps; k += 2) {
            const __m128i a = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(pa + k * 4));
            const __m128i b = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(pb + k * 4));
            const __m256i p = _mm256_shuffle_epi8(_mm256_set_m128i(b, a), mask);
            const __m256i w = _mm256_set_m128i(_mm_set1_epi32(static_cast<int32_t>(wb[k / 2])), _mm_set1_epi32(static_cast<int32_t>(wa[k / 2])));
            sum = _mm256_add_epi32(sum, _mm256_madd_epi16(p, w));
        }
        if (k < taps) {
            int32_t a, b;
            std::memcpy(&a, pa + k * 4, 4);
            std::memcpy(&b, pb + k * 4, 4);
            const __m256i p = _mm256_shuffle_epi8(_mm256_set_m128i(_mm_cvtsi32_si128(b), _mm_cvtsi32_si128(a)), mask);
            const __m256i w = _mm256_set_m128i(_mm_set1_epi32(static_cast<int32_t>(wb[k / 2])), _mm_set1_epi32(static_cast<int32_t>(wa[k / 2])));
            sum = _mm256_add_epi32(sum, _mm256_madd_epi16(p, w));
        }
        sum = _mm256_srai_epi32(sum, 8);
        const __m128i v = _mm_packs_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + static_cast<size_t>(x) * 4), v);
    }
    if (x < s.out_size) {
        // Не более одного пикселя: не читаем вектором отсутствующую следующую запись плана.
        const uint8_t* p = in + static_cast<size_t>(s.start[x]) * 4;
        const int16_t* w = q.weights.data() + static_cast<size_t>(x) * taps;
        for (uint32_t c = 0; c < 4; ++c) {
            int32_t sum = 128;
            for (uint32_t k = 0; k < taps; ++k) sum += p[k * 4 + c] * w[k];
            out[static_cast<size_t>(x) * 4 + c] = static_cast<int16_t>(sum >> 8);
        }
    }
}
BR_TARGET_AVX2 void h4_avx2(const uint8_t* in, int16_t* out, const AxisSampler& s, const Axis& q) {
    switch (s.taps) {
#define BR_H_CASE(N) case N: return h4_impl<N>(in, out, s, q)
    BR_H_CASE(2); BR_H_CASE(3); BR_H_CASE(4); BR_H_CASE(5); BR_H_CASE(6);
    BR_H_CASE(7); BR_H_CASE(8); BR_H_CASE(9); BR_H_CASE(10); BR_H_CASE(12);
#undef BR_H_CASE
    default: return h4_impl<0>(in, out, s, q);
    }
}
BR_TARGET_AVX2 void v_avx2(uint8_t* out, size_t n, const int16_t* const* rows, const int16_t* w, uint32_t taps) {
    __m256i coeff[(kMaxTaps + 1) / 2];
    for (uint32_t k = 0; k < taps; k += 2) {
        const uint32_t pair = static_cast<uint16_t>(w[k]) |
            (k + 1 < taps ? static_cast<uint32_t>(static_cast<uint16_t>(w[k + 1])) << 16 : 0u);
        coeff[k / 2] = _mm256_set1_epi32(static_cast<int32_t>(pair));
    }
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        __m256i lo = _mm256_set1_epi32(1 << 19), hi = lo;
        for (uint32_t k = 0; k < taps; k += 2) {
            const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[k] + i));
            const __m256i b = k + 1 < taps ? _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[k + 1] + i)) : _mm256_setzero_si256();
            lo = _mm256_add_epi32(lo, _mm256_madd_epi16(_mm256_unpacklo_epi16(a, b), coeff[k / 2]));
            hi = _mm256_add_epi32(hi, _mm256_madd_epi16(_mm256_unpackhi_epi16(a, b), coeff[k / 2]));
        }
        const __m256i v = _mm256_packs_epi32(_mm256_srai_epi32(lo, 20), _mm256_srai_epi32(hi, 20));
        const __m128i bytes = _mm_packus_epi16(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), bytes);
    }
    for (; i < n; ++i) {
        int32_t sum = 1 << 19;
        for (uint32_t k = 0; k < taps; ++k) sum += static_cast<int32_t>(rows[k][i]) * w[k];
        const int32_t v = sum >> 20;
        out[i] = static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}
}
const Kernels& avx2_kernels() noexcept {
    static const Kernels k{"fixed-avx2", h4_avx2, sse2_kernels().h1, v_avx2};
    return k;
}
}
#endif
