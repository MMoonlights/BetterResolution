#include "resize/resize_fixed.hpp"
#include "core/cpu.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace br::resize::fixed {
Axis quantize(const AxisSampler& s) {
    Axis out;
    if (!s.taps || s.taps > kMaxTaps) return out;
    out.weights.resize(s.weights.size());
    const size_t pairs = (s.taps + 1u) / 2u;
    out.pairs.resize(static_cast<size_t>(s.out_size) * pairs);
    for (uint32_t x = 0; x < s.out_size; ++x) {
        const size_t at = static_cast<size_t>(x) * s.taps;
        int q[kMaxTaps]{};
        int sum = 0;
        uint32_t peak = 0;
        for (uint32_t k = 0; k < s.taps; ++k) {
            const double w = s.weights[at + k];
            if (!std::isfinite(w) || std::abs(w) > 1.99) return Axis{};
            q[k] = static_cast<int>(std::lround(w * kOne));
            sum += q[k];
            if (std::abs(w) > std::abs(s.weights[at + peak])) peak = k;
        }
        q[peak] += kOne - sum;
        int l1 = 0;
        double error = 0;
        for (uint32_t k = 0; k < s.taps; ++k) {
            if (q[k] < std::numeric_limits<int16_t>::min() || q[k] > std::numeric_limits<int16_t>::max()) return Axis{};
            out.weights[at + k] = static_cast<int16_t>(q[k]);
            l1 += std::abs(q[k]);
            error += std::abs(static_cast<double>(q[k]) / kOne - s.weights[at + k]);
        }
        // Вместе с sum==16384 это ограничивает горизонтальный результат Q6 диапазоном
        // [-8160,24480]. Даже sum(|vertical weight|)*32768 с округлением помещается в int32.
        if (l1 > 2 * kOne) return Axis{};
        out.max_error = std::max(out.max_error, error);
        out.max_l1 = std::max(out.max_l1, static_cast<double>(l1) / kOne);
        for (size_t p = 0; p < pairs; ++p) {
            const uint32_t lo = static_cast<uint16_t>(q[2 * p]);
            const uint32_t hi = 2 * p + 1 < s.taps ? static_cast<uint16_t>(q[2 * p + 1]) : 0u;
            out.pairs[static_cast<size_t>(x) * pairs + p] = lo | (hi << 16);
        }
    }
    out.valid = true;
    return out;
}

