#include "codecs/png_filters.hpp"
#include "core/cpu.hpp"
#include <cstring>

namespace br::codec::png_detail {
void filter_row(int type, const uint8_t* row, const uint8_t* prev, size_t n, size_t bpp, uint8_t* out) noexcept {
    size_t i = 0;
    for (; i < std::min(bpp, n); ++i) out[i] = residual(type, row[i], 0, prev[i], 0);
#if BR_SIMD_X86
    if (cpu_features().sse2) for (; i + 16 <= n; i += 16) {
        const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + i));
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + i - bpp));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(prev + i));
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(prev + i - bpp));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), _mm_sub_epi8(x, predict(type, a, b, c)));
    }
#endif
    for (; i < n; ++i) out[i] = residual(type, row[i], row[i - bpp], prev[i], prev[i - bpp]);
}

// Байты, совпадающие со значением слева или сверху.
size_t count_repeats(const uint8_t* row, const uint8_t* prev, size_t n, size_t bpp) noexcept {
    size_t repeats = 0, i = 0;
    for (; i < std::min(bpp, n); ++i) repeats += row[i] == prev[i];
#if BR_SIMD_X86
    if (cpu_features().sse2) {
    const __m128i zero = _mm_setzero_si128();
    while (i + 16 <= n) {
        // Суммирует счётчики байтов до их переполнения.
        __m128i cnt = zero;
        for (int k = 0; k < 255 && i + 16 <= n; ++k, i += 16) {
            const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + i));
            const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + i - bpp));
            const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(prev + i));
            cnt = _mm_sub_epi8(cnt, _mm_or_si128(_mm_cmpeq_epi8(x, a), _mm_cmpeq_epi8(x, b)));
        }
        const __m128i sum = _mm_sad_epu8(cnt, zero);
        repeats += static_cast<size_t>(_mm_cvtsi128_si32(sum) + _mm_extract_epi16(sum, 4));
    }
    }
#endif
    for (; i < n; ++i) repeats += (row[i] == row[i - bpp]) | (row[i] == prev[i]);
    return repeats;
}

// score[t] = сумма модулей знаковых остатков байтов.
void score_filters(const uint8_t* row, const uint8_t* prev, size_t n, size_t bpp, uint64_t score[5]) noexcept {
#if BR_SIMD_X86
    if (n >= 64 && cpu_features().avx2) { score_filters_avx2(row, prev, n, bpp, score); return; }
#endif
    for (int t = 0; t < 5; ++t) score[t] = 0;
    size_t i = 0;
    for (; i < std::min(bpp, n); ++i)
        for (int t = 0; t < 5; ++t) score[t] += abs_i8(residual(t, row[i], 0, prev[i], 0));
#if BR_SIMD_X86
    if (cpu_features().sse2) {
    const __m128i zero = _mm_setzero_si128();
    __m128i acc[5] = {zero, zero, zero, zero, zero};
    auto absum = [&](__m128i v) { return _mm_sad_epu8(_mm_min_epu8(v, _mm_sub_epi8(zero, v)), zero); };
    for (; i + 16 <= n; i += 16) {
        const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + i));
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + i - bpp));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(prev + i));
        const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(prev + i - bpp));
        acc[0] = _mm_add_epi64(acc[0], absum(x));
        acc[1] = _mm_add_epi64(acc[1], absum(_mm_sub_epi8(x, a)));
        acc[2] = _mm_add_epi64(acc[2], absum(_mm_sub_epi8(x, b)));
        acc[3] = _mm_add_epi64(acc[3], absum(_mm_sub_epi8(x, avg_floor(a, b))));
        acc[4] = _mm_add_epi64(acc[4], absum(_mm_sub_epi8(x, paeth_pred(a, b, c))));
    }
    for (int t = 0; t < 5; ++t)
        score[t] += static_cast<uint64_t>(_mm_cvtsi128_si64(acc[t])) + static_cast<uint64_t>(_mm_cvtsi128_si64(_mm_unpackhi_epi64(acc[t], acc[t])));
    }
#endif
    for (; i < n; ++i) {
        const uint8_t x = row[i], a = row[i - bpp], b = prev[i], c = prev[i - bpp];
        for (int t = 0; t < 5; ++t) score[t] += abs_i8(residual(t, x, a, b, c));
    }
}

