#pragma once

#include <br/br.h>
#include "core/common.hpp"

namespace br {
class ThreadPool;
}

namespace br::codec {

bool detect_format(const uint8_t* data, size_t size, br_encoded_format& out) noexcept;
bool format_from_extension(const char* path, br_encoded_format& out) noexcept;
const char* mime_type(br_encoded_format format) noexcept;

br_status encode(const br_image_view& image, const br_encode_options& options, Bytes& out, ThreadPool* pool = nullptr);
br_status probe(const uint8_t* data, size_t size, br_image_info& info);
// UNKNOWN сохраняет исходный формат; остальные значения задают преобразование.
br_status decode(const uint8_t* data, size_t size, br_pixel_format format, br_mut_image_view& out);

}
