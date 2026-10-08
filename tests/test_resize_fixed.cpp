#include "test_framework.hpp"
#include "resize/resize_fixed.hpp"
#include "resize/cpu_resizer.hpp"
#include "core/cpu.hpp"
#include "core/frame.hpp"
#include "core/thread_pool.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <vector>

using namespace br::resize;
namespace {
std::vector<const fixed::Kernels*> implementations() {
    std::vector<const fixed::Kernels*> out{&fixed::scalar_kernels()};
#if BR_SIMD_X86
    if (br::cpu_features().sse2) out.push_back(&fixed::sse2_kernels());
    if (br::cpu_features().ssse3) out.push_back(&fixed::ssse3_kernels());
    if (br::cpu_features().avx2) out.push_back(&fixed::avx2_kernels());
#endif
    return out;
}
struct Image {
    br_image v{};
    Image(uint32_t w, uint32_t h, br_pixel_format f) { CHECK_OK(br_image_create(w, h, f, &v)); }
    ~Image() { br_image_free(&v); }
};
int difference(const br_image& a, const br_image& b) {
    int error = 0;
    const size_t n = static_cast<size_t>(a.width) * br_pixel_format_channels(a.format);
    for (uint32_t y = 0; y < a.height; ++y)
        for (size_t x = 0; x < n; ++x)
            error = std::max(error, std::abs(int(br::row_ptr(a, y)[x]) - int(br::row_ptr(b, y)[x])));
    return error;
}
}

TEST(resize_fixed_normalization_and_bounds) {
    for (uint32_t iw : {1u, 3u, 8u, 17u, 97u, 1920u})
        for (uint32_t ow : {1u, 2u, 9u, 33u, 73u, 1568u})
            for (auto filter : {BR_FILTER_BOX, BR_FILTER_TRIANGLE, BR_FILTER_MITCHELL, BR_FILTER_LANCZOS3, BR_FILTER_LANCZOS4}) {
                const auto s = build_axis_sampler(iw, ow, filter);
                const auto q = fixed::quantize(s);
                if (!q.valid) continue;
                for (uint32_t x = 0; x < ow; ++x) {
                    const int16_t* w = q.weights.data() + static_cast<size_t>(x) * s.taps;
                    CHECK(std::accumulate(w, w + s.taps, 0) == fixed::kOne);
                    CHECK(q.max_l1 <= 2.0);
                }
            }
    auto s = build_axis_sampler(8, 7, BR_FILTER_LANCZOS3);
    s.weights[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!fixed::quantize(s).valid);
    CHECK(!fixed::quantize(build_axis_sampler(1024, 1, BR_FILTER_BOX)).valid);
    CHECK(!fixed::compatible(fixed::Axis{}, fixed::Axis{}));
}

TEST(resize_fixed_kernels_match_scalar_all_tails) {
    brt::Rng rng(61641);
    const auto impls = implementations();
    // Проверяет все количества отсчётов, включая нечётный последний и хвосты на границах векторной обработки.
    for (uint32_t taps = 1; taps <= fixed::kMaxTaps; ++taps) {
        AxisSampler s;
        s.in_size = taps + 2;
        s.out_size = 17;
        s.taps = taps;
        s.start.resize(17);
        s.weights.resize(static_cast<size_t>(17) * taps);
        for (uint32_t x = 0; x < 17; ++x) {
            s.start[x] = static_cast<int32_t>(x % 3);
            for (uint32_t k = 0; k < taps; ++k) s.weights[static_cast<size_t>(x) * taps + k] = 1.0f / taps;
            // Отрицательные отсчёты проверяют, что выбросы Q6 не обрезаются преждевременно.
            if (taps >= 3) {
                auto* w = s.weights.data() + static_cast<size_t>(x) * taps;
                std::fill(w, w + taps, 0.0f);
                w[0] = -0.125f; w[taps / 2] = 1.25f; w[taps - 1] = -0.125f;
            }
        }
        const auto q = fixed::quantize(s);
        CHECK(q.valid);
        for (uint32_t lanes : {1u, 4u}) {
            // Точный размер входа позволяет ASan обнаружить чтение за концом даже на один байт.
            std::vector<uint8_t> in(static_cast<size_t>(s.in_size) * lanes);
            for (auto& v : in) v = static_cast<uint8_t>(rng.next());
            std::vector<int16_t> ref(17 * lanes), got(17 * lanes);
            (lanes == 1 ? fixed::scalar_kernels().h1 : fixed::scalar_kernels().h4)(in.data(), ref.data(), s, q);
            for (const auto* k : impls) {
                (lanes == 1 ? k->h1 : k->h4)(in.data(), got.data(), s, q);
                CHECK(ref == got);
            }
        }
        for (size_t n : {1u, 3u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 32u, 33u, 67u}) {
            std::vector<std::vector<int16_t>> storage(taps, std::vector<int16_t>(n));
            std::vector<const int16_t*> rows(taps);
            for (uint32_t k = 0; k < taps; ++k) {
                for (auto& v : storage[k]) v = static_cast<int16_t>(int(rng.next() % 65536u) - 32768);
                rows[k] = storage[k].data();
            }
            std::vector<uint8_t> ref(n), got(n);
            fixed::scalar_kernels().v(ref.data(), n, rows.data(), q.weights.data(), taps);
            for (const auto* k : impls) {
                k->v(got.data(), n, rows.data(), q.weights.data(), taps);
                CHECK(ref == got);
            }
        }
    }
}

