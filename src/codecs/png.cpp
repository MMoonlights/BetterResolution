#include "codecs/png.hpp"
#include "codecs/png_filters.hpp"

#include "codecs/deflate.hpp"
#include "codecs/palette.hpp"
#include "core/frame.hpp"
#include "core/cpu.hpp"
#include "core/simd.hpp"
#include "core/thread_pool.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace br::codec {
namespace {

constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
constexpr uint64_t kMaxPixels = uint64_t(1) << 28;

void write_chunk(Bytes& out, const char* type, const uint8_t* data, size_t size) {
    out.be32(static_cast<uint32_t>(size));
    const size_t crc_from = out.size();
    out.append(type, 4);
    out.append(data, size);
    out.be32(crc32(out.data() + crc_from, out.size() - crc_from));
}

using namespace png_detail;

bool all_gray(const br_image_view& img) noexcept {
    if (img.format == BR_PIXEL_GRAY8) return true;
    const uint32_t c = channels_for(img.format);
    for (uint32_t y = 0; y < img.height; ++y) {
        const uint8_t* p = row_ptr(img, y);
        uint32_t x = 0;
#if BR_SIMD_X86
        if (c == 4 && cpu_features().sse2) {
            const __m128i mask = _mm_set1_epi32(255);
            const __m128i zero = _mm_setzero_si128();
            for (; x + 4 <= img.width; x += 4, p += 16) {
                const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
                const __m128i g = _mm_srli_epi32(v, 8), b = _mm_srli_epi32(v, 16);
                const __m128i different = _mm_and_si128(_mm_or_si128(_mm_xor_si128(v, g), _mm_xor_si128(g, b)), mask);
                if (_mm_movemask_epi8(_mm_cmpeq_epi32(different, zero)) != 0xffff) return false;
            }
        }
#endif
        for (; x < img.width; ++x, p += c)
            if (p[0] != p[1] || p[1] != p[2]) return false;
    }
    return true;
}

}

