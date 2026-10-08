#include "resize/sampler.hpp"
#include "resize/kernels.hpp"
#include "core/common.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace br::resize {

AxisSampler build_axis_sampler(uint32_t in_size, double in_offset, double in_extent, uint32_t out_size, br_filter filter) {
    if (!in_size || !out_size || !(in_extent > 0.0)) raise(BR_E_INVALID_ARGUMENT, "invalid sampler dimensions");

    AxisSampler out;
    out.in_size = in_size;
    out.out_size = out_size;
    out.filter = filter;

    const double step = in_extent / static_cast<double>(out_size); // пикселей исходника на выходной пиксель
    const bool point = filter == BR_FILTER_POINT;
    if (point) {
        // POINT использует точные индексы и постоянные веса, не выделяя память для каждого пикселя.
        out.taps = 1;
        out.start.resize(out_size);
        out.weights.assign(out_size, 1.0f);
        out.lobe_begin.assign(out_size, 0);
        out.lobe_end.assign(out_size, 1);
        out.negative_lobes = false;
        const int32_t last = static_cast<int32_t>(in_size) - 1;
        for (uint32_t d = 0; d < out_size; ++d) {
            const double pos = in_offset + (static_cast<double>(d) + 0.5) * step;
            out.start[d] = std::clamp(static_cast<int32_t>(std::floor(pos)), 0, last);
        }
        return out;
    }
    const bool box = filter == BR_FILTER_BOX;
    const double kernel_scale = std::min(1.0, 1.0 / step);           // < 1 расширяет ядро при уменьшении
    const double support = kernel_support(filter) / kernel_scale;

    // Сначала собираются исходные (не перенесённые к границе) веса каждого выходного пикселя, чтобы определить число выборок.
    struct Raw { int32_t lo, hi; std::vector<double> w; };
    std::vector<Raw> raw(out_size);
    uint32_t taps = 1;
    const int32_t last = static_cast<int32_t>(in_size) - 1;

    for (uint32_t d = 0; d < out_size; ++d) {
        const double pos = in_offset + (static_cast<double>(d) + 0.5) * step; // непрерывная координата
        const double center = pos - 0.5;                                      // в координатах индексов выборок
        Raw& r = raw[d];
        int32_t left = static_cast<int32_t>(std::ceil(center - support - 1e-9));
        int32_t right = static_cast<int32_t>(std::floor(center + support + 1e-9));
        if (right < left) right = left;
        if (static_cast<int64_t>(right) - left > 65535) raise(BR_E_UNSUPPORTED, "resize factor too large");

        // Переносит выборки за пределами изображения к его краям.
        const int32_t lo = std::clamp(left, 0, last), hi = std::clamp(right, 0, last);
        r.lo = lo;
        r.hi = hi;
        r.w.assign(static_cast<size_t>(hi - lo + 1), 0.0);
        double sum = 0.0;
        for (int32_t i = left; i <= right; ++i) {
            double w;
            if (box) {
                // Точная доля площади входного пикселя [i, i+1), покрытая областью выходного пикселя.
                const double half = 0.5 * std::max(1.0, step);
                const double a = std::max(static_cast<double>(i), pos - half);
                const double b = std::min(static_cast<double>(i) + 1.0, pos + half);
                w = std::max(0.0, b - a);
            } else {
                w = kernel_value(filter, (static_cast<double>(i) - center) * kernel_scale);
            }
            r.w[static_cast<size_t>(std::clamp(i, 0, last) - lo)] += w;
            sum += w;
        }
        if (std::abs(sum) < 1e-12) {
            std::fill(r.w.begin(), r.w.end(), 0.0);
            const int32_t nearest = std::clamp(static_cast<int32_t>(std::floor(pos)), lo, hi);
            r.w[static_cast<size_t>(nearest - lo)] = 1.0;
            sum = 1.0;
        }
        for (double& w : r.w) w /= sum;
        // Удаляет крайние нулевые веса.
        while (r.hi > r.lo && std::abs(r.w.back()) < 1e-9) { r.w.pop_back(); --r.hi; }
        size_t front = 0;
        while (r.lo + static_cast<int32_t>(front) < r.hi && std::abs(r.w[front]) < 1e-9) ++front;
        if (front) { r.w.erase(r.w.begin(), r.w.begin() + static_cast<ptrdiff_t>(front)); r.lo += static_cast<int32_t>(front); }
        taps = std::max<uint32_t>(taps, static_cast<uint32_t>(r.hi - r.lo + 1));
    }

    taps = std::min(taps, in_size);
    out.taps = taps;
    out.start.resize(out_size);
    out.weights.assign(static_cast<size_t>(out_size) * taps, 0.0f);
    out.lobe_begin.resize(out_size);
    out.lobe_end.resize(out_size);
    out.negative_lobes = false;

    for (uint32_t d = 0; d < out_size; ++d) {
        const Raw& r = raw[d];
        const int32_t start = std::clamp(r.lo, 0, static_cast<int32_t>(in_size - taps));
        out.start[d] = start;
        float* w = &out.weights[static_cast<size_t>(d) * taps];
        size_t peak = 0;
        double peak_w = -1e300;
        for (size_t k = 0; k < r.w.size(); ++k) {
            const size_t slot = static_cast<size_t>(r.lo - start) + k;
            w[slot] = static_cast<float>(r.w[k]);
            if (r.w[k] < -1e-7) out.negative_lobes = true;
            if (r.w[k] > peak_w) { peak_w = r.w[k]; peak = slot; }
        }
        size_t b = peak, e = peak + 1;
        while (b > 0 && w[b - 1] > 0.0f) --b;
        while (e < taps && w[e] > 0.0f) ++e;
        out.lobe_begin[d] = static_cast<uint16_t>(b);
        out.lobe_end[d] = static_cast<uint16_t>(e);
    }
    return out;
}

}
