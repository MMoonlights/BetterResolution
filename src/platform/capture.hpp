#pragma once

#include <br/br.h>

#include <functional>
#include <memory>
#include <vector>

namespace br::capture {

class Session {
public:
    virtual ~Session() = default;
    virtual br_capture_backend backend() const noexcept = 0;
    virtual br_status size(uint32_t& w, uint32_t& h) = 0;
    // Возвращает текущее содержимое в BGRA8 в `img` (буфер выделяет или повторно использует вызывающий код).
    virtual br_status grab(uint32_t timeout_ms, br_mut_image_view& img, br_frame_info& info,
                           std::vector<br_rect_i32>& dirty) = 0;
    // Низкоуровневый вызов: ждёт новый кадр (BR_E_TIMEOUT, если его нет) и записывает в представление вызывающего кода.
    virtual br_status next(uint32_t timeout_ms, const br_mut_image_view& dst, std::vector<br_rect_i32>& dirty) = 0;
    virtual br_status next_d3d11(uint32_t timeout_ms, void** texture, uint32_t& w, uint32_t& h,
                                 std::vector<br_rect_i32>& dirty) = 0;
    // Необязательное уменьшение на GPU до чтения; возвращает BR_E_UNSUPPORTED, если недоступно.
    virtual br_status grab_scaled(uint32_t timeout_ms, uint32_t out_w, uint32_t out_h, const br_resize_options& options,
                                  br_mut_image_view& img, br_frame_info& info) {
        (void)timeout_ms; (void)out_w; (void)out_h; (void)options; (void)img; (void)info;
        return BR_E_UNSUPPORTED;
    }
};

br_status list_monitors(std::vector<br_monitor_info>& out);
br_status list_windows(std::vector<br_window_info>& out);
// cold_start_first выбирает быстрый запуск, если цель можно корректно захватить.
br_status open(const br_capture_options& options, std::unique_ptr<Session>& out, bool cold_start_first = false);
br_status open_native(const br_capture_options& options, bool cold_start_first, std::unique_ptr<Session>& out);
// Загружает графические API и создаёт временное устройство Direct3D.
br_status prewarm();
// Пауза для опроса с высокой точностью.
void sleep_ms(uint32_t ms);
// Точка подмены для тестов: если задана, open() возвращает сеанс фабрики вместо реального способа захвата.
using SessionFactory = std::function<br_status(const br_capture_options&, std::unique_ptr<Session>&)>;
void set_test_factory(SessionFactory factory);
br_status open_dxgi_output(uint32_t adapter, uint32_t output, std::unique_ptr<Session>& out);
br_status open_window(void* hwnd, std::unique_ptr<Session>& out);
void release_d3d11_texture(void* texture);
const char* features() noexcept;

}
