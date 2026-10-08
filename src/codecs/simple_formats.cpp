#include "codecs/simple_formats.hpp"
#include "core/frame.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <vector>

namespace br::codec {
namespace {
constexpr uint64_t kMaxPixels = uint64_t(1) << 28;

br_status check_dims(int64_t w, int64_t h) {
    if (w <= 0 || h <= 0 || w > (1 << 24) || h > (1 << 24)) return fail(BR_E_DECODE, "invalid image dimensions");
    if (static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > kMaxPixels) return fail(BR_E_UNSUPPORTED, "image too large");
    return BR_OK;
}
}


br_status encode_bmp(const br_image_view& img, Bytes& out) {
    if (!validate_image(img)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    const bool alpha = !is_opaque(img);
    const uint32_t bpp = alpha ? 32 : 24;
    const uint32_t row = (img.width * (bpp / 8) + 3u) & ~3u;
    const uint32_t header = 14 + (alpha ? 108 : 40);
    const uint64_t total = static_cast<uint64_t>(header) + static_cast<uint64_t>(row) * img.height;
    if (total > 0xffffffffull) return fail(BR_E_UNSUPPORTED, "image too large for BMP");
    out.clear();
    out.reserve(static_cast<size_t>(total));
    out.push('B');
    out.push('M');
    out.le32(static_cast<uint32_t>(total));
    out.le32(0);
    out.le32(header);
    out.le32(alpha ? 108 : 40);
    out.le32(img.width);
    out.le32(img.height); // Строки снизу вверх.
    out.le16(1);
    out.le16(bpp);
    out.le32(alpha ? 3 : 0); // BI_BITFIELDS или BI_RGB.
    out.le32(row * img.height);
    out.le32(2835); // 72 точки на дюйм.
    out.le32(2835);
    out.le32(0);
    out.le32(0);
    if (alpha) {
        out.le32(0x00ff0000u); // Красный канал.
        out.le32(0x0000ff00u); // Зелёный канал.
        out.le32(0x000000ffu); // Синий канал.
        out.le32(0xff000000u); // Альфа-канал.
        out.append("BGRs", 4); // Сигнатура sRGB в порядке little-endian.
        for (int i = 0; i < 12; ++i) out.le32(0);
    }
    std::vector<uint8_t> line(row, 0);
    const br_pixel_format f = alpha ? BR_PIXEL_BGRA8 : BR_PIXEL_BGR8;
    for (uint32_t y = img.height; y-- > 0;) {
        convert_row(row_ptr(img, y), img.format, line.data(), f, img.width);
        out.append(line.data(), row);
    }
    return BR_OK;
}

namespace {
struct BmpInfo {
    int64_t w{}, h{};
    bool top_down{false};
    uint32_t bpp{}, compression{}, offset{}, palette_off{}, palette_n{}, palette_entry{4};
    uint32_t mask[4]{};
    bool has_alpha_mask{false};
};

br_status parse_bmp(const uint8_t* d, size_t n, BmpInfo& bi) {
    if (n < 26 || d[0] != 'B' || d[1] != 'M') return fail(BR_E_DECODE, "not a BMP file");
    bi.offset = load_le32(d + 10);
    const uint32_t hs = load_le32(d + 14);
    if (hs == 12) {
        bi.w = load_le16(d + 18);
        bi.h = static_cast<int16_t>(load_le16(d + 20));
        bi.bpp = load_le16(d + 24);
        bi.palette_entry = 3;
    } else if (hs >= 40 && n >= 14 + 40) {
        bi.w = static_cast<int32_t>(load_le32(d + 18));
        bi.h = static_cast<int32_t>(load_le32(d + 22));
        bi.bpp = load_le16(d + 28);
        bi.compression = load_le32(d + 30);
        bi.palette_n = load_le32(d + 46);
        if (bi.compression == 3 || bi.compression == 6) {
            const size_t mo = 14 + 40;
            if (hs >= 52 && n >= 14 + 52) {
                for (int i = 0; i < 3; ++i) bi.mask[i] = load_le32(d + mo + 4 * i);
                if (hs >= 56 && n >= 14 + 56) { bi.mask[3] = load_le32(d + mo + 12); bi.has_alpha_mask = bi.mask[3] != 0; }
            } else if (n >= mo + 12) {
                for (int i = 0; i < 3; ++i) bi.mask[i] = load_le32(d + mo + 4 * i);
                if (bi.compression == 6 && n >= mo + 16) { bi.mask[3] = load_le32(d + mo + 12); bi.has_alpha_mask = bi.mask[3] != 0; }
            }
        }
    } else {
        return fail(BR_E_DECODE, "unsupported BMP header");
    }
    bi.palette_off = 14 + hs + ((hs == 40 && (bi.compression == 3 || bi.compression == 6)) ? (bi.compression == 6 ? 16u : 12u) : 0u);
    if (bi.h < 0) { bi.top_down = true; bi.h = -bi.h; }
    if (bi.compression != 0 && bi.compression != 3 && bi.compression != 6) return fail(BR_E_UNSUPPORTED, "compressed BMP is not supported");
    if (bi.bpp != 1 && bi.bpp != 4 && bi.bpp != 8 && bi.bpp != 16 && bi.bpp != 24 && bi.bpp != 32)
        return fail(BR_E_UNSUPPORTED, "unsupported BMP bit depth");
    return check_dims(bi.w, bi.h);
}

inline uint8_t mask_extract(uint32_t v, uint32_t mask) noexcept {
    if (!mask) return 0;
    const int shift = std::countr_zero(mask);
    // Маски могут заканчиваться на бите 31; нельзя сдвигать uint32_t на 32 бита.
    const int bits = std::countr_one(mask >> shift);
    const uint32_t x = (v & mask) >> shift;
    const uint32_t maxv = (bits >= 32) ? 0xffffffffu : ((1u << bits) - 1u);
    return static_cast<uint8_t>((static_cast<uint64_t>(x) * 255u + maxv / 2) / maxv);
}
}

br_status probe_bmp(const uint8_t* d, size_t n, br_image_info& info) {
    BmpInfo bi;
    const br_status st = parse_bmp(d, n, bi);
    if (st != BR_OK) return st;
    info = {};
    info.format = BR_ENCODE_BMP;
    info.width = static_cast<uint32_t>(bi.w);
    info.height = static_cast<uint32_t>(bi.h);
    info.bit_depth = static_cast<uint8_t>(bi.bpp);
    info.has_alpha = bi.bpp == 32;
    info.channels = info.has_alpha ? 4 : 3;
    return BR_OK;
}

br_status decode_bmp(const uint8_t* d, size_t n, br_mut_image_view& out) {
    BmpInfo bi;
    br_status st = parse_bmp(d, n, bi);
    if (st != BR_OK) return st;
    const uint32_t w = static_cast<uint32_t>(bi.w), h = static_cast<uint32_t>(bi.h);
    const size_t row = ((static_cast<size_t>(w) * bi.bpp + 31) / 32) * 4;
    if (bi.offset > n || row * h > n - bi.offset) return fail(BR_E_DECODE, "truncated BMP");
    uint8_t pal[256][4]{};
    if (bi.bpp <= 8) {
        uint32_t count = bi.palette_n ? std::min<uint32_t>(bi.palette_n, 256) : (1u << bi.bpp);
        if (bi.palette_off + static_cast<size_t>(count) * bi.palette_entry > n) return fail(BR_E_DECODE, "truncated BMP palette");
        for (uint32_t i = 0; i < count; ++i) std::memcpy(pal[i], d + bi.palette_off + static_cast<size_t>(i) * bi.palette_entry, 3);
    }
    uint32_t mask[4] = {bi.mask[0], bi.mask[1], bi.mask[2], bi.mask[3]};
    const bool bitfields = bi.compression == 3 || bi.compression == 6;
    if (!bitfields) {
        if (bi.bpp == 16) { mask[0] = 0x7c00; mask[1] = 0x03e0; mask[2] = 0x001f; mask[3] = 0; }
        if (bi.bpp == 32) { mask[0] = 0x00ff0000; mask[1] = 0x0000ff00; mask[2] = 0x000000ff; mask[3] = 0xff000000; }
    }
    // Для BI_RGB альфа-канал учитывается, только если хотя бы один пиксель имеет ненулевую альфу.
    bool alpha = (bi.bpp == 32 && mask[3]) || (bi.bpp == 16 && mask[3]);
    if (alpha && !bi.has_alpha_mask && !bitfields) {
        bool any = false;
        for (uint32_t y = 0; y < h && !any; ++y) {
            const uint8_t* s = d + bi.offset + static_cast<size_t>(y) * row;
            for (uint32_t x = 0; x < w; ++x) if (s[4 * x + 3]) { any = true; break; }
        }
        alpha = any;
    }
    br_mut_image_view img = alloc_image(w, h, alpha ? BR_PIXEL_RGBA8 : BR_PIXEL_RGB8, false);
    const uint32_t oc = alpha ? 4 : 3;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* s = d + bi.offset + static_cast<size_t>(bi.top_down ? y : h - 1 - y) * row;
        uint8_t* o = row_ptr(img, y);
        for (uint32_t x = 0; x < w; ++x, o += oc) {
            uint8_t r, g, b, a = 255;
            switch (bi.bpp) {
            case 1: case 4: case 8: {
                const uint32_t per = 8 / bi.bpp;
                const uint32_t idx = (s[x / per] >> (8 - bi.bpp * (x % per + 1))) & ((1u << bi.bpp) - 1);
                b = pal[idx][0]; g = pal[idx][1]; r = pal[idx][2];
                break;
            }
            case 24:
                b = s[3 * x]; g = s[3 * x + 1]; r = s[3 * x + 2];
                break;
            case 16: {
                const uint32_t v = load_le16(s + 2 * x);
                r = mask_extract(v, mask[0]); g = mask_extract(v, mask[1]); b = mask_extract(v, mask[2]);
                if (alpha) a = mask_extract(v, mask[3]);
                break;
            }
            default: {
                const uint32_t v = load_le32(s + 4 * x);
                r = mask_extract(v, mask[0]); g = mask_extract(v, mask[1]); b = mask_extract(v, mask[2]);
                if (alpha) a = mask_extract(v, mask[3]);
                break;
            }
            }
            o[0] = r; o[1] = g; o[2] = b;
            if (alpha) o[3] = a;
        }
    }
    out = img;
    return BR_OK;
}

namespace {
inline uint32_t qoi_hash(const uint8_t* p) noexcept { return (p[0] * 3u + p[1] * 5u + p[2] * 7u + p[3] * 11u) & 63u; }
}

br_status encode_qoi(const br_image_view& img, Bytes& out) {
    if (!validate_image(img)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    const bool alpha = !is_opaque(img);
    out.clear();
    out.reserve(static_cast<size_t>(img.width) * img.height + 64);
    out.append("qoif", 4);
    out.be32(img.width);
    out.be32(img.height);
    out.push(alpha ? 4 : 3);
    out.push(0);
    uint8_t index[64][4]{};
    uint8_t prev[4] = {0, 0, 0, 255};
    uint32_t run = 0;
    std::vector<uint8_t> line(static_cast<size_t>(img.width) * 4);
    const uint64_t total = static_cast<uint64_t>(img.width) * img.height;
    uint64_t n = 0;
    for (uint32_t y = 0; y < img.height; ++y) {
        convert_row(row_ptr(img, y), img.format, line.data(), BR_PIXEL_RGBA8, img.width);
        for (uint32_t x = 0; x < img.width; ++x, ++n) {
            const uint8_t* px = &line[4u * x];
            if (std::memcmp(px, prev, 4) == 0) {
                ++run;
                if (run == 62 || n + 1 == total) { out.push(static_cast<uint8_t>(0xc0 | (run - 1))); run = 0; }
                continue;
            }
            if (run) { out.push(static_cast<uint8_t>(0xc0 | (run - 1))); run = 0; }
            const uint32_t h = qoi_hash(px);
            if (std::memcmp(index[h], px, 4) == 0) {
                out.push(static_cast<uint8_t>(h));
            } else {
                std::memcpy(index[h], px, 4);
                if (px[3] == prev[3]) {
                    const int8_t dr = static_cast<int8_t>(px[0] - prev[0]);
                    const int8_t dg = static_cast<int8_t>(px[1] - prev[1]);
                    const int8_t db = static_cast<int8_t>(px[2] - prev[2]);
                    const int8_t drg = static_cast<int8_t>(dr - dg), dbg = static_cast<int8_t>(db - dg);
                    if (dr > -3 && dr < 2 && dg > -3 && dg < 2 && db > -3 && db < 2) {
                        out.push(static_cast<uint8_t>(0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2)));
                    } else if (drg > -9 && drg < 8 && dg > -33 && dg < 32 && dbg > -9 && dbg < 8) {
                        out.push(static_cast<uint8_t>(0x80 | (dg + 32)));
                        out.push(static_cast<uint8_t>(((drg + 8) << 4) | (dbg + 8)));
                    } else {
                        out.push(0xfe); out.push(px[0]); out.push(px[1]); out.push(px[2]);
                    }
                } else {
                    out.push(0xff); out.append(px, 4);
                }
            }
            std::memcpy(prev, px, 4);
        }
    }
    if (run) out.push(static_cast<uint8_t>(0xc0 | (run - 1)));
    static const uint8_t end[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    out.append(end, 8);
    return BR_OK;
}

br_status probe_qoi(const uint8_t* d, size_t n, br_image_info& info) {
    if (n < 14 || std::memcmp(d, "qoif", 4) != 0) return fail(BR_E_DECODE, "not a QOI file");
    info = {};
    info.format = BR_ENCODE_QOI;
    info.width = load_be32(d + 4);
    info.height = load_be32(d + 8);
    info.channels = d[12] == 4 ? 4 : 3;
    info.has_alpha = d[12] == 4;
    info.bit_depth = 8;
    return check_dims(info.width, info.height);
}

br_status decode_qoi(const uint8_t* d, size_t n, br_mut_image_view& out) {
    br_image_info info;
    br_status st = probe_qoi(d, n, info);
    if (st != BR_OK) return st;
    const bool alpha = info.channels == 4;
    br_mut_image_view img = alloc_image(info.width, info.height, alpha ? BR_PIXEL_RGBA8 : BR_PIXEL_RGB8, false);
    uint8_t index[64][4]{};
    uint8_t px[4] = {0, 0, 0, 255};
    size_t p = 14;
    uint32_t run = 0;
    const uint32_t oc = alpha ? 4 : 3;
    for (uint32_t y = 0; y < info.height; ++y) {
        uint8_t* o = row_ptr(img, y);
        for (uint32_t x = 0; x < info.width; ++x, o += oc) {
            if (run) {
                --run;
            } else if (p < n) {
                const uint8_t b = d[p++];
                if (b == 0xfe) {
                    if (p + 3 > n) break;
                    px[0] = d[p]; px[1] = d[p + 1]; px[2] = d[p + 2]; p += 3;
                } else if (b == 0xff) {
                    if (p + 4 > n) break;
                    std::memcpy(px, d + p, 4); p += 4;
                } else if ((b & 0xc0) == 0x00) {
                    std::memcpy(px, index[b], 4);
                } else if ((b & 0xc0) == 0x40) {
                    px[0] = static_cast<uint8_t>(px[0] + ((b >> 4) & 3) - 2);
                    px[1] = static_cast<uint8_t>(px[1] + ((b >> 2) & 3) - 2);
                    px[2] = static_cast<uint8_t>(px[2] + (b & 3) - 2);
                } else if ((b & 0xc0) == 0x80) {
                    if (p >= n) break;
                    const uint8_t b2 = d[p++];
                    const int dg = (b & 0x3f) - 32;
                    px[0] = static_cast<uint8_t>(px[0] + dg - 8 + ((b2 >> 4) & 15));
                    px[1] = static_cast<uint8_t>(px[1] + dg);
                    px[2] = static_cast<uint8_t>(px[2] + dg - 8 + (b2 & 15));
                } else {
                    run = b & 0x3f;
                }
                std::memcpy(index[qoi_hash(px)], px, 4);
            }
            std::memcpy(o, px, oc);
        }
    }
    out = img;
    return BR_OK;
}


br_status encode_pnm(const br_image_view& img, Bytes& out) {
    if (!validate_image(img)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    const bool gray = img.format == BR_PIXEL_GRAY8;
    char hdr[64];
    const int len = std::snprintf(hdr, sizeof(hdr), "P%c\n%u %u\n255\n", gray ? '5' : '6', img.width, img.height);
    out.clear();
    out.reserve(static_cast<size_t>(len) + static_cast<size_t>(img.width) * img.height * (gray ? 1 : 3));
    out.append(hdr, static_cast<size_t>(len));
    std::vector<uint8_t> line(static_cast<size_t>(img.width) * 3);
    for (uint32_t y = 0; y < img.height; ++y) {
        if (gray) { out.append(row_ptr(img, y), img.width); continue; }
        convert_row(row_ptr(img, y), img.format, line.data(), BR_PIXEL_RGB8, img.width);
        out.append(line.data(), line.size());
    }
    return BR_OK;
}

namespace {
struct PnmHeader { uint32_t w{}, h{}, maxval{}; bool gray{}; size_t data{}; };

br_status parse_pnm(const uint8_t* d, size_t n, PnmHeader& ph) {
    if (n < 3 || d[0] != 'P' || (d[1] != '5' && d[1] != '6')) return fail(BR_E_DECODE, "not a binary PNM file");
    ph.gray = d[1] == '5';
    size_t p = 2;
    uint32_t vals[3];
    for (int i = 0; i < 3; ++i) {
        for (;;) {
            while (p < n && (d[p] == ' ' || d[p] == '\t' || d[p] == '\r' || d[p] == '\n')) ++p;
            if (p < n && d[p] == '#') { while (p < n && d[p] != '\n') ++p; continue; }
            break;
        }
        if (p >= n || d[p] < '0' || d[p] > '9') return fail(BR_E_DECODE, "bad PNM header");
        uint64_t v = 0;
        while (p < n && d[p] >= '0' && d[p] <= '9') { v = v * 10 + (d[p++] - '0'); if (v > 0xffffffffu) return fail(BR_E_DECODE, "bad PNM header"); }
        vals[i] = static_cast<uint32_t>(v);
    }
    if (p >= n) return fail(BR_E_DECODE, "truncated PNM");
    ++p; // Пропускает один разделитель заголовка: байты пикселей тоже могут быть пробельными.
    ph.w = vals[0]; ph.h = vals[1]; ph.maxval = vals[2];
    ph.data = p;
    if (!ph.maxval || ph.maxval > 65535) return fail(BR_E_DECODE, "bad PNM maxval");
    return check_dims(ph.w, ph.h);
}
}

br_status probe_pnm(const uint8_t* d, size_t n, br_image_info& info) {
    PnmHeader ph;
    const br_status st = parse_pnm(d, n, ph);
    if (st != BR_OK) return st;
    info = {};
    info.format = BR_ENCODE_PNM;
    info.width = ph.w;
    info.height = ph.h;
    info.channels = ph.gray ? 1 : 3;
    info.bit_depth = ph.maxval > 255 ? 16 : 8;
    return BR_OK;
}

br_status decode_pnm(const uint8_t* d, size_t n, br_mut_image_view& out) {
    PnmHeader ph;
    br_status st = parse_pnm(d, n, ph);
    if (st != BR_OK) return st;
    const uint32_t c = ph.gray ? 1 : 3;
    const uint32_t bps = ph.maxval > 255 ? 2 : 1;
    const size_t need = static_cast<size_t>(ph.w) * ph.h * c * bps;
    if (n - ph.data < need) return fail(BR_E_DECODE, "truncated PNM");
    br_mut_image_view img = alloc_image(ph.w, ph.h, ph.gray ? BR_PIXEL_GRAY8 : BR_PIXEL_RGB8, false);
    const uint8_t* s = d + ph.data;
    for (uint32_t y = 0; y < ph.h; ++y) {
        uint8_t* o = row_ptr(img, y);
        for (uint32_t i = 0; i < ph.w * c; ++i) {
            const uint32_t v = bps == 2 ? load_be16(s + 2 * i) : s[i];
            o[i] = static_cast<uint8_t>((static_cast<uint64_t>(v) * 255u + ph.maxval / 2) / ph.maxval);
        }
        s += static_cast<size_t>(ph.w) * c * bps;
    }
    out = img;
    return BR_OK;
}

}
