#include "core/frame.hpp"
#include "core/common.hpp"
#include "core/cpu.hpp"
#include "core/simd.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace br {

uint32_t channels_for(br_pixel_format fmt) noexcept {
    switch (fmt) {
    case BR_PIXEL_GRAY8: return 1;
    case BR_PIXEL_RGB8:
    case BR_PIXEL_BGR8: return 3;
    case BR_PIXEL_RGBA8:
    case BR_PIXEL_BGRA8: return 4;
    default: return 0;
    }
}

bool has_alpha(br_pixel_format fmt) noexcept { return fmt == BR_PIXEL_RGBA8 || fmt == BR_PIXEL_BGRA8; }

bool Frame::allocate(uint32_t w, uint32_t h, br_pixel_format fmt) {
    const uint32_t c = channels_for(fmt);
    if (!w || !h || !c) return false;
    const uint64_t row = static_cast<uint64_t>(w) * c;
    const uint64_t total = row * h;
    if (row > static_cast<uint64_t>(std::numeric_limits<ptrdiff_t>::max()) ||
        total > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) return false;
    width = w;
    height = h;
    format = fmt;
    stride = static_cast<ptrdiff_t>(row);
    storage.assign(static_cast<size_t>(total), 0);
    return true;
}

br_image_view Frame::view() const noexcept {
    return {storage.data(), width, height, stride, format, color_space, static_cast<uint8_t>(premultiplied_alpha)};
}

br_mut_image_view Frame::mut_view() noexcept {
    return {storage.data(), width, height, stride, format, color_space, static_cast<uint8_t>(premultiplied_alpha)};
}

bool validate_image(const br_image_view& image) noexcept {
    const auto c = channels_for(image.format);
    if (!image.data || !image.width || !image.height || !c) return false;
    if (image.width > (1u << 28) || image.height > (1u << 28)) return false;
    const uint64_t min_stride = static_cast<uint64_t>(image.width) * c;
    if (image.stride == std::numeric_limits<ptrdiff_t>::min()) return false;
    const uint64_t abs_stride = image.stride < 0 ? static_cast<uint64_t>(-image.stride) : static_cast<uint64_t>(image.stride);
    const uint64_t max_offset = static_cast<uint64_t>(std::numeric_limits<ptrdiff_t>::max());
    return abs_stride >= min_stride && min_stride <= max_offset &&
        (image.height <= 1 || abs_stride <= (max_offset - min_stride) / (image.height - 1));
}

bool validate_image(const br_mut_image_view& image) noexcept { return validate_image(as_view(image)); }

br_mut_image_view alloc_image(uint32_t w, uint32_t h, br_pixel_format fmt, bool zero) {
    const uint32_t c = channels_for(fmt);
    if (!w || !h || !c) raise(BR_E_INVALID_ARGUMENT, "invalid image dimensions or pixel format");
    if (w > (1u << 28) || h > (1u << 28)) raise(BR_E_UNSUPPORTED, "image dimensions too large");
    const uint64_t row = static_cast<uint64_t>(w) * c;
    const uint64_t total = row * h;
    if (total > (uint64_t(1) << 40) || total > std::numeric_limits<size_t>::max())
        raise(BR_E_OUT_OF_MEMORY, "image too large");
    void* p = zero ? std::calloc(static_cast<size_t>(total), 1) : std::malloc(static_cast<size_t>(total));
    if (!p) raise(BR_E_OUT_OF_MEMORY, "out of memory allocating image");
    br_mut_image_view v{};
    v.data = static_cast<uint8_t*>(p);
    v.width = w;
    v.height = h;
    v.stride = static_cast<ptrdiff_t>(row);
    v.format = fmt;
    v.color_space = BR_COLOR_SRGB;
    v.premultiplied_alpha = 0;
    return v;
}

void free_image(br_mut_image_view& image) noexcept {
    std::free(image.data);
    image = br_mut_image_view{};
}

