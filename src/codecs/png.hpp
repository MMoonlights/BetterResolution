#pragma once

#include <br/br.h>
#include "core/common.hpp"

namespace br {
class ThreadPool;
}

namespace br::codec {

struct PngEncodeOptions {
    int effort{1}; // Уровень сжатия DEFLATE от 0 до 9.
    int palette{BR_PALETTE_AUTO}; // Режим палитры br_png_palette_mode.
    uint32_t max_colors{256};
    unsigned unfiltered_threshold{60}; // Порог повторов в процентах для выбора фильтра 0.
    ThreadPool* pool{nullptr}; // Параллельная фильтрация и полосы DEFLATE.
};

// AUTO использует точную палитру или форматы gray/RGB/RGBA; QUANTIZE может менять цвета.
br_status encode_png(const br_image_view& image, const PngEncodeOptions& options, Bytes& out);

br_status probe_png(const uint8_t* data, size_t size, br_image_info& info);
// UNKNOWN сохраняет исходный 8-битный формат (GRAY8/RGB8/RGBA8).
br_status decode_png(const uint8_t* data, size_t size, br_mut_image_view& out, br_pixel_format requested = BR_PIXEL_UNKNOWN);

}
