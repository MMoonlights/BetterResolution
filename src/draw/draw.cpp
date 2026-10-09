#include "draw/draw.hpp"

#include "core/common.hpp"
#include "core/frame.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace br::draw {
namespace {

#include "draw/font5x7.inc"

struct Painter {
    const br_mut_image_view& img;
    uint8_t r, g, b, a;
    int ir, ig, ib, ia; // Смещения каналов (-1 - канал отсутствует).
    uint32_t c;

    Painter(const br_mut_image_view& im, uint32_t argb) : img(im) {
        a = static_cast<uint8_t>(argb >> 24);
        r = static_cast<uint8_t>(argb >> 16);
        g = static_cast<uint8_t>(argb >> 8);
        b = static_cast<uint8_t>(argb);
        c = channels_for(im.format);
        switch (im.format) {
        case BR_PIXEL_RGBA8: ir = 0; ig = 1; ib = 2; ia = 3; break;
        case BR_PIXEL_BGRA8: ir = 2; ig = 1; ib = 0; ia = 3; break;
        case BR_PIXEL_RGB8: ir = 0; ig = 1; ib = 2; ia = -1; break;
        case BR_PIXEL_BGR8: ir = 2; ig = 1; ib = 0; ia = -1; break;
        default: ir = ig = ib = 0; ia = -1; break;
        }
    }
    static inline uint8_t mix(uint8_t dst, uint8_t src, uint32_t a) noexcept {
        return static_cast<uint8_t>((src * a + dst * (255u - a) + 127u) / 255u);
    }
    inline void put(int32_t x, int32_t y) const noexcept {
        if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= img.width || static_cast<uint32_t>(y) >= img.height || !a) return;
        uint8_t* p = row_ptr(img, static_cast<uint32_t>(y)) + static_cast<size_t>(x) * c;
        if (c == 1) {
            const uint8_t l = static_cast<uint8_t>((77u * r + 150u * g + 29u * b + 128u) >> 8);
            p[0] = mix(p[0], l, a);
            return;
        }
        p[ir] = mix(p[ir], r, a);
        p[ig] = mix(p[ig], g, a);
        p[ib] = mix(p[ib], b, a);
        if (ia >= 0) p[ia] = static_cast<uint8_t>(a + p[ia] * (255u - a) / 255u);
    }
    void fill(int32_t x0, int32_t y0, int32_t x1, int32_t y1) const noexcept { // Полуоткрытый интервал.
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min<int32_t>(x1, static_cast<int32_t>(img.width));
        y1 = std::min<int32_t>(y1, static_cast<int32_t>(img.height));
        for (int32_t y = y0; y < y1; ++y)
            for (int32_t x = x0; x < x1; ++x) put(x, y);
    }
};

const unsigned char* glyph(char ch) noexcept {
    unsigned char u = static_cast<unsigned char>(ch);
    if (u < 32 || u > 126) u = '?';
    return kFont5x7[u - 32];
}

// Округляет шаг до ближайшего значения из (1, 2, 2.5, 5) × 10^k к `target`.
double nice_step(double target) noexcept {
    if (!(target > 0)) return 1;
    const double p = std::pow(10.0, std::floor(std::log10(target)));
    const double m = target / p;
    const double n = m < 1.5 ? 1 : m < 2.25 ? 2 : m < 3.5 ? 2.5 : m < 7.5 ? 5 : 10;
    return n * p;
}

void format_number(char* buf, size_t n, double v) {
    if (std::abs(v - std::round(v)) < 1e-6) std::snprintf(buf, n, "%lld", static_cast<long long>(std::llround(v)));
    else std::snprintf(buf, n, "%.1f", v);
}

constexpr uint32_t kMarkPalette[] = {0xffe6194b, 0xff3cb44b, 0xff4363d8, 0xfff58231, 0xff911eb4,
                                     0xff008080, 0xfff032e6, 0xff9a6324, 0xff800000, 0xff000075};

}