br_status encode_png(const br_image_view& img, const PngEncodeOptions& o, Bytes& out) {
    if (!validate_image(img)) return fail(BR_E_INVALID_ARGUMENT, "invalid image");
    const bool opaque = is_opaque(img);
    const bool gray = all_gray(img);

    Palette pal;
    bool use_pal = false;
    if (o.palette == BR_PALETTE_AUTO) {
        // Непрозрачные палитры оттенков серого с более чем 16 цветами заменяются на GRAY8.
        use_pal = exact_palette(img, gray && opaque ? 16 : 256, pal);
    } else if (o.palette == BR_PALETTE_QUANTIZE) {
        const uint32_t maxc = std::clamp<uint32_t>(o.max_colors ? o.max_colors : 256, 2, 256);
        use_pal = exact_palette(img, maxc, pal);
        if (!use_pal && opaque) {
            quantize(img, maxc, pal);
            use_pal = true;
        }
    }
    if (use_pal && gray && opaque && pal.count > 16) use_pal = false;

    uint8_t color_type, depth = 8;
    uint32_t channels;
    if (use_pal) {
        color_type = 3;
        channels = 1;
        depth = pal.count <= 2 ? 1 : pal.count <= 4 ? 2 : pal.count <= 16 ? 4 : 8;
    } else if (gray && opaque) {
        color_type = 0;
        channels = 1;
    } else if (opaque) {
        color_type = 2;
        channels = 3;
    } else {
        color_type = 6;
        channels = 4;
    }

    const size_t row_bytes = (static_cast<size_t>(img.width) * channels * depth + 7) / 8;
    const size_t bpp = std::max<size_t>(1, channels * depth / 8);

    out.clear();
    out.reserve(static_cast<size_t>(img.width) * img.height / 4 + 1024);
    out.append(kSignature, 8);
    uint8_t ihdr[13];
    ihdr[0] = uint8_t(img.width >> 24); ihdr[1] = uint8_t(img.width >> 16); ihdr[2] = uint8_t(img.width >> 8); ihdr[3] = uint8_t(img.width);
    ihdr[4] = uint8_t(img.height >> 24); ihdr[5] = uint8_t(img.height >> 16); ihdr[6] = uint8_t(img.height >> 8); ihdr[7] = uint8_t(img.height);
    ihdr[8] = depth;
    ihdr[9] = color_type;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    write_chunk(out, "IHDR", ihdr, 13);

    std::vector<uint8_t> indices;
    if (use_pal) {
        uint8_t plte[768];
        uint8_t trns[256];
        uint32_t trns_len = 0;
        for (uint32_t i = 0; i < pal.count; ++i) {
            Rgba color = pal.colors[i];
            // PNG хранит прямую альфу; исходные цвета сохраняются в pal для поиска индекса.
            if (img.premultiplied_alpha && !opaque) {
                const uint32_t a = color.a;
                auto straight = [a](uint8_t v) {
                    return a ? static_cast<uint8_t>(std::min<uint32_t>(255u, (v * 255u + a / 2) / a)) : uint8_t(0);
                };
                color.r = straight(color.r); color.g = straight(color.g); color.b = straight(color.b);
            }
            plte[3 * i] = color.r;
            plte[3 * i + 1] = color.g;
            plte[3 * i + 2] = color.b;
            trns[i] = color.a;
            if (color.a != 255) trns_len = i + 1;
        }
        write_chunk(out, "PLTE", plte, 3u * pal.count);
        if (trns_len) write_chunk(out, "tRNS", trns, trns_len);
        indices.resize(static_cast<size_t>(img.width) * img.height);
        map_to_palette(img, pal, indices.data());
    }

    auto make_row = [&](uint32_t y, uint8_t* cur) {
        const uint8_t* src = row_ptr(img, y);
        if (use_pal) {
            const uint8_t* idx = indices.data() + static_cast<size_t>(y) * img.width;
            if (depth == 8) {
                std::memcpy(cur, idx, img.width);
            } else {
                std::memset(cur, 0, row_bytes);
                const uint32_t per = 8u / depth;
                for (uint32_t x = 0; x < img.width; ++x)
                    cur[x / per] |= static_cast<uint8_t>(idx[x] << (8 - depth * (x % per + 1)));
            }
        } else if (color_type == 0) {
            const uint32_t c = channels_for(img.format);
            for (uint32_t x = 0; x < img.width; ++x) cur[x] = src[static_cast<size_t>(x) * c];
        } else {
            const br_pixel_format f = color_type == 2 ? BR_PIXEL_RGB8 : BR_PIXEL_RGBA8;
            convert_row(src, img.format, cur, f, img.width);
            if (color_type == 6 && img.premultiplied_alpha) {
                for (uint32_t x = 0; x < img.width; ++x) {
                    uint8_t* p = &cur[4u * x];
                    const uint32_t a = p[3];
                    for (int k = 0; k < 3; ++k)
                        p[k] = a ? static_cast<uint8_t>(std::min<uint32_t>(255u, (p[k] * 255u + a / 2) / a)) : 0;
                }
            }
        }
    };
    const bool adaptive = !use_pal && o.effort > 0;
    auto filter_into = [&](const uint8_t* cur, const uint8_t* prev, uint8_t* dst) {
        int best = 0;
        if (adaptive && count_repeats(cur, prev, row_bytes, bpp) * 100 <= row_bytes * o.unfiltered_threshold) {
            uint64_t score[5];
            score_filters(cur, prev, row_bytes, bpp, score);
            for (int t = 1; t <= 4; ++t)
                if (score[t] < score[best]) best = t;
        }
        dst[0] = static_cast<uint8_t>(best);
        if (best == 0) std::memcpy(dst + 1, cur, row_bytes);
        else filter_row(best, cur, prev, row_bytes, bpp, dst + 1);
    };

    const int level = std::clamp(o.effort, 0, 9);
    const size_t line = row_bytes + 1;
    const uint64_t total = static_cast<uint64_t>(line) * img.height;
    const uint32_t threads = o.pool ? o.pool->threads() : 1;
    uint32_t nstrips = 1;
    if (threads > 1 && level > 0 && total >= (1u << 19)) {
        nstrips = static_cast<uint32_t>(std::min<uint64_t>({threads, total / (1u << 18), img.height}));
        nstrips = std::max(1u, nstrips);
    }

    // Записывает длину IDAT после сжатия.
    const size_t len_pos = out.size();
    out.be32(0);
    out.append("IDAT", 4);
    const size_t data_start = out.size();
    if (nstrips == 1) {
        deflate::Deflater z(out, level, true);
        z.set_hint_distances(static_cast<int>(bpp), static_cast<int>(line));
        std::vector<uint8_t> cur(row_bytes), prev_row(row_bytes, 0);
        for (uint32_t y = 0; y < img.height; ++y) {
            make_row(y, cur.data());
            filter_into(cur.data(), prev_row.data(), z.prepare_write(line)); // Для первой строки верхние соседи равны нулю.
            z.commit_write();
            std::swap(cur, prev_row);
        }
        z.finish();
    } else {
        // Предварительно заполняет полосы предыдущими 32 КиБ; синхронная очистка объединяет их в один поток zlib.
        std::vector<uint8_t> filtered(static_cast<size_t>(total));
        std::vector<Bytes> parts(nstrips);
        auto strip_rows = [&](uint32_t i, uint32_t& y0, uint32_t& y1) {
            y0 = static_cast<uint32_t>(static_cast<uint64_t>(img.height) * i / nstrips);
            y1 = static_cast<uint32_t>(static_cast<uint64_t>(img.height) * (i + 1) / nstrips);
        };
        o.pool->parallel_for(nstrips, 0, [&](uint32_t i) {
            uint32_t y0, y1;
            strip_rows(i, y0, y1);
            std::vector<uint8_t> cur(row_bytes), prev_row(row_bytes, 0);
            if (y0 > 0) make_row(y0 - 1, prev_row.data());
            for (uint32_t y = y0; y < y1; ++y) {
                make_row(y, cur.data());
                filter_into(cur.data(), prev_row.data(), filtered.data() + static_cast<size_t>(y) * line);
                std::swap(cur, prev_row);
            }
        });
        o.pool->parallel_for(nstrips, 0, [&](uint32_t i) {
            uint32_t y0, y1;
            strip_rows(i, y0, y1);
            const size_t from = static_cast<size_t>(y0) * line, to = static_cast<size_t>(y1) * line;
            deflate::Deflater z(parts[i], level, false);
            z.set_hint_distances(static_cast<int>(bpp), static_cast<int>(line));
            const size_t dict = std::min<size_t>(from, 32768);
            z.finish_contiguous(filtered.data() + from - dict, to - from + dict, dict, i + 1 == nstrips);
        });
        out.push(0x78);
        out.push(level <= 1 ? 0x01 : level <= 5 ? 0x5e : level <= 6 ? 0x9c : 0xda);
        for (const Bytes& p : parts) out.append(p.data(), p.size());
        out.be32(adler32_update(1, filtered.data(), filtered.size()));
    }
    const size_t data_len = out.size() - data_start;
    if (data_len > 0x7fffffffu) return fail(BR_E_UNSUPPORTED, "PNG data too large");
    out[len_pos] = uint8_t(data_len >> 24);
    out[len_pos + 1] = uint8_t(data_len >> 16);
    out[len_pos + 2] = uint8_t(data_len >> 8);
    out[len_pos + 3] = uint8_t(data_len);
    out.be32(crc32(out.data() + len_pos + 4, data_len + 4));
    write_chunk(out, "IEND", nullptr, 0);
    return BR_OK;
}

