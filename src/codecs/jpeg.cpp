#include "codecs/jpeg.hpp"
#include "codecs/jpeg_simd.hpp"
#include "core/cpu.hpp"
#include <bit>

#include "codecs/huffman.hpp"
#include "core/frame.hpp"
#include "core/thread_pool.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace br::codec {
namespace {

// Переводит индекс из зигзагообразного порядка в обычный порядок по строкам.
constexpr uint8_t kZigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// Таблицы квантования из приложения K ITU-T T.81 в обычном порядке.
constexpr uint8_t kStdLuma[64] = {
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr uint8_t kStdChroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// Таблицы Хаффмана из приложения K.3.
constexpr uint8_t kDcLumaBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
constexpr uint8_t kDcChromaBits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
constexpr uint8_t kDcVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr uint8_t kAcLumaBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
constexpr uint8_t kAcLumaVals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa};
constexpr uint8_t kAcChromaBits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
constexpr uint8_t kAcChromaVals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa};

// Коэффициенты AAN: s[0]=1; s[k]=sqrt(2)*cos(k*pi/16).
const float* aan_scale() {
    static const std::array<float, 8> s = [] {
        std::array<float, 8> a{};
        a[0] = 1.0f;
        for (int k = 1; k < 8; ++k) a[k] = static_cast<float>(std::sqrt(2.0) * std::cos(k * 3.14159265358979323846 / 16.0));
        return a;
    }();
    return s.data();
}

// Результат прямого ДКП AAN умножается на 8*s[u]*s[v].
void fdct_aan(float* d) noexcept {
    for (int pass = 0; pass < 2; ++pass) {
        const int step = pass == 0 ? 1 : 8; // Сначала строки, затем столбцы.
        const int next = pass == 0 ? 8 : 1;
        for (int i = 0; i < 8; ++i) {
            float* p = d + i * next;
            const float t0 = p[0 * step] + p[7 * step], t7 = p[0 * step] - p[7 * step];
            const float t1 = p[1 * step] + p[6 * step], t6 = p[1 * step] - p[6 * step];
            const float t2 = p[2 * step] + p[5 * step], t5 = p[2 * step] - p[5 * step];
            const float t3 = p[3 * step] + p[4 * step], t4 = p[3 * step] - p[4 * step];
            float t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
            p[0 * step] = t10 + t11;
            p[4 * step] = t10 - t11;
            const float z1 = (t12 + t13) * 0.707106781f;
            p[2 * step] = t13 + z1;
            p[6 * step] = t13 - z1;
            t10 = t4 + t5;
            t11 = t5 + t6;
            t12 = t6 + t7;
            const float z5 = (t10 - t12) * 0.382683433f;
            const float z2 = 0.541196100f * t10 + z5;
            const float z4 = 1.306562965f * t12 + z5;
            const float z3 = t11 * 0.707106781f;
            const float z11 = t7 + z3, z13 = t7 - z3;
            p[5 * step] = z13 + z2;
            p[3 * step] = z13 - z2;
            p[1 * step] = z11 + z4;
            p[7 * step] = z11 - z4;
        }
    }
}

// Вход обратного ДКП AAN предварительно умножается на s[u]*s[v]/8.
void idct_aan(float* d) noexcept {
    for (int pass = 0; pass < 2; ++pass) {
        const int step = pass == 0 ? 8 : 1; // Сначала столбцы, затем строки.
        const int next = pass == 0 ? 1 : 8;
        for (int i = 0; i < 8; ++i) {
            float* p = d + i * next;
            float t0 = p[0], t1 = p[2 * step], t2 = p[4 * step], t3 = p[6 * step];
            float t10 = t0 + t2, t11 = t0 - t2;
            float t13 = t1 + t3, t12 = (t1 - t3) * 1.414213562f - t13;
            t0 = t10 + t13;
            t3 = t10 - t13;
            t1 = t11 + t12;
            t2 = t11 - t12;
            const float t4 = p[1 * step], t5 = p[3 * step], t6 = p[5 * step], t7 = p[7 * step];
            const float z13 = t6 + t5, z10 = t6 - t5, z11 = t4 + t7, z12 = t4 - t7;
            const float o7 = z11 + z13;
            const float o11 = (z11 - z13) * 1.414213562f;
            const float z5 = (z10 + z12) * 1.847759065f;
            const float o10 = z5 - z12 * 1.082392200f;
            const float o12 = z5 - z10 * 2.613125930f;
            const float o6 = o12 - o7;
            const float o5 = o11 - o6;
            const float o4 = o10 - o5;
            p[0] = t0 + o7;
            p[7 * step] = t0 - o7;
            p[1 * step] = t1 + o6;
            p[6 * step] = t1 - o6;
            p[2 * step] = t2 + o5;
            p[5 * step] = t2 - o5;
            p[3 * step] = t3 + o4;
            p[4 * step] = t3 - o4;
        }
    }
}

inline uint8_t clamp_u8(float v) noexcept {
    const int i = static_cast<int>(v + (v >= 0 ? 0.5f : -0.5f));
    return static_cast<uint8_t>(i < 0 ? 0 : i > 255 ? 255 : i);
}


struct HuffTable {
    uint8_t bits[16]{};
    uint8_t vals[256]{};
    int nvals{0};
    uint16_t code[256]{};
    uint8_t size[256]{};

    void set(const uint8_t* b, const uint8_t* v, int n) {
        std::memcpy(bits, b, 16);
        std::memcpy(vals, v, static_cast<size_t>(n));
        nvals = n;
        derive();
    }
    void derive() {
        std::memset(size, 0, sizeof(size));
        uint32_t c = 0;
        int k = 0;
        for (int l = 1; l <= 16; ++l) {
            for (int i = 0; i < bits[l - 1]; ++i, ++k) {
                code[vals[k]] = static_cast<uint16_t>(c++);
                size[vals[k]] = static_cast<uint8_t>(l);
            }
            c <<= 1;
        }
    }
    // Ограничение кодов Хаффмана в 16 бит; резервирует код из одних единиц.
    void optimize(const uint32_t* freq) {
        uint32_t f[257];
        std::memcpy(f, freq, 256 * sizeof(uint32_t));
        f[256] = 1; // Псевдосимвол резервирует код из одних единиц.
        uint8_t lens[257];
        build_code_lengths(f, 257, 16, lens);
        // Оставляет псевдосимвол на максимальной длине.
        uint8_t maxl = 0;
        for (int i = 0; i < 257; ++i) maxl = std::max(maxl, lens[i]);
        if (lens[256] != maxl) {
            for (int i = 255; i >= 0; --i) {
                if (lens[i] == maxl) { std::swap(lens[i], lens[256]); break; }
            }
        }
        std::memset(bits, 0, sizeof(bits));
        nvals = 0;
        for (int l = 1; l <= 16; ++l)
            for (int s = 0; s < 256; ++s)
                if (lens[s] == l) { vals[nvals++] = static_cast<uint8_t>(s); bits[l - 1]++; }
        derive();
    }
};