bool compatible(const Axis& h, const Axis& v) noexcept {
    // Консервативная оценка ошибки пикселя до округления, включая округление по горизонтали в Q6.
    // Оставляем запас на суммирование float и таблицу кодирования RGB.
    return h.valid && v.valid &&
           v.max_l1 * (255.0 * h.max_error + 1.0 / 128.0) + 512.0 * v.max_error < 0.45;
}
namespace {
void h_scalar(const uint8_t* in, int16_t* out, const AxisSampler& s, const Axis& q, uint32_t channels) {
    for (uint32_t x = 0; x < s.out_size; ++x) {
        const uint8_t* p = in + static_cast<size_t>(s.start[x]) * channels;
        const int16_t* w = q.weights.data() + static_cast<size_t>(x) * s.taps;
        for (uint32_t c = 0; c < channels; ++c) {
            int32_t sum = 128;
            for (uint32_t k = 0; k < s.taps; ++k) sum += p[static_cast<size_t>(k) * channels + c] * w[k];
            // В C++20 сдвиг отрицательного числа вправо определён как деление с округлением вниз.
            out[static_cast<size_t>(x) * channels + c] = static_cast<int16_t>(sum >> 8);
        }
    }
}
void h4_scalar(const uint8_t* in, int16_t* out, const AxisSampler& s, const Axis& q) { h_scalar(in, out, s, q, 4); }
void h1_scalar(const uint8_t* in, int16_t* out, const AxisSampler& s, const Axis& q) { h_scalar(in, out, s, q, 1); }
void v_scalar(uint8_t* out, size_t n, const int16_t* const* rows, const int16_t* w, uint32_t taps) {
    for (size_t i = 0; i < n; ++i) {
        int32_t sum = 1 << 19;
        for (uint32_t k = 0; k < taps; ++k) sum += static_cast<int32_t>(rows[k][i]) * w[k];
        out[i] = static_cast<uint8_t>(std::clamp(sum >> 20, 0, 255));
    }
}

#if BR_SIMD_X86
inline __m128i pair_sse2(const uint8_t* p) noexcept {
    const __m128i v = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p)), _mm_setzero_si128());
    return _mm_unpacklo_epi16(v, _mm_srli_si128(v, 8));
}
inline __m128i last_pixel(const uint8_t* p) noexcept {
    int32_t pixel;
    std::memcpy(&pixel, p, 4);
    return _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(pixel), _mm_setzero_si128()), _mm_setzero_si128());
}
void h4_sse2(const uint8_t* in, int16_t* out, const AxisSampler& s, const Axis& q) {
    const uint32_t pairs = (s.taps + 1u) / 2u;
    for (uint32_t x = 0; x < s.out_size; ++x) {
        const uint8_t* p = in + static_cast<size_t>(s.start[x]) * 4;
        const uint32_t* w = q.pairs.data() + static_cast<size_t>(x) * pairs;
        __m128i sum = _mm_set1_epi32(128);
        uint32_t k = 0;
        for (; k + 2 <= s.taps; k += 2)
            sum = _mm_add_epi32(sum, _mm_madd_epi16(pair_sse2(p + k * 4), _mm_set1_epi32(static_cast<int32_t>(w[k / 2]))));
        if (k < s.taps) sum = _mm_add_epi32(sum, _mm_madd_epi16(last_pixel(p + k * 4), _mm_set1_epi32(static_cast<int32_t>(w[k / 2]))));
        const __m128i r = _mm_srai_epi32(sum, 8);
        _mm_storel_epi64(reinterpret_cast<__m128i*>(out + static_cast<size_t>(x) * 4), _mm_packs_epi32(r, r));
    }
}

