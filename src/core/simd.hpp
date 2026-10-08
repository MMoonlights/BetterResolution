#pragma once

// SSE2 - базовый набор инструкций x86-64; более широкие наборы используют BR_TARGET_* и выбор во время выполнения.
#if defined(__x86_64__) || defined(_M_X64)
#define BR_SIMD_X86 1
#include <immintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#define BR_TARGET_SSSE3
#define BR_TARGET_AVX2
#else
#define BR_TARGET_SSSE3 __attribute__((target("ssse3")))
#define BR_TARGET_AVX2 __attribute__((target("avx2")))
#endif
#else
#define BR_SIMD_X86 0
#endif
