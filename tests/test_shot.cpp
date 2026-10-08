// br_shot: кэш сеансов, цепочка изменения размера и кодирования, восстановление и ожидание стабильного кадра.
// Синтетический сеанс захвата позволяет проверять эту логику на любой платформе.
#include "test_framework.hpp"

#include <br/br_shot.h>

#include "core/common.hpp"
#include "core/frame.hpp"
#include "platform/capture.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

namespace {

struct FakeStats {
    std::atomic<int> opens{0};
    std::atomic<int> grabs{0};
    int fail_grab_after{-1};   // Номер захвата (с 1 в каждом сеансе), начиная с которого grab возвращает DEVICE_LOST.
    int change_for_grabs{0};   // Кадры меняются указанное число захватов сеанса, затем остаются неподвижными.
    bool always_change{false};
    bool stale_size{false};
    bool lie_about_change{false};
    uint8_t pixel_offset{0};
};
FakeStats g_stats;

class FakeSession final : public br::capture::Session {
public:
    FakeSession(uint32_t w, uint32_t h) : w_(w), h_(h) {}
    br_capture_backend backend() const noexcept override { return BR_BACKEND_GDI_SCREEN; }
    br_status size(uint32_t& w, uint32_t& h) override {
        w = g_stats.stale_size ? w_ / 2 : w_; h = g_stats.stale_size ? h_ / 2 : h_; return BR_OK;
    }
    br_status grab(uint32_t, br_mut_image_view& img, br_frame_info& info, std::vector<br_rect_i32>& dirty) override {
        ++grabs_;
        ++g_stats.grabs;
        if (g_stats.fail_grab_after > 0 && grabs_ >= g_stats.fail_grab_after) return br::fail(BR_E_DEVICE_LOST, "fake device lost");
        if (img.data && (img.width != w_ || img.height != h_ || img.format != BR_PIXEL_BGRA8)) br::free_image(img);
        if (!img.data) img = br::alloc_image(w_, h_, BR_PIXEL_BGRA8, false);
        const bool change = g_stats.always_change || grabs_ <= g_stats.change_for_grabs || grabs_ == 1;
        if (change) ++version_;
        for (uint32_t y = 0; y < h_; ++y) {
            uint8_t* row = br::row_ptr(img, y);
            for (uint32_t x = 0; x < w_; ++x) {
                row[4 * x + 0] = static_cast<uint8_t>(x + version_ + g_stats.pixel_offset);
                row[4 * x + 1] = static_cast<uint8_t>(y);
                row[4 * x + 2] = static_cast<uint8_t>((x ^ y) & 0xff);
                row[4 * x + 3] = 255;
            }
        }
        info = {};
        info.frame_id = version_;
        info.screen_rect = {40, 30, static_cast<int32_t>(w_), static_cast<int32_t>(h_)};
        info.image_to_screen = {1.0, 1.0, 40.0, 30.0};
        info.backend = BR_BACKEND_GDI_SCREEN;
        info.changed = change && !g_stats.lie_about_change ? 1 : 0;
        dirty.clear();
        return BR_OK;
    }
    br_status next(uint32_t, const br_mut_image_view&, std::vector<br_rect_i32>&) override { return BR_E_UNSUPPORTED; }
    br_status next_d3d11(uint32_t, void**, uint32_t&, uint32_t&, std::vector<br_rect_i32>&) override { return BR_E_UNSUPPORTED; }

private:
    uint32_t w_, h_;
    int grabs_{0};
    uint64_t version_{0};
};

struct FactoryScope {
    FactoryScope() {
        g_stats.opens = 0;
        g_stats.grabs = 0;
        g_stats.fail_grab_after = -1;
        g_stats.change_for_grabs = 0;
        g_stats.always_change = false;
        g_stats.stale_size = false;
        g_stats.lie_about_change = false;
        g_stats.pixel_offset = 0;
        br_shot_cache_clear();
        br::capture::set_test_factory([](const br_capture_options& o, std::unique_ptr<br::capture::Session>& out) {
            if (o.window == 0xdead) return br::fail(BR_E_NOT_FOUND, "fake: no such window");
            ++g_stats.opens;
            out = std::make_unique<FakeSession>(200, 120);
            return BR_OK;
        });
    }
    ~FactoryScope() {
        br_shot_cache_clear();
        br::capture::set_test_factory(nullptr);
    }
};

br_shot_options base_options(uint64_t window = 1) {
    br_shot_options o = br_shot_options_default();
    o.capture.target = BR_TARGET_WINDOW;
    o.capture.window = window;
    o.fit.max_width = 100;
    o.encode = br_encode_options_default(BR_ENCODE_PNG);
    return o;
}

} 