// Энтропийный поток от старшего бита к младшему с экранированием байта 0xFF.
class BitWriter {
public:
    explicit BitWriter(Bytes& out) : out_(out) {}
    inline void put(uint32_t value, int n) {
        acc_ = (acc_ << n) | (value & ((1u << n) - 1u));
        cnt_ += n;
        if (cnt_ >= 32) {
            cnt_ -= 32;
            const uint32_t w = static_cast<uint32_t>(acc_ >> cnt_);
            if ((((w & 0x7f7f7f7fu) + 0x01010101u) & w & 0x80808080u) == 0) {
                out_.push(static_cast<uint8_t>(w >> 24));
                out_.push(static_cast<uint8_t>(w >> 16));
                out_.push(static_cast<uint8_t>(w >> 8));
                out_.push(static_cast<uint8_t>(w));
            } else {
                for (int s = 24; s >= 0; s -= 8) {
                    const uint8_t b = static_cast<uint8_t>(w >> s);
                    out_.push(b);
                    if (b == 0xff) out_.push(0);
                }
            }
        }
    }
    void flush() {
        // Дополняет единицами до границы байта.
        const int pad = (8 - (cnt_ & 7)) & 7;
        if (pad) {
            acc_ = (acc_ << pad) | ((1u << pad) - 1u);
            cnt_ += pad;
        }
        while (cnt_ > 0) {
            cnt_ -= 8;
            const uint8_t b = static_cast<uint8_t>(acc_ >> cnt_);
            out_.push(b);
            if (b == 0xff) out_.push(0);
        }
        cnt_ = 0;
        acc_ = 0;
    }

private:
    Bytes& out_;
    uint64_t acc_{0};
    int cnt_{0};
};

inline int bit_count(int v) noexcept {
    const unsigned a = static_cast<unsigned>(v < 0 ? -v : v);
    return static_cast<int>(std::bit_width(a));
}

struct BlockStats {
    uint32_t dc[256]{};
    uint32_t ac[256]{};
    void add(const BlockStats& o) {
        for (int i = 0; i < 256; ++i) { dc[i] += o.dc[i]; ac[i] += o.ac[i]; }
    }
};

uint64_t count_block(const int16_t* q, int& pred, BlockStats& st) {
    uint64_t mask = 0;
    const int diff = q[0] - pred;
    pred = q[0];
    st.dc[bit_count(diff)]++;
    int run = 0;
    for (int k = 1; k < 64; ++k) {
        const int v = q[kZigzag[k]];
        if (!v) { ++run; continue; }
        mask |= uint64_t(1) << k;
        while (run >= 16) { st.ac[0xf0]++; run -= 16; }
        st.ac[(run << 4) | bit_count(v)]++;
        run = 0;
    }
    if (run) st.ac[0]++;
    return mask;
}

void encode_block(BitWriter& bw, const int16_t* q, int& pred, const HuffTable& dc, const HuffTable& ac) {
    const int diff = q[0] - pred;
    pred = q[0];
    int n = bit_count(diff);
    // Код и значение вместе занимают не более 16+11 бит.
    bw.put((static_cast<uint32_t>(dc.code[n]) << n) | (static_cast<uint32_t>(diff < 0 ? diff - 1 : diff) & ((1u << n) - 1u)),
           dc.size[n] + n);
    int run = 0;
    for (int k = 1; k < 64; ++k) {
        const int v = q[kZigzag[k]];
        if (!v) { ++run; continue; }
        while (run >= 16) { bw.put(ac.code[0xf0], ac.size[0xf0]); run -= 16; }
        n = bit_count(v);
        const int sym = (run << 4) | n;
        bw.put((static_cast<uint32_t>(ac.code[sym]) << n) | (static_cast<uint32_t>(v < 0 ? v - 1 : v) & ((1u << n) - 1u)),
               ac.size[sym] + n);
        run = 0;
    }
    if (run) bw.put(ac.code[0], ac.size[0]);
}

void encode_block_masked(BitWriter& bw, const int16_t* q, uint64_t mask, int& pred, const HuffTable& dc, const HuffTable& ac) {
    const int diff = q[0] - pred;
    pred = q[0];
    int n = bit_count(diff);
    bw.put((static_cast<uint32_t>(dc.code[n]) << n) | (static_cast<uint32_t>(diff < 0 ? diff - 1 : diff) & ((1u << n) - 1u)), dc.size[n] + n);
    int previous = 0;
    while (mask) {
        const int k = std::countr_zero(mask);
        mask &= mask - 1;
        int run = k - previous - 1;
        previous = k;
        while (run >= 16) { bw.put(ac.code[0xf0],ac.size[0xf0]); run -= 16; }
        const int v = q[kZigzag[k]];
        n = bit_count(v);
        const int symbol = (run << 4) | n;
        bw.put((static_cast<uint32_t>(ac.code[symbol]) << n) | (static_cast<uint32_t>(v < 0 ? v - 1 : v) & ((1u << n) - 1u)), ac.size[symbol] + n);
    }
    if (previous < 63) bw.put(ac.code[0],ac.size[0]);
}

void make_quant(const uint8_t* base, int quality, uint8_t* q) {
    quality = std::clamp(quality, 1, 100);
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int i = 0; i < 64; ++i) q[i] = static_cast<uint8_t>(std::clamp((base[i] * scale + 50) / 100, 1, 255));
}

void segment(Bytes& out, uint8_t marker, const uint8_t* payload, size_t n) {
    out.push(0xff);
    out.push(marker);
    out.be16(static_cast<uint32_t>(n + 2));
    out.append(payload, n);
}

void write_dht(Bytes& out, uint8_t tc_th, const HuffTable& t) {
    uint8_t buf[1 + 16 + 256];
    buf[0] = tc_th;
    std::memcpy(buf + 1, t.bits, 16);
    std::memcpy(buf + 17, t.vals, static_cast<size_t>(t.nvals));
    segment(out, 0xc4, buf, 17 + static_cast<size_t>(t.nvals));
}

