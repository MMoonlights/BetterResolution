#pragma once

#include "core/common.hpp"

#include <string>

namespace br {

// Функции для путей UTF-8 (в Windows преобразуют их в UTF-16).
br_status read_file(const char* path, Bytes& out, size_t max_size = size_t(1) << 31);
br_status write_file(const char* path, const uint8_t* data, size_t size);

// Стандартный Base64 с дополнением по RFC 4648.
size_t base64_size(size_t n) noexcept; // Число символов без завершающего NUL.
void base64_encode(const uint8_t* data, size_t n, char* out) noexcept;

}