// Восстанавливает ровно n байтов и не обращается к заполнению строки.
bool unfilter_row_scalar(uint8_t type, uint8_t* r, const uint8_t* u, size_t n, size_t bpp) noexcept {
    if (type > 4 || !bpp) return false;
    if (!type) return true;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t a = i >= bpp ? r[i - bpp] : 0;
        const uint8_t c = i >= bpp ? u[i - bpp] : 0;
        r[i] = static_cast<uint8_t>(r[i] - residual(type, 0, a, u[i], c));
    }
    return true;
}

#if BR_SIMD_X86
namespace {
template <int Bpp>
size_t unfilter_sub(uint8_t* r, size_t n) noexcept {
    __m128i last = _mm_setzero_si128();
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        __m128i x = _mm_add_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(r + i)),
                                   _mm_srli_si128(last, 16 - Bpp));
        x = _mm_add_epi8(x, _mm_slli_si128(x, Bpp));
        if constexpr (Bpp * 2 < 16) x = _mm_add_epi8(x, _mm_slli_si128(x, Bpp * 2));
        if constexpr (Bpp * 4 < 16) x = _mm_add_epi8(x, _mm_slli_si128(x, Bpp * 4));
        if constexpr (Bpp * 8 < 16) x = _mm_add_epi8(x, _mm_slli_si128(x, Bpp * 8));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(r + i), x);
        last = x;
    }
    return i;
}
template <int Bpp, int Type>
void unfilter_pixels(uint8_t* r, const uint8_t* u, size_t n) noexcept {
    __m128i a = _mm_setzero_si128(), c = a;
    size_t i = 0;
    for (; i + Bpp <= n; i += Bpp) {
        uint64_t rb = 0, ub = 0;
        std::memcpy(&rb, r + i, Bpp);
        std::memcpy(&ub, u + i, Bpp);
        const __m128i b = _mm_cvtsi64_si128(static_cast<long long>(ub));
        const __m128i pred = Type == 3 ? avg_floor(a, b) : paeth_pred(a, b, c);
        a = _mm_add_epi8(_mm_cvtsi64_si128(static_cast<long long>(rb)), pred);
        rb = static_cast<uint64_t>(_mm_cvtsi128_si64(a));
        std::memcpy(r + i, &rb, Bpp);
        c = b;
    }
    for (; i < n; ++i) r[i] = static_cast<uint8_t>(r[i] - residual(Type, 0, i >= Bpp ? r[i - Bpp] : 0, u[i], i >= Bpp ? u[i - Bpp] : 0));
}
}
#endif

bool unfilter_row(uint8_t type, uint8_t* r, const uint8_t* u, size_t n, size_t bpp) noexcept {
    if (type > 4 || !bpp) return false;
    if (!type) return true;
#if BR_SIMD_X86
    if (cpu_features().sse2) {
        size_t i = 0;
        if (type == 2) {
            for (; i + 16 <= n; i += 16) {
                const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r + i));
                const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(u + i));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(r + i), _mm_add_epi8(x, y));
            }
            for (; i < n; ++i) r[i] = static_cast<uint8_t>(r[i] + u[i]);
            return true;
        }
        if (type == 1) {
            switch (bpp) {
            case 1: i = unfilter_sub<1>(r, n); break;
            case 2: i = unfilter_sub<2>(r, n); break;
            case 3: i = unfilter_sub<3>(r, n); break;
            case 4: i = unfilter_sub<4>(r, n); break;
            case 6: i = unfilter_sub<6>(r, n); break;
            case 8: i = unfilter_sub<8>(r, n); break;
            default: break;
            }
            for (i = std::max(i, std::min(bpp, n)); i < n; ++i) r[i] = static_cast<uint8_t>(r[i] + r[i - bpp]);
            return true;
        }
        // PNG предсказывает байты каналов независимо, в том числе для 16-битных выборок.
        if (type == 3) {
            switch (bpp) {
            case 3: unfilter_pixels<3, 3>(r, u, n); return true;
            case 4: unfilter_pixels<4, 3>(r, u, n); return true;
            case 6: unfilter_pixels<6, 3>(r, u, n); return true;
            case 8: unfilter_pixels<8, 3>(r, u, n); return true;
            default: break;
            }
        } else {
            switch (bpp) {
            case 3: unfilter_pixels<3, 4>(r, u, n); return true;
            case 4: unfilter_pixels<4, 4>(r, u, n); return true;
            case 6: unfilter_pixels<6, 4>(r, u, n); return true;
            case 8: unfilter_pixels<8, 4>(r, u, n); return true;
            default: break;
            }
        }
    }
#endif
    return unfilter_row_scalar(type, r, u, n, bpp);
}
}
