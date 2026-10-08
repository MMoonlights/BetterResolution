#include "test_framework.hpp"
#include <br/br_automation.hpp>
#include <br/br_automation_vision.h>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
struct BridgeProvider {
    br_rect_i32 bounds{-96, 22, 6, 4};
    bool complete = true;
    static br_auto_status observe(void* u, const br_auto_observe_request*, const br_auto_operation*,
                                  br_auto_emit_fn emit, void* sink, br_auto_observe_info* info) {
        auto& p = *static_cast<BridgeProvider*>(u);
        br_auto_node n{};
        n.native_id = 1; n.incarnation = 1; n.bounds = p.bounds;
        const auto st = emit(sink, &n);
        info->complete = p.complete ? 1 : 0;
        return st;
    }
    br_auto_provider provider() {
        br_auto_provider p{}; p.struct_size = sizeof(p); p.abi_version = BR_AUTO_ABI_VERSION;
        p.name = "vision-bridge-test"; p.user = this; p.observe = &observe;
        return p;
    }
};
struct Image {
    br_image image{};
    Image() {
        CHECK_OK(br_image_create(10, 6, BR_PIXEL_RGB8, &image));
        for (uint32_t y = 0; y < image.height; ++y)
            for (uint32_t x = 0; x < image.width; ++x) {
                auto* p = image.data + ptrdiff_t(y) * image.stride + x * 3;
                p[0] = uint8_t(x * 20); p[1] = uint8_t(y * 30); p[2] = 71;
            }
    }
    ~Image() { br_image_free(&image); }
};
bool close(double a, double b) { return std::abs(a - b) < 1e-9; }
}

TEST(automation_snapshot_id_lookup_is_exact_and_reports_incomplete) {
    BridgeProvider p;
    br::AutomationSession session(p.provider());
    auto snapshot = session.observe(); const auto id = snapshot.element(0).id;
    CHECK(snapshot.find_id(id).id == id);
    br_auto_element e{};
    CHECK(br_auto_snapshot_find_id(snapshot.get(), id + 100, &e) == BR_AUTO_NOT_FOUND && !e.id);
    p.complete = false; auto partial = session.observe();
    CHECK(br_auto_snapshot_find_id(partial.get(), id, &e) == BR_AUTO_OK && e.id == id);
    CHECK(br_auto_snapshot_find_id(partial.get(), id + 100, &e) == BR_AUTO_INCOMPLETE && !e.id);
    CHECK(br_auto_snapshot_find_id(nullptr, id, &e) == BR_AUTO_INVALID_ARGUMENT);
    CHECK(br_auto_snapshot_find_id(snapshot.get(), 0, &e) == BR_AUTO_INVALID_ARGUMENT);
    CHECK(br_auto_snapshot_find_id(snapshot.get(), id, nullptr) == BR_AUTO_INVALID_ARGUMENT);
}

TEST(automation_vision_original_pixels_and_negative_screen_origin) {
    BridgeProvider p; br::AutomationSession session(p.provider()); auto snapshot = session.observe();
    const auto e = snapshot.element(0); Image original;
    const auto view = br_image_as_view(&original.image);
    const br_transform map{2, 2, -100, 20};
    br_image detail{}; br_auto_vision_plan plan{};
    CHECK_OK(br_auto_vision_prepare(nullptr, snapshot.get(), e.id, &view, &map, nullptr, &detail, &plan));
    CHECK(detail.width == 3 && detail.height == 2 && detail.format == BR_PIXEL_RGB8);
    CHECK(plan.snapshot_id == snapshot.info().id && plan.element_revision == e.revision);
    CHECK(plan.detail.requested_region.x == 2 && plan.detail.requested_region.y == 1);
    CHECK(!plan.clipped && close(plan.image_to_screen.tx, -96) && close(plan.image_to_screen.ty, 22));
    for (uint32_t y = 0; y < detail.height; ++y) {
        const auto* expected = original.image.data + ptrdiff_t(y + 1) * original.image.stride + 2 * 3;
        CHECK(std::memcmp(detail.data + ptrdiff_t(y) * detail.stride, expected, size_t(detail.width) * 3) == 0);
    }
    br_image_free(&detail);
}

TEST(automation_vision_zoom_border_and_fractional_coordinates) {
    BridgeProvider p; p.bounds = {-98, 21, 4, 3};
    br::AutomationSession session(p.provider()); auto snapshot = session.observe();
    const br_transform map{1.5, 1.5, -100, 20};
    auto options = br_vision_options_default(BR_VISION_ICON);
    options.context_margin = 1; options.border = 2; options.scale = 4;
    br_auto_vision_plan plan{};
    CHECK_OK(br_auto_vision_plan_detail(snapshot.get(), snapshot.element(0).id, 10, 6, &map, &options, &plan));
    CHECK(plan.detail.requested_region.x == 1 && plan.detail.requested_region.y == 0);
    CHECK(plan.detail.requested_region.width == 3 && plan.detail.requested_region.height == 3);
    CHECK(plan.detail.source_region.x == 0 && plan.detail.scale == 4);
    double x = 0, y = 0;
    br_transform_point(&plan.image_to_screen, 2, 2, &x, &y);
    CHECK(close(x, -100) && close(y, 20));
}

