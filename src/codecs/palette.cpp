#include "codecs/palette.hpp"
#include "core/frame.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace br::codec {
namespace {

inline uint32_t pack(Rgba c) noexcept {
    return uint32_t(c.r) | (uint32_t(c.g) << 8) | (uint32_t(c.b) << 16) | (uint32_t(c.a) << 24);
}
inline Rgba unpack(uint32_t v) noexcept {
    return {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
}
inline uint32_t mix(uint32_t v) noexcept {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

// Хеш-таблица цветов и индексов с открытой адресацией, не более 256 записей.
struct ColorMap {
    static constexpr uint32_t kSize = 1024;
    uint32_t keys[kSize];
    int16_t vals[kSize];
    ColorMap() { std::fill(std::begin(vals), std::end(vals), int16_t(-1)); }
    int find(uint32_t k) const noexcept {
        for (uint32_t i = mix(k) & (kSize - 1);; i = (i + 1) & (kSize - 1)) {
            if (vals[i] < 0) return -1;
            if (keys[i] == k) return vals[i];
        }
    }
    void insert(uint32_t k, int v) noexcept {
        uint32_t i = mix(k) & (kSize - 1);
        while (vals[i] >= 0 && keys[i] != k) i = (i + 1) & (kSize - 1);
        keys[i] = k;
        vals[i] = static_cast<int16_t>(v);
    }
};

inline int dist2(int r0, int g0, int b0, int a0, const Rgba& c) noexcept {
    const int dr = r0 - c.r, dg = g0 - c.g, db = b0 - c.b, da = a0 - c.a;
    return 3 * dr * dr + 6 * dg * dg + 1 * db * db + 4 * da * da;
}

int nearest(const Palette& p, int r, int g, int b, int a) noexcept {
    int best = 0, bd = 1 << 30;
    for (uint32_t i = 0; i < p.count; ++i) {
        const int d = dist2(r, g, b, a, p.colors[i]);
        if (d < bd) { bd = d; best = static_cast<int>(i); }
    }
    return best;
}

}

Rgba read_rgba(const br_image_view& img, const uint8_t* row, uint32_t x) noexcept {
    switch (img.format) {
    case BR_PIXEL_GRAY8: { const uint8_t v = row[x]; return {v, v, v, 255}; }
    case BR_PIXEL_RGB8: { const uint8_t* p = row + 3u * x; return {p[0], p[1], p[2], 255}; }
    case BR_PIXEL_BGR8: { const uint8_t* p = row + 3u * x; return {p[2], p[1], p[0], 255}; }
    case BR_PIXEL_RGBA8: { const uint8_t* p = row + 4u * x; return {p[0], p[1], p[2], p[3]}; }
    case BR_PIXEL_BGRA8: { const uint8_t* p = row + 4u * x; return {p[2], p[1], p[0], p[3]}; }
    default: return {0, 0, 0, 255};
    }
}

bool exact_palette(const br_image_view& img, uint32_t max_colors, Palette& out) {
    out.count = 0;
    ColorMap map;
    uint32_t last = 0;
    bool have_last = false;
    for (uint32_t y = 0; y < img.height; ++y) {
        const uint8_t* row = row_ptr(img, y);
        for (uint32_t x = 0; x < img.width; ++x) {
            const uint32_t k = pack(read_rgba(img, row, x));
            if (have_last && k == last) continue;
            last = k;
            have_last = true;
            if (map.find(k) >= 0) continue;
            if (out.count >= max_colors) return false;
            map.insert(k, static_cast<int>(out.count));
            out.colors[out.count++] = unpack(k);
        }
    }
    return true;
}

void quantize(const br_image_view& img, uint32_t max_colors, Palette& out) {
    max_colors = std::clamp<uint32_t>(max_colors, 2, 256);
    struct Bin { uint32_t count; uint64_t r, g, b; };
    std::vector<Bin> bins(1u << 15);
    for (uint32_t y = 0; y < img.height; ++y) {
        const uint8_t* row = row_ptr(img, y);
        for (uint32_t x = 0; x < img.width; ++x) {
            const Rgba c = read_rgba(img, row, x);
            Bin& b = bins[(uint32_t(c.r >> 3) << 10) | (uint32_t(c.g >> 3) << 5) | uint32_t(c.b >> 3)];
            b.count++;
            b.r += c.r;
            b.g += c.g;
            b.b += c.b;
        }
    }
    struct Entry { float c[3]; uint32_t count; };
    std::vector<Entry> e;
    for (const Bin& b : bins) {
        if (!b.count) continue;
        const float inv = 1.0f / static_cast<float>(b.count);
        e.push_back({{static_cast<float>(b.r) * inv, static_cast<float>(b.g) * inv, static_cast<float>(b.b) * inv}, b.count});
    }
    if (e.empty()) {
        out.count = 1;
        out.colors[0] = {0, 0, 0, 255};
        return;
    }

    // Диапазоны медианного разрезания: [begin,end).
    struct Box { size_t begin, end; };
    std::vector<Box> boxes{{0, e.size()}};
    auto range_of = [&](const Box& bx, int& axis) {
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (size_t i = bx.begin; i < bx.end; ++i)
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], e[i].c[k]); hi[k] = std::max(hi[k], e[i].c[k]); }
        const float w[3] = {1.7f, 2.4f, 1.0f}; // Перцептивные веса: зелёный > красный > синий.
        float best = -1.0f;
        axis = 0;
        for (int k = 0; k < 3; ++k) {
            const float r = (hi[k] - lo[k]) * w[k];
            if (r > best) { best = r; axis = k; }
        }
        return best;
    };
    while (boxes.size() < max_colors) {
        int best_box = -1, best_axis = 0;
        double best_score = 0.0;
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].end - boxes[i].begin < 2) continue;
            int axis = 0;
            const float r = range_of(boxes[i], axis);
            uint64_t cnt = 0;
            for (size_t j = boxes[i].begin; j < boxes[i].end; ++j) cnt += e[j].count;
            const double score = static_cast<double>(r) * std::sqrt(static_cast<double>(cnt));
            if (score > best_score) { best_score = score; best_box = static_cast<int>(i); best_axis = axis; }
        }
        if (best_box < 0) break;
        Box bx = boxes[static_cast<size_t>(best_box)];
        std::sort(e.begin() + static_cast<ptrdiff_t>(bx.begin), e.begin() + static_cast<ptrdiff_t>(bx.end),
                  [&](const Entry& a, const Entry& b) { return a.c[best_axis] < b.c[best_axis]; });
        uint64_t total = 0;
        for (size_t j = bx.begin; j < bx.end; ++j) total += e[j].count;
        uint64_t acc = 0;
        size_t split = bx.begin + 1;
        for (size_t j = bx.begin; j < bx.end - 1; ++j) {
            acc += e[j].count;
            split = j + 1;
            if (acc * 2 >= total) break;
        }
        boxes[static_cast<size_t>(best_box)] = {bx.begin, split};
        boxes.push_back({split, bx.end});
    }

    out.count = static_cast<uint32_t>(boxes.size());
    std::vector<double> sum(static_cast<size_t>(out.count) * 4);
    auto recompute = [&](const std::vector<int>* assign) {
        std::fill(sum.begin(), sum.end(), 0.0);
        if (!assign) {
            for (size_t bi = 0; bi < boxes.size(); ++bi)
                for (size_t j = boxes[bi].begin; j < boxes[bi].end; ++j)
                    for (int k = 0; k < 3; ++k) { sum[bi * 4 + static_cast<size_t>(k)] += e[j].c[k] * e[j].count; }
            for (size_t bi = 0; bi < boxes.size(); ++bi)
                for (size_t j = boxes[bi].begin; j < boxes[bi].end; ++j) sum[bi * 4 + 3] += e[j].count;
        } else {
            for (size_t j = 0; j < e.size(); ++j) {
                const size_t bi = static_cast<size_t>((*assign)[j]);
                for (int k = 0; k < 3; ++k) sum[bi * 4 + static_cast<size_t>(k)] += e[j].c[k] * e[j].count;
                sum[bi * 4 + 3] += e[j].count;
            }
        }
        for (uint32_t i = 0; i < out.count; ++i) {
            const double n = sum[i * 4 + 3];
            if (n <= 0) continue;
            out.colors[i] = {static_cast<uint8_t>(std::clamp(sum[i * 4] / n + 0.5, 0.0, 255.0)),
                             static_cast<uint8_t>(std::clamp(sum[i * 4 + 1] / n + 0.5, 0.0, 255.0)),
                             static_cast<uint8_t>(std::clamp(sum[i * 4 + 2] / n + 0.5, 0.0, 255.0)), 255};
        }
    };
    recompute(nullptr);
    std::vector<int> assign(e.size());
    for (int it = 0; it < 3; ++it) {
        for (size_t j = 0; j < e.size(); ++j)
            assign[j] = nearest(out, static_cast<int>(e[j].c[0] + 0.5f), static_cast<int>(e[j].c[1] + 0.5f),
                                static_cast<int>(e[j].c[2] + 0.5f), 255);
        recompute(&assign);
    }
}

