#pragma once

#include <br/br.h>
#include "core/common.hpp"

namespace br {
class ThreadPool;
}

namespace br::codec {

struct JpegEncodeOptions {
    int quality{90};
    bool subsample_420{false}; // false - режим 4:4:4.
    bool optimize{true}; // Оптимизированные таблицы Хаффмана за два прохода.
    ThreadPool* pool{nullptr}; // Параллельные полосы между точками перезапуска.
};

// Серое изображение кодируется одним компонентом.
br_status encode_jpeg(const br_image_view& image, const JpegEncodeOptions& options, Bytes& out);

br_status probe_jpeg(const uint8_t* data, size_t size, br_image_info& info);
// Обычный или прогрессивный JPEG с кодами Хаффмана; результат GRAY8/RGB8, включая преобразование CMYK/YCCK.
br_status decode_jpeg(const uint8_t* data, size_t size, br_mut_image_view& out);

}
