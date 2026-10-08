#pragma once

#include "resize/sampler.hpp"
#include "core/simd.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace br::resize::fixed {

// Коэффициенты Q14, промежуточные значения пикселей со знаком в Q6. Округление не обрезает
// горизонтальные ореолы; ограничивается только итоговый байтовый результат. Точная сумма коэффициентов
// сохраняет постоянные значения, в том числе alpha=255, при любом масштабе.
inline constexpr int kWeightBits = 14;
inline constexpr int kPixelBits = 6;
inline constexpr int kOne = 1 << kWeightBits;
inline constexpr uint32_t kMaxTaps = 64;
struct Axis {
    std::vector<int16_t> weights;
    std::vector<uint32_t> pairs; // два коэффициента со знаком, младшие/старшие 16 бит
    double max_error{};         // максимальное расстояние L1 до коэффициентов float
    double max_l1{};
    bool valid{};
};
Axis quantize(const AxisSampler& axis);
bool compatible(const Axis& h, const Axis& v) noexcept;

struct Kernels {
    const char* name;
    void (*h4)(const uint8_t*, int16_t*, const AxisSampler&, const Axis&);
    void (*h1)(const uint8_t*, int16_t*, const AxisSampler&, const Axis&);
    void (*v)(uint8_t*, size_t, const int16_t* const*, const int16_t*, uint32_t);
};
const Kernels& kernels() noexcept;
const Kernels& scalar_kernels() noexcept;
#if BR_SIMD_X86
const Kernels& sse2_kernels() noexcept;
const Kernels& ssse3_kernels() noexcept;
const Kernels& avx2_kernels() noexcept;
#endif

}