TEST(shot_pipeline_and_cache) {
    FactoryScope scope;
    br_shot_options o = base_options();
    br_shot_result r1{}, r2{};
    CHECK_OK(br_shot(nullptr, &o, &r1));
    CHECK(r1.session_reused == 0 && r1.width == 100 && r1.height == 60 && r1.data && r1.size > 0);
    CHECK(r1.timings.total_us >= r1.timings.capture_us);
    CHECK(r1.timings.resize_us > 0 || r1.timings.capture_us > 0);
    // Пиксели результата сопоставляются рабочему столу: кадр 200x120 в точке (40,30) показан в половинном размере.
    CHECK(r1.frame.image_to_screen.sx > 1.99 && r1.frame.image_to_screen.sx < 2.01);
    CHECK(r1.frame.image_to_screen.tx > 39.9 && r1.frame.image_to_screen.tx < 40.1);
    br_image decoded{};
    CHECK_OK(br_decode(r1.data, r1.size, BR_PIXEL_BGRA8, &decoded));
    CHECK(decoded.width == 100 && decoded.height == 60);
    br_image_free(&decoded);

    CHECK_OK(br_shot(nullptr, &o, &r2));
    CHECK(r2.session_reused == 1 && r2.timings.open_us == 0);
    CHECK(g_stats.opens == 1);
    br_shot_result_free(&r1);
    br_shot_result_free(&r2);
    CHECK(r1.data == nullptr && r1.size == 0);
}

TEST(shot_cache_scope) {
    FactoryScope scope;
    br_shot_options a = base_options(1), b = base_options(2);
    br_shot_result r{};
    CHECK_OK(br_shot(nullptr, &a, &r)); br_shot_result_free(&r);
    CHECK_OK(br_shot(nullptr, &b, &r)); br_shot_result_free(&r);
    CHECK(g_stats.opens == 2); // Другая цель - другой сеанс.
    CHECK_OK(br_shot(nullptr, &a, &r));
    CHECK(r.session_reused == 1 && g_stats.opens == 2);
    br_shot_result_free(&r);

    br_shot_options nc = base_options(3);
    nc.flags = BR_SHOT_NO_CACHE;
    CHECK_OK(br_shot(nullptr, &nc, &r)); br_shot_result_free(&r);
    CHECK_OK(br_shot(nullptr, &nc, &r));
    CHECK(r.session_reused == 0 && g_stats.opens == 4);
    br_shot_result_free(&r);

    br_shot_cache_clear();
    CHECK_OK(br_shot(nullptr, &a, &r));
    CHECK(r.session_reused == 0 && g_stats.opens == 5);
    br_shot_result_free(&r);
}

TEST(shot_keep_alive_expires) {
    FactoryScope scope;
    br_shot_options o = base_options();
    o.keep_alive_ms = 1;
    br_shot_result r{};
    CHECK_OK(br_shot(nullptr, &o, &r)); br_shot_result_free(&r);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_OK(br_shot(nullptr, &o, &r));
    CHECK(r.session_reused == 0 && g_stats.opens == 2);
    br_shot_result_free(&r);
}