TEST(resize_fixed_pipeline_within_one_of_float) {
    brt::Rng rng(73321);
    CpuResizer r;
    br::ThreadPool pool(1);
    int maximum = 0;
    const std::array<std::array<uint32_t, 4>, 9> shapes{{
        {1, 1, 17, 13}, {2, 3, 19, 17}, {9, 7, 5, 3}, {17, 15, 33, 31},
        {63, 57, 47, 43}, {129, 91, 79, 67}, {191, 101, 37, 31},
        {273, 185, 167, 113}, {320, 180, 261, 147}}};
    for (auto sf : {BR_PIXEL_GRAY8, BR_PIXEL_RGB8, BR_PIXEL_BGR8, BR_PIXEL_RGBA8, BR_PIXEL_BGRA8})
        for (const auto& dims : shapes) {
            Image src(dims[0], dims[1], sf);
            const auto sc = br_pixel_format_channels(sf);
            for (size_t i = 0; i < static_cast<size_t>(src.v.stride) * src.v.height; ++i)
                src.v.data[i] = static_cast<uint8_t>(rng.next());
            CHECK_OK(br_image_set_opaque(&src.v));
            for (auto df : {BR_PIXEL_GRAY8, BR_PIXEL_RGB8, BR_PIXEL_BGR8, BR_PIXEL_RGBA8, BR_PIXEL_BGRA8}) {
                Image ref(dims[2], dims[3], df), got(dims[2], dims[3], df);
                for (auto mode : {BR_RESIZE_UI_TEXT, BR_RESIZE_FAST}) {
                    auto o = br_resize_options_for(mode);
                    for (bool negative : {false, true}) {
                        auto v = br_image_as_view(&src.v);
                        if (negative) { v.data += v.stride * (v.height - 1); v.stride = -v.stride; }
                        CHECK_OK(r.resize(v, ref.v, o, pool, false));
                        CHECK_OK(r.resize(v, got.v, o, pool, true));
                        const int e = difference(ref.v, got.v);
                        if (e > 1) std::fprintf(stderr, "fixed error=%d sf=%d df=%d %ux%u -> %ux%u mode=%d\n",e,sf,df,dims[0],dims[1],dims[2],dims[3],mode);
                        CHECK(e <= 1);
                        maximum = std::max(maximum, e);
                    }
                }
            }
            (void)sc;
        }
    std::printf("    fixed vs float maximum absolute byte error: %d\n", maximum);
}

TEST(resize_fixed_constant_and_fallbacks) {
    CpuResizer r;
    br::ThreadPool pool(1);
    Image src(87, 69, BR_PIXEL_RGBA8), ref(53, 43, BR_PIXEL_BGRA8), got(53, 43, BR_PIXEL_BGRA8);
    for (uint32_t v = 0; v < 256; ++v) {
        CHECK_OK(br_image_fill(&src.v, 0xff000000u | v * 0x010101u));
        for (auto mode : {BR_RESIZE_UI_TEXT, BR_RESIZE_FAST}) {
            auto o = br_resize_options_for(mode);
            CHECK_OK(r.resize(br_image_as_view(&src.v), got.v, o, pool));
            bool ok = true;
            for (size_t i = 0; i < static_cast<size_t>(got.v.stride) * got.v.height; ++i)
                ok &= got.v.data[i] == (i % 4 == 3 ? 255 : v);
            CHECK(ok);
        }
    }
    brt::Rng rng(91);
    for (size_t i = 0; i < static_cast<size_t>(src.v.stride) * src.v.height; ++i) src.v.data[i] = static_cast<uint8_t>(rng.next());
    for (int config = 0; config < 5; ++config) {
        auto o = br_resize_options_for(BR_RESIZE_UI_TEXT);
        auto v = br_image_as_view(&src.v);
        if (config == 1) o.linear_light = 1;
        if (config == 2) o.antiring = 1;
        if (config == 3) v.color_space = BR_COLOR_LINEAR_SRGB;
        if (config == 4) o = br_resize_options_for(BR_RESIZE_QUALITY);
        CHECK_OK(r.resize(v, ref.v, o, pool, false));
        CHECK_OK(r.resize(v, got.v, o, pool, true));
        CHECK(difference(ref.v, got.v) == 0);
    }
}

TEST(resize_fixed_padded_negative_destination_no_overwrite) {
    CpuResizer r;
    br::ThreadPool pool(1);
    Image src(67, 43, BR_PIXEL_BGRA8), ref(33, 27, BR_PIXEL_BGRA8);
    brt::Rng rng(571);
    for (size_t i = 0; i < static_cast<size_t>(src.v.stride) * src.v.height; ++i) src.v.data[i] = static_cast<uint8_t>(rng.next());
    CHECK_OK(br_image_set_opaque(&src.v));
    const size_t stride = 33 * 4 + 19;
    std::vector<uint8_t> bytes(stride * 27 + 2, 0xa5);
    br_mut_image_view dst{bytes.data() + 1 + stride * 26, 33, 27, -static_cast<ptrdiff_t>(stride), BR_PIXEL_BGRA8, BR_COLOR_SRGB, 0};
    for (auto mode : {BR_RESIZE_UI_TEXT, BR_RESIZE_FAST}) {
        auto o = br_resize_options_for(mode);
        CHECK_OK(r.resize(br_image_as_view(&src.v), dst, o, pool));
        CHECK_OK(r.resize(br_image_as_view(&src.v), ref.v, o, pool));
        CHECK(brt::images_equal(br::as_view(dst), br::as_view(ref.v)));
        CHECK(bytes.front() == 0xa5 && bytes.back() == 0xa5);
        for (uint32_t y = 0; y < 27; ++y)
            CHECK(std::all_of(bytes.begin() + 1 + y * stride + 33 * 4, bytes.begin() + 1 + (y + 1) * stride, [](uint8_t v) { return v == 0xa5; }));
    }
}