namespace {

struct Header {
    uint32_t w{}, h{};
    uint8_t depth{}, ctype{}, interlace{};
};

br_status parse_ihdr(const uint8_t* data, size_t size, Header& hd) {
    if (size < 33 || std::memcmp(data, kSignature, 8) != 0) return fail(BR_E_DECODE, "not a PNG file");
    if (load_be32(data + 8) != 13 || std::memcmp(data + 12, "IHDR", 4) != 0) return fail(BR_E_DECODE, "missing IHDR");
    if (crc32(data + 12, 17) != load_be32(data + 29)) return fail(BR_E_DECODE, "PNG header checksum mismatch");
    const uint8_t* p = data + 16;
    hd.w = load_be32(p);
    hd.h = load_be32(p + 4);
    hd.depth = p[8];
    hd.ctype = p[9];
    hd.interlace = p[12];
    if (!hd.w || !hd.h || hd.w > (1u << 24) || hd.h > (1u << 24)) return fail(BR_E_DECODE, "invalid PNG dimensions");
    if (static_cast<uint64_t>(hd.w) * hd.h > kMaxPixels) return fail(BR_E_UNSUPPORTED, "PNG too large");
    if (p[10] != 0 || p[11] != 0 || hd.interlace > 1) return fail(BR_E_DECODE, "unsupported PNG compression/filter/interlace");
    const uint8_t d = hd.depth;
    bool ok = false;
    switch (hd.ctype) {
    case 0: ok = d == 1 || d == 2 || d == 4 || d == 8 || d == 16; break;
    case 3: ok = d == 1 || d == 2 || d == 4 || d == 8; break;
    case 2: case 4: case 6: ok = d == 8 || d == 16; break;
    default: ok = false;
    }
    if (!ok) return fail(BR_E_DECODE, "invalid PNG color type / bit depth");
    return BR_OK;
}

inline uint32_t channels_of(uint8_t ctype) noexcept {
    switch (ctype) {
    case 0: case 3: return 1;
    case 4: return 2;
    case 2: return 3;
    default: return 4;
    }
}

inline uint8_t to8(uint32_t v16) noexcept { return static_cast<uint8_t>((v16 * 255u + 32767u) / 65535u); }

template <size_t Channels>
bool expand_palette_row(const uint8_t* src, uint8_t* dst, uint32_t width, uint8_t depth,
                        const uint8_t colors[256][4], uint32_t count) noexcept {
    if (depth == 8) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t i = src[x];
            if (i >= count) return false;
            std::memcpy(dst + static_cast<size_t>(x) * Channels, colors[i], Channels);
        }
    } else {
        const uint32_t mask = (1u << depth) - 1;
        for (uint32_t x = 0; x < width; ++x) {
            const size_t bit = static_cast<size_t>(x) * depth;
            const uint32_t i = (src[bit >> 3] >> (8 - depth - (bit & 7))) & mask;
            if (i >= count) return false;
            std::memcpy(dst + static_cast<size_t>(x) * Channels, colors[i], Channels);
        }
    }
    return true;
}

}

