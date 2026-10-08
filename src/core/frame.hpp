#pragma once

#include <br/br.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace br {

// Изображение во внутреннем владении (память std::vector).
struct Frame {
    uint32_t width{};
    uint32_t height{};
    ptrdiff_t stride{};
    br_pixel_format format{BR_PIXEL_BGRA8};
    br_color_space color_space{BR_COLOR_SRGB};
    bool premultiplied_alpha{false};
    std::vector<uint8_t> storage;

    bool allocate(uint32_t w, uint32_t h, br_pixel_format fmt);
    br_image_view view() const noexcept;
    br_mut_image_view mut_view() noexcept;
};

uint32_t channels_for(br_pixel_format fmt) noexcept;
bool has_alpha(br_pixel_format fmt) noexcept;
bool validate_image(const br_image_view& image) noexcept;
bool validate_image(const br_mut_image_view& image) noexcept;

inline br_image_view as_view(const br_mut_image_view& m) noexcept {
    return {m.data, m.width, m.height, m.stride, m.format, m.color_space, m.premultiplied_alpha};
}
inline const uint8_t* row_ptr(const br_image_view& v, uint32_t y) noexcept {
    return v.data + static_cast<ptrdiff_t>(y) * v.stride;
}
inline uint8_t* row_ptr(const br_mut_image_view& v, uint32_t y) noexcept {
    return v.data + static_cast<ptrdiff_t>(y) * v.stride;
}

// Выделяет упакованное изображение через malloc (владелец - вызывающий код, освобождение через free_image).
// При ошибке выбрасывает br::Error.
br_mut_image_view alloc_image(uint32_t w, uint32_t h, br_pixel_format fmt, bool zero = true);
void free_image(br_mut_image_view& image) noexcept;

// Преобразует строку из `width` пикселей между форматами (альфа отбрасывается или устанавливается в 255).
void convert_row(const uint8_t* src, br_pixel_format sf, uint8_t* dst, br_pixel_format df, uint32_t width) noexcept;
// Преобразует представления одинакового размера (поддерживает предварительно умноженную и прямую альфу).
void convert_image(const br_image_view& src, const br_mut_image_view& dst) noexcept;
// Возвращает true, если все значения альфа равны 255 или формат не содержит альфа-канал.
bool is_opaque(const br_image_view& image) noexcept;
// Обрезает прямоугольник по [0,w) × [0,h); возвращает false, если результат пуст.
bool clip_rect(br_rect_i32& r, uint32_t w, uint32_t h) noexcept;

}
