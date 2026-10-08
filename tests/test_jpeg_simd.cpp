#include "test_framework.hpp"
#include "codecs/jpeg_simd.hpp"
#include "core/cpu.hpp"
#include <algorithm>
#include <array>
#include <vector>

TEST(jpeg_dct_sse2_matches_scalar_coefficients) {
#if BR_SIMD_X86
    if (!br::cpu_features().sse2) return;
    brt::Rng rng(177);
    for (int trial=0; trial<5000; ++trial) {
        float input[8*13], div[64];
        for (auto& v: input) v = static_cast<float>(rng.next()%65536) / 257.0f;
        if (trial < 256) std::fill(std::begin(input),std::end(input),static_cast<float>(trial));
        for (auto& d: div) d = 1.0f / static_cast<float>(1 + rng.next()%2048);
        int16_t scalar[64], simd[64];
        br::codec::jpeg_detail::quantize_block_scalar(input,13,div,scalar);
        br::codec::jpeg_detail::quantize_block_sse2(input,13,div,simd);
        CHECK(std::equal(std::begin(scalar),std::end(scalar),std::begin(simd)));
        if (br::cpu_features().avx2) {
            br::codec::jpeg_detail::quantize_block_avx2(input,13,div,simd);
            CHECK(std::equal(std::begin(scalar),std::end(scalar),std::begin(simd)));
        }
    }
#endif
}

TEST(jpeg_ycbcr_sse2_matches_scalar_arithmetic) {
#if BR_SIMD_X86
    if (!br::cpu_features().sse2) return;
    brt::Rng rng(31);
    for (uint32_t c: {3u,4u}) for (bool bgr: {false,true}) for (uint32_t w=1; w<=129; ++w) {
        std::vector<uint8_t> bytes(c*w);
        for (auto& v: bytes) v = static_cast<uint8_t>(rng.next());
        std::vector<float> y(w), cb(w), cr(w);
        const auto done = br::codec::jpeg_detail::ycbcr_sse2(bytes.data(),w,c,bgr,y.data(),cb.data(),cr.data());
        CHECK(done <= w);
        if (br::cpu_features().avx2) {
            std::vector<float> y8(w), cb8(w), cr8(w);
            const auto done8 = br::codec::jpeg_detail::ycbcr_avx2(bytes.data(),w,c,bgr,y8.data(),cb8.data(),cr8.data());
            CHECK(done8 <= done);
            CHECK(std::equal(y8.begin(),y8.begin()+done8,y.begin()));
            CHECK(std::equal(cb8.begin(),cb8.begin()+done8,cb.begin()));
            CHECK(std::equal(cr8.begin(),cr8.begin()+done8,cr.begin()));
        }
        for (uint32_t x=0; x<done; ++x) {
            const float r=bytes[x*c+(bgr?2:0)], g=bytes[x*c+1], b=bytes[x*c+(bgr?0:2)];
            CHECK(y[x] == 0.299f*r + 0.587f*g + 0.114f*b);
            CHECK(cb[x] == -0.168735892f*r - 0.331264108f*g + 0.5f*b + 128.0f);
            CHECK(cr[x] == 0.5f*r - 0.418687589f*g - 0.081312411f*b + 128.0f);
        }
    }
#endif
}
