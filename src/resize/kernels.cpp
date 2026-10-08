#include "resize/kernels.hpp"

#include <algorithm>
#include <cmath>

namespace br::resize {
namespace {
constexpr double pi = 3.1415926535897932384626433832795;

double sinc(double x) noexcept {
    if (std::abs(x) < 1e-12) return 1.0;
    const double p = pi * x;
    return std::sin(p) / p;
}

// Семейство фильтров Mitchell-Netravali: (B, C) = (1, 0) B-сплайн, (0, 0.5) Catmull-Rom, (1/3, 1/3) Mitchell.
double cubic_bc(double x, double B, double C) noexcept {
    x = std::abs(x);
    if (x < 1.0) {
        return ((12.0 - 9.0 * B - 6.0 * C) * x * x * x +
                (-18.0 + 12.0 * B + 6.0 * C) * x * x +
                (6.0 - 2.0 * B)) / 6.0;
    }
    if (x < 2.0) {
        return ((-B - 6.0 * C) * x * x * x +
                (6.0 * B + 30.0 * C) * x * x +
                (-12.0 * B - 48.0 * C) * x +
                (8.0 * B + 24.0 * C)) / 6.0;
    }
    return 0.0;
}
}

double kernel_support(br_filter filter) noexcept {
    switch (filter) {
    case BR_FILTER_POINT:
    case BR_FILTER_BOX: return 0.5;
    case BR_FILTER_TRIANGLE: return 1.0;
    case BR_FILTER_CUBIC_BSPLINE:
    case BR_FILTER_CATMULL_ROM:
    case BR_FILTER_MITCHELL:
    case BR_FILTER_LANCZOS2: return 2.0;
    case BR_FILTER_LANCZOS3: return 3.0;
    case BR_FILTER_LANCZOS4: return 4.0;
    default: return 2.0;
    }
}

double kernel_value(br_filter filter, double x) noexcept {
    const double ax = std::abs(x);
    switch (filter) {
    case BR_FILTER_POINT:
    case BR_FILTER_BOX: return ax <= 0.5 ? 1.0 : 0.0;
    case BR_FILTER_TRIANGLE: return std::max(0.0, 1.0 - ax);
    case BR_FILTER_CUBIC_BSPLINE: return cubic_bc(x, 1.0, 0.0);
    case BR_FILTER_CATMULL_ROM: return cubic_bc(x, 0.0, 0.5);
    case BR_FILTER_MITCHELL: return cubic_bc(x, 1.0 / 3.0, 1.0 / 3.0);
    case BR_FILTER_LANCZOS2: return ax < 2.0 ? sinc(x) * sinc(x / 2.0) : 0.0;
    case BR_FILTER_LANCZOS3: return ax < 3.0 ? sinc(x) * sinc(x / 3.0) : 0.0;
    case BR_FILTER_LANCZOS4: return ax < 4.0 ? sinc(x) * sinc(x / 4.0) : 0.0;
    default: return cubic_bc(x, 1.0 / 3.0, 1.0 / 3.0);
    }
}

br_filter choose_filter(br_filter requested, br_resize_mode mode, double sx, double sy) noexcept {
    if (requested != BR_FILTER_AUTO) return requested;
    const double s = std::min(sx, sy);
    const bool down = s < 1.0;
    switch (mode) {
    case BR_RESIZE_FAST: return down ? BR_FILTER_BOX : BR_FILTER_TRIANGLE;
    case BR_RESIZE_BALANCED: return down ? BR_FILTER_MITCHELL : BR_FILTER_CATMULL_ROM;
    case BR_RESIZE_UI_TEXT: return down ? BR_FILTER_LANCZOS3 : BR_FILTER_CATMULL_ROM;
    case BR_RESIZE_QUALITY:
    default: return s < 0.75 ? BR_FILTER_LANCZOS3 : BR_FILTER_CATMULL_ROM;
    }
}

}