namespace {
inline uint8_t luma(uint32_t r, uint32_t g, uint32_t b) noexcept {
    // Яркость по BT.601, фиксированная точка 8.8 (сумма весов равна 256).
    return static_cast<uint8_t>((77u * r + 150u * g + 29u * b + 128u) >> 8);
}

struct Layout {
    int r, g, b, a; // Смещения байтов каналов; a = -1, если альфа отсутствует.
    uint32_t c;
};

inline Layout layout(br_pixel_format f) noexcept {
    switch (f) {
    case BR_PIXEL_RGB8: return {0, 1, 2, -1, 3};
    case BR_PIXEL_BGR8: return {2, 1, 0, -1, 3};
    case BR_PIXEL_RGBA8: return {0, 1, 2, 3, 4};
    case BR_PIXEL_BGRA8: return {2, 1, 0, 3, 4};
    case BR_PIXEL_GRAY8: return {0, 0, 0, -1, 1};
    default: return {0, 0, 0, -1, 0};
    }
}
#if BR_SIMD_X86
// Возвращает число преобразованных начальных пикселей; остаток обрабатывает вызывающий код.
uint32_t swap4_sse2(const uint8_t* s, uint8_t* d, uint32_t w) noexcept {
    const __m128i ga = _mm_set1_epi32(static_cast<int>(0xff00ff00u)), lo = _mm_set1_epi32(0xff);
    uint32_t x = 0;
    for (; x + 4 <= w; x += 4) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 4u * x));
        const __m128i r = _mm_or_si128(_mm_and_si128(v, ga),
                                       _mm_or_si128(_mm_and_si128(_mm_srli_epi32(v, 16), lo), _mm_slli_epi32(_mm_and_si128(v, lo), 16)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d + 4u * x), r);
    }
    return x;
}

BR_TARGET_SSSE3 uint32_t pack4to3_ssse3(const uint8_t* s, uint8_t* d, uint32_t w, bool swap) noexcept {
    const __m128i m = swap ? _mm_setr_epi8(2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -1, -1, -1, -1)
                           : _mm_setr_epi8(0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1);
    uint32_t x = 0;
    for (; x + 16 <= w; x += 16) {
        const __m128i* p = reinterpret_cast<const __m128i*>(s + 4u * x);
        const __m128i a = _mm_shuffle_epi8(_mm_loadu_si128(p), m);
        const __m128i b = _mm_shuffle_epi8(_mm_loadu_si128(p + 1), m);
        const __m128i c = _mm_shuffle_epi8(_mm_loadu_si128(p + 2), m);
        const __m128i e = _mm_shuffle_epi8(_mm_loadu_si128(p + 3), m);
        __m128i* q = reinterpret_cast<__m128i*>(d + 3u * x);
        _mm_storeu_si128(q, _mm_or_si128(a, _mm_slli_si128(b, 12)));
        _mm_storeu_si128(q + 1, _mm_or_si128(_mm_srli_si128(b, 4), _mm_slli_si128(c, 8)));
        _mm_storeu_si128(q + 2, _mm_or_si128(_mm_srli_si128(c, 8), _mm_slli_si128(e, 4)));
    }
    return x;
}

BR_TARGET_SSSE3 uint32_t expand3to4_ssse3(const uint8_t* s, uint8_t* d, uint32_t w, bool swap) noexcept {
    const __m128i m = swap ? _mm_setr_epi8(2, 1, 0, -1, 5, 4, 3, -1, 8, 7, 6, -1, 11, 10, 9, -1)
                           : _mm_setr_epi8(0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1);
    const __m128i alpha = _mm_set1_epi32(static_cast<int>(0xff000000u));
    uint32_t x = 0;
    for (; x + 16 <= w; x += 16) {
        const __m128i* p = reinterpret_cast<const __m128i*>(s + 3u * x);
        const __m128i i0 = _mm_loadu_si128(p), i1 = _mm_loadu_si128(p + 1), i2 = _mm_loadu_si128(p + 2);
        __m128i* q = reinterpret_cast<__m128i*>(d + 4u * x);
        _mm_storeu_si128(q, _mm_or_si128(_mm_shuffle_epi8(i0, m), alpha));
        _mm_storeu_si128(q + 1, _mm_or_si128(_mm_shuffle_epi8(_mm_alignr_epi8(i1, i0, 12), m), alpha));
        _mm_storeu_si128(q + 2, _mm_or_si128(_mm_shuffle_epi8(_mm_alignr_epi8(i2, i1, 8), m), alpha));
        _mm_storeu_si128(q + 3, _mm_or_si128(_mm_shuffle_epi8(_mm_srli_si128(i2, 4), m), alpha));
    }
    return x;
}
#endif

