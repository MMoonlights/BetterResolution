#include "test_framework.hpp"
#include <br/br_text.h>
#include "core/frame.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {
struct Image {
    br_image value{};
    Image(uint32_t w,uint32_t h,br_pixel_format f=BR_PIXEL_GRAY8){CHECK_OK(br_image_create(w,h,f,&value));}
    Image()=default;
    ~Image(){br_image_free(&value);}
    br_image_view view()const{return br_image_as_view(&value);}
};
br_text_options plain(){auto o=br_text_options_default(BR_TEXT_OCR);o.scale=1;o.grayscale=BR_TEXT_MEDIAN;o.border=0;o.normalize=0;o.invert=0;return o;}
int luma(const uint8_t* p,int c){return c==1?p[0]:(77*p[0]+150*p[1]+29*p[2]+128)/256;}
}
TEST(text_presets_and_layout){
    static_assert(sizeof(br_text_options)==64);
    static_assert(sizeof(br_text_info)==72);
    auto a=br_text_options_default(BR_TEXT_SCREENSHOT);auto b=br_text_options_default(BR_TEXT_OCR);
    CHECK(a.struct_size==sizeof(a));CHECK(a.scale==1);CHECK(a.grayscale==BR_TEXT_KEEP_COLOR);
    CHECK(a.sharpen==0&&a.normalize==0);CHECK(b.normalize==0);
    CHECK(b.scale==2);CHECK(b.grayscale==BR_TEXT_LUMA);CHECK(b.threshold==BR_TEXT_THRESHOLD_NONE);
    CHECK(std::strstr(br_build_id(),"text-v1-")!=nullptr);
    CHECK(std::strstr(br_features(),"text-v1")!=nullptr);
}
TEST(text_plain_gray_is_exact_with_negative_stride_and_crop){
    Image input(35,9);for(uint32_t i=0;i<315;++i)input.value.data[i]=uint8_t(i*17);
    auto src=input.view();src.data+=8*src.stride;src.stride=-src.stride;
    auto o=plain();Image out;br_text_info info{};
    CHECK_OK(br_text_prepare(nullptr,&src,{3,1,23,6},&o,&out.value,&info));
    CHECK(out.value.width==23&&out.value.height==6);
    for(uint32_t y=0;y<6;++y)CHECK(!std::memcmp(out.value.data+y*23,src.data+ptrdiff_t(y+1)*src.stride+3,23));
    CHECK(info.source_region.x==3&&info.image_to_source.tx==3&&info.image_to_source.ty==1);
}
TEST(text_no_effect_matches_legacy_resize){
    for(auto fmt:{BR_PIXEL_GRAY8,BR_PIXEL_RGB8,BR_PIXEL_BGRA8}){
        Image input(91,67,fmt);brt::Rng rng(331);const uint32_t c=br_pixel_format_channels(fmt);
        for(size_t i=0;i<size_t(91*67)*c;++i)input.value.data[i]=uint8_t(rng.next());
        if(c==4)for(size_t i=3;i<size_t(91*67)*c;i+=4)input.value.data[i]=255;
        const auto src=input.view();Image flat;CHECK_OK(br_image_clone(&src,fmt==BR_PIXEL_GRAY8?BR_PIXEL_GRAY8:BR_PIXEL_RGB8,&flat.value));
        const auto f=flat.view();
        for(auto dims:{std::pair<uint32_t,uint32_t>{59,44},{91,67},{137,103}}){
            auto o=plain();o.grayscale=fmt==BR_PIXEL_GRAY8?BR_TEXT_MEDIAN:BR_TEXT_KEEP_COLOR;
            o.width=dims.first;o.height=dims.second;Image out,legacy;auto ro=br_resize_options_for(BR_RESIZE_UI_TEXT);
            CHECK_OK(br_text_prepare(nullptr,&src,{},&o,&out.value,nullptr));
            CHECK_OK(br_resize_to(nullptr,&f,o.width,o.height,&ro,&legacy.value));
            CHECK(brt::images_equal(out.view(),legacy.view()));
        }
    }
}
TEST(text_native_color_preserves_subpixel_details_and_source){
    for(auto fmt:{BR_PIXEL_RGB8,BR_PIXEL_BGR8,BR_PIXEL_RGBA8,BR_PIXEL_BGRA8}){
        Image input(37,11,fmt);brt::Rng rng(834);const uint32_t c=br_pixel_format_channels(fmt);
        for(size_t i=0;i<size_t(input.value.stride)*input.value.height;++i)input.value.data[i]=uint8_t(rng.next());
        if(c==4)for(size_t i=3;i<size_t(input.value.stride)*input.value.height;i+=4)input.value.data[i]=255;
        const std::vector<uint8_t> before(input.value.data,input.value.data+input.value.stride*input.value.height);
        auto v=input.view();v.data+=10*v.stride;v.stride=-v.stride;
        auto o=br_text_options_default(BR_TEXT_SCREENSHOT);Image out;br_text_info info{};
        CHECK_OK(br_text_prepare(nullptr,&v,{2,3,29,5},&o,&out.value,&info));
        CHECK(out.value.format==BR_PIXEL_RGB8);CHECK(out.value.color_space==BR_COLOR_SRGB);
        for(uint32_t y=0;y<5;++y)for(uint32_t x=0;x<29;++x){
            const auto* p=br::row_ptr(v,y+3)+(x+2)*c;const auto* q=br::row_ptr(out.value,y)+x*3;
            const bool bgr=fmt==BR_PIXEL_BGR8||fmt==BR_PIXEL_BGRA8;
            CHECK(q[0]==p[bgr?2:0]&&q[1]==p[1]&&q[2]==p[bgr?0:2]);
        }
        CHECK(std::equal(before.begin(),before.end(),input.value.data));
        CHECK(info.image_to_source.sx==1&&info.image_to_source.sy==1);
        CHECK(info.image_to_source.tx==2&&info.image_to_source.ty==3);
    }
}
TEST(text_explicit_resize_policy_preserves_geometry_and_controls_sampling){
    Image input(89,61,BR_PIXEL_RGB8);brt::Rng rng(917);
    for(size_t i=0;i<size_t(input.value.stride)*input.value.height;++i)input.value.data[i]=uint8_t(rng.next());
    const auto v=input.view();auto o=br_text_options_default(BR_TEXT_SCREENSHOT);o.width=53;o.height=41;
    for(auto filter:{BR_FILTER_BOX,BR_FILTER_TRIANGLE,BR_FILTER_CATMULL_ROM,BR_FILTER_LANCZOS3,BR_FILTER_POINT})
        for(uint8_t linear:{uint8_t(0),uint8_t(1)})for(uint8_t antiring:{uint8_t(0),uint8_t(1)}){
            auto ro=br_resize_options_for(BR_RESIZE_UI_TEXT);ro.filter=filter;ro.linear_light=linear;ro.antiring=antiring;
            Image out,ref;br_text_info info{};
            CHECK_OK(br_text_prepare_with_resize(nullptr,&v,{},&o,&ro,&out.value,&info));
            CHECK_OK(br_resize_to(nullptr,&v,o.width,o.height,&ro,&ref.value));
            CHECK(brt::images_equal(out.view(),ref.view()));
            CHECK(std::abs(info.image_to_source.sx-89.0/53)<1e-12);
            CHECK(std::abs(info.image_to_source.sy-61.0/41)<1e-12);
        }
    auto ro=br_resize_options_for(BR_RESIZE_UI_TEXT);ro.filter=BR_FILTER_POINT;o.width=178;o.height=122;
    Image out;CHECK_OK(br_text_prepare_with_resize(nullptr,&v,{},&o,&ro,&out.value,nullptr));
    for(uint32_t y=0;y<out.value.height;++y)for(uint32_t x=0;x<out.value.width;++x)
        CHECK(!std::memcmp(br::row_ptr(out.value,y)+x*3,br::row_ptr(v,y/2)+(x/2)*3,3));
}
TEST(text_invalid_explicit_resize_policy_leaves_outputs_unchanged){
    Image input(8,8);const auto v=input.view();auto o=br_text_options_default(BR_TEXT_SCREENSHOT);
    for(int which=0;which<7;++which){auto ro=br_resize_options_for(BR_RESIZE_UI_TEXT);switch(which){
        case 0:ro.filter=br_filter(99);break;case 1:ro.mode=br_resize_mode(99);break;
        case 2:ro.linear_light=2;break;case 3:ro.antiring=2;break;case 4:ro.multistage=2;break;
        case 5:ro.preserve_alpha=2;break;case 6:ro.threads=65;break;}
        br_image out{};br_text_info info{};info.inverted=123;
        CHECK(br_text_prepare_with_resize(nullptr,&v,{},&o,&ro,&out,&info)==BR_E_INVALID_ARGUMENT);
        CHECK(out.data==nullptr&&info.inverted==123);
    }
}
TEST(text_gray_median_alpha_and_linear){
    Image input(4,1,BR_PIXEL_RGBA8);uint8_t data[]={20,240,22,255, 0,0,0,0, 40,40,40,128, 100,100,100,255};
    std::memcpy(input.value.data,data,16);auto v=input.view();auto o=plain();Image out;
    CHECK_OK(br_text_prepare(nullptr,&v,{},&o,&out.value,nullptr));
    CHECK(out.value.data[0]==22);CHECK(out.value.data[1]==255);CHECK(out.value.data[2]==147);CHECK(out.value.data[3]==100);
    br_image_free(&out.value);input.value.premultiplied_alpha=1;input.value.data[8]=input.value.data[9]=input.value.data[10]=20;
    v=input.view();CHECK_OK(br_text_prepare(nullptr,&v,{},&o,&out.value,nullptr));CHECK(out.value.data[2]==147);
    br_image_free(&out.value);v.color_space=BR_COLOR_LINEAR_SRGB;CHECK_OK(br_text_prepare(nullptr,&v,{},&o,&out.value,nullptr));
    CHECK(out.value.data[3]>=167&&out.value.data[3]<=169);
}
TEST(text_border_transform_and_rounding){
    Image input(940,640);auto v=input.view();auto o=plain();o.width=611;o.height=416;o.border=8;
    Image out;br_text_info info{};CHECK_OK(br_text_prepare(nullptr,&v,{},&o,&out.value,&info));
    CHECK(out.value.width==627&&out.value.height==432);CHECK(info.content_rect.x==8);
    double x=0,y=0;br_transform_point(&info.image_to_source,8,8,&x,&y);CHECK(std::abs(x)<1e-10&&std::abs(y)<1e-10);
    br_transform_point(&info.image_to_source,619,424,&x,&y);CHECK(std::abs(x-940)<1e-8&&std::abs(y-640)<1e-8);
    br_transform_pixel_center(&info.image_to_source,8,8,&x,&y);
    CHECK(std::abs(x-(0.5*940/611-0.5))<1e-10);
    CHECK(std::abs(y-(0.5*640/416-0.5))<1e-10);
    br_image_free(&out.value);o.width=0;o.height=13;o.border=0;
    CHECK_OK(br_text_prepare(nullptr,&v,{},&o,&out.value,nullptr));CHECK(out.value.width==19&&out.value.height==13);
}
TEST(text_bounded_sharpen_matches_naive_reference){
    for(uint32_t w:{1u,2u,3u,17u,33u})for(uint32_t h:{1u,2u,3u,19u})for(int c:{1,3}){
        Image input(w,h,c==1?BR_PIXEL_GRAY8:BR_PIXEL_RGB8);brt::Rng rng(w*217+h);
        for(size_t i=0;i<size_t(w)*h*c;++i)input.value.data[i]=uint8_t(rng.next());
        auto v=input.view();auto o=plain();o.grayscale=c==1?BR_TEXT_MEDIAN:BR_TEXT_KEEP_COLOR;o.sharpen=73;
        Image out;CHECK_OK(br_text_prepare(nullptr,&v,{},&o,&out.value,nullptr));
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
            int sum=0,lo=255,hi=0;
            for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
                int xx=std::clamp(int(x)+dx,0,int(w)-1),yy=std::clamp(int(y)+dy,0,int(h)-1);
                int value=luma(v.data+(yy*w+xx)*c,c);sum+=value*(dy==0?2:1)*(dx==0?2:1);lo=std::min(lo,value);hi=std::max(hi,value);
            }
            auto* p=v.data+(y*w+x)*c;int l=luma(p,c),d=(l-(sum+8)/16)*73;
            d=d<0?-((-d+50)/100):(d+50)/100;d=std::clamp(d,-16,16);int delta=std::clamp(l+d,lo,hi)-l;
            for(int k=0;k<c;++k)CHECK(out.value.data[(y*w+x)*c+k]==std::clamp(int(p[k])+delta,0,255));
        }
    }
}
TEST(text_sauvola_matches_clipped_window_reference){
    for(uint32_t w:{1u,2u,7u,31u})for(uint32_t h:{1u,3u,13u})for(uint32_t window:{3u,7u,25u}){
        Image input(w,h);brt::Rng rng(99+w+h);for(uint32_t i=0;i<w*h;++i)input.value.data[i]=uint8_t(rng.next());
        auto v=input.view();auto o=plain();o.threshold=BR_TEXT_THRESHOLD_SAUVOLA;o.window=window;Image out;
        CHECK_OK(br_text_prepare(nullptr,&v,{},&o,&out.value,nullptr));const int r=int(window/2);
        for(int y=0;y<int(h);++y)for(int x=0;x<int(w);++x){
            double sum=0,sq=0,n=0;
            for(int yy=std::max(0,y-r);yy<=std::min(int(h)-1,y+r);++yy)
                for(int xx=std::max(0,x-r);xx<=std::min(int(w)-1,x+r);++xx){double a=v.data[yy*w+xx];sum+=a;sq+=a*a;++n;}
            double m=sum/n,sd=std::sqrt(std::max(0.0,sq/n-m*m)),t=m*(1+double(o.threshold_k)*(sd/128-1));
            CHECK(out.value.data[y*w+x]==(v.data[y*w+x]<=t?0:255));
        }
    }
}
TEST(text_uniform_images_stay_uniform_and_normalization_is_bounded){
    for(uint8_t value:{uint8_t(0),uint8_t(7),uint8_t(128),uint8_t(255)}){
        Image input(27,19);std::memset(input.value.data,value,27*19);auto v=input.view();auto o=plain();o.normalize=100;o.sharpen=100;Image out;
        CHECK_OK(br_text_prepare(nullptr,&v,{},&o,&out.value,nullptr));for(int i=0;i<27*19;++i)CHECK(out.value.data[i]==value);
    }
}
TEST(text_auto_invert_and_otsu){
    Image input(21,15);std::memset(input.value.data,20,315);for(int y=3;y<12;++y)for(int x=5;x<16;++x)input.value.data[y*21+x]=240;
    auto v=input.view();auto o=plain();o.invert=-1;o.threshold=BR_TEXT_THRESHOLD_OTSU;o.border=2;Image out;br_text_info info{};
    CHECK_OK(br_text_prepare(nullptr,&v,{},&o,&out.value,&info));CHECK(info.inverted==1);
    CHECK(out.value.data[0]==255);CHECK(out.value.data[5*out.value.width+7]==0);
    CHECK(info.threshold>=15&&info.threshold<235);
}
TEST(text_options_reject_invalid_and_leave_output_unchanged){
    Image input(8,8);auto v=input.view();auto base=plain();
    for(int which=0;which<13;++which){auto o=base;switch(which){
        case 0:o.struct_size=0;break;case 1:o.scale=std::numeric_limits<float>::quiet_NaN();break;
        case 2:o.width=UINT32_MAX;break;case 3:o.border=257;break;case 4:o.normalize=101;break;
        case 5:o.sharpen=101;break;case 6:o.invert=3;break;case 7:o.window=2;break;
        case 8:o.threshold_k=std::numeric_limits<float>::infinity();break;case 9:o.reserved=1;break;
        case 10:o.max_pixels=12;break;case 11:o.grayscale=BR_TEXT_KEEP_COLOR;o.invert=-1;break;
        case 12:o.grayscale=BR_TEXT_KEEP_COLOR;o.threshold=BR_TEXT_THRESHOLD_OTSU;break;}
        br_image out{};br_text_info info{};info.inverted=123;
        CHECK(br_text_prepare(nullptr,&v,{},&o,&out,&info)!=BR_OK);CHECK(out.data==nullptr);CHECK(info.inverted==123);
    }
    Image out(2,2);auto* old=out.value.data;CHECK(br_text_prepare(nullptr,&v,{},&base,&out.value,nullptr)==BR_E_INVALID_ARGUMENT);CHECK(old==out.value.data);
    auto vbad=v;vbad.stride=std::numeric_limits<ptrdiff_t>::min();br_image empty{};
    CHECK(br_text_prepare(nullptr,&vbad,{},&base,&empty,nullptr)==BR_E_INVALID_ARGUMENT);
}
TEST(detail_tiles_cover_roi_and_report_budget_errors){
    br_detail_options o{17,13,4,4096};size_t n=0;CHECK_OK(br_plan_detail_tiles(71,53,{3,2,66,49},&o,nullptr,0,&n));
    std::vector<br_detail_tile> tiles(n);CHECK_OK(br_plan_detail_tiles(71,53,{3,2,66,49},&o,tiles.data(),n,&n));
    std::vector<bool> covered(71*53,false);
    for(auto& t:tiles){auto r=t.source_region;CHECK(r.x>=3&&r.y>=2&&r.x+r.width<=69&&r.y+r.height<=51);
        CHECK(t.image_to_source.sx==1&&t.image_to_source.tx==r.x);
        for(int y=r.y;y<r.y+r.height;++y)for(int x=r.x;x<r.x+r.width;++x)covered[y*71+x]=true;}
    for(int y=2;y<51;++y)for(int x=3;x<69;++x)CHECK(covered[y*71+x]);
    tiles[0].source_region.x=-123;CHECK(br_plan_detail_tiles(71,53,{3,2,66,49},&o,tiles.data(),1,&n)==BR_E_BUFFER_TOO_SMALL);CHECK(tiles[0].source_region.x==-123);
    o.max_tiles=1;CHECK(br_plan_detail_tiles(71,53,{},&o,nullptr,0,&n)==BR_E_UNSUPPORTED);CHECK(n>1);
    o.overlap=17;CHECK(br_plan_detail_tiles(71,53,{},&o,nullptr,0,&n)==BR_E_INVALID_ARGUMENT);
    o=br_detail_options_default();CHECK_OK(br_plan_detail_tiles(1,1,{},&o,nullptr,0,&n));CHECK(n==1);
}