void map_to_palette(const br_image_view& img, const Palette& pal, uint8_t* indices) {
    ColorMap exact;
    for (uint32_t i = 0; i < pal.count; ++i) exact.insert(pack(pal.colors[i]), static_cast<int>(i));
    // Кэш квантованных цветов с прямым отображением.
    constexpr uint32_t kCache = 4096;
    std::vector<uint32_t> ckey(kCache, 0xffffffffu);
    std::vector<uint8_t> cval(kCache, 0);
    uint32_t last = 0;
    uint8_t last_idx = 0;
    bool have_last = false;
    for (uint32_t y = 0; y < img.height; ++y) {
        const uint8_t* row = row_ptr(img, y);
        uint8_t* out = indices + static_cast<size_t>(y) * img.width;
        for (uint32_t x = 0; x < img.width; ++x) {
            const Rgba c = read_rgba(img, row, x);
            const uint32_t k = pack(c);
            if (have_last && k == last) { out[x] = last_idx; continue; }
            int idx = exact.find(k);
            if (idx < 0) {
                const uint32_t slot = mix(k) & (kCache - 1);
                if (ckey[slot] == k) {
                    idx = cval[slot];
                } else {
                    idx = nearest(pal, c.r, c.g, c.b, c.a);
                    ckey[slot] = k;
                    cval[slot] = static_cast<uint8_t>(idx);
                }
            }
            last = k;
            last_idx = static_cast<uint8_t>(idx);
            have_last = true;
            out[x] = last_idx;
        }
    }
}

}
