#include <br/br.h>

#include "analysis/delta.hpp"
#include "api/context.hpp"
#include "analysis/diff.hpp"
#include "codecs/codec_registry.hpp"
#include "container/brf.hpp"
#include "core/common.hpp"
#include "core/frame.hpp"
#include "core/io.hpp"
#include "draw/draw.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

using namespace br;

extern "C" {

br_encode_options br_encode_options_default(br_encoded_format format) {
    br_encode_options o{};
    o.format = format;
    o.quality = 90;
    o.strip_metadata = 1;
    o.force_444 = 1;
    o.png_palette = BR_PALETTE_AUTO;
    o.jpeg_optimize = 1;
    o.effort = 1;
    o.max_colors = 256;
    return o;
}

br_status br_encode(const br_image_view* image, const br_encode_options* options, uint8_t* output, size_t capacity,
                    size_t* output_size) {
    if (!image || !output_size) return fail(BR_E_INVALID_ARGUMENT, "null argument");
    const br_encode_options o = options ? *options : br_encode_options_default(BR_ENCODE_PNG);
    return guarded([&] {
        Bytes out;
        const br_status st = codec::encode(*image, o, out, &resolve_context(nullptr)->pool);
        if (st != BR_OK) return st;
        *output_size = out.size();
        if (!output) return BR_OK;
        if (capacity < out.size()) return fail(BR_E_BUFFER_TOO_SMALL, "output buffer too small");
        std::memcpy(output, out.data(), out.size());
        return BR_OK;
    });
}

br_status br_encode_alloc(const br_image_view* image, const br_encode_options* options, uint8_t** out_data, size_t* out_size) {
    if (!image || !out_data || !out_size) return fail(BR_E_INVALID_ARGUMENT, "null argument");
    *out_data = nullptr;
    *out_size = 0;
    const br_encode_options o = options ? *options : br_encode_options_default(BR_ENCODE_PNG);
    return guarded([&] {
        Bytes out;
        const br_status st = codec::encode(*image, o, out, &resolve_context(nullptr)->pool);
        if (st != BR_OK) return st;
        *out_size = out.size();
        if (out.empty()) out.push(0); // Сохраняет действительное выделение памяти для пустого результата RAW.
        *out_data = out.release();
        return BR_OK;
    });
}

br_status br_probe(const void* data, size_t size, br_image_info* info) {
    if (!data || !info) return fail(BR_E_INVALID_ARGUMENT, "null argument");
    return guarded([&] { return codec::probe(static_cast<const uint8_t*>(data), size, *info); });
}

br_status br_decode(const void* data, size_t size, br_pixel_format format, br_image* out) {
    if (!data || !out) return fail(BR_E_INVALID_ARGUMENT, "null argument");
    return guarded([&] { return codec::decode(static_cast<const uint8_t*>(data), size, format, *out); });
}

br_status br_load(const char* path, br_pixel_format format, br_image* out) {
    if (!path || !out) return fail(BR_E_INVALID_ARGUMENT, "null argument");
    return guarded([&] {
        Bytes data;
        const br_status st = read_file(path, data);
        if (st != BR_OK) return st;
        return codec::decode(data.data(), data.size(), format, *out);
    });
}

br_status br_save(const char* path, const br_image_view* image, const br_encode_options* options) {
    if (!path || !image) return fail(BR_E_INVALID_ARGUMENT, "null argument");
    br_encode_options o;
    if (options) {
        o = *options;
    } else {
        br_encoded_format f;
        if (!codec::format_from_extension(path, f)) return fail(BR_E_UNSUPPORTED, "cannot infer image format from file extension");
        o = br_encode_options_default(f);
    }
    return guarded([&] {
        Bytes out;
        const br_status st = codec::encode(*image, o, out, &resolve_context(nullptr)->pool);
        if (st != BR_OK) return st;
        return write_file(path, out.data(), out.size());
    });
}

size_t br_base64_encode(const void* data, size_t size, char* out, size_t cap) {
    const size_t need = base64_size(size) + 1;
    if (out && cap >= need && (data || !size)) {
        base64_encode(static_cast<const uint8_t*>(data), size, out);
        out[need - 1] = '\0';
    }
    return need;
}

br_status br_brf_pack_raw(const br_image_view* image, const br_rect_i32* rois, size_t roi_count, uint8_t* output,
                          size_t capacity, size_t* output_size) {
    if (!image || !output_size || (roi_count && !rois)) return fail(BR_E_INVALID_ARGUMENT, "null argument");
    return guarded([&] {
        auto packed = brf::pack_raw(*image, rois, roi_count);
        *output_size = packed.size();
        if (!output) return BR_OK;
        if (capacity < packed.size()) return fail(BR_E_BUFFER_TOO_SMALL, "output buffer too small");
        std::memcpy(output, packed.data(), packed.size());
        return BR_OK;
    });
}

uint64_t br_hash_image_tiles(const br_image_view* image, uint32_t tw, uint32_t th, uint64_t* out_hashes, size_t capacity,
                             size_t* out_count) {
    if (out_count) *out_count = 0;
    if (!image || !tw || !th || !validate_image(*image)) return 0;
    try {
        auto hashes = analysis::hash_tiles(*image, tw, th);
        if (out_count) *out_count = hashes.size();
        if (out_hashes) std::copy_n(hashes.begin(), std::min(capacity, hashes.size()), out_hashes);
        uint64_t combined = 1469598103934665603ull;
        for (auto h : hashes) {
            combined ^= h;
            combined *= 1099511628211ull;
        }
        return combined;
    } catch (...) {
        return 0;
    }
}

br_diff_options br_diff_options_default(void) {
    br_diff_options o{};
    o.tile_size = 16;
    o.threshold = 0;
    o.merge_gap = 8;
    return o;
}

br_status br_diff(const br_image_view* a, const br_image_view* b, const br_diff_options* opt, br_rect_i32* rects,
                  size_t capacity, br_diff_result* result) {
    if (!a || !b) return fail(BR_E_INVALID_ARGUMENT, "null image");
    const br_diff_options o = opt ? *opt : br_diff_options_default();
    return guarded([&] {
        std::vector<br_rect_i32> found;
        br_diff_result r{};
        const br_status st = analysis::diff_images(*a, *b, o, found, r);
        if (st != BR_OK) return st;
        if (rects) std::copy_n(found.begin(), std::min(capacity, found.size()), rects);
        if (result) *result = r;
        return BR_OK;
    });
}

br_status br_draw_rect(br_mut_image_view* img, br_rect_i32 r, uint32_t argb, int32_t thickness) {
    if (!img) return fail(BR_E_INVALID_ARGUMENT, "null image");
    return draw::rect(*img, r, argb, thickness);
}

br_status br_draw_line(br_mut_image_view* img, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t argb) {
    if (!img) return fail(BR_E_INVALID_ARGUMENT, "null image");
    return draw::line(*img, x0, y0, x1, y1, argb);
}

br_status br_draw_text(br_mut_image_view* img, int32_t x, int32_t y, const char* text, uint32_t argb, uint32_t bg, uint32_t scale) {
    if (!img) return fail(BR_E_INVALID_ARGUMENT, "null image");
    return draw::text(*img, x, y, text, argb, bg, scale);
}

void br_measure_text(const char* text, uint32_t scale, uint32_t* w, uint32_t* h) {
    uint32_t mw = 0, mh = 0;
    draw::measure(text, scale, mw, mh);
    if (w) *w = mw;
    if (h) *h = mh;
}

br_grid_options br_grid_options_default(void) { return draw::default_grid(); }

br_status br_draw_grid(br_mut_image_view* img, const br_grid_options* opt) {
    if (!img) return fail(BR_E_INVALID_ARGUMENT, "null image");
    return draw::grid(*img, opt ? *opt : draw::default_grid());
}

br_status br_draw_marks(br_mut_image_view* img, const br_rect_i32* rects, size_t count, uint32_t first, uint32_t argb) {
    if (!img) return fail(BR_E_INVALID_ARGUMENT, "null image");
    return draw::marks(*img, rects, count, first, argb);
}

}
