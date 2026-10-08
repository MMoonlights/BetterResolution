#include "test_framework.hpp"
#include "codecs/png_filters.hpp"
#include "core/cpu.hpp"
#include "resize/resize_simd.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace br::codec::png_detail;

TEST(png_filters_match_scalar_exactly) {
    brt::Rng rng(919);
    for (size_t bpp : {1u,2u,3u,4u,6u,8u}) {
        std::vector<size_t> sizes;
        for (size_t n = 0; n <= 130; ++n) sizes.push_back(n);
        sizes.insert(sizes.end(), {255*16, 255*16+19, 8193, 65537});
        for (size_t n : sizes) {
            if (n > 65537) throw std::runtime_error("invalid test row size");
            // Намеренно не выравнивает данные; после строки нет доступного для чтения заполнения.
            auto raw = std::make_unique<uint8_t[]>(n + 1);
            auto up = std::make_unique<uint8_t[]>(n + 1);
            auto filtered = std::make_unique<uint8_t[]>(n + 1);
            uint8_t* r = raw.get()+1; uint8_t* u = up.get()+1; uint8_t* f = filtered.get()+1;
            for (size_t i = 0; i < n; ++i) { r[i] = static_cast<uint8_t>(rng.next()); u[i] = static_cast<uint8_t>(rng.next()); }
            uint64_t expected_score[5]{};
            size_t expected_repeats = 0;
            for (size_t i = 0; i < n; ++i) {
                const uint8_t a = i >= bpp ? r[i-bpp] : 0;
                const uint8_t c = i >= bpp ? u[i-bpp] : 0;
                for (int t = 0; t < 5; ++t) expected_score[t] += abs_i8(residual(t,r[i],a,u[i],c));
                expected_repeats += (r[i] == u[i]) || (i >= bpp && r[i] == a);
            }
            CHECK(count_repeats(r,u,n,bpp) == expected_repeats);
            uint64_t score[5]{};
            score_filters(r,u,n,bpp,score);
            CHECK(std::equal(std::begin(score),std::end(score),std::begin(expected_score)));
            for (int t = 0; t < 5; ++t) {
                filtered[0] = 177;
                if (t) filter_row(t,r,u,n,bpp,f); else std::memcpy(f,r,n);
                std::vector<uint8_t> reference(f,f+n);
                CHECK(unfilter_row_scalar(static_cast<uint8_t>(t),reference.data(),u,n,bpp));
                CHECK(unfilter_row(static_cast<uint8_t>(t),f,u,n,bpp));
                CHECK(std::equal(f,f+n,r));
                CHECK(std::equal(reference.begin(),reference.end(),r));
                CHECK(filtered[0] == 177);
            }
        }
    }
}

TEST(png_repeat_counter_does_not_wrap_on_flat_rows) {
    std::vector<uint8_t> a(100001, 219), b(100001, 219);
    for (size_t bpp : {1u,2u,3u,4u,6u,8u}) CHECK(count_repeats(a.data(),b.data(),a.size(),bpp) == a.size());
}

TEST(scalar_dispatch_is_observable) {
    if (!br::cpu_features().sse2) {
        CHECK(!br::cpu_features().avx2);
        CHECK(std::strcmp(br::resize::simd_kernels().name, "scalar") == 0);
    }
}
