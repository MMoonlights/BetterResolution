#pragma once

#include <br/br.h>
#include "core/common.hpp"

namespace br::codec {

// BMP: 24-битный без альфа-канала или 32-битный BGRA; RGB и битовые маски, упакованные значения, строки в обоих направлениях.
br_status encode_bmp(const br_image_view& image, Bytes& out);
br_status probe_bmp(const uint8_t* data, size_t size, br_image_info& info);
br_status decode_bmp(const uint8_t* data, size_t size, br_mut_image_view& out);

br_status encode_qoi(const br_image_view& image, Bytes& out);
br_status probe_qoi(const uint8_t* data, size_t size, br_image_info& info);
br_status decode_qoi(const uint8_t* data, size_t size, br_mut_image_view& out);

// Бинарные P5/P6; maxval <=65535.
br_status encode_pnm(const br_image_view& image, Bytes& out);
br_status probe_pnm(const uint8_t* data, size_t size, br_image_info& info);
br_status decode_pnm(const uint8_t* data, size_t size, br_mut_image_view& out);

}