bool image_is_gray(const br_image_view& img) noexcept {
    if (img.format == BR_PIXEL_GRAY8) return true;
    const uint32_t c = channels_for(img.format);
    for (uint32_t y = 0; y < img.height; ++y) {
        const uint8_t* p = row_ptr(img, y);
        for (uint32_t x = 0; x < img.width; ++x, p += c)
            if (p[0] != p[1] || p[1] != p[2]) return false;
    }
    return true;
}

struct EncoderSetup {
    const br_image_view* img;
    bool gray, sub;
    int mcu_w, mcu_h, blocks_per_mcu;
    uint32_t mcux, mcuy, pw;
    float div_y[64], div_c[64];
    int ro, go, bo; // Смещения байтов красного, зелёного и синего каналов в исходнике.
    uint32_t sc; // Число каналов исходника.
    bool simd;
};

void quantize_block(const float* src, size_t stride, const float* div, int16_t* q) {
    float b[64];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) b[y * 8 + x] = src[static_cast<size_t>(y) * stride + static_cast<size_t>(x)] - 128.0f;
    fdct_aan(b);
    for (int i = 0; i < 64; ++i) {
        const float v = b[i] * div[i];
        q[i] = static_cast<int16_t>(v < 0 ? static_cast<int>(v - 0.5f) : static_cast<int>(v + 0.5f));
    }
}

// Строки MCU [my0,my1), коэффициенты в порядке MCU.
void quantize_rows(const EncoderSetup& e, uint32_t my0, uint32_t my1, int16_t* coefs) {
    const uint32_t pw = e.pw;
    const int nplanes = e.gray ? 1 : 3;
    std::vector<float> plane[3];
    for (int c = 0; c < nplanes; ++c) plane[c].resize(static_cast<size_t>(pw) * e.mcu_h);
    const br_image_view& img = *e.img;
    auto quantize = &quantize_block;
#if BR_SIMD_X86
    if (e.simd) quantize = jpeg_detail::quantize_block_sse2;
    auto convert_simd = jpeg_detail::ycbcr_sse2;
    if (cpu_features().avx2) {
        quantize = jpeg_detail::quantize_block_avx2;
        convert_simd = jpeg_detail::ycbcr_avx2;
    }
#endif
    size_t bi = 0;
    for (uint32_t my = my0; my < my1; ++my) {
        for (int ry = 0; ry < e.mcu_h; ++ry) {
            const uint32_t sy = std::min<uint32_t>(my * static_cast<uint32_t>(e.mcu_h) + static_cast<uint32_t>(ry), img.height - 1);
            const uint8_t* s = row_ptr(img, sy);
            float* py = plane[0].data() + static_cast<size_t>(ry) * pw;
            if (e.gray) {
                for (uint32_t x = 0; x < img.width; ++x) py[x] = s[static_cast<size_t>(x) * e.sc];
                for (uint32_t x = img.width; x < pw; ++x) py[x] = py[img.width - 1];
                continue;
            }
            float* pcb = plane[1].data() + static_cast<size_t>(ry) * pw;
            float* pcr = plane[2].data() + static_cast<size_t>(ry) * pw;
            uint32_t done = 0;
#if BR_SIMD_X86
            if (e.simd) done = convert_simd(s, img.width, e.sc, e.ro == 2, py, pcb, pcr);
#endif
            const uint8_t* p = s + static_cast<size_t>(done) * e.sc;
            for (uint32_t x = done; x < img.width; ++x, p += e.sc) {
                const float r = p[e.ro], g = p[e.go], b = p[e.bo];
                py[x] = 0.299f * r + 0.587f * g + 0.114f * b;
                pcb[x] = -0.168735892f * r - 0.331264108f * g + 0.5f * b + 128.0f;
                pcr[x] = 0.5f * r - 0.418687589f * g - 0.081312411f * b + 128.0f;
            }
            for (uint32_t x = img.width; x < pw; ++x) {
                py[x] = py[img.width - 1];
                pcb[x] = pcb[img.width - 1];
                pcr[x] = pcr[img.width - 1];
            }
        }
        if (e.sub) {
            for (int c = 1; c < 3; ++c) {
                float* p = plane[c].data();
                for (int y = 0; y < 8; ++y)
                    for (uint32_t x = 0; x < pw / 2; ++x) {
                        const size_t a = static_cast<size_t>(2 * y) * pw + 2 * x;
                        p[static_cast<size_t>(y) * pw + x] = 0.25f * (p[a] + p[a + 1] + p[a + pw] + p[a + pw + 1]);
                    }
            }
        }
        for (uint32_t mx = 0; mx < e.mcux; ++mx) {
            int16_t* q = coefs + bi * 64;
            if (e.sub) {
                for (int by = 0; by < 2; ++by)
                    for (int bx = 0; bx < 2; ++bx)
                        quantize(plane[0].data() + static_cast<size_t>(by * 8) * pw + mx * 16 + static_cast<size_t>(bx * 8), pw, e.div_y, q + 64 * (by * 2 + bx));
                quantize(plane[1].data() + mx * 8, pw, e.div_c, q + 64 * 4);
                quantize(plane[2].data() + mx * 8, pw, e.div_c, q + 64 * 5);
            } else {
                quantize(plane[0].data() + mx * 8, pw, e.div_y, q);
                if (!e.gray) {
                    quantize(plane[1].data() + mx * 8, pw, e.div_c, q + 64);
                    quantize(plane[2].data() + mx * 8, pw, e.div_c, q + 128);
                }
            }
            bi += static_cast<size_t>(e.blocks_per_mcu);
        }
    }
}

}

namespace jpeg_detail {
void quantize_block_scalar(const float* src, size_t stride, const float* div, int16_t* q) noexcept {
    quantize_block(src,stride,div,q);
}
}

