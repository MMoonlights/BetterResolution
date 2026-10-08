#pragma once

#include <br/br.h>

#include <cstdint>
#include <vector>

namespace br::codec {

struct Rgba {
    uint8_t r, g, b, a;
};

struct Palette {
    Rgba colors[256];
    uint32_t count{0};
};

// Точная палитра; при превышении max_colors возвращает false, результат остаётся неполным.
bool exact_palette(const br_image_view& image, uint32_t max_colors, Palette& out);

// Непрозрачная палитра методом медианного разрезания и k-средних, без дизеринга.
void quantize(const br_image_view& image, uint32_t max_colors, Palette& out);

// Точный или ближайший индекс палитры; результат занимает width*height байтов.
void map_to_palette(const br_image_view& image, const Palette& palette, uint8_t* indices);

Rgba read_rgba(const br_image_view& image, const uint8_t* row, uint32_t x) noexcept;

}
