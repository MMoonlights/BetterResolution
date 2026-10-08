#include "codecs/codec_registry.hpp"

#include "codecs/jpeg.hpp"
#include "codecs/png.hpp"
#include "codecs/simple_formats.hpp"
#include "core/frame.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace br::codec {

bool detect_format(const uint8_t* d, size_t n, br_encoded_format& out) noexcept {
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') { out = BR_ENCODE_PNG; return true; }
    if (n >= 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff) { out = BR_ENCODE_JPEG; return true; }
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') { out = BR_ENCODE_BMP; return true; }
    if (n >= 4 && std::memcmp(d, "qoif", 4) == 0) { out = BR_ENCODE_QOI; return true; }
    if (n >= 2 && d[0] == 'P' && (d[1] == '5' || d[1] == '6')) { out = BR_ENCODE_PNM; return true; }
    return false;
}

bool format_from_extension(const char* path, br_encoded_format& out) noexcept {
    if (!path) return false;
    const char* dot = std::strrchr(path, '.');
    if (!dot) return false;
    char ext[8] = {};
    for (int i = 0; i < 7 && dot[i + 1]; ++i) ext[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(dot[i + 1])));
    struct E { const char* e; br_encoded_format f; };
    static const E table[] = {{"png", BR_ENCODE_PNG}, {"jpg", BR_ENCODE_JPEG}, {"jpeg", BR_ENCODE_JPEG}, {"jpe", BR_ENCODE_JPEG},
                              {"bmp", BR_ENCODE_BMP}, {"dib", BR_ENCODE_BMP}, {"qoi", BR_ENCODE_QOI}, {"ppm", BR_ENCODE_PNM},
                              {"pgm", BR_ENCODE_PNM}, {"pnm", BR_ENCODE_PNM}, {"raw", BR_ENCODE_RAW}, {"rgba", BR_ENCODE_RAW}};
    for (const E& e : table) {
        if (std::strcmp(ext, e.e) == 0) { out = e.f; return true; }
    }
    return false;
}

const char* mime_type(br_encoded_format f) noexcept {
    switch (f) {
    case BR_ENCODE_PNG: return "image/png";
    case BR_ENCODE_JPEG: return "image/jpeg";
    case BR_ENCODE_BMP: return "image/bmp";
    case BR_ENCODE_QOI: return "image/qoi";
    case BR_ENCODE_PNM: return "image/x-portable-anymap";
    default: return "application/octet-stream";
    }
}

br_status encode(const br_image_view& image, const br_encode_options& o, Bytes& out, ThreadPool* pool) {
    if (!validate_image(image)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    switch (o.format) {
    case BR_ENCODE_RAW: {
        const size_t row = static_cast<size_t>(image.width) * channels_for(image.format);
        out.clear();
        out.reserve(row * image.height);
        for (uint32_t y = 0; y < image.height; ++y) out.append(row_ptr(image, y), row);
        return BR_OK;
    }
    case BR_ENCODE_PNG: {
        PngEncodeOptions po;
        po.effort = o.effort;
        po.palette = o.png_palette;
        po.max_colors = o.max_colors ? o.max_colors : 256;
        po.pool = pool;
        return encode_png(image, po, out);
    }
    case BR_ENCODE_JPEG: {
        JpegEncodeOptions jo;
        jo.quality = o.quality ? o.quality : 90;
        jo.subsample_420 = !o.force_444;
        jo.optimize = o.jpeg_optimize != 0;
        jo.pool = pool;
        return encode_jpeg(image, jo, out);
    }
    case BR_ENCODE_BMP: return encode_bmp(image, out);
    case BR_ENCODE_QOI: return encode_qoi(image, out);
    case BR_ENCODE_PNM: return encode_pnm(image, out);
    default: return fail(BR_E_UNSUPPORTED, "unknown output format");
    }
}

br_status probe(const uint8_t* data, size_t size, br_image_info& info) {
    if (!data || !size) return fail(BR_E_INVALID_ARGUMENT, "empty input");
    br_encoded_format f;
    if (!detect_format(data, size, f)) return fail(BR_E_UNSUPPORTED, "unknown image format");
    switch (f) {
    case BR_ENCODE_PNG: return probe_png(data, size, info);
    case BR_ENCODE_JPEG: return probe_jpeg(data, size, info);
    case BR_ENCODE_BMP: return probe_bmp(data, size, info);
    case BR_ENCODE_QOI: return probe_qoi(data, size, info);
    case BR_ENCODE_PNM: return probe_pnm(data, size, info);
    default: return fail(BR_E_UNSUPPORTED, "unknown image format");
    }
}

br_status decode(const uint8_t* data, size_t size, br_pixel_format format, br_mut_image_view& out) {
    if (!data || !size) return fail(BR_E_INVALID_ARGUMENT, "empty input");
    if (format != BR_PIXEL_UNKNOWN && !channels_for(format)) return fail(BR_E_INVALID_ARGUMENT, "invalid pixel format");
    br_encoded_format f;
    if (!detect_format(data, size, f)) return fail(BR_E_UNSUPPORTED, "unknown image format");
    br_mut_image_view img{};
    br_status st;
    switch (f) {
    case BR_ENCODE_PNG: st = decode_png(data, size, img, format); break;
    case BR_ENCODE_JPEG: st = decode_jpeg(data, size, img); break;
    case BR_ENCODE_BMP: st = decode_bmp(data, size, img); break;
    case BR_ENCODE_QOI: st = decode_qoi(data, size, img); break;
    case BR_ENCODE_PNM: st = decode_pnm(data, size, img); break;
    default: return fail(BR_E_UNSUPPORTED, "unknown image format");
    }
    if (st != BR_OK) return st;
    if (format == BR_PIXEL_UNKNOWN || format == img.format) {
        out = img;
        return BR_OK;
    }
    br_mut_image_view conv;
    try {
        conv = alloc_image(img.width, img.height, format, false);
    } catch (...) {
        free_image(img);
        throw;
    }
    convert_image(as_view(img), conv);
    free_image(img);
    out = conv;
    return BR_OK;
}

}
