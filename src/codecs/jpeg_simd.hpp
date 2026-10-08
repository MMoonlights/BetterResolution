#pragma once
#include "core/simd.hpp"
#include <cstddef>
#include <cstdint>

namespace br::codec::jpeg_detail {
void quantize_block_scalar(const float* src, size_t stride, const float* div, int16_t* q) noexcept;
#if BR_SIMD_X86
void quantize_block_avx2(const float* src, size_t stride, const float* div, int16_t* q) noexcept;
uint32_t ycbcr_avx2(const uint8_t* src, uint32_t width, uint32_t channels, bool bgr,
                   float* y, float* cb, float* cr) noexcept;
// Скалярная арифметика AAN с округлением от нуля при равенстве половине.
void quantize_block_sse2(const float* src, size_t stride, const float* div, int16_t* q) noexcept;
uint32_t ycbcr_sse2(const uint8_t* src, uint32_t width, uint32_t channels, bool bgr,
                   float* y, float* cb, float* cr) noexcept;
#endif
}
