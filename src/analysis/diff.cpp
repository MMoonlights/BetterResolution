#include "analysis/diff.hpp"

#include "core/common.hpp"
#include "core/frame.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace br::analysis {
namespace {

struct TileInfo {
    int32_t x0, y0, x1, y1; // Точные границы изменённых пикселей включительно; при отсутствии изменений x0 > x1.
    uint32_t count;
};

inline bool overlaps_with_gap(const br_rect_i32& a, const br_rect_i32& b, int32_t gap) noexcept {
    return a.x <= b.x + b.width + gap && b.x <= a.x + a.width + gap && a.y <= b.y + b.height + gap &&
           b.y <= a.y + a.height + gap;
}

inline br_rect_i32 unite(const br_rect_i32& a, const br_rect_i32& b) noexcept {
    const int32_t x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const int32_t x1 = std::max(a.x + a.width, b.x + b.width), y1 = std::max(a.y + a.height, b.y + b.height);
    return {x0, y0, x1 - x0, y1 - y0};
}

}

br_status diff_images(const br_image_view& a, const br_image_view& b, const br_diff_options& o,
                      std::vector<br_rect_i32>& rects, br_diff_result& res) {
    if (!validate_image(a) || !validate_image(b)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    if (a.width != b.width || a.height != b.height) return fail(BR_E_INVALID_ARGUMENT, "images must have the same size");
    const uint32_t W = a.width, H = a.height;
    const uint32_t tile = std::clamp<uint32_t>(o.tile_size ? o.tile_size : 16, 1, 4096);
    const uint32_t tw = (W + tile - 1) / tile, th = (H + tile - 1) / tile;
    std::vector<TileInfo> tiles(static_cast<size_t>(tw) * th, TileInfo{INT32_MAX, INT32_MAX, -1, -1, 0});

    const bool same_format = a.format == b.format;
    const uint32_t c = same_format ? channels_for(a.format) : 4;
    const size_t row_bytes = static_cast<size_t>(W) * c;
    std::vector<uint8_t> ra, rb;
    if (!same_format) { ra.resize(row_bytes); rb.resize(row_bytes); }
    const int thr = o.threshold;

    uint64_t changed = 0;
    for (uint32_t y = 0; y < H; ++y) {
        const uint8_t* pa;
        const uint8_t* pb;
        if (same_format) {
            pa = row_ptr(a, y);
            pb = row_ptr(b, y);
        } else {
            convert_row(row_ptr(a, y), a.format, ra.data(), BR_PIXEL_RGBA8, W);
            convert_row(row_ptr(b, y), b.format, rb.data(), BR_PIXEL_RGBA8, W);
            pa = ra.data();
            pb = rb.data();
        }
        if (std::memcmp(pa, pb, row_bytes) == 0) continue;
        TileInfo* trow = &tiles[static_cast<size_t>(y / tile) * tw];
        for (uint32_t t = 0; t < tw; ++t) {
            const uint32_t x0 = t * tile, x1 = std::min(W, x0 + tile);
            const size_t off = static_cast<size_t>(x0) * c, len = static_cast<size_t>(x1 - x0) * c;
            if (std::memcmp(pa + off, pb + off, len) == 0) continue;
            TileInfo& ti = trow[t];
            for (uint32_t x = x0; x < x1; ++x) {
                const uint8_t* p = pa + static_cast<size_t>(x) * c;
                const uint8_t* q = pb + static_cast<size_t>(x) * c;
                bool diff = false;
                for (uint32_t k = 0; k < c; ++k) {
                    const int d = static_cast<int>(p[k]) - static_cast<int>(q[k]);
                    if (d > thr || d < -thr) { diff = true; break; }
                }
                if (!diff) continue;
                ++ti.count;
                ++changed;
                ti.x0 = std::min<int32_t>(ti.x0, static_cast<int32_t>(x));
                ti.x1 = std::max<int32_t>(ti.x1, static_cast<int32_t>(x));
                ti.y0 = std::min<int32_t>(ti.y0, static_cast<int32_t>(y));
                ti.y1 = std::max<int32_t>(ti.y1, static_cast<int32_t>(y));
            }
        }
    }

    // Находит связанные группы изменённых плиток, соединяя плитки на расстоянии не более `reach` плиток.
    const int32_t gap = static_cast<int32_t>(o.merge_gap);
    const int32_t reach = std::max<int32_t>(1, (gap + static_cast<int32_t>(tile) - 1) / static_cast<int32_t>(tile));
    std::vector<int32_t> comp(tiles.size(), -1);
    std::vector<br_rect_i32> found;
    std::vector<uint32_t> stack;
    for (uint32_t i = 0; i < tiles.size(); ++i) {
        if (!tiles[i].count || comp[i] >= 0) continue;
        const int32_t id = static_cast<int32_t>(found.size());
        br_rect_i32 r{tiles[i].x0, tiles[i].y0, tiles[i].x1 - tiles[i].x0 + 1, tiles[i].y1 - tiles[i].y0 + 1};
        comp[i] = id;
        stack.assign(1, i);
        while (!stack.empty()) {
            const uint32_t cur = stack.back();
            stack.pop_back();
            const int32_t cx = static_cast<int32_t>(cur % tw), cy = static_cast<int32_t>(cur / tw);
            for (int32_t dy = -reach; dy <= reach; ++dy) {
                for (int32_t dx = -reach; dx <= reach; ++dx) {
                    const int32_t nx = cx + dx, ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= static_cast<int32_t>(tw) || ny >= static_cast<int32_t>(th)) continue;
                    const uint32_t n = static_cast<uint32_t>(ny) * tw + static_cast<uint32_t>(nx);
                    if (!tiles[n].count || comp[n] >= 0) continue;
                    const TileInfo& t = tiles[n];
                    const br_rect_i32 tr{t.x0, t.y0, t.x1 - t.x0 + 1, t.y1 - t.y0 + 1};
                    // Проверка расстояния по пикселям разделяет виджеты, даже если они попали в одну область плитки.
                    const br_rect_i32 cr{tiles[cur].x0, tiles[cur].y0, tiles[cur].x1 - tiles[cur].x0 + 1, tiles[cur].y1 - tiles[cur].y0 + 1};
                    if (!overlaps_with_gap(cr, tr, std::max(gap, 1))) continue;
                    comp[n] = id;
                    r = unite(r, tr);
                    stack.push_back(n);
                }
            }
        }
        found.push_back(r);
    }
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < found.size() && !merged; ++i)
            for (size_t j = i + 1; j < found.size(); ++j)
                if (overlaps_with_gap(found[i], found[j], gap)) {
                    found[i] = unite(found[i], found[j]);
                    found.erase(found.begin() + static_cast<ptrdiff_t>(j));
                    merged = true;
                    break;
                }
    }
    std::sort(found.begin(), found.end(), [](const br_rect_i32& p, const br_rect_i32& q) {
        return p.y != q.y ? p.y < q.y : p.x < q.x;
    });

    res = {};
    res.changed_pixels = changed;
    res.changed_fraction = static_cast<double>(changed) / (static_cast<double>(W) * H);
    res.rect_count = static_cast<uint32_t>(found.size());
    for (const auto& r : found) res.bounds = res.bounds.width ? unite(res.bounds, r) : r;
    rects = std::move(found);
    return BR_OK;
}

}
