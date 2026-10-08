#include <type_traits>
#include <br/br_text.h>
#include "api/context.hpp"
#include "core/common.hpp"
#include "core/frame.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace {
using Hist = std::array<uint64_t, 256>;
constexpr uint64_t kDefaultPixels = uint64_t(64) * 1024 * 1024;
struct Owned {
    br_image image{};
    ~Owned() { br::free_image(image); }
    Owned() = default;
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    br_image release() { auto v = image; image = {}; return v; }
};
int luminance(const uint8_t* p, uint32_t c) {
    return c == 1 ? p[0] : (77 * p[0] + 150 * p[1] + 29 * p[2] + 128) / 256;
}
Hist histogram(const br_image& image) {
    Hist h{};
    const uint32_t c = br::channels_for(image.format);
    for (uint32_t y = 0; y < image.height; ++y) {
        const auto* p = br::row_ptr(image, y);
        for (uint32_t x = 0; x < image.width; ++x, p += c) ++h[luminance(p,c)];
    }
    return h;
}
bool dark_border(const br_image& image) {
    const uint32_t c = br::channels_for(image.format);
    uint64_t dark = 0, count = 0;
    auto sample=[&](uint32_t x,uint32_t y){
        dark+=luminance(br::row_ptr(image,y)+size_t(x)*c,c)<128;++count;
    };
    for(uint32_t x=0;x<image.width;++x){sample(x,0);if(image.height>1)sample(x,image.height-1);}
    for(uint32_t y=1;y+1<image.height;++y){sample(0,y);if(image.width>1)sample(image.width-1,y);}
    return dark * 2 > count;
}
void invert(br_image& image) {
    const uint32_t n = image.width * br::channels_for(image.format);
    for (uint32_t y=0; y<image.height; ++y) {
        auto* p = br::row_ptr(image,y);
        for (uint32_t x=0; x<n; ++x) p[x] = uint8_t(255-p[x]);
    }
}
int rounded_percent(int delta, uint32_t strength) {
    const int v=delta*int(strength);
    return v < 0 ? -((-v+50)/100) : (v+50)/100;
}
void normalize(br_image& image, uint32_t strength) {
    const auto h=histogram(image);
    const uint64_t tail=uint64_t(image.width)*image.height/1000;
    uint64_t sum=0; int low=0, high=255;
    while (low<255 && (sum+=h[size_t(low)])<=tail) ++low;
    sum=0;
    while (high>0 && (sum+=h[size_t(high)])<=tail) --high;
    if (high-low<32) return; // Не усиливает почти однородный шум сенсора или сжатия.
    const int span=std::max(64,high-low); // Усиление не более 4.
    const uint32_t c=br::channels_for(image.format);
    for (uint32_t y=0; y<image.height; ++y) {
        auto* p=br::row_ptr(image,y);
        for (uint32_t x=0; x<image.width; ++x,p+=c) {
            const int l=luminance(p,c);
            const int target=std::clamp(((l-low)*255+span/2)/span,0,255);
            const int d=rounded_percent(target-l,strength);
            for (uint32_t k=0;k<c;++k) p[k]=uint8_t(std::clamp(int(p[k])+d,0,255));
        }
    }
}
// Кольцевой буфер строк: временная память O(width), исходные соседние строки сохраняются даже при записи на месте.
// Общая разница яркости сохраняет цветность. Ограничение локальным минимумом и максимумом предотвращает появление ореолов.
void sharpen(br_image& image, uint32_t strength) {
    const uint32_t w=image.width,c=br::channels_for(image.format);
    struct Row {
        std::vector<uint8_t> color,luma,low,high;
        std::vector<uint16_t> sum;
        int64_t id{-1};
    };
    std::array<Row,3> ring;
    for(auto& row:ring){row.color.resize(size_t(w)*c);row.luma.resize(w);
        row.low.resize(w);row.high.resize(w);row.sum.resize(w);}
    auto load=[&](uint32_t y)->const Row* {
        auto& row=ring[y%3];
        if(row.id!=y){
            std::memcpy(row.color.data(),br::row_ptr(image,y),size_t(w)*c);
            for(uint32_t x=0;x<w;++x)row.luma[x]=uint8_t(luminance(row.color.data()+size_t(x)*c,c));
            for(uint32_t x=0;x<w;++x){
                const int a=row.luma[x?x-1:0],b=row.luma[x],d=row.luma[std::min(x+1,w-1)];
                row.sum[x]=uint16_t(a+2*b+d);row.low[x]=uint8_t(std::min({a,b,d}));row.high[x]=uint8_t(std::max({a,b,d}));
            }
            row.id=y;
        }
        return &row;
    };
    for(uint32_t y=0;y<image.height;++y){
        const Row* rows[3]={load(y?y-1:0),load(y),load(std::min(y+1,image.height-1))};
        auto* out=br::row_ptr(image,y);
        for(uint32_t x=0;x<w;++x){
            const int low=std::min({rows[0]->low[x],rows[1]->low[x],rows[2]->low[x]});
            const int high=std::max({rows[0]->high[x],rows[1]->high[x],rows[2]->high[x]});
            const int blur=(rows[0]->sum[x]+2*rows[1]->sum[x]+rows[2]->sum[x]+8)/16;
            const int l=rows[1]->luma[x];
            const int d=std::clamp(rounded_percent(l-blur,strength),-16,16);
            const int delta=std::clamp(l+d,low,high)-l;
            for(uint32_t k=0;k<c;++k)out[size_t(x)*c+k]=uint8_t(std::clamp(int(rows[1]->color[size_t(x)*c+k])+delta,0,255));
        }
    }
}

