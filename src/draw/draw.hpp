#pragma once

#include <br/br.h>

namespace br::draw {
br_status ellipse(const br_mut_image_view&, br_rect_i32, uint32_t argb, int32_t thickness);

br_status rect(const br_mut_image_view& img, br_rect_i32 r, uint32_t argb, int32_t thickness);
br_status line(const br_mut_image_view& img, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t argb);
br_status text(const br_mut_image_view& img, int32_t x, int32_t y, const char* s, uint32_t argb, uint32_t bg, uint32_t scale);
void measure(const char* s, uint32_t scale, uint32_t& w, uint32_t& h) noexcept;
br_status grid(const br_mut_image_view& img, const br_grid_options& o);
br_status marks(const br_mut_image_view& img, const br_rect_i32* rects, size_t n, uint32_t first, uint32_t argb);

br_grid_options default_grid() noexcept;

}