// Преобразует начальные пиксели с помощью SIMD, если он доступен; возвращает их количество.
uint32_t convert_row_simd(const uint8_t* s, br_pixel_format sf, uint8_t* d, br_pixel_format df, uint32_t width) noexcept {
#if BR_SIMD_X86
    if (!cpu_features().sse2) return 0;
    const bool s4 = sf == BR_PIXEL_RGBA8 || sf == BR_PIXEL_BGRA8;
    const bool d4 = df == BR_PIXEL_RGBA8 || df == BR_PIXEL_BGRA8;
    const bool s3 = sf == BR_PIXEL_RGB8 || sf == BR_PIXEL_BGR8;
    const bool d3 = df == BR_PIXEL_RGB8 || df == BR_PIXEL_BGR8;
    const bool swap = (sf == BR_PIXEL_RGBA8 || sf == BR_PIXEL_RGB8) != (df == BR_PIXEL_RGBA8 || df == BR_PIXEL_RGB8);
    if (s4 && d4 && swap) return swap4_sse2(s, d, width);
    if (!cpu_features().ssse3) return 0;
    if (s4 && d3) return pack4to3_ssse3(s, d, width, swap);
    if (s3 && d4) return expand3to4_ssse3(s, d, width, swap);
#else
    (void)s; (void)sf; (void)d; (void)df; (void)width;
#endif
    return 0;
}

}

void convert_row(const uint8_t* s, br_pixel_format sf, uint8_t* d, br_pixel_format df, uint32_t width) noexcept {
    if (sf == df) {
        std::memmove(d, s, static_cast<size_t>(width) * channels_for(sf));
        return;
    }
    const Layout sl = layout(sf), dl = layout(df);
    if (sf != BR_PIXEL_GRAY8 && df != BR_PIXEL_GRAY8) {
        const uint32_t done = convert_row_simd(s, sf, d, df, width);
        s += static_cast<size_t>(done) * sl.c;
        d += static_cast<size_t>(done) * dl.c;
        width -= done;
    }
    if (df == BR_PIXEL_GRAY8) {
        if (sf == BR_PIXEL_GRAY8) { std::memcpy(d, s, width); return; }
        for (uint32_t x = 0; x < width; ++x, s += sl.c) d[x] = luma(s[sl.r], s[sl.g], s[sl.b]);
        return;
    }
    for (uint32_t x = 0; x < width; ++x) {
        const uint8_t r = s[sl.r], g = s[sl.g], b = s[sl.b];
        const uint8_t a = sl.a >= 0 ? s[sl.a] : 255;
        d[dl.r] = r;
        d[dl.g] = g;
        d[dl.b] = b;
        if (dl.a >= 0) d[dl.a] = a;
        s += sl.c;
        d += dl.c;
    }
}

