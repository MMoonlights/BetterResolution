#pragma once

namespace br {

struct CpuFeatures {
    bool sse2{false};
    bool ssse3{false};
    bool avx2{false};
    bool fma{false};
};

// Определяется один раз; переменная окружения BR_DISABLE_SIMD=1 отключает SIMD (для тестирования).
const CpuFeatures& cpu_features() noexcept;

}
