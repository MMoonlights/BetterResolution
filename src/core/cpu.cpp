#include "core/cpu.hpp"

#include <cstdlib>
#include <cstring>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#include <intrin.h>
#define BR_X86 1
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#define BR_X86 1
#endif

namespace br {
namespace {

#if defined(BR_X86)
void cpuid(int leaf, int sub, int out[4]) noexcept {
#if defined(_MSC_VER)
    __cpuidex(out, leaf, sub);
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    __cpuid_count(static_cast<unsigned>(leaf), static_cast<unsigned>(sub), a, b, c, d);
    out[0] = static_cast<int>(a); out[1] = static_cast<int>(b); out[2] = static_cast<int>(c); out[3] = static_cast<int>(d);
#endif
}

unsigned long long xgetbv0() noexcept {
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    unsigned eax = 0, edx = 0;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<unsigned long long>(edx) << 32) | eax;
#endif
}
#endif

CpuFeatures detect() noexcept {
    CpuFeatures f;
    const char* env = std::getenv("BR_DISABLE_SIMD");
    if (env && std::strcmp(env, "0") != 0 && env[0]) return f;
#if defined(BR_X86)
    int r[4]{};
    cpuid(0, 0, r);
    const int max_leaf = r[0];
    if (max_leaf < 1) return f;
    cpuid(1, 0, r);
    const bool osxsave = (r[2] >> 27) & 1;
    const bool avx = (r[2] >> 28) & 1;
    f.sse2 = (r[3] >> 26) & 1;
    f.ssse3 = (r[2] >> 9) & 1;
    f.fma = (r[2] >> 12) & 1;
    bool ymm_ok = false;
    if (osxsave && avx) ymm_ok = (xgetbv0() & 0x6) == 0x6;
    if (max_leaf >= 7) {
        cpuid(7, 0, r);
        f.avx2 = ymm_ok && ((r[1] >> 5) & 1);
    }
    f.fma = f.fma && ymm_ok;
#endif
    return f;
}
}

const CpuFeatures& cpu_features() noexcept {
    static const CpuFeatures f = detect();
    return f;
}

}
