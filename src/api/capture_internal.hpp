#pragma once

#include <br/br.h>

#include "api/context.hpp"
#include "platform/capture.hpp"

#include <memory>
#include <vector>

// Не виден в публичном API; используется в api_capture.cpp и api_shot.cpp.
struct br_capture {
    br_context* context{};
    std::unique_ptr<br::capture::Session> session;
    std::vector<br_rect_i32> dirty;
    br_image scratch{}; // Кадр полного разрешения, повторно используемый grab_fit.
    ~br_capture();
};

namespace br {

struct GrabTimings {
    uint64_t capture_us{0}; // Получение кадра, включая уменьшение на GPU при gpu_scaled.
    uint64_t resize_us{0};  // Уменьшение на CPU; 0, если оно выполнено на GPU или не выполнялось.
    bool gpu_scaled{false};
};

// Открывает сеанс захвата. Параметр cold_start_first описан в capture::open.
br_status open_capture(br_context* ctx, const br_capture_options& options, bool cold_start_first, br_capture** out);

// Захват, вписывание и изменение размера. `out` используется повторно, если его размер уже подходит.
// image_to_screen в `info` переводит координаты пикселей результата в координаты рабочего стола.
// При scale > 0 и scale != 1 размер кадра умножается на scale вместо применения `fit`.
br_status grab_fit_timed(br_capture* c, uint32_t timeout_ms, const br_fit_options* fit, const br_resize_options* options,
                         br_image* out, br_frame_info* info, GrabTimings* timings, double scale = 0.0);

}
