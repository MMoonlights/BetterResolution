#include "container/brf.hpp"
#include "analysis/delta.hpp"
#include "core/frame.hpp"

#include <cstring>
#include <stdexcept>

namespace br::brf {

std::vector<uint8_t> pack_raw(const br_image_view& image, const br_rect_i32* rois, size_t roi_count) {
    if (!br::validate_image(image)) throw std::invalid_argument("invalid image");
    const uint32_t c = br::channels_for(image.format);
    const size_t packed_stride = static_cast<size_t>(image.width) * c;
    const size_t payload_size = packed_stride * image.height;
    const size_t roi_bytes = roi_count * sizeof(RoiRecord);
    const size_t total = sizeof(Header) + roi_bytes + payload_size;
    std::vector<uint8_t> out(total);

    Header h{};
    h.magic_value = magic;
    h.version_major = 1;
    h.version_minor = 0;
    h.header_size = sizeof(Header);
    h.width = image.width;
    h.height = image.height;
    h.stride = static_cast<uint32_t>(packed_stride);
    h.pixel_format = static_cast<uint32_t>(image.format);
    h.color_space = static_cast<uint32_t>(image.color_space);
    h.roi_count = static_cast<uint32_t>(roi_count);
    h.roi_table_offset = sizeof(Header);
    h.payload_offset = sizeof(Header) + roi_bytes;
    h.payload_size = payload_size;

    uint8_t* payload = out.data() + h.payload_offset;
    for (uint32_t y = 0; y < image.height; ++y) {
        const auto* src = image.data + static_cast<ptrdiff_t>(y) * image.stride;
        std::memcpy(payload + static_cast<size_t>(y) * packed_stride, src, packed_stride);
    }
    h.frame_hash = br::analysis::fnv1a64(payload, payload_size);
    std::memcpy(out.data(), &h, sizeof(h));

    auto* records = reinterpret_cast<RoiRecord*>(out.data() + h.roi_table_offset);
    for (size_t i = 0; i < roi_count; ++i) {
        records[i] = {rois[i].x, rois[i].y, rois[i].width, rois[i].height, 0};
    }
    return out;
}

}
