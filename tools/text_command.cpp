#include <br/br.hpp>
#include <br/br_text.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
uint32_t integer(const std::string& s){
    if(s.empty()||s[0]=='-')throw std::runtime_error("expected non-negative integer");
    size_t end=0;const auto n=std::stoull(s,&end);
    if(end!=s.size()||n>std::numeric_limits<uint32_t>::max())throw std::runtime_error("integer out of range");
    return uint32_t(n);
}
double real(const std::string& s){size_t end=0;const double n=std::stod(s,&end);
    if(end!=s.size()||!std::isfinite(n))throw std::runtime_error("invalid real value");
    return n;}
uint8_t boolean(const std::string& s){
    if(s=="1"||s=="yes"||s=="on")return 1;
    if(s=="0"||s=="no"||s=="off")return 0;
    throw std::runtime_error("expected 0 or 1");
}
br_filter filter(const std::string& s){
    if(s=="auto")return BR_FILTER_AUTO;
    if(s=="area"||s=="box")return BR_FILTER_BOX;
    if(s=="triangle"||s=="bilinear")return BR_FILTER_TRIANGLE;
    if(s=="bspline")return BR_FILTER_CUBIC_BSPLINE;
    if(s=="catmull"||s=="catmull-rom")return BR_FILTER_CATMULL_ROM;
    if(s=="mitchell")return BR_FILTER_MITCHELL;
    if(s=="lanczos2")return BR_FILTER_LANCZOS2;
    if(s=="lanczos3")return BR_FILTER_LANCZOS3;
    if(s=="lanczos4")return BR_FILTER_LANCZOS4;
    if(s=="point"||s=="nearest")return BR_FILTER_POINT;
    throw std::runtime_error("unknown resize filter");
}
br_rect_i32 rectangle(const std::string& s){
    br_rect_i32 r{};int32_t* parts[]={&r.x,&r.y,&r.width,&r.height};size_t off=0;
    for(int i=0;i<4;++i){size_t end=s.find(',',off);if((i<3)==(end==std::string::npos))throw std::runtime_error("region must be x,y,w,h");
        const std::string v=s.substr(off,end-off);size_t used=0;auto n=std::stoll(v,&used);
        if(used!=v.size()||n<INT32_MIN||n>INT32_MAX)throw std::runtime_error("region out of range");
        *parts[i]=int32_t(n);off=end+1;}
    return r;
}
}
int br_text_command(const std::vector<std::string>& args){
    try{
        if(args.size()<2){std::fprintf(stderr,
            "br text <in> <out> [--preset screenshot|ocr] [--scale N | --width W --height H]\n"
            " [--region x,y,w,h] [--gray color|luma|median] [--border N] [--normalize 0..100]\n"
            " [--sharpen 0..100] [--invert auto|yes|no] [--threshold none|otsu|sauvola]\n"
            " [--filter auto|area|triangle|bspline|catmull|mitchell|lanczos2|lanczos3|lanczos4|point]\n"
            " [--antiring 0|1] [--linear-light 0|1] [--multistage 0|1]\n"
            " [--window N] [--k N] [--threads N] [--max-pixels N] [--tiles WxH --overlap N]\n"
            "Output is PNG/PNM/etc from extension. Use PNG for exact text; no OCR is run.\n");return 1;}
        br_text_preset preset=BR_TEXT_SCREENSHOT;
        for(size_t i=2;i<args.size();i+=2){if(i+1>=args.size())throw std::runtime_error("option needs a value");
            if(args[i]=="--preset"){if(args[i+1]=="ocr")preset=BR_TEXT_OCR;else if(args[i+1]!="screenshot")throw std::runtime_error("unknown preset");}}
        auto o=br_text_options_default(preset);auto ro=br_resize_options_for(BR_RESIZE_UI_TEXT);
        br_rect_i32 region{};auto detail=br_detail_options_default();bool tiles=false;
        for(size_t i=2;i<args.size();i+=2){const auto& key=args[i];const auto& v=args[i+1];
            if(key=="--preset")continue;
            else if(key=="--scale")o.scale=float(real(v));
            else if(key=="--width")o.width=integer(v);
            else if(key=="--height")o.height=integer(v);
            else if(key=="--border")o.border=integer(v);
            else if(key=="--normalize")o.normalize=integer(v);
            else if(key=="--sharpen")o.sharpen=integer(v);
            else if(key=="--filter")ro.filter=filter(v);
            else if(key=="--antiring")ro.antiring=boolean(v);
            else if(key=="--linear-light")ro.linear_light=boolean(v);
            else if(key=="--multistage")ro.multistage=boolean(v);
            else if(key=="--threads")o.threads=integer(v);
            else if(key=="--max-pixels")o.max_pixels=integer(v);
            else if(key=="--window")o.window=integer(v);
            else if(key=="--k")o.threshold_k=float(real(v));
            else if(key=="--region")region=rectangle(v);
            else if(key=="--overlap")detail.overlap=integer(v);
            else if(key=="--tiles"){
                const size_t at=v.find('x');if(at==std::string::npos)throw std::runtime_error("tiles must be WxH");
                detail.tile_width=integer(v.substr(0,at));detail.tile_height=integer(v.substr(at+1));tiles=true;
            }else if(key=="--gray"){
                if(v=="color")o.grayscale=BR_TEXT_KEEP_COLOR;
                else if(v=="luma")o.grayscale=BR_TEXT_LUMA;
                else if(v=="median")o.grayscale=BR_TEXT_MEDIAN;else throw std::runtime_error("unknown gray mode");
            }else if(key=="--invert"){
                if(v=="auto")o.invert=-1;else if(v=="yes")o.invert=1;else if(v=="no")o.invert=0;else throw std::runtime_error("unknown invert mode");
            }else if(key=="--threshold"){
                if(v=="none")o.threshold=BR_TEXT_THRESHOLD_NONE;else if(v=="otsu")o.threshold=BR_TEXT_THRESHOLD_OTSU;
                else if(v=="sauvola")o.threshold=BR_TEXT_THRESHOLD_SAUVOLA;else throw std::runtime_error("unknown threshold");
            }else throw std::runtime_error("unknown text option: "+key);
        }
        auto input=br::Image::load(args[0]);const auto view=input.view();
        if(tiles){
            for(size_t i=2;i<args.size();i+=2)
                if(args[i]!="--tiles"&&args[i]!="--overlap"&&args[i]!="--region")
                    throw std::runtime_error("native tiles cannot be combined with preparation options");
            size_t n=0;br::check(br_plan_detail_tiles(view.width,view.height,region,&detail,nullptr,0,&n),"tiles");
            std::vector<br_detail_tile> list(n);br::check(br_plan_detail_tiles(view.width,view.height,region,&detail,list.data(),n,&n),"tiles");
            // В этом режиме out задаёт префикс файлов: точные обрезки исходного размера, всегда PNG, из одного кадра.
            std::printf("{\"tiles\":[");
            for(size_t i=0;i<n;++i){br_image raw{};br_transform t{};
                br::check(br_zoom(nullptr,&view,list[i].source_region,0,0,nullptr,&raw,&t),"native crop");
                br::Image crop(raw);const auto name=args[1]+"-"+std::to_string(i)+".png";crop.save(name,br::Encode::png(1));
                const auto& r=list[i].source_region;
                std::printf("%s{\"index\":%zu,\"region\":[%d,%d,%d,%d],\"image_to_source\":[1,1,%d,%d]}",i?",":"",i,r.x,r.y,r.width,r.height,r.x,r.y);
            }
            std::puts("]}");return 0;
        }
        br_image raw{};br_text_info info{};
        br::check(br_text_prepare_with_resize(nullptr,&view,region,&o,&ro,&raw,&info),"text prepare");br::Image result(raw);result.save(args[1]);
        const auto& t=info.image_to_source;
        const auto& r=info.source_region;const auto& c=info.content_rect;
        std::printf("{\"width\":%u,\"height\":%u,\"source_region\":[%d,%d,%d,%d],\"content_rect\":[%d,%d,%d,%d],\"inverted\":%u,\"threshold\":%u,\"image_to_source\":[%.17g,%.17g,%.17g,%.17g]}\n",
            result.width(),result.height(),r.x,r.y,r.width,r.height,c.x,c.y,c.width,c.height,
            info.inverted,info.threshold,t.sx,t.sy,t.tx,t.ty);return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"br text: %s\n",e.what());return 1;}
}
