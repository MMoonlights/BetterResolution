#pragma once

#include <br/br.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace br::analysis {

uint64_t fnv1a64(const uint8_t* data, size_t size, uint64_t seed = 1469598103934665603ull) noexcept;
std::vector<uint64_t> hash_tiles(const br_image_view& image, uint32_t tile_w, uint32_t tile_h);

}
