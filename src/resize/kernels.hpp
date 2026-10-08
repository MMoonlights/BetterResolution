#pragma once
#include <br/br.h>

namespace br::resize {

// Радиус поддержки ядра без масштабирования (в исходных пикселях при масштабе 1).
double kernel_support(br_filter filter) noexcept;
double kernel_value(br_filter filter, double x) noexcept;
br_filter choose_filter(br_filter requested, br_resize_mode mode, double scale_x, double scale_y) noexcept;

}
