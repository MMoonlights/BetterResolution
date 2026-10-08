#pragma once

#include <br/br.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace br::brf {

constexpr uint32_t magic = 0x31465242u; // "BRF1"

#pragma pack(push, 1)
struct Header {
    uint32_t magic_value;
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t header_size;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
    uint32_t color_space;
    uint32_t roi_count;
    uint64_t roi_table_offset;
    uint64_t payload_offset;
    uint64_t payload_size;
    uint64_t frame_hash;
};

struct RoiRecord {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint32_t flags;
};
#pragma pack(pop)

std::vector<uint8_t> pack_raw(const br_image_view& image, const br_rect_i32* rois, size_t roi_count);

}