TEST(shot_recovers_from_stale_session) {
    FactoryScope scope;
    br_shot_options o = base_options();
    br_shot_result r{};
    CHECK_OK(br_shot(nullptr, &o, &r)); br_shot_result_free(&r);
    g_stats.fail_grab_after = 2; // Следующий захват кэшированного сеанса сообщает DEVICE_LOST; новый сеанс падает на втором захвате.
    CHECK_OK(br_shot(nullptr, &o, &r));
    CHECK(r.session_reused == 0 && g_stats.opens == 2 && r.data);
    br_shot_result_free(&r);
    // Ошибка нового сеанса возвращается вызывающему коду и не приводит к бесконечным повторам.
    g_stats.fail_grab_after = 1;
    br_shot_cache_clear();
    CHECK(br_shot(nullptr, &o, &r) == BR_E_DEVICE_LOST);
    CHECK(r.data == nullptr);
    // Ошибки открытия возвращаются без изменений.
    br_shot_options missing = base_options(0xdead);
    CHECK(br_shot(nullptr, &missing, &r) == BR_E_NOT_FOUND);
}

TEST(shot_settle_waits_for_quiet) {
    FactoryScope scope;
    br_shot_options o = base_options();
    o.settle_ms = 5;
    g_stats.change_for_grabs = 6; // Кадр меняется в течение шести опросов, затем стабилизируется.
    br_shot_result r{};
    CHECK_OK(br_shot(nullptr, &o, &r));
    CHECK(g_stats.grabs >= 6 + 1);       // Опрос продолжается после последнего изменения, затем выполняется финальный захват.
    CHECK(r.timings.settle_us >= 5000);  // Выжидает хотя бы один интервал без изменений.
    br_shot_result_free(&r);

    // Для нестабильной цели ожидание ограничено settle_timeout_ms, после чего всё равно возвращается кадр.
    br_shot_cache_clear();
    g_stats.always_change = true;
    o.settle_ms = 50;
    o.settle_timeout_ms = 30;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_OK(br_shot(nullptr, &o, &r));
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 1000 && r.data);
    br_shot_result_free(&r);
}

TEST(shot_return_image_and_validation) {
    FactoryScope scope;
    br_shot_options o = base_options();
    o.flags = BR_SHOT_RETURN_IMAGE | BR_SHOT_NO_ENCODE;
    o.fit = {};
    br_shot_result r{};
    CHECK_OK(br_shot(nullptr, &o, &r));
    CHECK(r.data == nullptr && r.image.data && r.image.width == 200 && r.image.height == 120);
    CHECK(r.timings.encode_us == 0 && r.timings.resize_us == 0);
    br_shot_result_free(&r);
    CHECK(r.image.data == nullptr);
    // При втором вызове буфер записи выделяется заново, поскольку изображение передано вызывающему коду.
    CHECK_OK(br_shot(nullptr, &o, &r));
    CHECK(r.session_reused == 1 && r.image.data);
    br_shot_result_free(&r);

    br_shot_options bad = o;
    bad.struct_size = 0;
    CHECK(br_shot(nullptr, &bad, &r) == BR_E_INVALID_ARGUMENT);
    CHECK(br_shot(nullptr, nullptr, &r) == BR_E_INVALID_ARGUMENT);
    CHECK(br_shot(nullptr, &o, nullptr) == BR_E_INVALID_ARGUMENT);
}

TEST(shot_simple_flat_entry_point) {
    FactoryScope scope;
    br_shot_result r{};
    CHECK_OK(br_shot_simple(7, 0, 100, 0, 85, 0, &r));
    CHECK(r.width == 100 && r.height == 60 && r.data && r.size > 2 && r.data[0] == 0xff && r.data[1] == 0xd8); // JPEG
    br_shot_result_free(&r);
    CHECK_OK(br_shot_simple(7, 0, 100, 0, 0, 0, &r)); // PNG, тот же сеанс из кэша.
    CHECK(r.session_reused == 1 && r.size > 8 && r.data[1] == 'P');
    br_shot_result_free(&r);
    CHECK(g_stats.opens == 1);
    CHECK(br_shot_simple(7, 0, 100, 0, 101, 0, &r) == BR_E_INVALID_ARGUMENT);
    CHECK(br_shot_simple(7, 0, 100, 0, 85, 0, nullptr) == BR_E_INVALID_ARGUMENT);
}

