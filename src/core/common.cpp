#include "core/common.hpp"
#include "core/cpu.hpp"
#include "core/simd.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace br {
namespace {
thread_local char g_error[512] = "";
}

void set_error(const char* message) noexcept {
    if (!message) message = "";
    std::snprintf(g_error, sizeof(g_error), "%s", message);
}

void set_errorf(const char* fmt, ...) noexcept {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(g_error, sizeof(g_error), fmt, ap);
    va_end(ap);
}

const char* last_error() noexcept { return g_error; }

namespace {
// Таблицы Slicing-by-8, создаваемые один раз.
struct CrcTables {
    uint32_t t[8][256];
    CrcTables() noexcept {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : (c >> 1);
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i) {
            for (int s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xffu];
        }
    }
};
const CrcTables& crc_tables() noexcept {
    static const CrcTables tables;
    return tables;
}
}

uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n) noexcept {
    const auto& T = crc_tables().t;
    uint32_t c = ~crc;
    while (n >= 8) {
        c ^= load_le32(p);
        c = T[7][c & 0xff] ^ T[6][(c >> 8) & 0xff] ^ T[5][(c >> 16) & 0xff] ^ T[4][c >> 24] ^
            T[3][p[4]] ^ T[2][p[5]] ^ T[1][p[6]] ^ T[0][p[7]];
        p += 8;
        n -= 8;
    }
    if (n >= 4) {
        c ^= uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        c = T[3][c & 0xff] ^ T[2][(c >> 8) & 0xff] ^ T[1][(c >> 16) & 0xff] ^ T[0][c >> 24];
        p += 4;
        n -= 4;
    }
    while (n--) c = T[0][(c ^ *p++) & 0xffu] ^ (c >> 8);
    return ~c;
}

namespace {
constexpr uint32_t kAdlerMod = 65521u;
constexpr size_t kAdlerNmax = 5552; // Наибольшее n, для которого 255 n (n + 1) / 2 + (n + 1) (mod - 1) < 2^32.

#if BR_SIMD_X86
// По 32 байта за шаг: s1 += сумма байтов, s2 += 32 * s1 + сумма((32 - i) * byte_i).
BR_TARGET_SSSE3 uint32_t adler32_ssse3(uint32_t a, uint32_t b, const uint8_t*& p, size_t& n) noexcept {
    const __m128i w1 = _mm_setr_epi8(32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17);
    const __m128i w2 = _mm_setr_epi8(16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1);
    const __m128i zero = _mm_setzero_si128(), ones = _mm_set1_epi16(1);
    while (n >= 32) {
        size_t blocks = std::min(n, kAdlerNmax) / 32;
        n -= blocks * 32;
        __m128i vs1 = _mm_setzero_si128(), vprev = _mm_cvtsi32_si128(static_cast<int>(a * blocks));
        __m128i vs2 = _mm_cvtsi32_si128(static_cast<int>(b));
        while (blocks--) {
            const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
            const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16));
            vprev = _mm_add_epi32(vprev, vs1);
            vs1 = _mm_add_epi32(vs1, _mm_add_epi32(_mm_sad_epu8(x, zero), _mm_sad_epu8(y, zero)));
            vs2 = _mm_add_epi32(vs2, _mm_madd_epi16(_mm_add_epi16(_mm_maddubs_epi16(x, w1), _mm_maddubs_epi16(y, w2)), ones));
            p += 32;
        }
        vs2 = _mm_add_epi32(vs2, _mm_slli_epi32(vprev, 5));
        vs1 = _mm_add_epi32(vs1, _mm_shuffle_epi32(vs1, _MM_SHUFFLE(1, 0, 3, 2)));
        vs2 = _mm_add_epi32(vs2, _mm_shuffle_epi32(vs2, _MM_SHUFFLE(2, 3, 0, 1)));
        vs2 = _mm_add_epi32(vs2, _mm_shuffle_epi32(vs2, _MM_SHUFFLE(1, 0, 3, 2)));
        a = (a + static_cast<uint32_t>(_mm_cvtsi128_si32(vs1))) % kAdlerMod;
        b = static_cast<uint32_t>(_mm_cvtsi128_si32(vs2)) % kAdlerMod;
    }
    return (b << 16) | a;
}
#endif
}

uint32_t adler32_update(uint32_t adler, const uint8_t* p, size_t n) noexcept {
    uint32_t a = adler & 0xffffu, b = adler >> 16;
#if BR_SIMD_X86
    if (n >= 64 && cpu_features().ssse3) {
        const uint32_t r = adler32_ssse3(a, b, p, n);
        a = r & 0xffffu;
        b = r >> 16;
    }
#endif
    while (n) {
        size_t chunk = n < kAdlerNmax ? n : kAdlerNmax;
        n -= chunk;
        while (chunk >= 8) {
            a += p[0]; b += a; a += p[1]; b += a; a += p[2]; b += a; a += p[3]; b += a;
            a += p[4]; b += a; a += p[5]; b += a; a += p[6]; b += a; a += p[7]; b += a;
            p += 8;
            chunk -= 8;
        }
        while (chunk--) { a += *p++; b += a; }
        a %= kAdlerMod;
        b %= kAdlerMod;
    }
    return (b << 16) | a;
}

uint64_t monotonic_us() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

}
