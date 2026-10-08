#pragma once

#include <br/br.h>
#include "resize/sampler.hpp"
#include "resize/resize_fixed.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace br {
class ThreadPool;
}

namespace br::resize {

struct ResizePlan {
    uint32_t shrink_x{1}, shrink_y{1}; // целочисленное предварительное уменьшение BOX
    uint32_t in_w{}, in_h{};           // размер после предварительного уменьшения
    AxisSampler horizontal;
    AxisSampler vertical;
    fixed::Axis fixed_horizontal, fixed_vertical;
    bool fixed_compatible{};
};

struct PlanKey {
    uint32_t sw{}, sh{}, dw{}, dh{};
    br_filter filter{};
    uint32_t nx{}, ny{};
    bool operator==(const PlanKey&) const noexcept = default;
};

// Разделённое изменение размера с кэшированием планов и буфером строк O(dst_width * taps).
class CpuResizer {
public:
    // allow_fixed=false включает внутренний эталонный режим для проверки численных регрессий.
    br_status resize(const br_image_view& src, const br_mut_image_view& dst, const br_resize_options& options,
                     ThreadPool& pool, bool allow_fixed = true);

private:
    std::shared_ptr<const ResizePlan> get_plan(const PlanKey& key);

    std::mutex cache_mutex_;
    std::vector<std::pair<PlanKey, std::shared_ptr<const ResizePlan>>> cache_; // сначала самый недавно использованный
};

}