TEST(shot_scale_factor) {
    FactoryScope scope;
    br_shot_options o = base_options();
    o.fit = {};
    o.scale = 0.65;
    br_shot_result r{};
    CHECK_OK(br_shot(nullptr, &o, &r));
    CHECK(r.width == 130 && r.height == 78); // 200x120 * 0.65
    CHECK(r.frame.image_to_screen.sx > 1.53 && r.frame.image_to_screen.sx < 1.54);
    br_shot_result_free(&r);
    o.scale = 1.0; // Ровно 1: без пересчёта пикселей.
    CHECK_OK(br_shot(nullptr, &o, &r));
    CHECK(r.width == 200 && r.height == 120 && r.timings.resize_us == 0);
    br_shot_result_free(&r);
    o.scale = -1.0;
    CHECK(br_shot(nullptr, &o, &r) == BR_E_INVALID_ARGUMENT);
    o.scale = 9.0;
    CHECK(br_shot(nullptr, &o, &r) == BR_E_INVALID_ARGUMENT);
}

TEST(shot_flat_entry_point) {
    FactoryScope scope;
    uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t w = 0, h = 0, info[3] = {9, 9, 9};
    int32_t rect[4] = {};
    double m[4] = {};
    uint64_t t[6] = {};
    CHECK_OK(br_shot_flat(BR_TARGET_WINDOW, 5, 0, nullptr, 0.5, 0, 0, 85, 0, 0, &data, &size, &w, &h, rect, m, t, info));
    CHECK(data && size > 2 && data[0] == 0xff && data[1] == 0xd8);
    CHECK(w == 100 && h == 60 && rect[0] == 40 && rect[1] == 30 && rect[2] == 200 && rect[3] == 120);
    CHECK(m[0] > 1.99 && m[0] < 2.01 && m[2] > 39.9 && m[2] < 40.1);
    CHECK(t[5] >= t[2] && info[1] == 0);
    br_free(data);
    CHECK_OK(br_shot_flat(BR_TARGET_WINDOW, 5, 0, nullptr, 0.5, 0, 0, 0, 0, 0, &data, &size, &w, &h, nullptr, nullptr, nullptr, info));
    CHECK(info[1] == 1 && size > 8 && data[1] == 'P'); // PNG from the cached session
    br_free(data);
    CHECK_OK(br_shot_flat(BR_TARGET_WINDOW_FULL, 5, 0, nullptr, 0.0, 0, 0, 0, 0, BR_SHOT_NO_CACHE,
        &data, &size, &w, &h, nullptr, nullptr, nullptr, info));
    CHECK(data && size > 8 && data[1] == 'P');
    br_free(data);
    int32_t region[4] = {10, 20, 50, 40};
    CHECK_OK(br_shot_flat(BR_TARGET_REGION, 0, 0, region, 0.0, 0, 0, 85, 0, BR_SHOT_NO_CACHE, &data, &size, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
    br_free(data);
    CHECK(br_shot_flat(BR_TARGET_REGION, 0, 0, nullptr, 0.0, 0, 0, 85, 0, 0, &data, &size, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == BR_E_INVALID_ARGUMENT);
    CHECK(br_shot_flat(99, 0, 0, nullptr, 0.0, 0, 0, 85, 0, 0, &data, &size, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == BR_E_INVALID_ARGUMENT);
    CHECK(br_shot_flat(BR_TARGET_WINDOW, 5, 0, nullptr, 0.0, 0, 0, 85, 0, BR_SHOT_RETURN_IMAGE, &data, &size, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == BR_E_INVALID_ARGUMENT);
    CHECK(br_shot_flat(BR_TARGET_WINDOW, 5, 0, nullptr, 0.0, 0, 0, 101, 0, 0, &data, &size, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == BR_E_INVALID_ARGUMENT);
    CHECK(br_shot_flat(BR_TARGET_WINDOW, 5, 0, nullptr, 0.0, 0, 0, 85, 0, 0, nullptr, &size, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == BR_E_INVALID_ARGUMENT);
}

TEST(shot_vision_profiles_and_exact_encoding_cache) {
    FactoryScope scope;
    br_shot_options o = br_shot_options_for(BR_SHOT_VISION);
    CHECK(o.encode.format == BR_ENCODE_PNG && o.scale == 0 && o.fit.max_width == 0);
    CHECK((o.flags & BR_SHOT_REUSE_ENCODED) != 0);
    CHECK(br_shot_options_default().encode.format == BR_ENCODE_JPEG);
    CHECK(br_shot_options_for(BR_SHOT_COMPACT).encode.quality == 95);
    br_shot_result a{}, b{};
    CHECK_OK(br_shot(nullptr, &o, &a));
    CHECK(a.width == 200 && a.height == 120 && a.data[1] == 'P');
    CHECK_OK(br_shot(nullptr, &o, &b));
    CHECK(b.session_reused && b.timings.encode_us == 0 && a.size == b.size);
    CHECK(a.data != b.data && std::memcmp(a.data, b.data, a.size) == 0);
    br_shot_result_free(&b);

    // Признак отсутствия изменений от способа захвата (или тот же frame_id) не должен скрывать отличающиеся пиксели,
    // например наложенный указатель. Повторное использование определяется равенством пикселей, а не метаданными или хешем.
    g_stats.lie_about_change = true;
    g_stats.pixel_offset = 17;
    CHECK_OK(br_shot(nullptr, &o, &b));
    CHECK(a.size != b.size || std::memcmp(a.data, b.data, a.size) != 0);
    br_image decoded{};
    CHECK_OK(br_decode(b.data, b.size, BR_PIXEL_BGRA8, &decoded));
    CHECK(br::row_ptr(decoded, 0)[0] == 18);
    br_image_free(&decoded);
    br_shot_result_free(&b);

    // При смене кодировщика или размера кэш не используется; каждым результатом владеет вызывающий код.
    o.encode = br_encode_options_default(BR_ENCODE_JPEG);
    CHECK_OK(br_shot(nullptr, &o, &b));
    CHECK(b.data[0] == 0xff && b.data[1] == 0xd8);
    br_shot_result_free(&b);
    o.scale = 0.5;
    CHECK_OK(br_shot(nullptr, &o, &b));
    CHECK(b.width == 100 && b.height == 60);
    br_shot_result_free(&a);
    br_shot_result_free(&b);
    o.flags = 1u << 31;
    CHECK(br_shot(nullptr, &o, &b) == BR_E_INVALID_ARGUMENT);
}

TEST(shot_uses_actual_frame_geometry) {
    FactoryScope scope;
    g_stats.stale_size = true; // size() сообщает 100x60, а grab() возвращает новый кадр 200x120.
    br_shot_options o = br_shot_options_for(BR_SHOT_VISION);
    o.flags |= BR_SHOT_RETURN_IMAGE;
    br_shot_result r{};
    CHECK_OK(br_shot(nullptr, &o, &r));
    CHECK(r.width == 200 && r.height == 120 && r.image.width == 200);
    CHECK(r.frame.image_to_screen.sx == 1 && r.frame.image_to_screen.tx == 40);
    br_shot_result_free(&r);
    o.scale = 0.5;
    CHECK_OK(br_shot(nullptr, &o, &r));
    CHECK(r.width == 100 && r.height == 60);
    CHECK(r.frame.image_to_screen.sx == 2 && r.frame.image_to_screen.tx == 40);
    br_shot_result_free(&r);
    o.scale = 0;
    o.fit.max_width = 150;
    CHECK_OK(br_shot(nullptr, &o, &r));
    CHECK(r.width == 150 && r.height == 90);
    CHECK(r.frame.image_to_screen.sx > 1.333 && r.frame.image_to_screen.sx < 1.334);
    br_shot_result_free(&r);
}
