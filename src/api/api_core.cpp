#include <br/br.h>
#include "build_id.hpp"

#include "api/context.hpp"
#include "core/common.hpp"
#include "core/cpu.hpp"
#include "core/frame.hpp"
#include "platform/capture.hpp"
#include "resize/resize_simd.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace br {

br_context* resolve_context(br_context* c) {
    if (c) return c;
    // Намеренно не освобождается: это исключает ожидание рабочих потоков при статическом уничтожении.
    static br_context* global = new br_context();
    return global;
}

}

using namespace br;

extern "C" {

const char* br_version_string(void) { return BR_VERSION_STRING; }

uint32_t br_version(void) { return (BR_VERSION_MAJOR << 16) | (BR_VERSION_MINOR << 8) | BR_VERSION_PATCH; }

const char* br_status_string(br_status s) {
    switch (s) {
    case BR_OK: return "ok";
    case BR_E_INVALID_ARGUMENT: return "invalid argument";
    case BR_E_OUT_OF_MEMORY: return "out of memory";
    case BR_E_UNSUPPORTED: return "unsupported";
    case BR_E_NOT_FOUND: return "not found";
    case BR_E_TIMEOUT: return "timeout";
    case BR_E_DEVICE_LOST: return "device lost";
    case BR_E_IO: return "io error";
    case BR_E_BUFFER_TOO_SMALL: return "buffer too small";
    case BR_E_DECODE: return "corrupt or unsupported data";
    default: return "internal error";
    }
}

const char* br_last_error(void) { return br::last_error(); }

const char* br_build_id(void) { return BR_BUILD_ID; }

const char* br_features(void) {
    static const std::string f = [] {
        std::string s = resize::simd_kernels().name;
        s += ",threads,text-v1,text-resize-policy,detail-tiles,vision-detail-v1,vision-shot,exact-encode-cache";
#if defined(BR_HAS_AUTOMATION)
        s += ",automation-v1,automation-providers,automation-target-read,automation-vision,automation-select-named";
#endif
#if defined(BR_HAS_WINDOWS_AUTOMATION)
        s += ",automation-windows-uia,automation-windows-isolated";
#endif
        const char* cap = capture::features();
        if (cap && *cap) { s += ","; s += cap; }
        return s;
    }();
    return f.c_str();
}

void* br_alloc(size_t size) { return std::malloc(size ? size : 1); }
void br_free(void* p) { std::free(p); }

br_status br_context_create(br_context** out) {
    if (!out) return fail(BR_E_INVALID_ARGUMENT, "null output");
    *out = nullptr;
    return guarded([&] {
        *out = new br_context();
        return BR_OK;
    });
}

void br_context_destroy(br_context* c) { delete c; }

br_status br_context_set_threads(br_context* c, uint32_t threads) {
    return guarded([&] {
        resolve_context(c)->pool.set_threads(threads);
        return BR_OK;
    });
}

uint32_t br_pixel_format_channels(br_pixel_format f) { return channels_for(f); }

br_status br_image_create(uint32_t w, uint32_t h, br_pixel_format f, br_image* out) {
    if (!out) return fail(BR_E_INVALID_ARGUMENT, "null output");
    return guarded([&] {
        *out = alloc_image(w, h, f, true);
        return BR_OK;
    });
}

void br_image_free(br_image* img) {
    if (img) free_image(*img);
}

br_image_view br_image_as_view(const br_mut_image_view* img) {
    return img ? as_view(*img) : br_image_view{};
}

br_status br_image_clone(const br_image_view* src, br_pixel_format f, br_image* out) {
    if (!src || !out || !validate_image(*src)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    return guarded([&] {
        br_mut_image_view img = alloc_image(src->width, src->height, f == BR_PIXEL_UNKNOWN ? src->format : f, false);
        img.color_space = src->color_space;
        img.premultiplied_alpha = has_alpha(img.format) ? src->premultiplied_alpha : 0;
        convert_image(*src, img);
        *out = img;
        return BR_OK;
    });
}

br_status br_convert(const br_image_view* src, br_mut_image_view* dst) {
    if (!src || !dst || !validate_image(*src) || !validate_image(*dst)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    if (src->width != dst->width || src->height != dst->height) return fail(BR_E_INVALID_ARGUMENT, "size mismatch");
    convert_image(*src, *dst);
    return BR_OK;
}

br_status br_image_crop(const br_image_view* src, br_rect_i32 r, br_image_view* out) {
    if (!src || !out || !validate_image(*src)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    if (!clip_rect(r, src->width, src->height)) return fail(BR_E_INVALID_ARGUMENT, "crop rectangle is outside the image");
    *out = *src;
    out->data = row_ptr(*src, static_cast<uint32_t>(r.y)) + static_cast<size_t>(r.x) * channels_for(src->format);
    out->width = static_cast<uint32_t>(r.width);
    out->height = static_cast<uint32_t>(r.height);
    return BR_OK;
}

br_status br_image_crop_mut(br_mut_image_view* src, br_rect_i32 r, br_mut_image_view* out) {
    if (!src || !out || !validate_image(*src)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    if (!clip_rect(r, src->width, src->height)) return fail(BR_E_INVALID_ARGUMENT, "crop rectangle is outside the image");
    *out = *src;
    out->data = row_ptr(*src, static_cast<uint32_t>(r.y)) + static_cast<size_t>(r.x) * channels_for(src->format);
    out->width = static_cast<uint32_t>(r.width);
    out->height = static_cast<uint32_t>(r.height);
    return BR_OK;
}

br_status br_image_fill(br_mut_image_view* img, uint32_t argb) {
    if (!img || !validate_image(*img)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    const uint8_t px[4] = {uint8_t(argb >> 16), uint8_t(argb >> 8), uint8_t(argb), uint8_t(argb >> 24)};
    const uint32_t c = channels_for(img->format);
    uint8_t conv[4];
    convert_row(px, BR_PIXEL_RGBA8, conv, img->format, 1);
    for (uint32_t y = 0; y < img->height; ++y) {
        uint8_t* p = row_ptr(*img, y);
        for (uint32_t x = 0; x < img->width; ++x) std::memcpy(p + static_cast<size_t>(x) * c, conv, c);
    }
    return BR_OK;
}

br_status br_image_set_opaque(br_mut_image_view* img) {
    if (!img || !validate_image(*img)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    if (!has_alpha(img->format)) return BR_OK;
    for (uint32_t y = 0; y < img->height; ++y) {
        uint8_t* p = row_ptr(*img, y) + 3;
        for (uint32_t x = 0; x < img->width; ++x) p[4u * x] = 255;
    }
    return BR_OK;
}


br_transform br_transform_identity(void) { return {1.0, 1.0, 0.0, 0.0}; }

br_transform br_transform_from_rects(br_rect_i32 s, br_rect_i32 d) {
    if (s.width == 0 || s.height == 0) return {1.0, 1.0, 0.0, 0.0};
    const double sx = static_cast<double>(d.width) / s.width;
    const double sy = static_cast<double>(d.height) / s.height;
    return {sx, sy, d.x - s.x * sx, d.y - s.y * sy};
}

void br_transform_point(const br_transform* t, double x, double y, double* ox, double* oy) {
    if (!t) return;
    if (ox) *ox = x * t->sx + t->tx;
    if (oy) *oy = y * t->sy + t->ty;
}

br_transform br_transform_inverse(br_transform t) {
    if (t.sx == 0.0 || t.sy == 0.0) return {1.0, 1.0, 0.0, 0.0};
    return {1.0 / t.sx, 1.0 / t.sy, -t.tx / t.sx, -t.ty / t.sy};
}

br_transform br_transform_compose(br_transform a, br_transform b) {
    return {a.sx * b.sx, a.sy * b.sy, a.tx * b.sx + b.tx, a.ty * b.sy + b.ty};
}

br_rect_i32 br_transform_rect(const br_transform* t, br_rect_i32 r) {
    if (!t) return r;
    const double x0 = r.x * t->sx + t->tx, x1 = (r.x + r.width) * t->sx + t->tx;
    const double y0 = r.y * t->sy + t->ty, y1 = (r.y + r.height) * t->sy + t->ty;
    const double lx = std::floor(std::min(x0, x1) + 1e-9), hx = std::ceil(std::max(x0, x1) - 1e-9);
    const double ly = std::floor(std::min(y0, y1) + 1e-9), hy = std::ceil(std::max(y0, y1) - 1e-9);
    return {static_cast<int32_t>(lx), static_cast<int32_t>(ly), static_cast<int32_t>(hx - lx), static_cast<int32_t>(hy - ly)};
}

void br_fit_dimensions(uint32_t w, uint32_t h, const br_fit_options* f, uint32_t* ow, uint32_t* oh) {
    if (!w || !h) {
        if (ow) *ow = 0;
        if (oh) *oh = 0;
        return;
    }
    br_fit_options o = f ? *f : br_fit_options{};
    double s = o.allow_upscale ? 1e30 : 1.0;
    bool constrained = false;
    auto lim = [&](double v) { s = std::min(s, v); constrained = true; };
    if (o.max_width) lim(static_cast<double>(o.max_width) / w);
    if (o.max_height) lim(static_cast<double>(o.max_height) / h);
    if (o.max_long_edge) lim(static_cast<double>(o.max_long_edge) / std::max(w, h));
    if (o.max_short_edge) lim(static_cast<double>(o.max_short_edge) / std::min(w, h));
    if (o.max_pixels) lim(std::sqrt(static_cast<double>(o.max_pixels) / (static_cast<double>(w) * h)));
    if (!constrained) s = 1.0;
    uint32_t rw = static_cast<uint32_t>(std::max(1.0, std::floor(w * s + 1e-7)));
    uint32_t rh = static_cast<uint32_t>(std::max(1.0, std::floor(h * s + 1e-7)));
    if (s == 1.0) { rw = w; rh = h; }
    if (o.max_pixels) {
        while (static_cast<uint64_t>(rw) * rh > o.max_pixels && (rw > 1 || rh > 1)) {
            if (rw >= rh && rw > 1) --rw; else if (rh > 1) --rh;
        }
    }
    if (o.multiple_of > 1) {
        const uint32_t m = o.multiple_of;
        rw = std::max(m, rw / m * m);
        rh = std::max(m, rh / m * m);
    }
    if (ow) *ow = rw;
    if (oh) *oh = rh;
}

}
