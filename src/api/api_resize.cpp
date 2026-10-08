#include <br/br.h>

#include "api/context.hpp"
#include "core/common.hpp"
#include "core/frame.hpp"

#include <algorithm>
#include <cmath>

using namespace br;

extern "C" {

br_resize_options br_resize_options_for(br_resize_mode mode) {
    br_resize_options o{};
    o.filter = BR_FILTER_AUTO;
    o.mode = mode;
    o.preserve_alpha = 1;
    o.antiring = 1;
    o.multistage = 1;
    o.threads = 0;
    switch (mode) {
    case BR_RESIZE_FAST: o.linear_light = 0; o.antiring = 0; break;
    case BR_RESIZE_UI_TEXT:
        // Фильтрация в гамма-пространстве сохраняет тонкие тёмные штрихи интерфейса.
        o.linear_light = 0;
        o.antiring = 0;
        break;
    case BR_RESIZE_BALANCED: o.linear_light = 1; break;
    case BR_RESIZE_QUALITY:
    default: o.linear_light = 1; break;
    }
    return o;
}

br_resize_options br_resize_options_default(void) { return br_resize_options_for(BR_RESIZE_QUALITY); }

br_status br_resize(br_context* ctx, const br_image_view* src, br_mut_image_view* dst, const br_resize_options* opt) {
    if (!src || !dst) return fail(BR_E_INVALID_ARGUMENT, "null image");
    const br_resize_options o = opt ? *opt : br_resize_options_default();
    return guarded([&] {
        br_context* c = resolve_context(ctx);
        return c->resizer.resize(*src, *dst, o, c->pool);
    });
}

br_status br_resize_to(br_context* ctx, const br_image_view* src, uint32_t w, uint32_t h, const br_resize_options* opt,
                       br_image* out) {
    if (!src || !out || !validate_image(*src)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    if (!w && !h) return fail(BR_E_INVALID_ARGUMENT, "target size is zero");
    if (!w) w = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(src->width) * h / src->height)));
    if (!h) h = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(src->height) * w / src->width)));
    return guarded([&] {
        br_mut_image_view img = alloc_image(w, h, src->format, false);
        img.color_space = src->color_space;
        img.premultiplied_alpha = src->premultiplied_alpha;
        const br_status st = br_resize(ctx, src, &img, opt);
        if (st != BR_OK) {
            free_image(img);
            return st;
        }
        *out = img;
        return BR_OK;
    });
}

br_status br_resize_fit(br_context* ctx, const br_image_view* src, const br_fit_options* fit, const br_resize_options* opt,
                        br_image* out, br_transform* out_t) {
    if (!src || !out || !validate_image(*src)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    uint32_t w = 0, h = 0;
    br_fit_dimensions(src->width, src->height, fit, &w, &h);
    const br_status st = br_resize_to(ctx, src, w, h, opt, out);
    if (st == BR_OK && out_t) {
        *out_t = br_transform_from_rects({0, 0, static_cast<int32_t>(src->width), static_cast<int32_t>(src->height)},
                                         {0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)});
    }
    return st;
}

br_status br_zoom(br_context* ctx, const br_image_view* src, br_rect_i32 region, uint32_t w, uint32_t h,
                  const br_resize_options* opt, br_image* out, br_transform* out_to_src) {
    if (!src || !out || !validate_image(*src)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    br_image_view crop;
    br_status st = br_image_crop(src, region, &crop);
    if (st != BR_OK) return st;
    if (!clip_rect(region, src->width, src->height)) return fail(BR_E_INVALID_ARGUMENT, "empty region");
    if (!w && !h) { w = crop.width; h = crop.height; }
    if (!w) w = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(crop.width) * h / crop.height)));
    if (!h) h = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(crop.height) * w / crop.width)));
    st = br_resize_to(ctx, &crop, w, h, opt, out);
    if (st == BR_OK && out_to_src)
        *out_to_src = br_transform_from_rects({0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)}, region);
    return st;
}

}