uint32_t otsu(br_image& image) {
    const auto hist=histogram(image);
    const uint64_t n=uint64_t(image.width)*image.height;
    double all=0;for(int i=0;i<256;++i)all+=double(hist[size_t(i)])*i;
    uint64_t left=0;double lsum=0,best=-1;uint32_t threshold=127;
    for(uint32_t i=0;i<255;++i){
        left+=hist[i];lsum+=double(hist[i])*i;
        if(!left || left==n)continue;
        const double d=lsum/double(left)-(all-lsum)/double(n-left);
        const double between=double(left)*double(n-left)*d*d;
        if(between>best){best=between;threshold=i;}
    }
    for(uint32_t y=0;y<image.height;++y){
        auto* p=br::row_ptr(image,y);
        for(uint32_t x=0;x<image.width;++x)p[x]=p[x]<=threshold?0:255;
    }
    return threshold;
}
// Скользящие суммы и суммы квадратов столбцов вместо интегральных изображений всего кадра.
void sauvola(br_image& image,uint32_t window,float k) {
    const uint32_t w=image.width,h=image.height,r=window/2;
    std::vector<uint64_t> sums(w),squares(w);
    const size_t slots=std::min(window,h);
    std::vector<uint8_t> history(size_t(w)*slots);
    uint32_t top=0,bottom=0;
    auto add=[&](uint32_t y){
        auto* keep=history.data()+size_t(y%slots)*w;
        const auto* p=br::row_ptr(image,y);
        std::memcpy(keep,p,w);
        for(uint32_t x=0;x<w;++x){sums[x]+=p[x];squares[x]+=uint32_t(p[x])*p[x];}
    };
    for(uint32_t y=0;y<h;++y){
        const uint32_t next_top=y>r?y-r:0,next_bottom=std::min(h,y+r+1);
        while(top<next_top){
            const auto* p=history.data()+size_t(top%slots)*w;
            for(uint32_t x=0;x<w;++x){sums[x]-=p[x];squares[x]-=uint32_t(p[x])*p[x];}
            ++top;
        }
        while(bottom<next_bottom)add(bottom++);
        uint32_t left=0,right=0;uint64_t sum=0,sq=0;
        auto* p=br::row_ptr(image,y);
        for(uint32_t x=0;x<w;++x){
            const uint32_t a=x>r?x-r:0,b=std::min(w,x+r+1);
            while(left<a){sum-=sums[left];sq-=squares[left++];}
            while(right<b){sum+=sums[right];sq+=squares[right++];}
            const double count=double((bottom-top)*(right-left));
            const double mean=double(sum)/count;
            const double sd=std::sqrt(std::max(0.0,double(sq)/count-mean*mean));
            const double threshold=mean*(1.0+double(k)*(sd/128.0-1.0));
            p[x]=double(p[x])<=threshold?0:255;
        }
    }
}
uint8_t linear_to_srgb(uint8_t v){
    const double a=double(v)/255;
    return uint8_t(std::lround(255*(a<=0.0031308?12.92*a:1.055*std::pow(a,1/2.4)-0.055)));
}
void flatten(const br_image_view& src,br_text_gray gray,br_image& dest){
    if(gray==BR_TEXT_KEEP_COLOR && src.color_space!=BR_COLOR_LINEAR_SRGB && br::is_opaque(src)){
        br::convert_image(src,dest);return;
    }
    const uint32_t sc=br::channels_for(src.format),dc=br::channels_for(dest.format);
    const bool bgr=src.format==BR_PIXEL_BGR8||src.format==BR_PIXEL_BGRA8;
    std::array<uint8_t,256> lut{};
    if(src.color_space==BR_COLOR_LINEAR_SRGB)for(int i=0;i<256;++i)lut[size_t(i)]=linear_to_srgb(uint8_t(i));
    for(uint32_t y=0;y<src.height;++y){
        const auto* s=br::row_ptr(src,y);auto* d=br::row_ptr(dest,y);
        for(uint32_t x=0;x<src.width;++x,s+=sc,d+=dc){
            int rgb[3]={s[bgr?2:0],s[sc==1?0:1],s[sc==1?0:(bgr?0:2)]};
            const int a=sc==4?s[3]:255;
            for(int& v:rgb){
                v=src.premultiplied_alpha?std::min(255,v+255-a):(v*a+255*(255-a)+127)/255;
                if(src.color_space==BR_COLOR_LINEAR_SRGB)v=lut[size_t(v)];
            }
            if(gray==BR_TEXT_KEEP_COLOR){for(int j=0;j<3;++j)d[j]=uint8_t(rgb[j]);}
            else if(gray==BR_TEXT_LUMA)d[0]=uint8_t((77*rgb[0]+150*rgb[1]+29*rgb[2]+128)/256);
            else d[0]=uint8_t(rgb[0]+rgb[1]+rgb[2]-std::min({rgb[0],rgb[1],rgb[2]})-std::max({rgb[0],rgb[1],rgb[2]}));
        }
    }
}
bool region_of(uint32_t w,uint32_t h,br_rect_i32& r){
    if(!w||!h||w>(1u<<28)||h>(1u<<28))return false;
    if(!r.x&&!r.y&&!r.width&&!r.height)r={0,0,int32_t(w),int32_t(h)};
    return br::clip_rect(r,w,h);
}
uint32_t dimension(double n){
    if(!std::isfinite(n)||n<0||n>double(1u<<28))br::raise(BR_E_INVALID_ARGUMENT,"text output dimensions out of range");
    return uint32_t(std::max(1.0,std::floor(n+0.5)));
}
}
extern "C" {
br_text_options br_text_options_default(br_text_preset preset){
    br_text_options o{};o.struct_size=sizeof(o);o.scale=1;o.window=25;o.threshold_k=0.2f;
    o.max_pixels=kDefaultPixels;o.sharpen=0;
    if(preset==BR_TEXT_OCR){o.grayscale=BR_TEXT_LUMA;o.scale=2;o.border=8;o.invert=-1;o.normalize=0;o.sharpen=0;}
    return o;
}
br_status br_text_prepare(br_context* ctx,const br_image_view* src,br_rect_i32 region,
    const br_text_options* options,br_image* out,br_text_info* info){
    return br_text_prepare_with_resize(ctx,src,region,options,nullptr,out,info);
}
br_status br_text_prepare_with_resize(br_context* ctx,const br_image_view* src,br_rect_i32 region,
    const br_text_options* options,const br_resize_options* resize_options,br_image* out,br_text_info* info){
    if(!src||!out||out->data||!br::validate_image(*src)||!region_of(src->width,src->height,region))
        return br::fail(BR_E_INVALID_ARGUMENT,"invalid text source, region or nonempty output");
    if(options && options->struct_size!=sizeof(br_text_options))return br::fail(BR_E_INVALID_ARGUMENT,"text options size mismatch");
    const br_text_options o=options?*options:br_text_options_default(BR_TEXT_SCREENSHOT);
    // Проверяем исходное число до чтения enum, чтобы неверный ввод из C не вызывал UB.
    if(resize_options){
        std::underlying_type_t<br_filter> filter{};
        std::underlying_type_t<br_resize_mode> mode{};
        std::memcpy(&filter,&resize_options->filter,sizeof(filter));
        std::memcpy(&mode,&resize_options->mode,sizeof(mode));
        if(static_cast<uint64_t>(filter)>BR_FILTER_POINT||static_cast<uint64_t>(mode)>BR_RESIZE_UI_TEXT)
            return br::fail(BR_E_INVALID_ARGUMENT,"invalid text resize policy");
    }
    auto ro=resize_options?*resize_options:br_resize_options_for(BR_RESIZE_UI_TEXT);
    if(ro.linear_light>1||ro.antiring>1||ro.multistage>1||ro.preserve_alpha>1||ro.threads>64)
        return br::fail(BR_E_INVALID_ARGUMENT,"invalid text resize policy");
    if(o.threads)ro.threads=o.threads;
    ro.preserve_alpha=0; // Исходная альфа сводится перед изменением размера.
    if(o.reserved||o.grayscale<BR_TEXT_KEEP_COLOR||o.grayscale>BR_TEXT_MEDIAN||
       o.threshold<BR_TEXT_THRESHOLD_NONE||o.threshold>BR_TEXT_THRESHOLD_SAUVOLA||
       o.border>256||o.normalize>100||o.sharpen>100||o.invert < -1||o.invert>1||o.threads>64||
       !std::isfinite(o.scale)||o.scale<0.1f||o.scale>8||!std::isfinite(o.threshold_k)||o.threshold_k<0||o.threshold_k>1||
       o.window<3||o.window>127||!(o.window&1)||
       (o.grayscale==BR_TEXT_KEEP_COLOR&&(o.invert==-1||o.threshold!=BR_TEXT_THRESHOLD_NONE)))
        return br::fail(BR_E_INVALID_ARGUMENT,"invalid text options (auto invert/threshold require gray)");
    return br::guarded([&]{
        const uint64_t limit=o.max_pixels?o.max_pixels:kDefaultPixels;
        uint32_t w=o.width,h=o.height;
        if(!w&&!h){w=dimension(double(region.width)*o.scale);h=dimension(double(region.height)*o.scale);}
        else if(!w)w=dimension(double(region.width)*h/region.height);
        else if(!h)h=dimension(double(region.height)*w/region.width);
        const uint64_t ow=uint64_t(w)+2*o.border,oh=uint64_t(h)+2*o.border;
        if(ow>(1u<<28)||oh>(1u<<28)||ow*oh>limit||uint64_t(region.width)*region.height>limit)
            return br::fail(BR_E_UNSUPPORTED,"text pixel budget exceeded");
        br_image_view crop{};
        br_status st=br_image_crop(src,region,&crop);if(st!=BR_OK)return st;
        Owned flat,scaled,padded;
        const auto fmt=o.grayscale==BR_TEXT_KEEP_COLOR?BR_PIXEL_RGB8:BR_PIXEL_GRAY8;
        // Изменяет размер непрозрачных строк BGRA/RGB напрямую, без копии всего кадра в RGB.
        const bool direct=crop.color_space!=BR_COLOR_LINEAR_SRGB &&
            ((o.grayscale==BR_TEXT_KEEP_COLOR && br::is_opaque(crop)) || crop.format==BR_PIXEL_GRAY8);
        br_image_view work=crop;
        br_image* image=&flat.image;
        if(!direct){
            flat.image=br::alloc_image(crop.width,crop.height,fmt,false);
            flatten(crop,o.grayscale,flat.image);work=br::as_view(flat.image);
        }
        if(direct || w!=crop.width || h!=crop.height){
            scaled.image=br::alloc_image(w,h,fmt,false);
            if(w==crop.width&&h==crop.height)br::convert_image(work,scaled.image);
            else {
                st=br_resize(ctx,&work,&scaled.image,&ro);
                if(st!=BR_OK)return st;
            }
            image=&scaled.image;
        }
        br_text_info result{};result.source_region=region;
        result.inverted=o.invert==1||(o.invert==-1&&dark_border(*image));
        if(result.inverted)invert(*image);
        if(o.normalize)normalize(*image,o.normalize);
        if(o.sharpen)sharpen(*image,o.sharpen);
        if(o.threshold==BR_TEXT_THRESHOLD_OTSU)result.threshold=otsu(*image);
        else if(o.threshold==BR_TEXT_THRESHOLD_SAUVOLA)sauvola(*image,o.window,o.threshold_k);
        result.content_rect={int32_t(o.border),int32_t(o.border),int32_t(w),int32_t(h)};
        const double sx=double(region.width)/w,sy=double(region.height)/h;
        result.image_to_source={sx,sy,region.x-double(o.border)*sx,region.y-double(o.border)*sy};
        if(o.border){
            padded.image=br::alloc_image(uint32_t(ow),uint32_t(oh),fmt,false);
            const uint32_t c=br::channels_for(fmt);
            const int fill=dark_border(*image)?0:255;
            std::memset(padded.image.data,fill,size_t(padded.image.stride)*padded.image.height);
            for(uint32_t y=0;y<h;++y)std::memcpy(br::row_ptr(padded.image,y+o.border)+size_t(o.border)*c,br::row_ptr(*image,y),size_t(w)*c);
            *out=padded.release();
        }else *out=image==&flat.image?flat.release():scaled.release();
        if(info)*info=result;
        return BR_OK;
    });
}
br_detail_options br_detail_options_default(void){return {768,768,32,4096};}
br_status br_plan_detail_tiles(uint32_t w,uint32_t h,br_rect_i32 r,const br_detail_options* options,
    br_detail_tile* tiles,size_t capacity,size_t* count){
    if(!count)return br::fail(BR_E_INVALID_ARGUMENT,"null tile count");
    *count=0;const auto o=options?*options:br_detail_options_default();
    if(!region_of(w,h,r)||!o.tile_width||!o.tile_height||o.overlap>=o.tile_width||o.overlap>=o.tile_height)
        return br::fail(BR_E_INVALID_ARGUMENT,"invalid detail geometry");
    const uint32_t tw=std::min(o.tile_width,uint32_t(r.width)),th=std::min(o.tile_height,uint32_t(r.height));
    const uint32_t dx=o.tile_width-o.overlap,dy=o.tile_height-o.overlap;
    const uint64_t nx=1+(uint64_t(r.width)-tw+dx-1)/dx,ny=1+(uint64_t(r.height)-th+dy-1)/dy;
    const uint64_t n=nx*ny;
    if(n>std::numeric_limits<size_t>::max())return br::fail(BR_E_UNSUPPORTED,"detail count overflow");
    *count=size_t(n);
    if(n>(o.max_tiles?o.max_tiles:4096))return br::fail(BR_E_UNSUPPORTED,"detail tile budget exceeded");
    if(!tiles)return BR_OK;
    if(capacity<n)return br::fail(BR_E_BUFFER_TOO_SMALL,"detail tile array too small");
    size_t i=0;
    for(uint64_t y=0;y<ny;++y)for(uint64_t x=0;x<nx;++x){
        const int32_t xx=r.x+int32_t(std::min(x*dx,uint64_t(r.width)-tw));
        const int32_t yy=r.y+int32_t(std::min(y*dy,uint64_t(r.height)-th));
        tiles[i++]={{xx,yy,int32_t(tw),int32_t(th)},{1,1,double(xx),double(yy)}};
    }
    return BR_OK;
}
void br_transform_pixel_center(const br_transform* t,double x,double y,double* ox,double* oy){
    if(!t)return;
    if(ox)*ox=(x+0.5)*t->sx+t->tx-0.5;
    if(oy)*oy=(y+0.5)*t->sy+t->ty-0.5;
}
}