br_status encode_jpeg(const br_image_view& img, const JpegEncodeOptions& o, Bytes& out) {
    if (!validate_image(img)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    if (img.width > 65535 || img.height > 65535) return fail(BR_E_UNSUPPORTED, "JPEG dimensions limited to 65535");

    EncoderSetup e{};
    e.img = &img;
    e.simd = cpu_features().sse2;
    e.gray = image_is_gray(img);
    e.sub = !e.gray && o.subsample_420;
    e.mcu_w = e.sub ? 16 : 8;
    e.mcu_h = e.sub ? 16 : 8;
    e.blocks_per_mcu = e.gray ? 1 : (e.sub ? 6 : 3);
    e.mcux = (img.width + static_cast<uint32_t>(e.mcu_w) - 1) / static_cast<uint32_t>(e.mcu_w);
    e.mcuy = (img.height + static_cast<uint32_t>(e.mcu_h) - 1) / static_cast<uint32_t>(e.mcu_h);
    e.pw = e.mcux * static_cast<uint32_t>(e.mcu_w);
    e.sc = channels_for(img.format);
    switch (img.format) {
    case BR_PIXEL_BGRA8: case BR_PIXEL_BGR8: e.ro = 2; e.go = 1; e.bo = 0; break;
    case BR_PIXEL_GRAY8: e.ro = e.go = e.bo = 0; break;
    default: e.ro = 0; e.go = 1; e.bo = 2; break;
    }

    uint8_t qy[64], qc[64];
    make_quant(kStdLuma, o.quality, qy);
    make_quant(kStdChroma, o.quality, qc);
    const float* s = aan_scale();
    for (int v = 0; v < 8; ++v)
        for (int u = 0; u < 8; ++u) {
            e.div_y[v * 8 + u] = 1.0f / (qy[v * 8 + u] * s[u] * s[v] * 8.0f);
            e.div_c[v * 8 + u] = 1.0f / (qc[v * 8 + u] * s[u] * s[v] * 8.0f);
        }

    // Каждая полоса представляет собой отдельный интервал перезапуска JPEG.
    const uint64_t pixels = static_cast<uint64_t>(img.width) * img.height;
    uint32_t threads = o.pool ? o.pool->threads() : 1;
    uint32_t nstrips = (threads > 1 && pixels >= 256u * 1024u) ? std::min<uint32_t>(threads * 2, e.mcuy) : 1;
    uint32_t rows_per = (e.mcuy + nstrips - 1) / nstrips;
    if (nstrips > 1) {
        const uint32_t max_rows = std::max<uint32_t>(1, 65535u / e.mcux); // Интервал перезапуска занимает 16 бит.
        rows_per = std::min(rows_per, max_rows);
        nstrips = (e.mcuy + rows_per - 1) / rows_per;
    }
    const uint32_t restart_interval = nstrips > 1 ? rows_per * e.mcux : 0;

    const size_t blocks_per_row = static_cast<size_t>(e.mcux) * static_cast<size_t>(e.blocks_per_mcu);
    std::vector<int16_t> coefs(blocks_per_row * e.mcuy * 64);
    std::vector<uint64_t> masks(o.optimize ? blocks_per_row * e.mcuy : 0);
    std::vector<BlockStats> stats(nstrips * 2);
    std::vector<Bytes> parts(nstrips);

    HuffTable dcy, acy, dcc, acc;
    dcy.set(kDcLumaBits, kDcVals, 12);
    acy.set(kAcLumaBits, kAcLumaVals, 162);
    dcc.set(kDcChromaBits, kDcVals, 12);
    acc.set(kAcChromaBits, kAcChromaVals, 162);
    const int ny = e.sub ? 4 : 1;

    auto for_strips = [&](const std::function<void(uint32_t)>& fn) {
        if (nstrips > 1 && o.pool) o.pool->parallel_for(nstrips, 0, fn);
        else for (uint32_t i = 0; i < nstrips; ++i) fn(i);
    };
    for_strips([&](uint32_t si) {
        const uint32_t my0 = si * rows_per, my1 = std::min(e.mcuy, my0 + rows_per);
        int16_t* base = coefs.data() + static_cast<size_t>(my0) * blocks_per_row * 64;
        quantize_rows(e, my0, my1, base);
        if (!o.optimize) return;
        uint64_t* nz = masks.data() + static_cast<size_t>(my0) * blocks_per_row;
        int pred[3] = {0, 0, 0};
        const size_t nb = static_cast<size_t>(my1 - my0) * blocks_per_row;
        for (size_t b = 0; b < nb;) {
            for (int i = 0; i < ny; ++i, ++b) nz[b] = count_block(base + b * 64, pred[0], stats[si * 2]);
            if (!e.gray) {
                nz[b] = count_block(base + b * 64, pred[1], stats[si * 2 + 1]); ++b;
                nz[b] = count_block(base + b * 64, pred[2], stats[si * 2 + 1]); ++b;
            }
        }
    });
    if (o.optimize) {
        BlockStats sy, sc;
        for (uint32_t i = 0; i < nstrips; ++i) {
            sy.add(stats[i * 2]);
            sc.add(stats[i * 2 + 1]);
        }
        dcy.optimize(sy.dc);
        acy.optimize(sy.ac);
        if (!e.gray) {
            dcc.optimize(sc.dc);
            acc.optimize(sc.ac);
        }
    }
    for_strips([&](uint32_t si) {
        const uint32_t my0 = si * rows_per, my1 = std::min(e.mcuy, my0 + rows_per);
        const int16_t* base = coefs.data() + static_cast<size_t>(my0) * blocks_per_row * 64;
        const size_t nb = static_cast<size_t>(my1 - my0) * blocks_per_row;
        Bytes& p = parts[si];
        p.reserve(nb * 24);
        BitWriter w(p);
        int pred[3] = {0, 0, 0};
        const uint64_t* nz = o.optimize ? masks.data() + static_cast<size_t>(my0) * blocks_per_row : nullptr;
        auto encode_one = [&](size_t block, int& previous, const HuffTable& dc, const HuffTable& ac) {
            if (nz) encode_block_masked(w,base + block * 64,nz[block],previous,dc,ac);
            else encode_block(w,base + block * 64,previous,dc,ac);
        };
        for (size_t b = 0; b < nb;) {
            for (int i = 0; i < ny; ++i, ++b) encode_one(b, pred[0], dcy, acy);
            if (!e.gray) {
                encode_one(b, pred[1], dcc, acc); ++b;
                encode_one(b, pred[2], dcc, acc); ++b;
            }
        }
        w.flush();
    });

    size_t total = 1024;
    for (const Bytes& p : parts) total += p.size() + 2;
    out.clear();
    out.reserve(total);
    out.push(0xff);
    out.push(0xd8);
    static const uint8_t jfif[14] = {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    segment(out, 0xe0, jfif, sizeof(jfif));
    uint8_t dqt[130];
    dqt[0] = 0;
    for (int i = 0; i < 64; ++i) dqt[1 + i] = qy[kZigzag[i]];
    size_t n = 65;
    if (!e.gray) {
        dqt[65] = 1;
        for (int i = 0; i < 64; ++i) dqt[66 + i] = qc[kZigzag[i]];
        n = 130;
    }
    segment(out, 0xdb, dqt, n);
    const uint8_t sof[15] = {8, uint8_t(img.height >> 8), uint8_t(img.height), uint8_t(img.width >> 8), uint8_t(img.width),
                             static_cast<uint8_t>(e.gray ? 1 : 3), 1, uint8_t(e.sub ? 0x22 : 0x11), 0, 2, 0x11, 1, 3, 0x11, 1};
    segment(out, 0xc0, sof, e.gray ? 9 : 15);
    write_dht(out, 0x00, dcy);
    write_dht(out, 0x10, acy);
    if (!e.gray) {
        write_dht(out, 0x01, dcc);
        write_dht(out, 0x11, acc);
    }
    if (restart_interval) {
        const uint8_t dri[2] = {uint8_t(restart_interval >> 8), uint8_t(restart_interval)};
        segment(out, 0xdd, dri, 2);
    }
    if (e.gray) {
        const uint8_t sos[6] = {1, 1, 0x00, 0, 63, 0};
        segment(out, 0xda, sos, 6);
    } else {
        const uint8_t sos[10] = {3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0};
        segment(out, 0xda, sos, 10);
    }
    for (uint32_t i = 0; i < nstrips; ++i) {
        if (i) {
            out.push(0xff);
            out.push(static_cast<uint8_t>(0xd0 + ((i - 1) & 7)));
        }
        out.append(parts[i].data(), parts[i].size());
    }
    out.push(0xff);
    out.push(0xd9);
    return BR_OK;
}

namespace {

constexpr int kLookBits = 9;

struct DecHuff {
    bool present{false};
    uint16_t look[1 << kLookBits]; // (length << 8) | symbol; ноль включает медленный путь.
    int32_t maxcode[18];
    int32_t valoff[17];
    uint8_t vals[256];

    bool build(const uint8_t* bits, const uint8_t* v, int n) {
        std::memcpy(vals, v, static_cast<size_t>(n));
        std::memset(look, 0, sizeof(look));
        int32_t code = 0;
        int k = 0;
        for (int l = 1; l <= 16; ++l) {
            valoff[l] = k - code;
            if (bits[l - 1]) {
                for (int i = 0; i < bits[l - 1]; ++i, ++k, ++code) {
                    if (l <= kLookBits) {
                        const int shift = kLookBits - l;
                        for (int f = 0; f < (1 << shift); ++f) look[(code << shift) | f] = static_cast<uint16_t>((l << 8) | vals[k]);
                    }
                }
                maxcode[l] = code - 1;
            } else {
                maxcode[l] = -1;
            }
            if (code > (1 << l)) return false;
            code <<= 1;
        }
        maxcode[17] = 0x7fffffff;
        present = true;
        return true;
    }
};

struct BitReader {
    const uint8_t* p{};
    const uint8_t* end{};
    uint64_t buf{0};
    int cnt{0};
    bool marker{false};
    size_t zeros{0}; // Биты заполнения после маркера или конца данных.

    void refill() noexcept {
        while (cnt <= 56) {
            uint32_t b = 0;
            if (!marker && p < end) {
                b = *p;
                if (b == 0xff) {
                    const uint32_t nb = p + 1 < end ? p[1] : 0xd9;
                    if (nb == 0x00) { p += 2; }
                    else { marker = true; b = 0; ++zeros; }
                } else {
                    ++p;
                }
            } else {
                ++zeros;
            }
            buf |= static_cast<uint64_t>(b) << (56 - cnt);
            cnt += 8;
        }
    }
    inline uint32_t bits(int n) noexcept {
        if (!n) return 0;
        if (cnt < n) refill();
        const uint32_t v = static_cast<uint32_t>(buf >> (64 - n));
        buf <<= n;
        cnt -= n;
        return v;
    }
    inline int extend(int n) noexcept {
        if (!n) return 0;
        const int v = static_cast<int>(bits(n));
        return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v;
    }
    int decode(const DecHuff& h) noexcept {
        if (cnt < 16) refill();
        const uint16_t e = h.look[buf >> (64 - kLookBits)];
        if (e) {
            const int l = e >> 8;
            buf <<= l;
            cnt -= l;
            return e & 0xff;
        }
        int l = kLookBits + 1;
        int32_t code = static_cast<int32_t>(buf >> (64 - l));
        while (l <= 16 && code > h.maxcode[l]) {
            ++l;
            code = static_cast<int32_t>(buf >> (64 - l));
        }
        if (l > 16) return -1;
        buf <<= l;
        cnt -= l;
        return h.vals[(h.valoff[l] + code) & 0xff];
    }
    void reset() noexcept {
        buf = 0;
        cnt = 0;
        marker = false;
    }
};

struct Comp {
    int id{}, h{1}, v{1}, tq{};
    int td{}, ta{};
    uint32_t bw{}, bh{}; // Блоки, дополненные до полных MCU.
    uint32_t cw{}, ch{}; // Размер выборок компонента до заполнения.
    std::vector<int16_t> coef; // Коэффициенты прогрессивного JPEG в обычном порядке.
    std::vector<uint8_t> plane;
    int dc_pred{0};
};

struct Decoder {
    const uint8_t* data{};
    size_t size{};
    uint32_t width{}, height{};
    bool progressive{false};
    std::vector<Comp> comps;
    int hmax{1}, vmax{1};
    uint32_t mcux{}, mcuy{};
    uint16_t qt[4][64]{};
    bool qt_present[4]{};
    DecHuff dc[4], ac[4];
    uint32_t restart_interval{0};
    int adobe_transform{-1};
    bool frame_seen{false};
    bool coef_mode{false};
    uint32_t eobrun{0};

    br_status fail_decode(const char* msg) { return fail(BR_E_DECODE, msg); }

    br_status parse_sof(const uint8_t* s, size_t len, uint8_t marker) {
        if (len < 6) return fail_decode("short SOF");
        if (s[0] != 8) return fail(BR_E_UNSUPPORTED, "only 8-bit JPEG is supported");
        height = load_be16(s + 1);
        width = load_be16(s + 3);
        const int n = s[5];
        if (!width || !height) return fail(BR_E_UNSUPPORTED, "JPEG with DNL height is not supported");
        if (n != 1 && n != 3 && n != 4) return fail(BR_E_UNSUPPORTED, "unsupported JPEG component count");
        if (len < 6 + 3u * static_cast<size_t>(n)) return fail_decode("short SOF");
        if (static_cast<uint64_t>(width) * height > (uint64_t(1) << 28)) return fail(BR_E_UNSUPPORTED, "JPEG too large");
        progressive = marker == 0xc2;
        comps.resize(static_cast<size_t>(n));
        hmax = vmax = 1;
        for (int i = 0; i < n; ++i) {
            Comp& c = comps[static_cast<size_t>(i)];
            c.id = s[6 + 3 * i];
            c.h = s[7 + 3 * i] >> 4;
            c.v = s[7 + 3 * i] & 15;
            c.tq = s[8 + 3 * i] & 3;
            if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4) return fail_decode("bad sampling factors");
            hmax = std::max(hmax, c.h);
            vmax = std::max(vmax, c.v);
        }
        mcux = (width + 8u * hmax - 1) / (8u * hmax);
        mcuy = (height + 8u * vmax - 1) / (8u * vmax);
        coef_mode = progressive;
        for (Comp& c : comps) {
            c.bw = mcux * static_cast<uint32_t>(c.h);
            c.bh = mcuy * static_cast<uint32_t>(c.v);
            c.cw = (width * static_cast<uint32_t>(c.h) + hmax - 1) / static_cast<uint32_t>(hmax);
            c.ch = (height * static_cast<uint32_t>(c.v) + vmax - 1) / static_cast<uint32_t>(vmax);
            c.plane.assign(static_cast<size_t>(c.bw) * 8 * c.bh * 8, 0);
            if (coef_mode) c.coef.assign(static_cast<size_t>(c.bw) * c.bh * 64, 0);
        }
        frame_seen = true;
        return BR_OK;
    }

    void idct_store(const int16_t* coef, const uint16_t* q, uint8_t* dst, size_t stride) {
        const float* s = aan_scale();
        float b[64];
        for (int v = 0; v < 8; ++v)
            for (int u = 0; u < 8; ++u) b[v * 8 + u] = static_cast<float>(coef[v * 8 + u]) * q[v * 8 + u] * s[u] * s[v] * 0.125f;
        idct_aan(b);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) dst[static_cast<size_t>(y) * stride + static_cast<size_t>(x)] = clamp_u8(b[y * 8 + x] + 128.0f);
    }

    bool decode_baseline_block(BitReader& br, Comp& c, int16_t* blk) {
        std::memset(blk, 0, 64 * sizeof(int16_t));
        const int t = br.decode(dc[c.td]);
        if (t < 0 || t > 16) return false;
        c.dc_pred += br.extend(t);
        blk[0] = static_cast<int16_t>(c.dc_pred);
        for (int k = 1; k < 64;) {
            const int rs = br.decode(ac[c.ta]);
            if (rs < 0) return false;
            const int r = rs >> 4, sz = rs & 15;
            if (!sz) {
                if (r != 15) break;
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) return false;
            blk[kZigzag[k++]] = static_cast<int16_t>(br.extend(sz));
        }
        return true;
    }

    bool decode_prog_block(BitReader& br, Comp& c, int16_t* blk, int ss, int se, int ah, int al) {
        if (ss == 0) {
            if (ah == 0) {
                const int t = br.decode(dc[c.td]);
                if (t < 0 || t > 16) return false;
                c.dc_pred += br.extend(t);
                blk[0] = static_cast<int16_t>(c.dc_pred * (1 << al));
            } else if (br.bits(1)) {
                blk[0] = static_cast<int16_t>(blk[0] | (1 << al));
            }
            return true;
        }
        if (ah == 0) {
            if (eobrun) { --eobrun; return true; }
            for (int k = ss; k <= se;) {
                const int rs = br.decode(ac[c.ta]);
                if (rs < 0) return false;
                const int r = rs >> 4, sz = rs & 15;
                if (!sz) {
                    if (r < 15) {
                        eobrun = (1u << r) - 1;
                        if (r) eobrun += br.bits(r);
                        break;
                    }
                    k += 16;
                    continue;
                }
                k += r;
                if (k > 63) return false;
                blk[kZigzag[k++]] = static_cast<int16_t>(br.extend(sz) * (1 << al));
            }
            return true;
        }
        const int bit = 1 << al;
        int k = ss;
        if (eobrun) {
            --eobrun;
            for (; k <= se; ++k) {
                int16_t& v = blk[kZigzag[k]];
                if (v && br.bits(1) && !(v & bit)) v = static_cast<int16_t>(v > 0 ? v + bit : v - bit);
            }
            return true;
        }
        while (k <= se) {
            const int rs = br.decode(ac[c.ta]);
            if (rs < 0) return false;
            int r = rs >> 4;
            const int sz = rs & 15;
            int val = 0;
            if (!sz) {
                if (r < 15) {
                    eobrun = (1u << r) - 1;
                    if (r) eobrun += br.bits(r);
                    r = 64;
                }
            } else {
                if (sz != 1) return false;
                val = br.bits(1) ? bit : -bit;
            }
            while (k <= se) {
                int16_t& v = blk[kZigzag[k++]];
                if (v) {
                    if (br.bits(1) && !(v & bit)) v = static_cast<int16_t>(v > 0 ? v + bit : v - bit);
                } else {
                    if (r == 0) {
                        v = static_cast<int16_t>(val);
                        break;
                    }
                    --r;
                }
            }
        }
        return true;
    }

    br_status decode_scan(const uint8_t* s, size_t len, const uint8_t*& pos) {
        if (!frame_seen) return fail_decode("SOS before SOF");
        const int ns = s[0];
        if (ns < 1 || ns > 4 || len < 4 + 2u * static_cast<size_t>(ns)) return fail_decode("bad SOS");
        std::vector<Comp*> sc;
        for (int i = 0; i < ns; ++i) {
            const int id = s[1 + 2 * i];
            Comp* found = nullptr;
            for (Comp& c : comps) if (c.id == id) found = &c;
            if (!found) return fail_decode("SOS references unknown component");
            found->td = s[2 + 2 * i] >> 4;
            found->ta = s[2 + 2 * i] & 15;
            if (found->td > 3 || found->ta > 3) return fail_decode("bad table index");
            sc.push_back(found);
        }
        const int ss = s[1 + 2 * ns], se = s[2 + 2 * ns], ah = s[3 + 2 * ns] >> 4, al = s[3 + 2 * ns] & 15;
        if (progressive) {
            if (ss > se || se > 63 || (ss == 0 && se != 0) || (ss > 0 && ns != 1) || al > 13) return fail_decode("bad progressive scan");
        }
        for (Comp* c : sc) {
            if (!qt_present[c->tq] && !progressive) return fail_decode("missing quantization table");
            if ((!progressive || ss == 0) && ah == 0 && !dc[c->td].present) return fail_decode("missing DC table");
            if ((!progressive || ss > 0) && !ac[c->ta].present) return fail_decode("missing AC table");
            c->dc_pred = 0;
        }
        eobrun = 0;
        BitReader br;
        br.p = pos;
        br.end = data + size;

        uint32_t restarts_left = restart_interval;
        auto handle_restart = [&]() {
            if (!restart_interval) return;
            if (--restarts_left) return;
            restarts_left = restart_interval;
            // Пропускает данные до следующего маркера перезапуска.
            br.reset();
            while (br.p + 1 < br.end && !(br.p[0] == 0xff && br.p[1] >= 0xd0 && br.p[1] <= 0xd7)) {
                if (br.p[0] == 0xff && br.p[1] != 0 && br.p[1] != 0xff) break; // Останавливается на другом маркере.
                ++br.p;
            }
            if (br.p + 1 < br.end && br.p[0] == 0xff && br.p[1] >= 0xd0 && br.p[1] <= 0xd7) br.p += 2;
            for (Comp* c : sc) c->dc_pred = 0;
            eobrun = 0;
        };

        int16_t tmp[64];
        if (ns == 1) {
            Comp& c = *sc[0];
            const uint32_t bw = (c.cw + 7) / 8, bh = (c.ch + 7) / 8;
            for (uint32_t by = 0; by < bh; ++by) {
                for (uint32_t bx = 0; bx < bw; ++bx) {
                    bool ok;
                    if (coef_mode) {
                        ok = decode_prog_block(br, c, &c.coef[(static_cast<size_t>(by) * c.bw + bx) * 64], progressive ? ss : 0,
                                               progressive ? se : 63, ah, al);
                    } else {
                        ok = decode_baseline_block(br, c, tmp);
                        if (ok) idct_store(tmp, qt[c.tq], &c.plane[(static_cast<size_t>(by) * 8 * c.bw + bx) * 8], static_cast<size_t>(c.bw) * 8);
                    }
                    if (!ok) return fail_decode("corrupt JPEG scan data");
                    handle_restart();
                }
            }
        } else {
            for (uint32_t my = 0; my < mcuy; ++my) {
                for (uint32_t mx = 0; mx < mcux; ++mx) {
                    for (Comp* cp : sc) {
                        Comp& c = *cp;
                        for (int vy = 0; vy < c.v; ++vy) {
                            for (int hx = 0; hx < c.h; ++hx) {
                                const uint32_t bx = mx * static_cast<uint32_t>(c.h) + static_cast<uint32_t>(hx);
                                const uint32_t by = my * static_cast<uint32_t>(c.v) + static_cast<uint32_t>(vy);
                                bool ok;
                                if (coef_mode) {
                                    ok = decode_prog_block(br, c, &c.coef[(static_cast<size_t>(by) * c.bw + bx) * 64], ss, se, ah, al);
                                } else {
                                    ok = decode_baseline_block(br, c, tmp);
                                    if (ok) idct_store(tmp, qt[c.tq], &c.plane[(static_cast<size_t>(by) * 8 * c.bw + bx) * 8], static_cast<size_t>(c.bw) * 8);
                                }
                                if (!ok) return fail_decode("corrupt JPEG scan data");
                            }
                        }
                    }
                    handle_restart();
                }
            }
        }
        // Продолжает разбор маркеров после энтропийных данных.
        const uint8_t* q = br.p;
        while (q + 1 < data + size && !(q[0] == 0xff && q[1] != 0 && !(q[1] >= 0xd0 && q[1] <= 0xd7))) ++q;
        pos = q;
        return BR_OK;
    }

    br_status run(br_mut_image_view& out) {
        if (size < 4 || data[0] != 0xff || data[1] != 0xd8) return fail_decode("not a JPEG file");
        const uint8_t* p = data + 2;
        const uint8_t* end = data + size;
        bool done = false;
        while (!done && p < end) {
            if (*p != 0xff) { ++p; continue; }
            while (p < end && *p == 0xff) ++p;
            if (p >= end) break;
            const uint8_t m = *p++;
            if (m == 0xd8 || (m >= 0xd0 && m <= 0xd7) || m == 0x01) continue;
            if (m == 0xd9) break;
            if (end - p < 2) return fail_decode("truncated JPEG marker");
            const size_t len = load_be16(p);
            if (len < 2 || static_cast<size_t>(end - p) < len) return fail_decode("truncated JPEG segment");
            const uint8_t* s = p + 2;
            const size_t n = len - 2;
            switch (m) {
            case 0xc0: case 0xc1: case 0xc2: {
                if (frame_seen) return fail_decode("multiple frames");
                const br_status st = parse_sof(s, n, m);
                if (st != BR_OK) return st;
                break;
            }
            case 0xc3: case 0xc5: case 0xc6: case 0xc7: case 0xc9: case 0xca: case 0xcb: case 0xcd: case 0xce: case 0xcf:
                return fail(BR_E_UNSUPPORTED, "lossless/hierarchical/arithmetic JPEG is not supported");
            case 0xc4: {
                size_t o = 0;
                while (o + 17 <= n) {
                    const int tc = s[o] >> 4, th = s[o] & 15;
                    if (tc > 1 || th > 3) return fail_decode("bad DHT");
                    int total = 0;
                    for (int i = 0; i < 16; ++i) total += s[o + 1 + static_cast<size_t>(i)];
                    if (total > 256 || o + 17 + static_cast<size_t>(total) > n) return fail_decode("bad DHT");
                    DecHuff& h = tc ? ac[th] : dc[th];
                    if (!h.build(s + o + 1, s + o + 17, total)) return fail_decode("bad Huffman table");
                    o += 17 + static_cast<size_t>(total);
                }
                break;
            }
            case 0xdb: {
                size_t o = 0;
                while (o < n) {
                    const int pq = s[o] >> 4, tq = s[o] & 15;
                    if (tq > 3 || pq > 1 || o + 1 + (pq ? 128u : 64u) > n) return fail_decode("bad DQT");
                    for (int i = 0; i < 64; ++i)
                        qt[tq][kZigzag[i]] = pq ? static_cast<uint16_t>(load_be16(s + o + 1 + 2 * i)) : s[o + 1 + static_cast<size_t>(i)];
                    qt_present[tq] = true;
                    o += 1 + (pq ? 128u : 64u);
                }
                break;
            }
            case 0xdd:
                if (n >= 2) restart_interval = load_be16(s);
                break;
            case 0xee:
                if (n >= 12 && std::memcmp(s, "Adobe", 5) == 0) adobe_transform = s[11];
                break;
            case 0xda: {
                const uint8_t* pos = p + len;
                const br_status st = decode_scan(s, n, pos);
                if (st != BR_OK) return st;
                p = pos;
                continue;
            }
            default:
                break;
            }
            p += len;
        }
        if (!frame_seen) return fail_decode("JPEG has no frame");

        if (coef_mode) {
            for (Comp& c : comps) {
                for (uint32_t by = 0; by < c.bh; ++by)
                    for (uint32_t bx = 0; bx < c.bw; ++bx)
                        idct_store(&c.coef[(static_cast<size_t>(by) * c.bw + bx) * 64], qt[c.tq],
                                   &c.plane[(static_cast<size_t>(by) * 8 * c.bw + bx) * 8], static_cast<size_t>(c.bw) * 8);
                std::vector<int16_t>().swap(c.coef);
            }
        }
        return convert(out);
    }

    br_status convert(br_mut_image_view& out) {
        const size_t nc = comps.size();
        const br_pixel_format fmt = nc == 1 ? BR_PIXEL_GRAY8 : BR_PIXEL_RGB8;
        // Билинейные координаты для компонентов с пониженной частотой дискретизации.
        struct Map { std::vector<uint32_t> i0, i1; std::vector<float> f; };
        std::vector<Map> mx(nc), my(nc);
        for (size_t k = 0; k < nc; ++k) {
            const Comp& c = comps[k];
            // Для компонентов полного разрешения выборка из плоскости выполняется напрямую.
            if (c.h == hmax && c.v == vmax) continue;
            auto build = [](Map& m, uint32_t out_n, uint32_t in_n, double ratio) {
                m.i0.resize(out_n); m.i1.resize(out_n); m.f.resize(out_n);
                for (uint32_t i = 0; i < out_n; ++i) {
                    double pos = (i + 0.5) * ratio - 0.5;
                    if (pos < 0) pos = 0;
                    uint32_t a = static_cast<uint32_t>(pos);
                    if (a >= in_n) a = in_n - 1;
                    const uint32_t b = std::min(a + 1, in_n - 1);
                    m.i0[i] = a; m.i1[i] = b; m.f[i] = static_cast<float>(pos - a);
                }
            };
            build(mx[k], width, c.cw, static_cast<double>(c.h) / hmax);
            build(my[k], height, c.ch, static_cast<double>(c.v) / vmax);
        }
        const bool rgb_ids = nc == 3 && comps[0].id == 'R' && comps[1].id == 'G' && comps[2].id == 'B';
        const bool ycc = nc >= 3 && (adobe_transform >= 0 ? adobe_transform != 0 : !(rgb_ids || nc == 4));
        std::vector<float> samp(nc);
        // Выделяет выходной буфер после временных аллокаций, которые могут завершиться ошибкой: представление не освобождается автоматически.
        br_mut_image_view img = alloc_image(width, height, fmt, false);
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t* d = row_ptr(img, y);
            for (uint32_t x = 0; x < width; ++x) {
                for (size_t k = 0; k < nc; ++k) {
                    const Comp& c = comps[k];
                    const size_t stride = static_cast<size_t>(c.bw) * 8;
                    if (c.h == hmax && c.v == vmax) {
                        samp[k] = c.plane[static_cast<size_t>(y) * stride + x];
                        continue;
                    }
                    const uint8_t* r0 = &c.plane[static_cast<size_t>(my[k].i0[y]) * stride];
                    const uint8_t* r1 = &c.plane[static_cast<size_t>(my[k].i1[y]) * stride];
                    const uint32_t a = mx[k].i0[x], b = mx[k].i1[x];
                    const float fx = mx[k].f[x], fy = my[k].f[y];
                    const float top = r0[a] + (r0[b] - r0[a]) * fx;
                    const float bot = r1[a] + (r1[b] - r1[a]) * fx;
                    samp[k] = top + (bot - top) * fy;
                }
                if (nc == 1) { d[x] = clamp_u8(samp[0]); continue; }
                float r, g, b;
                if (ycc) {
                    const float Y = samp[0], cb = samp[1] - 128.0f, cr = samp[2] - 128.0f;
                    r = Y + 1.402f * cr;
                    g = Y - 0.344136286f * cb - 0.714136286f * cr;
                    b = Y + 1.772f * cb;
                } else {
                    r = samp[0]; g = samp[1]; b = samp[2];
                }
                if (nc == 4) {
                    // В Adobe CMYK цвета инвертированы; YCCK сначала преобразуется в инвертированный CMY.
                    const float k = samp[3];
                    if (adobe_transform == 2) { r = 255.0f - r; g = 255.0f - g; b = 255.0f - b; }
                    r = std::clamp(r, 0.0f, 255.0f) * k / 255.0f;
                    g = std::clamp(g, 0.0f, 255.0f) * k / 255.0f;
                    b = std::clamp(b, 0.0f, 255.0f) * k / 255.0f;
                }
                d[3 * x] = clamp_u8(r);
                d[3 * x + 1] = clamp_u8(g);
                d[3 * x + 2] = clamp_u8(b);
            }
        }
        out = img;
        return BR_OK;
    }
};

}