TEST(automation_vision_clipping_and_reflected_frames) {
    BridgeProvider p; p.bounds = {-103, 18, 7, 6};
    br::AutomationSession session(p.provider()); auto snapshot = session.observe();
    const br_transform map{1, 1, -100, 20}; br_auto_vision_plan plan{};
    CHECK_OK(br_auto_vision_plan_detail(snapshot.get(), snapshot.element(0).id, 10, 6, &map, nullptr, &plan));
    CHECK(plan.clipped && plan.detail.requested_region.x == 0 && plan.detail.requested_region.y == 0);
    CHECK(plan.detail.requested_region.width == 4 && plan.detail.requested_region.height == 4);
    p.bounds = {4, 1, 3, 2}; snapshot = session.observe();
    const br_transform flipped{-1, 1, 10, 0};
    CHECK_OK(br_auto_vision_plan_detail(snapshot.get(), snapshot.element(0).id, 10, 6, &flipped, nullptr, &plan));
    CHECK(plan.detail.requested_region.x == 3 && plan.detail.requested_region.width == 3);
    CHECK(close(plan.image_to_screen.sx, -1) && close(plan.image_to_screen.tx, 7));
}

TEST(automation_vision_rejects_bad_mapping_empty_and_foreign_ids_without_outputs) {
    BridgeProvider p; br::AutomationSession session(p.provider()); auto snapshot = session.observe();
    const auto id = snapshot.element(0).id; const br_transform good{2, 2, -100, 20};
    br_auto_vision_plan plan{}; plan.version = 77;
    auto bad = good; bad.sx = 0;
    CHECK(br_auto_vision_plan_detail(snapshot.get(), id, 10, 6, &bad, nullptr, &plan) == BR_E_INVALID_ARGUMENT);
    CHECK(plan.version == 77);
    bad = good; bad.tx = std::numeric_limits<double>::infinity();
    CHECK(br_auto_vision_plan_detail(snapshot.get(), id, 10, 6, &bad, nullptr, &plan) == BR_E_INVALID_ARGUMENT);
    bad = good; bad.sy = std::numeric_limits<double>::quiet_NaN();
    CHECK(br_auto_vision_plan_detail(snapshot.get(), id, 10, 6, &bad, nullptr, &plan) == BR_E_INVALID_ARGUMENT);
    br::AutomationSession other(p.provider()); auto other_snapshot = other.observe();
    CHECK(br_auto_vision_plan_detail(snapshot.get(), other_snapshot.element(0).id, 10, 6, &good, nullptr, &plan) == BR_E_NOT_FOUND);
    CHECK(br_auto_vision_plan_detail(snapshot.get(), id, 10, 6, nullptr, nullptr, &plan) == BR_E_INVALID_ARGUMENT);
    p.bounds = {1000, 1000, 5, 5}; snapshot = session.observe();
    CHECK(br_auto_vision_plan_detail(snapshot.get(), id, 10, 6, &good, nullptr, &plan) == BR_E_NOT_FOUND);
    p.bounds = {}; snapshot = session.observe();
    CHECK(br_auto_vision_plan_detail(snapshot.get(), id, 10, 6, &good, nullptr, &plan) == BR_E_INVALID_ARGUMENT);
    p.complete = false; snapshot = session.observe();
    CHECK(br_auto_vision_plan_detail(snapshot.get(), id + 100, 10, 6, &good, nullptr, &plan) == BR_E_UNSUPPORTED);
    CHECK(plan.version == 77);
}

TEST(automation_vision_negative_stride_and_failure_preserves_ownership) {
    BridgeProvider p; br::AutomationSession session(p.provider()); auto snapshot = session.observe();
    Image original; auto view = br_image_as_view(&original.image);
    view.data += ptrdiff_t(view.height - 1) * view.stride; view.stride = -view.stride;
    const br_transform map{2, 2, -100, 20}; br_auto_vision_plan plan{}; br_image detail{};
    CHECK_OK(br_auto_vision_prepare(nullptr, snapshot.get(), snapshot.element(0).id, &view, &map, nullptr, &detail, &plan));
    CHECK(detail.data[0] == 40 && detail.data[1] == 120 && detail.data[2] == 71);
    const auto owned = detail; const auto prev_plan = plan;
    CHECK(br_auto_vision_prepare(nullptr, snapshot.get(), snapshot.element(0).id, &view, &map, nullptr, &detail, &plan) == BR_E_INVALID_ARGUMENT);
    CHECK(detail.data == owned.data && plan.element_id == prev_plan.element_id);
    br_image_free(&detail);
}
