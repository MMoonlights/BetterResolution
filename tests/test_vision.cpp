#include "test_framework.hpp"
#include <br/br_vision.h>
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
    Image(uint32_t w, uint32_t h, br_pixel_format format = BR_PIXEL_RGB8) { CHECK_OK(br_image_create(w,h,format,&value)); }
    Image() = default;
    ~Image() { br_image_free(&value); }
    br_image_view view() const { return br_image_as_view(&value); }
};
bool rect(br_rect_i32 r, int32_t x, int32_t y, int32_t w, int32_t h) {
    return r.x==x && r.y==y && r.width==w && r.height==h;
}
}

TEST(vision_presets_and_versioned_layout) {
    static_assert(sizeof(br_vision_options)==40);
    static_assert(sizeof(br_vision_plan)==104);
    const auto native=br_vision_options_default(BR_VISION_NATIVE), text=br_vision_options_default(BR_VISION_TEXT), icon=br_vision_options_default(BR_VISION_ICON);
    CHECK(native.struct_size==sizeof(native) && native.version==BR_VISION_API_VERSION);
    CHECK(native.scale==1 && native.context_margin==0 && native.border==0);
    CHECK(text.scale==2 && text.context_margin==4 && text.max_pixels==2u*1024*1024);
    CHECK(icon.scale==4 && icon.context_margin==16 && icon.border==0);
}

TEST(vision_context_clips_at_edges_without_dropping_requested_roi) {
    auto o=br_vision_options_default(BR_VISION_ICON);br_vision_plan p{};
    CHECK_OK(br_vision_plan_detail(100,80,{1,2,8,9},&o,&p));
    CHECK(rect(p.requested_region,1,2,8,9));CHECK(rect(p.source_region,0,0,25,27));
    CHECK(rect(p.content_rect,0,0,100,108));CHECK(p.scale==4 && !p.chose_limited);
    CHECK_OK(br_vision_plan_detail(100,80,{95,75,20,20},&o,&p));
    CHECK(rect(p.requested_region,95,75,5,5));CHECK(rect(p.source_region,79,59,21,21));
    CHECK_OK(br_vision_plan_detail(100,80,{-5,-7,12,15},&o,&p));
    CHECK(rect(p.requested_region,0,0,7,8));CHECK(rect(p.source_region,0,0,23,24));
    CHECK_OK(br_vision_plan_detail(100,80,{},&o,&p));
    CHECK(rect(p.requested_region,0,0,100,80) && rect(p.source_region,0,0,100,80));
}

TEST(vision_budget_chooses_largest_integer_zoom_and_never_shrinks_native) {
    auto o=br_vision_options_default(BR_VISION_ICON);o.context_margin=0;o.scale=8;
    o.max_pixels=30*30;br_vision_plan p{};
    CHECK_OK(br_vision_plan_detail(10,10,{},&o,&p));CHECK(p.scale==3 && p.desired_scale==8 && p.chose_limited==1);
    o.border=1;CHECK_OK(br_vision_plan_detail(10,10,{},&o,&p));CHECK(p.scale==2);
    o.max_pixels=0;o.max_long_edge=32;CHECK_OK(br_vision_plan_detail(10,10,{},&o,&p));CHECK(p.scale==3);
    o.max_long_edge=11;p.scale=123;
    CHECK(br_vision_plan_detail(10,10,{},&o,&p)==BR_E_UNSUPPORTED);CHECK(p.scale==123);
    o.border=0;o.max_long_edge=0;o.max_pixels=99;
    CHECK(br_vision_plan_detail(10,10,{},&o,&p)==BR_E_UNSUPPORTED);CHECK(p.scale==123);
    o.max_pixels=0;o.scale=8;
    CHECK_OK(br_vision_plan_detail(1000,1000,{},&o,&p));CHECK(p.scale==1 && p.chose_limited==1);
    CHECK(br_vision_plan_detail(2000,2000,{},&o,&p)==BR_E_UNSUPPORTED);
    o.context_margin=16;o.max_pixels=300;
    CHECK(br_vision_plan_detail(20,20,{9,9,2,2},&o,&p)==BR_E_UNSUPPORTED); // Сохраняет контекст и не отбрасывает его молча ради вписывания.
}

TEST(vision_icon_point_zoom_is_exact_with_context_border_and_negative_stride) {
    Image input(7,6,BR_PIXEL_BGRA8);brt::Rng rng(8845);
    for(size_t i=0;i<size_t(input.value.stride)*input.value.height;i+=4){
        input.value.data[i]=uint8_t(rng.next());input.value.data[i+1]=uint8_t(rng.next());
        input.value.data[i+2]=uint8_t(rng.next());input.value.data[i+3]=255;
    }
    const std::vector<uint8_t> original(input.value.data,input.value.data+input.value.stride*input.value.height);
    auto src=input.view();src.data+=5*src.stride;src.stride=-src.stride;
    auto o=br_vision_options_default(BR_VISION_ICON);o.context_margin=1;o.border=2;
    Image out;br_vision_plan plan{};
    CHECK_OK(br_vision_prepare(nullptr,&src,{2,2,3,2},&o,&out.value,&plan));
    CHECK(rect(plan.source_region,1,1,5,4));CHECK(rect(plan.content_rect,2,2,20,16));
    CHECK(out.value.width==24 && out.value.height==20 && out.value.format==BR_PIXEL_RGB8);
    for(uint32_t y=0;y<16;++y)for(uint32_t x=0;x<20;++x){
        const auto* p=br::row_ptr(src,1+y/4)+(1+x/4)*4;
        const auto* q=br::row_ptr(out.value,y+2)+(x+2)*3;
        CHECK(q[0]==p[2] && q[1]==p[1] && q[2]==p[0]);
    }
    CHECK(std::equal(original.begin(),original.end(),input.value.data));
    double x=0,y=0;br_transform_point(&plan.image_to_source,2,2,&x,&y);CHECK(x==1 && y==1);
    br_transform_point(&plan.image_to_source,22,18,&x,&y);CHECK(x==6 && y==5);
    br_transform_pixel_center(&plan.image_to_source,2,2,&x,&y);CHECK(std::abs(x-0.625)<1e-12 && std::abs(y-0.625)<1e-12);
}

