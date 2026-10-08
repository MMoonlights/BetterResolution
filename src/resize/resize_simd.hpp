#pragma once

#include "resize/sampler.hpp"

#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
#define BR_HAVE_AVX2_TU 1
#endif

namespace br::resize {

struct SimdKernels {
    const char* name;
    // Горизонтальный проход по пикселям с 4 каналами: out[x] = sum_k w[x,k] * in[start[x] + k].
    void (*h4)(const float* in, float* out, const AxisSampler& s, bool antiring);
    // Горизонтальный проход по пикселям с 1 каналом.
    void (*h1)(const float* in, float* out, const AxisSampler& s, bool antiring);
    // Вертикальный проход: out[i] = sum_k w[k] * rows[k][i]; защита от ореолов ограничивает значения диапазоном rows[lb..le).
    void (*v)(float* out, size_t n, const float* const* rows, const float* w, uint32_t taps,
              uint32_t lb, uint32_t le, bool antiring);
    // dst[i] = src[i] / 255
    void (*u8_to_f32)(const uint8_t* src, float* dst, size_t n);
    // dst[i] = round(clamp(src[i], 0, 1) * 255)
    void (*f32_to_u8)(const float* src, uint8_t* dst, size_t n);
};

// Наиболее быстрая реализация для текущего процессора.
const SimdKernels& simd_kernels() noexcept;

namespace scalar { const SimdKernels& kernels() noexcept; }
namespace generic { const SimdKernels& kernels() noexcept; }
#if defined(BR_HAVE_AVX2_TU)
namespace avx2 { const SimdKernels& kernels() noexcept; }
#endif

}
