#include "test_framework.hpp"

#include <algorithm>
#include <array>
#include <atomic>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

TEST(version_and_errors) {
    CHECK(br_version() == ((BR_VERSION_MAJOR << 16) | (BR_VERSION_MINOR << 8) | BR_VERSION_PATCH));
    CHECK(std::strcmp(br_version_string(), BR_VERSION_STRING) == 0);
    br_image img{};
    CHECK(br_image_create(0, 10, BR_PIXEL_RGB8, &img) == BR_E_INVALID_ARGUMENT);
    CHECK(std::strlen(br_last_error()) > 0);
    CHECK(br_resize(nullptr, nullptr, nullptr, nullptr) == BR_E_INVALID_ARGUMENT);
    CHECK(br_features() && std::strlen(br_features()) > 0);
}

TEST(crop_clone_convert) {
    br_image img = brt::make_photo_image(100, 80, BR_PIXEL_RGBA8, 6);
    const br_image_view v = br_image_as_view(&img);
    br_image_view c{};
    CHECK_OK(br_image_crop(&v, {90, 70, 50, 50}, &c));
    CHECK(c.width == 10 && c.height == 10 && c.data == img.data + 70 * img.stride + 90 * 4);
    CHECK(br_image_crop(&v, {200, 200, 5, 5}, &c) != BR_OK);
    br_image gray{};
    CHECK_OK(br_image_clone(&v, BR_PIXEL_GRAY8, &gray));
    CHECK(gray.format == BR_PIXEL_GRAY8 && gray.stride == 100);
    br_image_free(&gray);
    br_image_free(&img);
}

TEST(load_save_files) {
    br_image img = brt::make_ui_image(120, 90, BR_PIXEL_RGB8, 13);
    const br_image_view v = br_image_as_view(&img);
    for (const char* ext : {".png", ".jpg", ".bmp", ".qoi", ".ppm"}) {
        const std::string path = std::string("br_test_tmp") + ext;
        CHECK_OK(br_save(path.c_str(), &v, nullptr));
        br_image back{};
        CHECK_OK(br_load(path.c_str(), BR_PIXEL_RGB8, &back));
        CHECK(back.width == 120 && back.height == 90);
        const br_image_view bv = br_image_as_view(&back);
        if (std::strcmp(ext, ".jpg") != 0) CHECK(brt::images_equal(v, bv));
        else CHECK(brt::psnr(v, bv) > 30);
        br_image_free(&back);
        std::remove(path.c_str());
    }
    br_image missing{};
    CHECK(br_load("definitely/not/here.png", BR_PIXEL_UNKNOWN, &missing) == BR_E_IO);
    br_image_free(&img);
}

TEST(capture_api_is_safe_everywhere) {
    size_t n = 0;
    const br_status st = br_monitors(nullptr, 0, &n);
#if defined(_WIN32)
    (void)st;
#else
    CHECK(st == BR_E_UNSUPPORTED);
#endif
    br_capture* cap = nullptr;
    br_capture_options o = br_capture_options_default();
    if (br_capture_open(nullptr, &o, &cap) == BR_OK) br_capture_destroy(cap);
}

TEST(default_context_resize_during_thread_reconfiguration) {
    // Публичный контекст nullptr общий и потокобезопасный. Для явных контекстов сохраняется
    // прежнее требование одного вызывающего потока. Захват и системный интерфейс не используются.
    constexpr uint32_t callers = 4, iterations = 40, changes = 96;
    struct Images {
        std::array<br_image, 3 + callers> values{};
        ~Images() { for (auto& image : values) br_image_free(&image); }
    } images;
    images.values[0] = brt::make_ui_image(512, 384, BR_PIXEL_BGRA8, 881032);
    const auto source = br_image_as_view(&images.values[0]);
    CHECK_OK(br_context_set_threads(nullptr, 1));
    const std::array<br_resize_mode, 2> modes{BR_RESIZE_UI_TEXT, BR_RESIZE_QUALITY};
    for (size_t index = 0; index < modes.size(); ++index) {
        auto options = br_resize_options_for(modes[index]);
        CHECK_OK(br_resize_to(nullptr, &source, 333, 249, &options, &images.values[index + 1]));
    }
    for (uint32_t index = 0; index < callers; ++index)
        CHECK_OK(br_image_create(333, 249, BR_PIXEL_BGRA8, &images.values[3 + index]));

    std::atomic<bool> begin{false};
    std::atomic<uint32_t> failures{0}, completed{0}, reconfigured{0};
    std::vector<std::thread> workers;
    std::thread setter;
    try {
        workers.reserve(callers);
        for (uint32_t index = 0; index < callers; ++index) {
            workers.emplace_back([&, index] {
                while (!begin.load(std::memory_order_acquire)) std::this_thread::yield();
                for (uint32_t run = 0; run < iterations; ++run) {
                    const size_t mode = (index + run) % modes.size();
                    const auto options = br_resize_options_for(modes[mode]); // threads=0 проверяет атомарное чтение настройки.
                    auto& output = images.values[3 + index];
                    if (br_resize(nullptr, &source, &output, &options) != BR_OK ||
                        !brt::images_equal(br_image_as_view(&output), br_image_as_view(&images.values[mode + 1])))
                        failures.fetch_add(1, std::memory_order_relaxed);
                    completed.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        setter = std::thread([&] {
            const std::array<uint32_t, 4> counts{1, 4, 2, 6};
            while (!begin.load(std::memory_order_acquire)) std::this_thread::yield();
            for (uint32_t run = 0; run < changes; ++run) {
                if (br_context_set_threads(nullptr, counts[run % counts.size()]) != BR_OK)
                    failures.fetch_add(1, std::memory_order_relaxed);
                reconfigured.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        });
    } catch (...) {
        // Не оставляет уже созданные тестовые потоки заблокированными при ошибке создания системного потока.
        begin.store(true, std::memory_order_release);
        for (auto& worker : workers) worker.join();
        if (setter.joinable()) setter.join();
        br_context_set_threads(nullptr, 0);
        throw;
    }
    begin.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    setter.join();
    CHECK(completed.load(std::memory_order_relaxed) == callers * iterations);
    CHECK(reconfigured.load(std::memory_order_relaxed) == changes);
    CHECK(failures.load(std::memory_order_relaxed) == 0);
    CHECK_OK(br_context_set_threads(nullptr, 0));
}
