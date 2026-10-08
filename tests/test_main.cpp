#include "test_framework.hpp"

#include <cmath>
#include <cstring>

namespace brt {

std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
int g_failures = 0;

void check(bool ok, const char* expr, const char* file, int line) {
    if (ok) return;
    ++g_failures;
    std::fprintf(stderr, "  FAILED %s:%d: %s\n", file, line, expr);
}

void check_status(br_status st, const char* expr, const char* file, int line) {
    if (st == BR_OK) return;
    ++g_failures;
    std::fprintf(stderr, "  FAILED %s:%d: %s -> %s (%s)\n", file, line, expr, br_status_string(st), br_last_error());
}

static void put(br_image& img, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t rgba[4] = {r, g, b, a};
    br_image_view one{rgba, 1, 1, 4, BR_PIXEL_RGBA8, BR_COLOR_SRGB, 0};
    br_mut_image_view dst = img;
    dst.data = img.data + static_cast<ptrdiff_t>(y) * img.stride + static_cast<size_t>(x) * br_pixel_format_channels(img.format);
    dst.width = dst.height = 1;
    br_convert(&one, &dst);
}

br_image make_ui_image(uint32_t w, uint32_t h, br_pixel_format fmt, uint64_t seed) {
    br_image img{};
    br_image_create(w, h, fmt, &img);
    br_image_fill(&img, 0xfff6f6f6);
    Rng rng(seed);
    br_draw_rect(&img, {0, 0, static_cast<int32_t>(w), 24}, 0xffffde66, 0);
    for (int i = 0; i < 12; ++i) {
        const int32_t x = static_cast<int32_t>(rng.next() % w), y = static_cast<int32_t>(rng.next() % h);
        br_draw_rect(&img, {x, y, static_cast<int32_t>(20 + rng.next() % 200), static_cast<int32_t>(10 + rng.next() % 60)},
                     0xff000000u | (rng.next() & 0xffffff), 1);
    }
    for (uint32_t y = 30; y + 10 < h; y += 22) {
        br_draw_line(&img, 0, static_cast<int32_t>(y), static_cast<int32_t>(w) - 1, static_cast<int32_t>(y), 0xffd2d2d2);
        br_draw_text(&img, 6, static_cast<int32_t>(y) + 6, "Nomenklatura 1234.56 OK", 0xff202020, 0, 1);
    }
    
    for (int i = 0; i < 500; ++i) {
        const uint8_t v = static_cast<uint8_t>(64 + rng.next() % 160);
        put(img, rng.next() % w, rng.next() % h, v, v, static_cast<uint8_t>(v + 10), 255);
    }
    return img;
}

br_image make_photo_image(uint32_t w, uint32_t h, br_pixel_format fmt, uint64_t seed) {
    br_image rgba{};
    br_image_create(w, h, BR_PIXEL_RGBA8, &rgba);
    Rng rng(seed);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = rgba.data + static_cast<size_t>(y) * rgba.stride + 4u * x;
            const double r = 128 + 100 * std::sin(x / 37.0 + y / 53.0) + static_cast<int>(rng.next() % 13) - 6;
            const double g = 128 + 90 * std::cos(x / 71.0) + static_cast<int>(rng.next() % 13) - 6;
            const double b = 128 + 80 * std::sin(y / 29.0) + static_cast<int>(rng.next() % 13) - 6;
            p[0] = static_cast<uint8_t>(r < 0 ? 0 : r > 255 ? 255 : r);
            p[1] = static_cast<uint8_t>(g < 0 ? 0 : g > 255 ? 255 : g);
            p[2] = static_cast<uint8_t>(b < 0 ? 0 : b > 255 ? 255 : b);
            p[3] = 255;
        }
    if (fmt == BR_PIXEL_RGBA8) return rgba;
    br_image out{};
    const br_image_view v = br_image_as_view(&rgba);
    br_image_clone(&v, fmt, &out);
    br_image_free(&rgba);
    return out;
}

bool images_equal(const br_image_view& a, const br_image_view& b) {
    if (a.width != b.width || a.height != b.height || a.format != b.format) return false;
    const size_t row = static_cast<size_t>(a.width) * br_pixel_format_channels(a.format);
    for (uint32_t y = 0; y < a.height; ++y)
        if (std::memcmp(a.data + static_cast<ptrdiff_t>(y) * a.stride, b.data + static_cast<ptrdiff_t>(y) * b.stride, row)) return false;
    return true;
}

double psnr(const br_image_view& a, const br_image_view& b) {
    br_image ra{}, rb{};
    br_image_clone(&a, BR_PIXEL_RGB8, &ra);
    br_image_clone(&b, BR_PIXEL_RGB8, &rb);
    double se = 0;
    const size_t n = static_cast<size_t>(a.width) * a.height * 3;
    for (size_t i = 0; i < n; ++i) {
        const double d = static_cast<double>(ra.data[i]) - rb.data[i];
        se += d * d;
    }
    br_image_free(&ra);
    br_image_free(&rb);
    const double mse = se / static_cast<double>(n);
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

} 

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (const auto& c : brt::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        const int before = brt::g_failures;
        c.fn();
        ++run;
        std::printf("%s %s\n", brt::g_failures == before ? "[ ok ]" : "[FAIL]", c.name);
    }
    std::printf("%d tests, %d failed checks (features: %s)\n", run, brt::g_failures, br_features());
    return brt::g_failures ? 1 : 0;
}
