#include "codecs/png_filters.hpp"
#if BR_SIMD_X86
namespace br::codec::png_detail {
namespace {
BR_TARGET_AVX2 inline __m256i avg_floor256(__m256i a, __m256i b) noexcept {
    return _mm256_sub_epi8(_mm256_avg_epu8(a, b), _mm256_and_si256(_mm256_xor_si256(a, b), _mm256_set1_epi8(1)));
}
// Paeth: da=|b-c|, db=|a-c|, dc=|a+b-2c|; насыщение dc сохраняет выбор (da и db <= 255).
BR_TARGET_AVX2 inline __m256i paeth_pred256(__m256i a, __m256i b, __m256i c) noexcept {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i ac = _mm256_subs_epu8(a, c), ca = _mm256_subs_epu8(c, a);
    const __m256i bc = _mm256_subs_epu8(b, c), cb = _mm256_subs_epu8(c, b);
    const __m256i da = _mm256_or_si256(bc, cb), db = _mm256_or_si256(ac, ca);
    // Значения (a-c) и (b-c) имеют одинаковый знак.
    const __m256i same = _mm256_or_si256(_mm256_and_si256(_mm256_cmpeq_epi8(ac, zero), _mm256_cmpeq_epi8(bc, zero)),
                                      _mm256_and_si256(_mm256_cmpeq_epi8(ca, zero), _mm256_cmpeq_epi8(cb, zero)));
    const __m256i dc = _mm256_or_si256(_mm256_and_si256(same, _mm256_adds_epu8(da, db)),
                                    _mm256_andnot_si256(same, _mm256_sub_epi8(_mm256_max_epu8(da, db), _mm256_min_epu8(da, db))));
    const __m256i pick_a = _mm256_cmpeq_epi8(_mm256_min_epu8(_mm256_min_epu8(da, db), dc), da);
    const __m256i pick_b = _mm256_cmpeq_epi8(_mm256_min_epu8(db, dc), db);
    const __m256i b_or_c = _mm256_or_si256(_mm256_and_si256(pick_b, b), _mm256_andnot_si256(pick_b, c));
    return _mm256_or_si256(_mm256_and_si256(pick_a, a), _mm256_andnot_si256(pick_a, b_or_c));
}

BR_TARGET_AVX2 inline __m256i absum(__m256i v) noexcept {
    const __m256i zero = _mm256_setzero_si256();
    return _mm256_sad_epu8(_mm256_min_epu8(v, _mm256_sub_epi8(zero, v)), zero);
}
}
BR_TARGET_AVX2 void score_filters_avx2(const uint8_t* row, const uint8_t* prev, size_t n, size_t bpp, uint64_t score[5]) noexcept {
    for (int t = 0; t < 5; ++t) score[t] = 0;
    size_t i = 0;
    for (; i < std::min(bpp, n); ++i)
        for (int t = 0; t < 5; ++t) score[t] += abs_i8(residual(t, row[i], 0, prev[i], 0));
    const __m256i zero = _mm256_setzero_si256();
    __m256i acc[5] = {zero, zero, zero, zero, zero};
    for (; i + 32 <= n; i += 32) {
        const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row + i));
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row + i - bpp));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(prev + i));
        const __m256i c = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(prev + i - bpp));
        acc[0] = _mm256_add_epi64(acc[0], absum(x));
        acc[1] = _mm256_add_epi64(acc[1], absum(_mm256_sub_epi8(x, a)));
        acc[2] = _mm256_add_epi64(acc[2], absum(_mm256_sub_epi8(x, b)));
        acc[3] = _mm256_add_epi64(acc[3], absum(_mm256_sub_epi8(x, avg_floor256(a, b))));
        acc[4] = _mm256_add_epi64(acc[4], absum(_mm256_sub_epi8(x, paeth_pred256(a, b, c))));
    }

    for (int t = 0; t < 5; ++t) {
        uint64_t lanes[4];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(lanes), acc[t]);
        score[t] += lanes[0] + lanes[1] + lanes[2] + lanes[3];
    }
    for (; i < n; ++i)
        for (int t = 0; t < 5; ++t) score[t] += abs_i8(residual(t, row[i], row[i - bpp], prev[i], prev[i - bpp]));
}
}
#endif
