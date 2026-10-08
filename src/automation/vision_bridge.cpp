#include <br/br_automation_vision.h>
#include "core/common.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {
br_status make_plan(const br_auto_snapshot* snapshot, uint64_t id, uint32_t width, uint32_t height,
                    const br_transform* map, const br_vision_options* options, br_auto_vision_plan& plan) {
    if (!snapshot || !id || !map || !width || !height || width > INT32_MAX || height > INT32_MAX ||
        !std::isfinite(map->sx) || !std::isfinite(map->sy) || !std::isfinite(map->tx) || !std::isfinite(map->ty) ||
        map->sx == 0 || map->sy == 0)
        return br::fail(BR_E_INVALID_ARGUMENT, "invalid element detail arguments or coordinate map");
    br_auto_element e{};
    const auto found = br_auto_snapshot_find_id(snapshot, id, &e);
    if (found == BR_AUTO_NOT_FOUND) return br::fail(BR_E_NOT_FOUND, "element is absent from snapshot");
    if (found == BR_AUTO_INCOMPLETE) return br::fail(BR_E_UNSUPPORTED, "incomplete snapshot cannot prove element absence");
    if (found != BR_AUTO_OK) return br::fail(BR_E_INVALID_ARGUMENT, "invalid snapshot or element ID");
    if (e.bounds.width <= 0 || e.bounds.height <= 0)
        return br::fail(BR_E_INVALID_ARGUMENT, "element has no nonempty bounds");

    const double x0 = (double(e.bounds.x) - map->tx) / map->sx;
    const double y0 = (double(e.bounds.y) - map->ty) / map->sy;
    const double x1 = (double(int64_t(e.bounds.x) + e.bounds.width) - map->tx) / map->sx;
    const double y1 = (double(int64_t(e.bounds.y) + e.bounds.height) - map->ty) / map->sy;
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1))
        return br::fail(BR_E_INVALID_ARGUMENT, "coordinate map exceeds finite range");
    const double left = std::floor(std::min(x0, x1)), top = std::floor(std::min(y0, y1));
    const double right = std::ceil(std::max(x0, x1)), bottom = std::ceil(std::max(y0, y1));
    const double l = std::max(0.0, left), t = std::max(0.0, top);
    const double r = std::min(double(width), right), b = std::min(double(height), bottom);
    if (l >= r || t >= b) return br::fail(BR_E_NOT_FOUND, "element is outside the supplied frame");
    const br_rect_i32 region{int32_t(l), int32_t(t), int32_t(r - l), int32_t(b - t)};
    br_vision_plan detail{};
    const auto st = br_vision_plan_detail(width, height, region, options, &detail);
    if (st != BR_OK) return st;
    plan = {};
    plan.struct_size = sizeof(plan); plan.version = BR_AUTO_VISION_VERSION;
    plan.snapshot_id = br_auto_snapshot_get_info(snapshot).id;
    plan.element_id = e.id; plan.element_revision = e.revision; plan.screen_bounds = e.bounds;
    plan.detail = detail;
    plan.image_to_screen = br_transform_compose(detail.image_to_source, *map);
    if (!std::isfinite(plan.image_to_screen.sx) || !std::isfinite(plan.image_to_screen.sy) ||
        !std::isfinite(plan.image_to_screen.tx) || !std::isfinite(plan.image_to_screen.ty))
        return br::fail(BR_E_INVALID_ARGUMENT, "detail coordinate map exceeds finite range");
    plan.clipped = l != left || t != top || r != right || b != bottom;
    return BR_OK;
}
}

extern "C" {
br_status br_auto_vision_plan_detail(const br_auto_snapshot* snapshot, uint64_t id, uint32_t width, uint32_t height,
                                   const br_transform* map, const br_vision_options* options, br_auto_vision_plan* out) {
    if (!out) return br::fail(BR_E_INVALID_ARGUMENT, "null detail plan");
    br_auto_vision_plan plan{};
    const auto st = make_plan(snapshot, id, width, height, map, options, plan);
    if (st == BR_OK) *out = plan;
    return st;
}
br_status br_auto_vision_prepare(br_context* context, const br_auto_snapshot* snapshot, uint64_t id,
                                const br_image_view* original, const br_transform* map,
                                const br_vision_options* options, br_image* image, br_auto_vision_plan* out) {
    if (!original || !image) return br::fail(BR_E_INVALID_ARGUMENT, "null original image or detail output");
    br_auto_vision_plan plan{};
    const auto st = make_plan(snapshot, id, original->width, original->height, map, options, plan);
    if (st != BR_OK) return st;
    const auto prepared = br_vision_prepare(context, original, plan.detail.requested_region, options, image, nullptr);
    if (prepared == BR_OK && out) *out = plan;
    return prepared;
}
}
