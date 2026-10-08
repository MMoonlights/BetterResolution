#include "analysis/delta.hpp"
#include "core/frame.hpp"

#include <algorithm>
#include <stdexcept>

namespace br::analysis {

uint64_t fnv1a64(const uint8_t* data, size_t size, uint64_t seed) noexcept {
    uint64_t h = seed;
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::vector<uint64_t> hash_tiles(const br_image_view& image, uint32_t tile_w, uint32_t tile_h) {
    if (!br::validate_image(image) || !tile_w || !tile_h) throw std::invalid_argument("invalid tile hash input");
    const uint32_t c = br::channels_for(image.format);
    const uint32_t cols = (image.width + tile_w - 1) / tile_w;
    const uint32_t rows = (image.height + tile_h - 1) / tile_h;
    std::vector<uint64_t> out(static_cast<size_t>(cols) * rows);

    for (uint32_t ty = 0; ty < rows; ++ty) {
        for (uint32_t tx = 0; tx < cols; ++tx) {
            const uint32_t x0 = tx * tile_w;
            const uint32_t y0 = ty * tile_h;
            const uint32_t w = std::min(tile_w, image.width - x0);
            const uint32_t h = std::min(tile_h, image.height - y0);
            uint64_t hash = 1469598103934665603ull;
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* row = image.data + static_cast<ptrdiff_t>(y0 + y) * image.stride + static_cast<size_t>(x0) * c;
                hash = fnv1a64(row, static_cast<size_t>(w) * c, hash);
            }
            out[static_cast<size_t>(ty) * cols + tx] = hash;
        }
    }
    return out;
}

}
