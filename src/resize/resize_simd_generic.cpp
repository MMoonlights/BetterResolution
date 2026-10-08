#define BR_SIMD_NS generic
#define BR_SIMD_AVX2 0
#include "resize/resize_simd.inl"

#include "core/cpu.hpp"

namespace br::resize {

const SimdKernels& simd_kernels() noexcept {
    if (!br::cpu_features().sse2) return scalar::kernels();
#if defined(BR_HAVE_AVX2_TU)
    if (br::cpu_features().avx2 && br::cpu_features().fma) return avx2::kernels();
#endif
    return generic::kernels();
}

}