br_status probe_png(const uint8_t* data, size_t size, br_image_info& info) {
    Header hd;
    const br_status st = parse_ihdr(data, size, hd);
    if (st != BR_OK) return st;
    info = {};
    info.format = BR_ENCODE_PNG;
    info.width = hd.w;
    info.height = hd.h;
    info.bit_depth = hd.depth;
    info.progressive = hd.interlace;
    bool trns = false;
    for (size_t pos = 8; pos + 12 <= size;) {
        const uint32_t len = load_be32(data + pos);
        if (std::memcmp(data + pos + 4, "tRNS", 4) == 0) trns = true;
        if (std::memcmp(data + pos + 4, "IDAT", 4) == 0) break;
        if (static_cast<uint64_t>(len) + 12 > size - pos) break;
        pos += 12 + static_cast<size_t>(len);
    }
    info.has_alpha = hd.ctype == 4 || hd.ctype == 6 || trns;
    info.channels = info.has_alpha ? 4 : (hd.ctype == 0 ? 1 : 3);
    return BR_OK;
}

br_status decode_png(const uint8_t* data, size_t size, br_mut_image_view& out_img, br_pixel_format requested) {
    Header hd;
    br_status st = parse_ihdr(data, size, hd);
    if (st != BR_OK) return st;

    uint8_t plte[256][3]{};
    uint8_t palpha[256];
    std::fill(std::begin(palpha), std::end(palpha), uint8_t(255));
    uint32_t plte_count = 0;
    bool has_trns = false;
    uint32_t trns_gray = 0, trns_rgb[3] = {0, 0, 0};
    Bytes idat;
    const uint8_t* idat_data = nullptr;
    size_t idat_size = 0;
    bool ended = false;
    bool seen_plte = false, seen_idat = false, idat_ended = false;

    for (size_t pos = 8; pos + 8 <= size;) {
        const uint32_t len = load_be32(data + pos);
        const uint8_t* type = data + pos + 4;
        if (static_cast<uint64_t>(len) + 12 > size - pos) {
            return fail(BR_E_DECODE, "truncated PNG chunk");
        }
        const uint8_t* body = data + pos + 8;
        if (crc32(type, static_cast<size_t>(len) + 4) != load_be32(body + len))
            return fail(BR_E_DECODE, "PNG chunk checksum mismatch");
        const bool is_idat = std::memcmp(type, "IDAT", 4) == 0;
        if (seen_idat && !is_idat) idat_ended = true;
        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (pos != 8) return fail(BR_E_DECODE, "duplicate IHDR");
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            if (seen_plte || seen_idat || has_trns || !len || len % 3 || len > 768 || hd.ctype == 0 || hd.ctype == 4)
                return fail(BR_E_DECODE, "invalid PLTE");
            plte_count = len / 3;
            if (hd.ctype == 3 && plte_count > (1u << hd.depth)) return fail(BR_E_DECODE, "palette exceeds PNG bit depth");
            seen_plte = true;
            for (uint32_t i = 0; i < plte_count; ++i) std::memcpy(plte[i], body + 3 * i, 3);
        } else if (std::memcmp(type, "tRNS", 4) == 0) {
            if (has_trns || seen_idat) return fail(BR_E_DECODE, "invalid tRNS order");
            has_trns = true;
            if (hd.ctype == 3) {
                if (!seen_plte || !len || len > plte_count) return fail(BR_E_DECODE, "invalid palette tRNS");
                for (uint32_t i = 0; i < len; ++i) palpha[i] = body[i];
            } else if (hd.ctype == 0 && len == 2) {
                trns_gray = load_be16(body);
            } else if (hd.ctype == 2 && len == 6) {
                trns_rgb[0] = load_be16(body);
                trns_rgb[1] = load_be16(body + 2);
                trns_rgb[2] = load_be16(body + 4);
            } else {
                return fail(BR_E_DECODE, "invalid tRNS");
            }
        } else if (is_idat) {
            if (idat_ended || (hd.ctype == 3 && !seen_plte)) return fail(BR_E_DECODE, "invalid IDAT order");
            seen_idat = true;
            if (len) {
                if (!idat_data) { idat_data = body; idat_size = len; }
                else {
                    if (idat.empty()) idat.append(idat_data, idat_size);
                    idat.append(body, len);
                    idat_size += len;
                }
            }
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            if (len || !seen_idat) return fail(BR_E_DECODE, "invalid IEND");
            ended = true;
            break;
        } else if (!(type[0] & 32u)) {
            return fail(BR_E_UNSUPPORTED, "unknown critical PNG chunk");
        }
        pos += 12 + static_cast<size_t>(len);
    }
    if (!ended) return fail(BR_E_DECODE, "missing IEND");
    if (!idat.empty()) idat_data = idat.data();
    if (!idat_size) return fail(BR_E_DECODE, "PNG has no image data");
    if (hd.ctype == 3 && !plte_count) return fail(BR_E_DECODE, "palette image without PLTE");

    const uint32_t ch = channels_of(hd.ctype);
    const uint32_t bits_pp = ch * hd.depth;
    const size_t bpp = std::max<uint32_t>(1, bits_pp / 8);

    // Геометрия проходов Adam7.
    struct Pass { uint32_t x0, y0, dx, dy; };
    static constexpr Pass kAdam7[7] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    static constexpr Pass kSingle[1] = {{0, 0, 1, 1}};
    const Pass* passes = hd.interlace ? kAdam7 : kSingle;
    const int npass = hd.interlace ? 7 : 1;
    size_t expected = 0;
    for (int p = 0; p < npass; ++p) {
        const uint32_t pw = (hd.w + passes[p].dx - 1 - passes[p].x0) / passes[p].dx;
        const uint32_t ph = (hd.h + passes[p].dy - 1 - passes[p].y0) / passes[p].dy;
        if (hd.w <= passes[p].x0 || hd.h <= passes[p].y0) continue;
        expected += static_cast<size_t>(ph) * (1 + (static_cast<size_t>(pw) * bits_pp + 7) / 8);
    }

    Bytes raw;
    raw.reserve(expected);
    size_t consumed = 0;
    st = deflate::inflate(idat_data, idat_size, true, raw, expected, &consumed);
    if (st != BR_OK) return st;
    if (consumed != idat_size) return fail(BR_E_DECODE, "trailing PNG compressed data");
    if (raw.size() < expected) return fail(BR_E_DECODE, "PNG image data truncated");

    const bool alpha_out = hd.ctype == 4 || hd.ctype == 6 || has_trns;
    const br_pixel_format nat = alpha_out ? BR_PIXEL_RGBA8 : (hd.ctype == 0 ? BR_PIXEL_GRAY8 : BR_PIXEL_RGB8);
    if (!hd.interlace && hd.ctype == 3) {
        const br_pixel_format dest = requested == BR_PIXEL_UNKNOWN ? nat : requested;
        const uint32_t dc = channels_for(dest);
        uint8_t colors[256][4]{};
        for (uint32_t i = 0; i < plte_count; ++i) {
            const uint8_t rgba[] = {plte[i][0], plte[i][1], plte[i][2], palpha[i]};
            convert_row(rgba, BR_PIXEL_RGBA8, colors[i], dest, 1);
        }
        const size_t rb = (static_cast<size_t>(hd.w) * hd.depth + 7) / 8;
        std::vector<uint8_t> zero(rb, 0);
        br_mut_image_view img = alloc_image(hd.w, hd.h, dest, false);
        const uint8_t* prev = zero.data();
        uint8_t* row = raw.data();
        for (uint32_t y = 0; y < hd.h; ++y, row += rb + 1) {
            if (!unfilter_row(row[0], row + 1, prev, rb, 1)) {
                free_image(img);
                return fail(BR_E_DECODE, "invalid PNG filter type");
            }
            uint8_t* dst = row_ptr(img, y);
            bool valid;
            switch (dc) {
            case 1: valid = expand_palette_row<1>(row + 1, dst, hd.w, hd.depth, colors, plte_count); break;
            case 3: valid = expand_palette_row<3>(row + 1, dst, hd.w, hd.depth, colors, plte_count); break;
            default: valid = expand_palette_row<4>(row + 1, dst, hd.w, hd.depth, colors, plte_count); break;
            }
            if (!valid) {
                free_image(img);
                return fail(BR_E_DECODE, "PNG palette index out of range");
            }
            prev = row + 1;
        }
        out_img = img;
        return BR_OK;
    }
    // Эти строки без чересстрочной развёртки уже соответствуют обычному 8-битному формату пикселей.
    if (!hd.interlace && hd.depth == 8 && !has_trns && (hd.ctype == 0 || hd.ctype == 2 || hd.ctype == 6)) {
        const br_pixel_format dest = requested == BR_PIXEL_UNKNOWN ? nat : requested;
        const size_t rb = static_cast<size_t>(hd.w) * ch;
        std::vector<uint8_t> zero(rb, 0);
        br_mut_image_view img = alloc_image(hd.w, hd.h, dest, false);
        const uint8_t* prev = zero.data();
        uint8_t* row = raw.data();
        for (uint32_t y = 0; y < hd.h; ++y, row += rb + 1) {
            if (!unfilter_row(row[0], row + 1, prev, rb, bpp)) {
                free_image(img);
                return fail(BR_E_DECODE, "invalid PNG filter type");
            }
            convert_row(row + 1, nat, row_ptr(img, y), dest, hd.w);
            prev = row + 1;
        }
        out_img = img;
        return BR_OK;
    }
    const uint32_t oc = channels_for(nat);
    std::vector<uint8_t> prior((static_cast<size_t>(hd.w) * bits_pp + 7) / 8);
    br_mut_image_view img = alloc_image(hd.w, hd.h, nat, false);

    uint8_t* src = raw.data();
    const uint32_t maxv = (1u << hd.depth) - 1;
    const uint32_t gray_scale = hd.depth < 8 ? 255u / maxv : 1;
    for (int p = 0; p < npass; ++p) {
        const Pass& ps = passes[p];
        if (hd.w <= ps.x0 || hd.h <= ps.y0) continue;
        const uint32_t pw = (hd.w + ps.dx - 1 - ps.x0) / ps.dx;
        const uint32_t ph = (hd.h + ps.dy - 1 - ps.y0) / ps.dy;
        const size_t rb = (static_cast<size_t>(pw) * bits_pp + 7) / 8;
        prior.assign(rb, 0);
        const uint8_t* u = prior.data();
        for (uint32_t py = 0; py < ph; ++py) {
            const uint8_t ft = *src++;
            uint8_t* r = src;
            src += rb;
            if (!unfilter_row(ft, r, u, rb, bpp)) {
                free_image(img);
                return fail(BR_E_DECODE, "invalid PNG filter type");
            }
            uint8_t* drow = row_ptr(img, ps.y0 + py * ps.dy);
            for (uint32_t px = 0; px < pw; ++px) {
                uint8_t* d = drow + static_cast<size_t>(ps.x0 + px * ps.dx) * oc;
                uint32_t s[4] = {0, 0, 0, 0};
                if (hd.depth == 8) {
                    for (uint32_t c = 0; c < ch; ++c) s[c] = r[static_cast<size_t>(px) * ch + c];
                } else if (hd.depth == 16) {
                    for (uint32_t c = 0; c < ch; ++c) s[c] = load_be16(r + (static_cast<size_t>(px) * ch + c) * 2);
                } else {
                    const size_t bit = static_cast<size_t>(px) * hd.depth;
                    s[0] = (r[bit >> 3] >> (8 - hd.depth - (bit & 7))) & maxv;
                }
                switch (hd.ctype) {
                case 3: {
                    const uint32_t i = s[0];
                    if (i >= plte_count) {
                        free_image(img);
                        return fail(BR_E_DECODE, "PNG palette index out of range");
                    }
                    d[0] = plte[i][0]; d[1] = plte[i][1]; d[2] = plte[i][2];
                    if (oc == 4) d[3] = palpha[i];
                    break;
                }
                case 0: {
                    const uint8_t v = hd.depth == 16 ? to8(s[0]) : static_cast<uint8_t>(s[0] * gray_scale);
                    if (oc == 1) { d[0] = v; break; }
                    d[0] = d[1] = d[2] = v;
                    d[3] = (has_trns && s[0] == trns_gray) ? 0 : 255;
                    break;
                }
                case 4: {
                    const uint8_t v = hd.depth == 16 ? to8(s[0]) : static_cast<uint8_t>(s[0]);
                    d[0] = d[1] = d[2] = v;
                    d[3] = hd.depth == 16 ? to8(s[1]) : static_cast<uint8_t>(s[1]);
                    break;
                }
                case 2: {
                    for (int c = 0; c < 3; ++c) d[c] = hd.depth == 16 ? to8(s[c]) : static_cast<uint8_t>(s[c]);
                    if (oc == 4) d[3] = (has_trns && s[0] == trns_rgb[0] && s[1] == trns_rgb[1] && s[2] == trns_rgb[2]) ? 0 : 255;
                    break;
                }
                default: {
                    for (int c = 0; c < 4; ++c) d[c] = hd.depth == 16 ? to8(s[c]) : static_cast<uint8_t>(s[c]);
                    break;
                }
                }
            }
            u = r;
        }
    }
    out_img = img;
    return BR_OK;
}

}
