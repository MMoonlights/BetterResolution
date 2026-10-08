#pragma once
#if defined(_WIN32)

#include "platform/windows/win_common.hpp"

namespace br::win {

// Уменьшение на D3D11 за два прохода с планами коэффициентов CPU; вход B8G8R8A8, выход BGRA8.
class GpuScaler {
public:
    GpuScaler();
    ~GpuScaler();
    GpuScaler(const GpuScaler&) = delete;
    GpuScaler& operator=(const GpuScaler&) = delete;

    br_status init(ID3D11Device* device);
    br_status run(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, uint32_t src_w, uint32_t src_h, uint32_t out_w,
                  uint32_t out_h, br_filter filter, bool linear, bool antiring, br_mut_image_view& dst);

private:
    struct Impl;
    Impl* impl_;
};

}

#endif
