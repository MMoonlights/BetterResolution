#pragma once
#if defined(_WIN32)

#include "platform/capture.hpp"
#include "platform/windows/win_common.hpp"
#include "core/common.hpp"

#include <memory>
#include <vector>

namespace br::win {

struct CursorShape {
    std::vector<uint8_t> pixels; // BGRA (цвет) или 1-битные маски (монохром).
    uint32_t type{0};            // DXGI_OUTDUPL_POINTER_SHAPE_TYPE_* (1 - монохром, 2 - цвет, 4 - цвет с маской).
    uint32_t width{0}, height{0}, pitch{0};
    int32_t x{0}, y{0};          // Координаты верхнего левого угла в изображении.
    bool visible{false};
};

// Хранит последний доступный кадр CPU; grab не требует появления нового кадра.
class CachedSession : public capture::Session {
public:
    ~CachedSession() override;
    br_capture_backend backend() const noexcept override { return backend_; }
    br_status size(uint32_t& w, uint32_t& h) override;
    br_status grab(uint32_t timeout_ms, br_mut_image_view& img, br_frame_info& info, std::vector<br_rect_i32>& dirty) override;
    br_status next(uint32_t timeout_ms, const br_mut_image_view& dst, std::vector<br_rect_i32>& dirty) override;
    br_status next_d3d11(uint32_t, void**, uint32_t&, uint32_t&, std::vector<br_rect_i32>&) override {
        return fail(BR_E_UNSUPPORTED, "this capture backend has no D3D11 texture");
    }

protected:
    struct Update {
        bool updated{false};
        bool dirty_known{false};
        std::vector<br_rect_i32> dirty;
    };
    // При wait_for_new ожидает изменений; иначе ожидает только первый кадр.
    virtual br_status update(uint32_t timeout_ms, bool wait_for_new, Update& u) = 0;
    // Ожидаемый размер кадра до получения первого кадра (0, если неизвестен).
    virtual void expected_size(uint32_t& w, uint32_t& h) { w = cache_.width; h = cache_.height; }

    void ensure_cache(uint32_t w, uint32_t h); // Выделяет или перевыделяет cache_ в формате BGRA8.
    void fill_info(br_frame_info& info, bool changed, bool dirty_known) const;
    void composite_cursor(const br_mut_image_view& img) const;

    br_capture_backend backend_{BR_BACKEND_AUTO};
    br_mut_image_view cache_{};
    bool have_{false};
    uint64_t frame_id_{0};
    uint64_t frame_time_{0};
    br_rect_i32 screen_rect_{};  // Область рабочего стола, занимаемая изображением.
    double scale_x_{1.0}, scale_y_{1.0};
    bool include_cursor_{false};
    CursorShape cursor_;
};

// Фабрики способов захвата. `crop` (width > 0) задаёт прямоугольную область виртуального рабочего стола.
br_status open_dxgi(uint32_t adapter, uint32_t output, const br_rect_i32* crop, bool cursor, std::unique_ptr<capture::Session>& out);
br_status open_wgc_window(HWND hwnd, bool client_only, const br_rect_i32* crop, bool cursor, bool border,
                          std::unique_ptr<capture::Session>& out);
br_status open_wgc_monitor(HMONITOR mon, const br_rect_i32& mon_rect, const br_rect_i32* crop, bool cursor, bool border,
                           std::unique_ptr<capture::Session>& out);
// Auto повторно проверяет перекрытие при каждом захвате; Screen/Print принудительно выбирают способ.
enum class GdiMode { Auto, Screen, Print, PrintFull };
br_status open_gdi_region(const br_rect_i32& desktop_rect, bool cursor, std::unique_ptr<capture::Session>& out);
br_status open_gdi_window(HWND hwnd, bool client_only, const br_rect_i32* crop, bool cursor, GdiMode mode,
                          std::unique_ptr<capture::Session>& out);
// Загружает графические API и создаёт временное устройство.
br_status prewarm_graphics();

// Однократно копирует прямоугольную область рабочего стола через GDI BitBlt в изображение BGRA того же размера (альфа = 255).
br_status gdi_copy_rect(const br_rect_i32& desktop_rect, const br_mut_image_view& dst);

void copy_rect_from_mapped(const uint8_t* src, size_t src_pitch, int32_t src_x, int32_t src_y, const br_mut_image_view& dst,
                           const br_rect_i32& dst_rect);

}

#endif