br_status probe_jpeg(const uint8_t* data, size_t size, br_image_info& info) {
    if (size < 4 || data[0] != 0xff || data[1] != 0xd8) return fail(BR_E_DECODE, "not a JPEG file");
    size_t p = 2;
    while (p + 4 <= size) {
        if (data[p] != 0xff) { ++p; continue; }
        const uint8_t m = data[p + 1];
        if (m == 0xff) { ++p; continue; }
        if (m == 0xd8 || m == 0x01 || (m >= 0xd0 && m <= 0xd7)) { p += 2; continue; }
        const size_t len = load_be16(data + p + 2);
        if ((m >= 0xc0 && m <= 0xcf) && m != 0xc4 && m != 0xc8 && m != 0xcc) {
            if (p + 4 + 6 > size) break;
            const uint8_t* s = data + p + 4;
            info = {};
            info.format = BR_ENCODE_JPEG;
            info.height = load_be16(s + 1);
            info.width = load_be16(s + 3);
            info.channels = s[5] == 1 ? 1 : 3;
            info.bit_depth = s[0];
            info.progressive = m == 0xc2;
            return BR_OK;
        }
        p += 2 + len;
    }
    return fail(BR_E_DECODE, "JPEG frame header not found");
}

br_status decode_jpeg(const uint8_t* data, size_t size, br_mut_image_view& out) {
    auto dec = std::make_unique<Decoder>();
    dec->data = data;
    dec->size = size;
    return dec->run(out);
}

}
