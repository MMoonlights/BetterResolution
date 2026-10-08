#include <br/br_vision.h>
#include <br/br_text.h>
#include "core/common.hpp"
#include "core/frame.hpp"
#include <algorithm>
#include <cstdint>

namespace {
constexpr uint64_t kDefaultPixels = uint64_t(2) * 1024 * 1024;
constexpr uint32_t kMaxDimension = 1u << 28;

bool valid(const br_vision_options& o) {
    return o.struct_size == sizeof(o) && o.version == BR_VISION_API_VERSION && !o.reserved &&
        o.preset >= BR_VISION_NATIVE && o.preset <= BR_VISION_ICON &&
        o.scale >= 1 && o.scale <= 8 && o.context_margin <= 256 && o.border <= 256;
}

bool empty(const br_image& image) {
    return !image.data && !image.width && !image.height && !image.stride &&
        !image.format && !image.color_space && !image.premultiplied_alpha;
}

bool fits(const br_rect_i32& region, uint32_t zoom, const br_vision_options& o) {
    const uint64_t w = uint64_t(region.width) * zoom + uint64_t(o.border) * 2;
    const uint64_t h = uint64_t(region.height) * zoom + uint64_t(o.border) * 2;
    return w <= kMaxDimension && h <= kMaxDimension &&
        (!o.max_long_edge || std::max(w, h) <= o.max_long_edge) &&
        w * h <= (o.max_pixels ? o.max_pixels : kDefaultPixels);
}
}

extern "C" {
br_vision_options br_vision_options_default(br_vision_preset preset) {
    br_vision_options o{};
    o.struct_size = sizeof(o); o.version = BR_VISION_API_VERSION; o.preset = preset;
    o.scale = preset == BR_VISION_ICON ? 4 : preset == BR_VISION_TEXT ? 2 : 1;
    o.context_margin = preset == BR_VISION_ICON ? 16 : preset == BR_VISION_TEXT ? 4 : 0;
    o.max_pixels = kDefaultPixels;
    return o;
}

br_status br_vision_plan_detail(uint32_t w, uint32_t h, br_rect_i32 region,
    const br_vision_options* options, br_vision_plan* out) {
    if (!out || !w || !h || w > kMaxDimension || h > kMaxDimension)
        return br::fail(BR_E_INVALID_ARGUMENT, "invalid vision source dimensions or null plan");
    if (options && options->struct_size != sizeof(br_vision_options))
        return br::fail(BR_E_INVALID_ARGUMENT, "vision options size mismatch");
    const auto o = options ? *options : br_vision_options_default(BR_VISION_NATIVE);
    if (!valid(o)) return br::fail(BR_E_INVALID_ARGUMENT, "invalid vision options or API version");
    if (!region.x && !region.y && !region.width && !region.height)
        region = {0, 0, int32_t(w), int32_t(h)};
    if (!br::clip_rect(region, w, h)) return br::fail(BR_E_INVALID_ARGUMENT, "empty vision source region");
    br_vision_plan plan{};
    plan.struct_size = sizeof(plan); plan.version = BR_VISION_API_VERSION;
    plan.requested_region = region;
    const int64_t margin = o.context_margin;
    const int64_t x = std::max(int64_t(0), int64_t(region.x) - margin);
    const int64_t y = std::max(int64_t(0), int64_t(region.y) - margin);
    const int64_t right = std::min(int64_t(w), int64_t(region.x) + region.width + margin);
    const int64_t bottom = std::min(int64_t(h), int64_t(region.y) + region.height + margin);
    plan.source_region = {int32_t(x), int32_t(y), int32_t(right - x), int32_t(bottom - y)};
    uint32_t zoom = o.scale;
    while (zoom > 1 && !fits(plan.source_region, zoom, o)) --zoom;
    if (!fits(plan.source_region, zoom, o))
        return br::fail(BR_E_UNSUPPORTED, "native vision detail with context/border exceeds output limits; choose a smaller ROI");
    plan.scale = zoom; plan.desired_scale = o.scale; plan.chose_limited = zoom != o.scale;
    plan.content_rect = {int32_t(o.border), int32_t(o.border), plan.source_region.width * int32_t(zoom),
                         plan.source_region.height * int32_t(zoom)};
    const double s = 1.0 / zoom;
    plan.image_to_source = {s, s, double(x) - o.border * s, double(y) - o.border * s};
    *out = plan;
    return BR_OK;
}

br_status br_vision_prepare(br_context* ctx, const br_image_view* src, br_rect_i32 region,
    const br_vision_options* options, br_image* out, br_vision_plan* out_plan) {
    if (!src || !out || !empty(*out) || !br::validate_image(*src))
        return br::fail(BR_E_INVALID_ARGUMENT, "invalid vision source or nonempty output");
    if (options && options->struct_size != sizeof(br_vision_options))
        return br::fail(BR_E_INVALID_ARGUMENT, "vision options size mismatch");
    const auto o = options ? *options : br_vision_options_default(BR_VISION_NATIVE);
    br_vision_plan plan{};
    auto status = br_vision_plan_detail(src->width, src->height, region, &o, &plan);
    if (status != BR_OK) return status;
    auto text = br_text_options_default(BR_TEXT_SCREENSHOT);
    text.width = uint32_t(plan.content_rect.width); text.height = uint32_t(plan.content_rect.height);
    text.border = o.border; text.max_pixels = o.max_pixels ? o.max_pixels : kDefaultPixels;
    auto resize = br_resize_options_for(BR_RESIZE_UI_TEXT);
    resize.filter = o.preset == BR_VISION_TEXT ? BR_FILTER_CATMULL_ROM : BR_FILTER_POINT;
    br_image image{};
    status = br_text_prepare_with_resize(ctx, src, plan.source_region, &text, &resize, &image, nullptr);
    if (status != BR_OK) return status;
    *out = image;
    if (out_plan) *out_plan = plan;
    return BR_OK;
}
}
