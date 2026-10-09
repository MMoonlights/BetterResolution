#pragma once
#include <br/br.h>
#include "core/common.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace br::observe {
inline std::vector<br_rect_i32> ocr_tiles(uint32_t width,uint32_t height,uint32_t edge,uint32_t overlap,size_t max_tiles=4096) {
    if(!width||!height||width>INT32_MAX||height>INT32_MAX||!edge||edge>INT32_MAX||overlap>=edge||!max_tiles)
        raise(BR_E_INVALID_ARGUMENT,"invalid OCR tile geometry");
    std::vector<br_rect_i32> tiles;
    for(uint32_t y=0;;) {
        const uint32_t h=std::min(edge,height-y);
        for(uint32_t x=0;;) {
            const uint32_t w=std::min(edge,width-x);
            if(tiles.size()>=max_tiles)raise(BR_E_UNSUPPORTED,"OCR tile limit exceeded");
            tiles.push_back({int32_t(x),int32_t(y),int32_t(w),int32_t(h)});
            if(uint64_t(x)+w>=width)break;
            x+=edge-overlap;
        }
        if(uint64_t(y)+h>=height)break;
        y+=edge-overlap;
    }
    return tiles;
}
inline bool same_ocr_region(br_rect_i32 a,br_rect_i32 b) {
    if(a.width<=0||a.height<=0||b.width<=0||b.height<=0)return false;
    const int64_t w=std::min(int64_t(a.x)+a.width,int64_t(b.x)+b.width)-std::max(a.x,b.x);
    const int64_t h=std::min(int64_t(a.y)+a.height,int64_t(b.y)+b.height)-std::max(a.y,b.y);
    if(w<=0||h<=0)return false;
    const double area=double(w)*h;
    return area>=0.6*std::min(double(a.width)*a.height,double(b.width)*b.height);
}
}