// Одна инструкция pshufb заменяет распаковку, сдвиг и чередование для пары пикселей u8.
template <uint32_t T>
BR_TARGET_SSSE3 void h4_shuffle(const uint8_t* in, int16_t* out, const AxisSampler& s, const Axis& q) {
    const uint32_t taps = T ? T : s.taps;
    const uint32_t pairs = (taps + 1u) / 2u;
    const __m128i mask = _mm_setr_epi8(0,-1,4,-1,1,-1,5,-1,2,-1,6,-1,3,-1,7,-1);
    for (uint32_t x = 0; x < s.out_size; ++x) {
        const uint8_t* p = in + static_cast<size_t>(s.start[x]) * 4;
        const uint32_t* w = q.pairs.data() + static_cast<size_t>(x) * pairs;
        __m128i sum = _mm_set1_epi32(128);
        uint32_t k = 0;
        for (; k + 2 <= taps; k += 2) {
            const __m128i v = _mm_shuffle_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p + k * 4)), mask);
            sum = _mm_add_epi32(sum, _mm_madd_epi16(v, _mm_set1_epi32(static_cast<int32_t>(w[k / 2]))));
        }
        if (k < taps) sum = _mm_add_epi32(sum, _mm_madd_epi16(last_pixel(p + k * 4), _mm_set1_epi32(static_cast<int32_t>(w[k / 2]))));
        const __m128i r = _mm_srai_epi32(sum, 8);
        _mm_storel_epi64(reinterpret_cast<__m128i*>(out + static_cast<size_t>(x) * 4), _mm_packs_epi32(r, r));
    }
}
BR_TARGET_SSSE3 void h4_ssse3(const uint8_t* in, int16_t* out, const AxisSampler& s, const Axis& q) {
    switch (s.taps) {
#define BR_H_CASE(N) case N: return h4_shuffle<N>(in, out, s, q)
    BR_H_CASE(2); BR_H_CASE(3); BR_H_CASE(4); BR_H_CASE(5); BR_H_CASE(6);
    BR_H_CASE(7); BR_H_CASE(8); BR_H_CASE(9); BR_H_CASE(10); BR_H_CASE(12);
#undef BR_H_CASE
    default: return h4_shuffle<0>(in, out, s, q);
    }
}
void h1_sse2(const uint8_t* in, int16_t* out, const AxisSampler& s, const Axis& q) {
    for (uint32_t x = 0; x < s.out_size; ++x) {
        const uint8_t* p = in + s.start[x];
        const int16_t* w = q.weights.data() + static_cast<size_t>(x) * s.taps;
        __m128i sum = _mm_setzero_si128();
        uint32_t k = 0;
        for (; k + 8 <= s.taps; k += 8) {
            const __m128i v = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p + k)), _mm_setzero_si128());
            sum = _mm_add_epi32(sum, _mm_madd_epi16(v, _mm_loadu_si128(reinterpret_cast<const __m128i*>(w + k))));
        }
        sum = _mm_add_epi32(sum, _mm_srli_si128(sum, 8));
        sum = _mm_add_epi32(sum, _mm_srli_si128(sum, 4));
        int32_t v = 128 + _mm_cvtsi128_si32(sum);
        for (; k < s.taps; ++k) v += p[k] * w[k];
        out[x] = static_cast<int16_t>(v >> 8);
    }
}
void v_sse2(uint8_t* out, size_t n, const int16_t* const* rows, const int16_t* w, uint32_t taps) {
    __m128i coeff[(kMaxTaps + 1) / 2];
    for (uint32_t k = 0; k < taps; k += 2) {
        const uint32_t pair = static_cast<uint16_t>(w[k]) |
            (k + 1 < taps ? static_cast<uint32_t>(static_cast<uint16_t>(w[k + 1])) << 16 : 0u);
        coeff[k / 2] = _mm_set1_epi32(static_cast<int32_t>(pair));
    }
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m128i lo = _mm_set1_epi32(1 << 19), hi = lo;
        for (uint32_t k = 0; k < taps; k += 2) {
            const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rows[k] + i));
            const __m128i b = k + 1 < taps ? _mm_loadu_si128(reinterpret_cast<const __m128i*>(rows[k + 1] + i)) : _mm_setzero_si128();
            lo = _mm_add_epi32(lo, _mm_madd_epi16(_mm_unpacklo_epi16(a, b), coeff[k / 2]));
            hi = _mm_add_epi32(hi, _mm_madd_epi16(_mm_unpackhi_epi16(a, b), coeff[k / 2]));
        }
        const __m128i v = _mm_packs_epi32(_mm_srai_epi32(lo, 20), _mm_srai_epi32(hi, 20));
        _mm_storel_epi64(reinterpret_cast<__m128i*>(out + i), _mm_packus_epi16(v, v));
    }
    if (i < n) {
        const int16_t* tail[kMaxTaps];
        for (uint32_t k = 0; k < taps; ++k) tail[k] = rows[k] + i;
        v_scalar(out + i, n - i, tail, w, taps);
    }
}
#endif
}
const Kernels& scalar_kernels() noexcept {
    static const Kernels k{"fixed-scalar", h4_scalar, h1_scalar, v_scalar};
    return k;
}
#if BR_SIMD_X86
const Kernels& sse2_kernels() noexcept {
    static const Kernels k{"fixed-sse2", h4_sse2, h1_sse2, v_sse2};
    return k;
}
const Kernels& ssse3_kernels() noexcept {
    static const Kernels k{"fixed-ssse3", h4_ssse3, h1_sse2, v_sse2};
    return k;
}
#endif
const Kernels& kernels() noexcept {
#if BR_SIMD_X86
    const auto& cpu = cpu_features();
    if (cpu.avx2) return avx2_kernels();
    if (cpu.ssse3) return ssse3_kernels();
    if (cpu.sse2) return sse2_kernels();
#endif
    return scalar_kernels();
}
}
