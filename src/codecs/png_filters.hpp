#pragma once
#include "core/simd.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace br::codec::png_detail {
inline uint8_t paeth(int a, int b, int c) noexcept {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
    return static_cast<uint8_t>(pb <= pc ? b : c);
}

inline uint64_t abs_i8(uint8_t v) noexcept { return v < 128 ? v : 256u - v; }

#if BR_SIMD_X86
inline __m128i avg_floor(__m128i a, __m128i b) noexcept {
    return _mm_sub_epi8(_mm_avg_epu8(a, b), _mm_and_si128(_mm_xor_si128(a, b), _mm_set1_epi8(1)));
}
// Paeth: da=|b-c|, db=|a-c|, dc=|a+b-2c|; насыщение dc сохраняет выбор (da и db <= 255).
inline __m128i paeth_pred(__m128i a, __m128i b, __m128i c) noexcept {
    const __m128i zero = _mm_setzero_si128();
    const __m128i ac = _mm_subs_epu8(a, c), ca = _mm_subs_epu8(c, a);
    const __m128i bc = _mm_subs_epu8(b, c), cb = _mm_subs_epu8(c, b);
    const __m128i da = _mm_or_si128(bc, cb), db = _mm_or_si128(ac, ca);
    // Значения (a-c) и (b-c) имеют одинаковый знак.
    const __m128i same = _mm_or_si128(_mm_and_si128(_mm_cmpeq_epi8(ac, zero), _mm_cmpeq_epi8(bc, zero)),
                                      _mm_and_si128(_mm_cmpeq_epi8(ca, zero), _mm_cmpeq_epi8(cb, zero)));
    const __m128i dc = _mm_or_si128(_mm_and_si128(same, _mm_adds_epu8(da, db)),
                                    _mm_andnot_si128(same, _mm_sub_epi8(_mm_max_epu8(da, db), _mm_min_epu8(da, db))));
    const __m128i pick_a = _mm_cmpeq_epi8(_mm_min_epu8(_mm_min_epu8(da, db), dc), da);
    const __m128i pick_b = _mm_cmpeq_epi8(_mm_min_epu8(db, dc), db);
    const __m128i b_or_c = _mm_or_si128(_mm_and_si128(pick_b, b), _mm_andnot_si128(pick_b, c));
    return _mm_or_si128(_mm_and_si128(pick_a, a), _mm_andnot_si128(pick_a, b_or_c));
}
inline __m128i predict(int type, __m128i a, __m128i b, __m128i c) noexcept {
    switch (type) {
    case 1: return a;
    case 2: return b;
    case 3: return avg_floor(a, b);
    default: return paeth_pred(a, b, c);
    }
}
#endif

// Соседние байты для фильтра: a - слева, b - сверху, c - сверху слева.
inline uint8_t residual(int type, uint8_t x, uint8_t a, uint8_t b, uint8_t c) noexcept {
    switch (type) {
    case 0: return x;
    case 1: return static_cast<uint8_t>(x - a);
    case 2: return static_cast<uint8_t>(x - b);
    case 3: return static_cast<uint8_t>(x - ((a + b) >> 1));
    default: return static_cast<uint8_t>(x - paeth(a, b, c));
    }
}


void filter_row(int type, const uint8_t* row, const uint8_t* prev, size_t n, size_t bpp, uint8_t* out) noexcept;
size_t count_repeats(const uint8_t* row, const uint8_t* prev, size_t n, size_t bpp) noexcept;
void score_filters(const uint8_t* row, const uint8_t* prev, size_t n, size_t bpp, uint64_t score[5]) noexcept;
bool unfilter_row(uint8_t type, uint8_t* row, const uint8_t* prev, size_t n, size_t bpp) noexcept;
bool unfilter_row_scalar(uint8_t type, uint8_t* row, const uint8_t* prev, size_t n, size_t bpp) noexcept;
#if BR_SIMD_X86
void score_filters_avx2(const uint8_t* row, const uint8_t* prev, size_t n, size_t bpp, uint64_t score[5]) noexcept;
#endif
}
