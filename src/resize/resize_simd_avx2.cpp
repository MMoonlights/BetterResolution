// AVX2 + FMA включаются для отдельных функций после проверки процессора во время выполнения.
// Стандартные заголовки подключаются до директив target, чтобы общий встроенный код не зависел от набора инструкций.
#include "resize/resize_simd.hpp"

#if defined(BR_HAVE_AVX2_TU)
#include <immintrin.h>

#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx2,fma"))), apply_to = function)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC target("avx2,fma")
#endif

#define BR_SIMD_NS avx2
#define BR_SIMD_AVX2 1
#include "resize/resize_simd.inl"

#if defined(__clang__)
#pragma clang attribute pop
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif
#endif
