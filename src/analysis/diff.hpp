#pragma once

#include <br/br.h>

#include <vector>

namespace br::analysis {

// Для изображений одинакового размера возвращает точные границы изменённых пикселей в порядке чтения.
// Объединяет изменённые плитки в группы в пределах `merge_gap`.
br_status diff_images(const br_image_view& before, const br_image_view& after, const br_diff_options& options,
                      std::vector<br_rect_i32>& rects, br_diff_result& result);

}