TEST(vision_native_and_text_prepare_use_original_rgb_without_ocr_changes) {
    Image input(19,13);brt::Rng rng(1467);
    for(size_t i=0;i<size_t(input.value.stride)*input.value.height;++i)input.value.data[i]=uint8_t(rng.next());
    const auto src=input.view();Image out;br_vision_plan plan{};
    CHECK_OK(br_vision_prepare(nullptr,&src,{},nullptr,&out.value,&plan));
    CHECK(brt::images_equal(src,out.view()));CHECK(plan.scale==1 && plan.image_to_source.sx==1);
    br_image_free(&out.value);
    auto o=br_vision_options_default(BR_VISION_TEXT);
    CHECK_OK(br_vision_prepare(nullptr,&src,{6,5,4,3},&o,&out.value,&plan));
    CHECK(rect(plan.source_region,2,1,12,11));CHECK(out.value.width==24 && out.value.height==22);
    br_image_view cropped{};CHECK_OK(br_image_crop(&src,plan.source_region,&cropped));
    auto ro=br_resize_options_for(BR_RESIZE_UI_TEXT);ro.filter=BR_FILTER_CATMULL_ROM;
    Image reference;CHECK_OK(br_resize_to(nullptr,&cropped,24,22,&ro,&reference.value));
    CHECK(brt::images_equal(reference.view(),out.view()));
}

TEST(vision_small_dark_ui_keeps_color_and_zero_border) {
    Image input(3,1);const uint8_t pixels[]{18,19,20, 0,120,215, 235,240,248};
    std::memcpy(input.value.data,pixels,sizeof(pixels));const auto src=input.view();
    for(auto preset:{BR_VISION_NATIVE,BR_VISION_ICON}){
        auto o=br_vision_options_default(preset);Image out;br_vision_plan plan{};
        CHECK_OK(br_vision_prepare(nullptr,&src,{},&o,&out.value,&plan));
        CHECK(out.value.width==3*o.scale && out.value.height==o.scale);
        CHECK(plan.content_rect.x==0 && plan.content_rect.y==0);
        for(uint32_t y=0;y<out.value.height;++y)for(uint32_t x=0;x<out.value.width;++x)
            CHECK(!std::memcmp(br::row_ptr(out.value,y)+x*3,pixels+(x/o.scale)*3,3));
    }
}

TEST(vision_invalid_arguments_and_budget_leave_both_outputs_unchanged) {
    Image input(9,7);const auto src=input.view();const auto base=br_vision_options_default(BR_VISION_TEXT);
    for(int which=0;which<9;++which){auto o=base;switch(which){
        case 0:o.struct_size=0;break;case 1:o.version=2;break;case 2:o.reserved=1;break;
        case 3:o.scale=0;break;case 4:o.scale=9;break;case 5:o.context_margin=257;break;
        case 6:o.border=257;break;case 7:o.preset=br_vision_preset(3);break;case 8:o.max_pixels=10;break;}
        br_image out{};br_vision_plan plan{};plan.scale=4321;
        CHECK(br_vision_prepare(nullptr,&src,{},&o,&out,&plan)!=BR_OK);
        CHECK(!out.data && !out.width && plan.scale==4321);
    }
    br_image metadataOnly{};metadataOnly.width=7;
    CHECK(br_vision_prepare(nullptr,&src,{},&base,&metadataOnly,nullptr)==BR_E_INVALID_ARGUMENT);
    CHECK(metadataOnly.width==7 && !metadataOnly.data);
    Image occupied(2,2);auto* old=occupied.value.data;br_vision_plan plan{};plan.scale=4321;
    CHECK(br_vision_prepare(nullptr,&src,{},&base,&occupied.value,&plan)==BR_E_INVALID_ARGUMENT);
    CHECK(occupied.value.data==old && plan.scale==4321);
    auto bad=src;bad.stride=std::numeric_limits<ptrdiff_t>::min();br_image empty{};
    CHECK(br_vision_prepare(nullptr,&bad,{},&base,&empty,&plan)==BR_E_INVALID_ARGUMENT);CHECK(plan.scale==4321);
    CHECK(br_vision_plan_detail(9,7,{8,6,0,0},&base,&plan)==BR_E_INVALID_ARGUMENT);CHECK(plan.scale==4321);
    CHECK(br_vision_plan_detail(9,7,{},&base,nullptr)==BR_E_INVALID_ARGUMENT);
}