void convert_image(const br_image_view& src, const br_mut_image_view& dst) noexcept {
    const bool fix_alpha = has_alpha(src.format) && has_alpha(dst.format) &&
                           (src.premultiplied_alpha != 0) != (dst.premultiplied_alpha != 0);
    const bool unpremul_drop = has_alpha(src.format) && !has_alpha(dst.format) && src.premultiplied_alpha;
    const uint32_t dc = channels_for(dst.format);
    for (uint32_t y = 0; y < src.height; ++y) {
        uint8_t* d = row_ptr(dst, y);
        if (unpremul_drop) {
            // Убирает предварительное умножение каждого пикселя BGRA/RGBA перед удалением альфа-канала.
            const uint8_t* s = row_ptr(src, y);
            uint8_t px[4];
            for (uint32_t x = 0; x < src.width; ++x) {
                const uint32_t a = s[4 * x + 3];
                for (int ch = 0; ch < 3; ++ch)
                    px[ch] = a ? static_cast<uint8_t>(std::min<uint32_t>(255u, (s[4 * x + ch] * 255u + a / 2) / a)) : 0;
                px[3] = static_cast<uint8_t>(a);
                convert_row(px, src.format, d + static_cast<size_t>(x) * dc, dst.format, 1);
            }
            continue;
        }
        convert_row(row_ptr(src, y), src.format, d, dst.format, src.width);
        if (fix_alpha) {
            for (uint32_t x = 0; x < src.width; ++x) {
                uint8_t* p = d + 4u * x;
                const uint32_t a = p[3];
                if (dst.premultiplied_alpha) {
                    for (int ch = 0; ch < 3; ++ch) p[ch] = static_cast<uint8_t>((p[ch] * a + 127u) / 255u);
                } else {
                    for (int ch = 0; ch < 3; ++ch)
                        p[ch] = a ? static_cast<uint8_t>(std::min<uint32_t>(255u, (p[ch] * 255u + a / 2) / a)) : 0;
                }
            }
        }
    }
}

bool is_opaque(const br_image_view& image) noexcept {
    if (!has_alpha(image.format)) return true;
    for (uint32_t y = 0; y < image.height; ++y) {
        const uint8_t* row = row_ptr(image, y);
        uint32_t acc = 0xffffffffu;
        uint32_t x = 0;
#if BR_SIMD_X86
        if (cpu_features().sse2) {
        __m128i m = _mm_set1_epi32(-1);
        for (; x + 16 <= image.width; x += 16) {
            const __m128i* p = reinterpret_cast<const __m128i*>(row + 4u * x);
            m = _mm_and_si128(m, _mm_and_si128(_mm_and_si128(_mm_loadu_si128(p), _mm_loadu_si128(p + 1)),
                                               _mm_and_si128(_mm_loadu_si128(p + 2), _mm_loadu_si128(p + 3))));
        }
        m = _mm_and_si128(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(1, 0, 3, 2)));
        m = _mm_and_si128(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(2, 3, 0, 1)));
        acc = static_cast<uint32_t>(_mm_cvtsi128_si32(m));
        }
#endif
        // Обрабатывает пиксели целиком (альфа - старший байт little-endian uint32 для RGBA и BGRA).
        for (; x + 8 <= image.width; x += 8) {
            uint32_t v[8];
            std::memcpy(v, row + 4u * x, sizeof(v));
            acc &= v[0] & v[1] & v[2] & v[3] & v[4] & v[5] & v[6] & v[7];
        }
        for (; x < image.width; ++x) acc &= 0x00ffffffu | (static_cast<uint32_t>(row[4u * x + 3]) << 24);
        if ((acc >> 24) != 0xffu) return false;
    }
    return true;
}

bool clip_rect(br_rect_i32& r, uint32_t w, uint32_t h) noexcept {
    int64_t x0 = r.x, y0 = r.y;
    int64_t x1 = x0 + std::max<int64_t>(0, r.width), y1 = y0 + std::max<int64_t>(0, r.height);
    x0 = std::clamp<int64_t>(x0, 0, w);
    y0 = std::clamp<int64_t>(y0, 0, h);
    x1 = std::clamp<int64_t>(x1, 0, w);
    y1 = std::clamp<int64_t>(y1, 0, h);
    r = {static_cast<int32_t>(x0), static_cast<int32_t>(y0), static_cast<int32_t>(x1 - x0), static_cast<int32_t>(y1 - y0)};
    return r.width > 0 && r.height > 0;
}

}
