#pragma once

#include <br/br.h>

#include <cstdint>
#include <vector>

namespace br::resize {

// Для каждого выходного пикселя берутся taps выборок в пределах изображения от start[i]; веса - weights[i * taps + k].
// Выборки за краями переносятся к границе; неиспользуемые веса заполняются нулями для SIMD без ветвлений.
struct AxisSampler {
    uint32_t in_size{};
    uint32_t out_size{};
    uint32_t taps{};
    br_filter filter{BR_FILTER_MITCHELL};
    bool negative_lobes{false};
    std::vector<int32_t> start;
    std::vector<float> weights;
    // Главный лепесток (непрерывная область положительных весов вокруг пика) относительно start.
    std::vector<uint16_t> lobe_begin;
    std::vector<uint16_t> lobe_end;
};

// Сопоставляет out_size выходных выборок интервалу входных данных [in_offset, in_offset + in_extent)
// (в пикселях исходного изображения) на оси с in_size выборками.
AxisSampler build_axis_sampler(uint32_t in_size, double in_offset, double in_extent, uint32_t out_size, br_filter filter);

inline AxisSampler build_axis_sampler(uint32_t in_size, uint32_t out_size, br_filter filter) {
    return build_axis_sampler(in_size, 0.0, static_cast<double>(in_size), out_size, filter);
}

}
