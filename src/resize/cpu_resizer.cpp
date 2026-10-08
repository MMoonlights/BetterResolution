#include "resize/cpu_resizer.hpp"

#include "core/common.hpp"
#include "core/frame.hpp"
#include "core/thread_pool.hpp"
#include "resize/kernels.hpp"
#include "resize/resize_simd.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace br::resize {
namespace {

constexpr int kEncSteps = 16384; // разрешение таблицы кодирования (рабочий диапазон [0, 1])

double srgb_to_linear_d(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double linear_to_srgb_d(double v) {
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

struct Luts {
    float dec_identity[256];
    float dec_srgb_to_linear[256];
    float dec_linear_to_srgb[256];
    uint8_t enc_identity[kEncSteps + 1];
    uint8_t enc_linear_to_srgb[kEncSteps + 1];
    uint8_t enc_srgb_to_linear[kEncSteps + 1];
    Luts() {
        for (int i = 0; i < 256; ++i) {
            const double v = i / 255.0;
            dec_identity[i] = static_cast<float>(v);
            dec_srgb_to_linear[i] = static_cast<float>(srgb_to_linear_d(v));
            dec_linear_to_srgb[i] = static_cast<float>(linear_to_srgb_d(v));
        }
        for (int i = 0; i <= kEncSteps; ++i) {
            const double v = static_cast<double>(i) / kEncSteps;
            enc_identity[i] = static_cast<uint8_t>(std::lround(v * 255.0));
            enc_linear_to_srgb[i] = static_cast<uint8_t>(std::lround(linear_to_srgb_d(v) * 255.0));
            enc_srgb_to_linear[i] = static_cast<uint8_t>(std::lround(srgb_to_linear_d(v) * 255.0));
        }
    }
};

const Luts& luts() {
    static const Luts l;
    return l;
}

inline int enc_index(float v) noexcept {
    // Ограничивает значение диапазоном [0, kEncSteps], в том числе при NaN.
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return kEncSteps;
    return static_cast<int>(v * static_cast<float>(kEncSteps) + 0.5f);
}

// Для копирования и POINT также нужно преобразовать гамму и альфа-канал. Сначала преобразуем
// цвет без предварительного умножения на альфа-канал, чтобы не затемнять прозрачные пиксели.
void convert_native(const br_image_view& src, const br_mut_image_view& dst) {
    const bool src_linear = src.color_space == BR_COLOR_LINEAR_SRGB;
    const bool dst_linear = dst.color_space == BR_COLOR_LINEAR_SRGB;
    if (src_linear == dst_linear) { convert_image(src, dst); return; }
    const auto& t = luts();
    const float* transfer = src_linear ? t.dec_linear_to_srgb : t.dec_srgb_to_linear;
    const uint32_t sc = channels_for(src.format), dc = channels_for(dst.format);
    for (uint32_t y = 0; y < src.height; ++y) {
        const auto* s = row_ptr(src, y); auto* d = row_ptr(dst, y);
        for (uint32_t x = 0; x < src.width; ++x, s += sc, d += dc) {
            uint8_t pixel[4]{};
            const uint32_t alpha = sc == 4 ? s[3] : 255;
            for (uint32_t c = 0; c < std::min(sc, 3u); ++c) {
                const uint32_t v = sc == 4 && src.premultiplied_alpha ?
                    (alpha ? std::min(255u, (uint32_t(s[c]) * 255 + alpha / 2) / alpha) : 0) : s[c];
                pixel[c] = static_cast<uint8_t>(std::lround(transfer[v] * 255.0f));
            }
            pixel[3] = static_cast<uint8_t>(alpha);
            convert_row(pixel, src.format, d, dst.format, 1);
            if (dc == 4 && dst.premultiplied_alpha)
                for (uint32_t c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>((uint32_t(d[c]) * alpha + 127) / 255);
        }
    }
}

struct Job {
    const br_image_view* src{};
    const br_mut_image_view* dst{};
    const ResizePlan* plan{};
    const SimdKernels* k{};
    const float* dec{};      // таблица декодирования цветового канала
    const uint8_t* enc{};    // таблица кодирования цветового канала
    uint32_t sc{};           // каналы исходника (1, 3, 4)
    uint32_t L{};            // каналов float на пиксель (1 или 4)
    bool premul_in{};
    bool unpremul_out{};
    bool unpremul_transfer_in{}; // сначала декодировать цвет исходника без умножения на альфа-канал
    bool repremul_transfer_out{}; // закодировать цвет, затем умножить байты результата на альфа-канал
    bool antiring_h{};
    bool antiring_v{};
    bool fast_io{};          // без изменения гаммы и альфа-канала, число каналов совпадает: SIMD u8<->float
};

// Декодирует строку y исходного изображения и добавляет её в acc (in_w * L значений float) или заменяет его.
void decode_row(const Job& j, uint32_t y, float* acc, bool accumulate, float* scratch) {
    const uint8_t* s = row_ptr(*j.src, y);
    const uint32_t w = j.src->width;
    const uint32_t nx = j.plan->shrink_x;
    if (j.fast_io) {
        const uint32_t L = j.L;
        if (nx == 1 && !accumulate) {
            j.k->u8_to_f32(s, acc, static_cast<size_t>(w) * L);
            return;
        }
        j.k->u8_to_f32(s, scratch, static_cast<size_t>(w) * L);
        const float* p = scratch;
        float* d = acc;
        uint32_t cnt = 0;
        for (uint32_t x = 0; x < w; ++x, p += L) {
            if (accumulate || cnt) for (uint32_t l = 0; l < L; ++l) d[l] += p[l];
            else for (uint32_t l = 0; l < L; ++l) d[l] = p[l];
            if (++cnt == nx) { cnt = 0; d += L; }
        }
        return;
    }
    const float* dec = j.dec;
    const float* lin = luts().dec_identity;
    if (j.L == 1) {
        if (nx == 1 && !accumulate) {
            for (uint32_t x = 0; x < w; ++x) acc[x] = dec[s[x]];
            return;
        }
        for (uint32_t x = 0, ox = 0, cnt = 0; x < w; ++x) {
            if (accumulate || cnt) acc[ox] += dec[s[x]]; else acc[ox] = dec[s[x]];
            if (++cnt == nx) { cnt = 0; ++ox; }
        }
        return;
    }
    const uint32_t sc = j.sc;
    float* d = acc;
    uint32_t cnt = 0;
    for (uint32_t x = 0; x < w; ++x, s += sc) {
        float r = dec[s[0]], g = dec[s[1]], b = dec[s[2]], a = 1.0f;
        if (sc == 4) {
            a = lin[s[3]];
            if (j.unpremul_transfer_in) {
                if (s[3]) {
                    const auto straight=[&](uint8_t v){return std::min(255u,(uint32_t(v)*255+s[3]/2)/s[3]);};
                    r=dec[straight(s[0])]*a;g=dec[straight(s[1])]*a;b=dec[straight(s[2])]*a;
                } else r=g=b=0.0f;
            } else if (j.premul_in) { r *= a; g *= a; b *= a; }
        }
        if (accumulate || cnt) { d[0] += r; d[1] += g; d[2] += b; d[3] += a; }
        else { d[0] = r; d[1] = g; d[2] = b; d[3] = a; }
        if (++cnt == nx) { cnt = 0; d += 4; }
    }
}

// Возвращает строку r предварительно уменьшенного входного изображения в формате float.
void load_row(const Job& j, uint32_t r, float* acc, float* scratch) {
    const ResizePlan& p = *j.plan;
    if (p.shrink_x == 1 && p.shrink_y == 1) {
        decode_row(j, r, acc, false, scratch);
        return;
    }
    const uint32_t y0 = r * p.shrink_y;
    const uint32_t y1 = std::min(y0 + p.shrink_y, j.src->height);
    for (uint32_t y = y0; y < y1; ++y) decode_row(j, y, acc, y != y0, scratch);
    const uint32_t rows = y1 - y0;
    const uint32_t L = j.L;
    for (uint32_t ox = 0; ox < p.in_w; ++ox) {
        const uint32_t x0 = ox * p.shrink_x;
        const uint32_t cols = std::min(p.shrink_x, j.src->width - x0);
        const float inv = 1.0f / static_cast<float>(rows * cols);
        for (uint32_t l = 0; l < L; ++l) acc[ox * L + l] *= inv;
    }
}

void store_row(const Job& j, const float* in, uint8_t* out, uint32_t w) {
    if (j.fast_io) {
        j.k->f32_to_u8(in, out, static_cast<size_t>(w) * j.L);
        return;
    }
    const uint8_t* enc = j.enc;
    const uint8_t* enc_lin = luts().enc_identity;
    if (j.L == 1) {
        for (uint32_t x = 0; x < w; ++x) out[x] = enc[enc_index(in[x])];
        return;
    }
    const uint32_t sc = j.sc;
    for (uint32_t x = 0; x < w; ++x, in += 4, out += sc) {
        float r = in[0], g = in[1], b = in[2];
        const float a = std::clamp(in[3], 0.0f, 1.0f);
        if (j.unpremul_out) {
            if (a > (0.5f / 255.0f)) {
                const float inv = 1.0f / a;
                r *= inv; g *= inv; b *= inv;
            } else {
                r = g = b = 0.0f;
            }
        }
        out[0] = enc[enc_index(r)];
        out[1] = enc[enc_index(g)];
        out[2] = enc[enc_index(b)];
        if (j.repremul_transfer_out) {
            const uint32_t alpha=enc_lin[enc_index(a)];
            for(uint32_t c=0;c<3;++c)out[c]=static_cast<uint8_t>((uint32_t(out[c])*alpha+127)/255);
        }
        if (sc == 4) out[3] = enc_lin[enc_index(a)];
    }
}

void run_band(const Job& j, uint32_t y0, uint32_t y1) {
    const ResizePlan& p = *j.plan;
    const AxisSampler& hs = p.horizontal;
    const AxisSampler& vs = p.vertical;
    const uint32_t L = j.L;
    const uint32_t dw = j.dst->width;
    const size_t row_floats = static_cast<size_t>(dw) * L;
    const uint32_t T = vs.taps;

    std::vector<float> acc(static_cast<size_t>(p.in_w) * L + 4);
    std::vector<float> scratch(j.fast_io && (p.shrink_x > 1 || p.shrink_y > 1) ? static_cast<size_t>(j.src->width) * L + 4 : 0);
    std::vector<float> ring(row_floats * T);
    std::vector<int64_t> ring_id(T, -1);
    std::vector<float> out(row_floats);
    std::vector<const float*> rows(T);
    const bool convert = j.dst->format != j.src->format;
    std::vector<uint8_t> tmp(convert ? static_cast<size_t>(dw) * j.sc : 0);

    for (uint32_t y = y0; y < y1; ++y) {
        const int32_t first = vs.start[y];
        for (uint32_t k = 0; k < T; ++k) {
            const int64_t r = first + static_cast<int64_t>(k);
            const size_t slot = static_cast<size_t>(r % T);
            float* dst_row = ring.data() + slot * row_floats;
            if (ring_id[slot] != r) {
                load_row(j, static_cast<uint32_t>(r), acc.data(), scratch.data());
                if (L == 4) j.k->h4(acc.data(), dst_row, hs, j.antiring_h);
                else j.k->h1(acc.data(), dst_row, hs, j.antiring_h);
                ring_id[slot] = r;
            }
            rows[k] = dst_row;
        }
        j.k->v(out.data(), row_floats, rows.data(), vs.weights.data() + static_cast<size_t>(y) * T, T,
               vs.lobe_begin[y], vs.lobe_end[y], j.antiring_v);
        uint8_t* d = row_ptr(*j.dst, y);
        if (convert) {
            store_row(j, out.data(), tmp.data(), dw);
            convert_row(tmp.data(), j.src->format, d, j.dst->format, dw);
        } else {
            store_row(j, out.data(), d, dw);
        }
    }
}

// Целочисленный путь для байтовых изображений в пространстве гаммы. Для трёх каналов используется
// одна временная расширенная строка, а не полноразмерное изображение float; BGRA читается напрямую.
void run_fixed_band(const Job& j, uint32_t y0, uint32_t y1) {
    const ResizePlan& p = *j.plan;
    const auto& hs = p.horizontal;
    const auto& vs = p.vertical;
    const auto& kernels = fixed::kernels();
    const uint32_t lanes = j.sc == 1 ? 1 : 4;
    const size_t n = static_cast<size_t>(j.dst->width) * lanes;
    const uint32_t taps = vs.taps;
    std::vector<int16_t> ring(n * taps);
    std::vector<int64_t> ids(taps, -1);
    std::vector<const int16_t*> rows(taps);
    const br_pixel_format work_format = j.sc == 3 ?
        (j.src->format == BR_PIXEL_RGB8 ? BR_PIXEL_RGBA8 : BR_PIXEL_BGRA8) : j.src->format;
    std::vector<uint8_t> expanded(j.sc == 3 ? static_cast<size_t>(j.src->width) * 4 : 0);
    const bool convert = j.dst->format != work_format;
    std::vector<uint8_t> tmp(convert ? n : 0);
    for (uint32_t y = y0; y < y1; ++y) {
        for (uint32_t k = 0; k < taps; ++k) {
            const uint32_t r = static_cast<uint32_t>(vs.start[y]) + k;
            const size_t slot = r % taps;
            int16_t* d = ring.data() + slot * n;
            if (ids[slot] != r) {
                const uint8_t* s = row_ptr(*j.src, r);
                if (j.sc == 3) {
                    convert_row(s, j.src->format, expanded.data(), work_format, j.src->width);
                    s = expanded.data();
                }
                if (lanes == 4) kernels.h4(s, d, hs, p.fixed_horizontal);
                else kernels.h1(s, d, hs, p.fixed_horizontal);
                ids[slot] = r;
            }
            rows[k] = d;
        }
        uint8_t* d = row_ptr(*j.dst, y);
        kernels.v(convert ? tmp.data() : d, n, rows.data(),
                  p.fixed_vertical.weights.data() + static_cast<size_t>(y) * taps, taps);
        if (convert) convert_row(tmp.data(), work_format, d, j.dst->format, j.dst->width);
    }
}

void resize_point(const br_image_view& src, const br_mut_image_view& dst) {
    const AxisSampler hs = build_axis_sampler(src.width, dst.width, BR_FILTER_POINT);
    const AxisSampler vs = build_axis_sampler(src.height, dst.height, BR_FILTER_POINT);
    const uint32_t c = channels_for(src.format);
    std::vector<uint8_t> tmp(static_cast<size_t>(dst.width) * c);
    for (uint32_t y = 0; y < dst.height; ++y) {
        if(y && vs.start[y] == vs.start[y-1]) {
            std::memmove(row_ptr(dst,y),row_ptr(dst,y-1),static_cast<size_t>(dst.width)*channels_for(dst.format));
            continue;
        }
        const uint8_t* s = row_ptr(src, static_cast<uint32_t>(vs.start[y]));
        uint8_t* t = tmp.data();
        for (uint32_t x = 0; x < dst.width; ++x, t += c)
            std::memcpy(t, s + static_cast<size_t>(hs.start[x]) * c, c);
        br_image_view source_row{tmp.data(),dst.width,1,static_cast<ptrdiff_t>(tmp.size()),
                                 src.format,src.color_space,src.premultiplied_alpha};
        auto destination_row = dst;
        destination_row.data = row_ptr(dst,y); destination_row.height = 1;
        convert_native(source_row,destination_row);
    }
}

}

std::shared_ptr<const ResizePlan> CpuResizer::get_plan(const PlanKey& key) {
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        for (size_t i = 0; i < cache_.size(); ++i) {
            if (cache_[i].first == key) {
                auto plan = cache_[i].second;
                if (i) std::rotate(cache_.begin(), cache_.begin() + static_cast<ptrdiff_t>(i), cache_.begin() + static_cast<ptrdiff_t>(i) + 1);
                return plan;
            }
        }
    }
    auto plan = std::make_shared<ResizePlan>();
    plan->shrink_x = key.nx;
    plan->shrink_y = key.ny;
    plan->in_w = (key.sw + key.nx - 1) / key.nx;
    plan->in_h = (key.sh + key.ny - 1) / key.ny;
    plan->horizontal = build_axis_sampler(plan->in_w, 0.0, static_cast<double>(key.sw) / key.nx, key.dw, key.filter);
    plan->vertical = build_axis_sampler(plan->in_h, 0.0, static_cast<double>(key.sh) / key.ny, key.dh, key.filter);
    if (key.nx == 1 && key.ny == 1) {
        plan->fixed_horizontal = fixed::quantize(plan->horizontal);
        plan->fixed_vertical = fixed::quantize(plan->vertical);
        plan->fixed_compatible = fixed::compatible(plan->fixed_horizontal, plan->fixed_vertical);
    }
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_.insert(cache_.begin(), {key, plan});
    if (cache_.size() > 16) cache_.pop_back();
    return plan;
}

br_status CpuResizer::resize(const br_image_view& src, const br_mut_image_view& dst, const br_resize_options& o,
                             ThreadPool& pool, bool allow_fixed) {
    if (!validate_image(src) || !validate_image(dst)) return fail(BR_E_INVALID_ARGUMENT, "invalid source or destination image");
    if (src.width == dst.width && src.height == dst.height) {
        convert_native(src, dst);
        return BR_OK;
    }

    const double sx = static_cast<double>(dst.width) / src.width;
    const double sy = static_cast<double>(dst.height) / src.height;
    const br_filter filter = choose_filter(o.filter, o.mode, sx, sy);
    if (filter < BR_FILTER_BOX || filter > BR_FILTER_POINT) return fail(BR_E_INVALID_ARGUMENT, "unknown filter");
    if (filter == BR_FILTER_POINT) {
        resize_point(src, dst);
        return BR_OK;
    }

    // При изменении размера от серого изображения к цветному (или наоборот) используется
    // временное изображение исходного формата, чтобы в конвейере float оставалось 1 или 4 канала.
    const uint32_t sc = channels_for(src.format);
    const uint32_t dc = channels_for(dst.format);
    if ((sc == 1) != (dc == 1)) {
        Frame tmp;
        if (!tmp.allocate(dst.width, dst.height, src.format)) return fail(BR_E_OUT_OF_MEMORY, "out of memory");
        tmp.color_space = dst.color_space;
        tmp.premultiplied_alpha = dst.premultiplied_alpha != 0;
        br_mut_image_view tv = tmp.mut_view();
        const br_status st = resize(src, tv, o, pool, allow_fixed);
        if (st != BR_OK) return st;
        convert_image(tmp.view(), dst);
        return BR_OK;
    }

    uint32_t nx = 1, ny = 1;
    if (o.multistage && filter != BR_FILTER_BOX) {
        // После предварительного уменьшения BOX финальное ядро сокращает размер как минимум вдвое.
        nx = std::max(1u, static_cast<uint32_t>(std::floor(1.0 / sx / 2.0)));
        ny = std::max(1u, static_cast<uint32_t>(std::floor(1.0 / sy / 2.0)));
    }

    std::shared_ptr<const ResizePlan> plan;
    try {
        plan = get_plan({src.width, src.height, dst.width, dst.height, filter, nx, ny});
    } catch (const Error& e) {
        return fail(e.status, e.what());
    }

    const Luts& t = luts();
    const bool src_linear = src.color_space == BR_COLOR_LINEAR_SRGB;
    const bool dst_linear = dst.color_space == BR_COLOR_LINEAR_SRGB;
    const bool work_linear = o.linear_light || src_linear;
    Job j;
    j.src = &src;
    j.dst = &dst;
    j.plan = plan.get();
    j.k = &simd_kernels();
    j.sc = sc;
    j.L = sc == 1 ? 1 : 4;
    if (work_linear) {
        j.dec = src_linear ? t.dec_identity : t.dec_srgb_to_linear;
        j.enc = dst_linear ? t.enc_identity : t.enc_linear_to_srgb;
    } else {
        j.dec = src_linear ? t.dec_linear_to_srgb : t.dec_identity;
        j.enc = dst_linear ? t.enc_srgb_to_linear : t.enc_identity;
    }
    // Для непрозрачного источника предварительное умножение на альфа-канал не требуется.
    const bool alpha = sc == 4 && has_alpha(src.format) && o.preserve_alpha && !is_opaque(src);
    j.premul_in = alpha && !src.premultiplied_alpha;
    j.unpremul_transfer_in = alpha && src.premultiplied_alpha && j.dec != t.dec_identity;
    j.repremul_transfer_out = alpha && dst.premultiplied_alpha && j.enc != t.enc_identity;
    j.unpremul_out = alpha && (!dst.premultiplied_alpha || j.repremul_transfer_out);
    j.fast_io = !j.premul_in && !j.unpremul_out && j.dec == t.dec_identity && j.enc == t.enc_identity &&
                (sc == 4 || sc == 1);
    j.antiring_h = o.antiring && plan->horizontal.negative_lobes;
    j.antiring_v = o.antiring && plan->vertical.negative_lobes;

    const bool use_fixed = allow_fixed && plan->fixed_compatible &&
        (o.mode == BR_RESIZE_UI_TEXT || o.mode == BR_RESIZE_FAST) &&
        !work_linear && !dst_linear && !alpha && !j.antiring_h && !j.antiring_v;
    const auto run = use_fixed ? run_fixed_band : run_band;

    const uint64_t work = static_cast<uint64_t>(src.width) * src.height + static_cast<uint64_t>(dst.width) * dst.height;
    const uint32_t threads = o.threads ? o.threads : pool.threads();
    uint32_t bands = 1;
    if (threads > 1 && work >= 256u * 1024u) bands = std::min<uint32_t>(threads, dst.height / 8 + 1);

    try {
        if (bands <= 1) {
            run(j, 0, dst.height);
        } else {
            pool.parallel_for(bands, threads, [&](uint32_t b) {
                const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(dst.height) * b / bands);
                const uint32_t y1 = static_cast<uint32_t>(static_cast<uint64_t>(dst.height) * (b + 1) / bands);
                run(j, y0, y1);
            });
        }
    } catch (const std::bad_alloc&) {
        return fail(BR_E_OUT_OF_MEMORY, "out of memory during resize");
    }
    return BR_OK;
}

}