br_status rect(const br_mut_image_view& img, br_rect_i32 r, uint32_t argb, int32_t t) {
    if (!validate_image(img)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    if (r.width <= 0 || r.height <= 0) return BR_OK;
    Painter p(img, argb);
    const int32_t x0 = r.x, y0 = r.y, x1 = r.x + r.width, y1 = r.y + r.height;
    if (t <= 0 || t * 2 >= std::min(r.width, r.height)) {
        p.fill(x0, y0, x1, y1);
        return BR_OK;
    }
    p.fill(x0, y0, x1, y0 + t);
    p.fill(x0, y1 - t, x1, y1);
    p.fill(x0, y0 + t, x0 + t, y1 - t);
    p.fill(x1 - t, y0 + t, x1, y1 - t);
    return BR_OK;
}

br_status ellipse(const br_mut_image_view& img, br_rect_i32 r, uint32_t argb, int32_t thickness) {
    if (!validate_image(img)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    if (r.width <= 0 || r.height <= 0) return BR_OK;
    const double rx = double(r.width) / 2, ry = double(r.height) / 2;
    const double cx = double(r.x) + rx, cy = double(r.y) + ry;
    const double ix = std::max(0.0, rx - thickness), iy = std::max(0.0, ry - thickness);
    const auto y0 = int32_t(std::clamp(double(r.y), 0.0, double(img.height)));
    const auto y1 = int32_t(std::clamp(double(r.y) + r.height, 0.0, double(img.height)));
    Painter p(img, argb);
    const auto span = [&](double l, double right, int32_t y) {
        const auto a = int32_t(std::clamp(std::ceil(l - 0.5), 0.0, double(img.width)));
        const auto b = int32_t(std::clamp(std::ceil(right - 0.5), 0.0, double(img.width)));
        p.fill(a, y, b, y + 1);
    };
    for (int32_t y = y0; y < y1; ++y) {
        const double dy = double(y) + 0.5 - cy;
        const double outer = rx * std::sqrt(std::max(0.0, 1 - dy * dy / (ry * ry)));
        if (thickness <= 0 || ix == 0 || iy == 0 || std::abs(dy) >= iy) span(cx - outer, cx + outer, y);
        else {
            const double inner = ix * std::sqrt(std::max(0.0, 1 - dy * dy / (iy * iy)));
            span(cx - outer, cx - inner, y); span(cx + inner, cx + outer, y);
        }
    }
    return BR_OK;
}

br_status line(const br_mut_image_view& img, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t argb) {
    if (!validate_image(img)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    Painter p(img, argb);
    const int32_t dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int32_t dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (int guard = 0; guard < (1 << 26); ++guard) {
        p.put(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        const int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    return BR_OK;
}

void measure(const char* s, uint32_t scale, uint32_t& w, uint32_t& h) noexcept {
    scale = std::max(1u, scale);
    uint32_t lines = 0, cur = 0, best = 0;
    if (s && *s) lines = 1;
    for (const char* q = s; q && *q; ++q) {
        if (*q == '\n') { best = std::max(best, cur); cur = 0; ++lines; continue; }
        ++cur;
    }
    best = std::max(best, cur);
    w = best ? (best * 6 - 1) * scale : 0;
    h = lines ? (lines * 8 - 1) * scale : 0;
}

br_status text(const br_mut_image_view& img, int32_t x, int32_t y, const char* s, uint32_t argb, uint32_t bg, uint32_t scale) {
    if (!validate_image(img)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    if (!s) return fail(BR_E_INVALID_ARGUMENT, "null text");
    scale = std::clamp(scale, 1u, 64u);
    const int32_t sc = static_cast<int32_t>(scale);
    if (bg >> 24) {
        uint32_t w, h;
        measure(s, scale, w, h);
        Painter(img, bg).fill(x - sc, y - sc, x + static_cast<int32_t>(w) + sc, y + static_cast<int32_t>(h) + sc);
    }
    Painter p(img, argb);
    int32_t cx = x, cy = y;
    for (const char* q = s; *q; ++q) {
        if (*q == '\n') { cx = x; cy += 8 * sc; continue; }
        const unsigned char* gl = glyph(*q);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (gl[row] & (0x10 >> col)) p.fill(cx + col * sc, cy + row * sc, cx + (col + 1) * sc, cy + (row + 1) * sc);
        cx += 6 * sc;
    }
    return BR_OK;
}

br_grid_options default_grid() noexcept {
    br_grid_options o{};
    o.step = 0;
    o.line_argb = 0x66ff0000u;
    o.label_argb = 0xffffffffu;
    o.label_background_argb = 0xccc00000u;
    o.label_scale = 0;
    o.labels = 1;
    o.label_transform = {1.0, 1.0, 0.0, 0.0};
    return o;
}

br_status grid(const br_mut_image_view& img, const br_grid_options& o) {
    if (!validate_image(img)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    br_transform t = o.label_transform;
    if (t.sx == 0.0) t.sx = 1.0;
    if (t.sy == 0.0) t.sy = 1.0;
    const double W = img.width, H = img.height;
    // Шаг в единицах подписей.
    double step_x, step_y;
    if (o.step) {
        step_x = o.step * std::abs(t.sx);
        step_y = o.step * std::abs(t.sy);
    } else {
        const double target_px = std::clamp(std::max(W, H) / 12.0, 40.0, 200.0);
        step_x = nice_step(target_px * std::abs(t.sx));
        step_y = nice_step(target_px * std::abs(t.sy));
        const double s = std::max(step_x, step_y);
        step_x = step_y = s;
    }
    const uint32_t scale = o.label_scale ? o.label_scale : (std::max(W, H) >= 2400 ? 2u : 1u);
    Painter lp(img, o.line_argb);
    char buf[32];

    // Вертикальные линии: значение подписи v -> координата x изображения = (v - tx) / sx.
    const double lx0 = std::min(t.tx, W * t.sx + t.tx), lx1 = std::max(t.tx, W * t.sx + t.tx);
    for (double v = std::ceil(lx0 / step_x) * step_x; v <= lx1; v += step_x) {
        const int32_t x = static_cast<int32_t>(std::lround((v - t.tx) / t.sx));
        if (x < 0 || x >= static_cast<int32_t>(img.width)) continue;
        lp.fill(x, 0, x + 1, static_cast<int32_t>(img.height));
        if (o.labels) {
            format_number(buf, sizeof(buf), v);
            text(img, x + 2 * static_cast<int32_t>(scale), static_cast<int32_t>(scale), buf, o.label_argb, o.label_background_argb, scale);
        }
    }
    const double ly0 = std::min(t.ty, H * t.sy + t.ty), ly1 = std::max(t.ty, H * t.sy + t.ty);
    for (double v = std::ceil(ly0 / step_y) * step_y; v <= ly1; v += step_y) {
        const int32_t y = static_cast<int32_t>(std::lround((v - t.ty) / t.sy));
        if (y < 0 || y >= static_cast<int32_t>(img.height)) continue;
        lp.fill(0, y, static_cast<int32_t>(img.width), y + 1);
        if (o.labels && y > 10 * static_cast<int32_t>(scale)) {
            format_number(buf, sizeof(buf), v);
            text(img, static_cast<int32_t>(scale), y + 2 * static_cast<int32_t>(scale), buf, o.label_argb, o.label_background_argb, scale);
        }
    }
    return BR_OK;
}

br_status marks(const br_mut_image_view& img, const br_rect_i32* rects, size_t n, uint32_t first, uint32_t argb) {
    if (!validate_image(img)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    if (n && !rects) return fail(BR_E_INVALID_ARGUMENT, "null rects");
    const uint32_t scale = std::max(img.width, img.height) >= 2400 ? 2u : 1u;
    char buf[16];
    for (size_t i = 0; i < n; ++i) {
        const uint32_t color = argb ? argb : kMarkPalette[i % (sizeof(kMarkPalette) / sizeof(kMarkPalette[0]))];
        rect(img, rects[i], color, 2);
        std::snprintf(buf, sizeof(buf), "%u", first + static_cast<uint32_t>(i));
        uint32_t w, h;
        measure(buf, scale, w, h);
        int32_t lx = rects[i].x, ly = rects[i].y - static_cast<int32_t>(h) - 2 * static_cast<int32_t>(scale);
        if (ly < 0) ly = rects[i].y + 2 * static_cast<int32_t>(scale);
        text(img, lx + static_cast<int32_t>(scale), ly + static_cast<int32_t>(scale), buf, 0xffffffffu, color | 0xff000000u, scale);
    }
    return BR_OK;
}

}
