#include "test_framework.hpp"
#include "resize/sampler.hpp"

#include <algorithm>

#include <cmath>
#include <cstring>

TEST(resize_constant_image_stays_constant) {
    for (br_filter f : {BR_FILTER_BOX, BR_FILTER_TRIANGLE, BR_FILTER_CATMULL_ROM, BR_FILTER_MITCHELL, BR_FILTER_LANCZOS3,
                        BR_FILTER_LANCZOS4, BR_FILTER_POINT}) {
        br_image src{};
        CHECK_OK(br_image_create(333, 217, BR_PIXEL_BGRA8, &src));
        br_image_fill(&src, 0xff336699);
        const br_image_view v = br_image_as_view(&src);
        for (uint32_t w : {7u, 100u, 333u, 800u}) {
            br_resize_options o = br_resize_options_default();
            o.filter = f;
            br_image dst{};
            CHECK_OK(br_resize_to(nullptr, &v, w, 0, &o, &dst));
            bool ok = true;
            for (uint32_t y = 0; y < dst.height; ++y)
                for (uint32_t x = 0; x < dst.width; ++x) {
                    const uint8_t* p = dst.data + y * dst.stride + 4 * x;
                    ok &= p[0] == 0x99 && p[1] == 0x66 && p[2] == 0x33 && p[3] == 0xff;
                }
            CHECK(ok);
            br_image_free(&dst);
        }
        br_image_free(&src);
    }
}

TEST(resize_point_sampler_preserves_known_centres_offsets_and_clipping) {
    struct Case { uint32_t input, output; double offset, extent; std::vector<int32_t> expected; };
    const Case cases[]{
        {5,3,0,5,{0,2,4}},
        {5,10,0,5,{0,0,1,1,2,2,3,3,4,4}},
        {5,4,-.75,6,{0,1,3,4}},
        {5,2,-2.5,10,{0,4}},
        {5,6,.75,3,{1,1,2,2,3,3}},
        {1,4,-5,15,{0,0,0,0}},
    };
    for(const auto& c:cases){
        const auto sampler=br::resize::build_axis_sampler(c.input,c.offset,c.extent,c.output,BR_FILTER_POINT);
        CHECK(sampler.in_size==c.input && sampler.out_size==c.output && sampler.filter==BR_FILTER_POINT);
        CHECK(sampler.taps==1 && !sampler.negative_lobes);
        CHECK(sampler.start==c.expected);
        CHECK(sampler.weights.size()==c.output && sampler.lobe_begin.size()==c.output && sampler.lobe_end.size()==c.output);
        for(uint32_t i=0;i<c.output;++i){
            CHECK(sampler.weights[i]==1.0f && sampler.lobe_begin[i]==0 && sampler.lobe_end[i]==1);
        }
    }
}

TEST(resize_point_pixel_oracle_up_down_and_negative_source_stride) {
    br_image input{};CHECK_OK(br_image_create(7,5,BR_PIXEL_RGB8,&input));
    for(uint32_t y=0;y<5;++y)for(uint32_t x=0;x<7;++x)for(uint32_t c=0;c<3;++c)
        input.data[y*input.stride+x*3+c]=uint8_t(y*23+x*5+c);
    auto options=br_resize_options_for(BR_RESIZE_UI_TEXT);options.filter=BR_FILTER_POINT;
    for(bool negative:{false,true}){
        auto view=br_image_as_view(&input);
        if(negative){view.data+=4*view.stride;view.stride=-view.stride;}
        br_image up{},down{};
        CHECK_OK(br_resize_to(nullptr,&view,14,15,&options,&up));
        for(uint32_t y=0;y<15;++y)for(uint32_t x=0;x<14;++x){
            const uint32_t source_y=negative?4-y/3:y/3;
            for(uint32_t c=0;c<3;++c)
                CHECK(up.data[y*up.stride+x*3+c]==source_y*23+(x/2)*5+c);
        }
        CHECK_OK(br_resize_to(nullptr,&view,4,3,&options,&down));
        // При 7->4 берутся столбцы 0,2,4,6; при 5->3 - строки 0,2,4.
        for(uint32_t y=0;y<3;++y)for(uint32_t x=0;x<4;++x){
            const uint32_t source_y=negative?4-y*2:y*2;
            for(uint32_t c=0;c<3;++c)
                CHECK(down.data[y*down.stride+x*3+c]==source_y*23+x*10+c);
        }
        br_image_free(&up);br_image_free(&down);
    }
    br_image_free(&input);
}

TEST(resize_matches_reference_area_average) {
    // При уменьшении вдвое блочным фильтром в гамма-пространстве должен получиться точный средний цвет блока 2x2.
    br_image src = brt::make_photo_image(64, 48, BR_PIXEL_RGB8, 4);
    const br_image_view v = br_image_as_view(&src);
    br_resize_options o = br_resize_options_for(BR_RESIZE_FAST);
    o.filter = BR_FILTER_BOX;
    o.linear_light = 0;
    br_image dst{};
    CHECK_OK(br_resize_to(nullptr, &v, 32, 24, &o, &dst));
    int maxerr = 0;
    for (uint32_t y = 0; y < 24; ++y)
        for (uint32_t x = 0; x < 32; ++x)
            for (int c = 0; c < 3; ++c) {
                const uint8_t* s = src.data;
                const int sum = s[(2 * y) * src.stride + 3 * (2 * x) + c] + s[(2 * y) * src.stride + 3 * (2 * x + 1) + c] +
                                s[(2 * y + 1) * src.stride + 3 * (2 * x) + c] + s[(2 * y + 1) * src.stride + 3 * (2 * x + 1) + c];
                const int ref = (sum + 2) / 4;
                maxerr = std::max(maxerr, std::abs(ref - dst.data[y * dst.stride + 3 * x + c]));
            }
    CHECK(maxerr <= 1);
    br_image_free(&dst);
    br_image_free(&src);
}

TEST(resize_simd_matches_scalar_and_threads) {
    br_image src = brt::make_ui_image(1280, 720, BR_PIXEL_BGRA8, 21);
    const br_image_view v = br_image_as_view(&src);
    br_context* one = nullptr;
    br_context* many = nullptr;
    CHECK_OK(br_context_create(&one));
    CHECK_OK(br_context_create(&many));
    CHECK_OK(br_context_set_threads(one, 1));
    CHECK_OK(br_context_set_threads(many, 4));
    for (br_resize_mode m : {BR_RESIZE_QUALITY, BR_RESIZE_UI_TEXT, BR_RESIZE_FAST}) {
        br_resize_options o = br_resize_options_for(m);
        br_image a{}, b{};
        CHECK_OK(br_resize_to(one, &v, 501, 0, &o, &a));
        CHECK_OK(br_resize_to(many, &v, 501, 0, &o, &b));
        const br_image_view av = br_image_as_view(&a), bv = br_image_as_view(&b);
        CHECK(brt::images_equal(av, bv));
        br_image_free(&a);
        br_image_free(&b);
    }
    br_context_destroy(one);
    br_context_destroy(many);
    br_image_free(&src);
}

TEST(resize_antiring_bounds_overshoot) {
    // Резкая чёрно-белая граница: защита от выбросов должна удерживать результат в [0, 255] без ореолов.
    br_image src{};
    CHECK_OK(br_image_create(200, 10, BR_PIXEL_GRAY8, &src));
    for (uint32_t y = 0; y < 10; ++y)
        for (uint32_t x = 0; x < 200; ++x) src.data[y * src.stride + x] = x < 100 ? 20 : 235;
    const br_image_view v = br_image_as_view(&src);
    br_resize_options o = br_resize_options_default();
    o.filter = BR_FILTER_LANCZOS3;
    o.linear_light = 0;
    br_image up{};
    o.antiring = 0;
    CHECK_OK(br_resize_to(nullptr, &v, 700, 10, &o, &up));
    int lo = 255, hi = 0;
    for (uint32_t x = 0; x < 700; ++x) { lo = std::min<int>(lo, up.data[x]); hi = std::max<int>(hi, up.data[x]); }
    CHECK(lo < 20 || hi > 235); // Выбросы без ограничения диапазона.
    br_image_free(&up);
    o.antiring = 1;
    CHECK_OK(br_resize_to(nullptr, &v, 700, 10, &o, &up));
    lo = 255; hi = 0;
    for (uint32_t x = 0; x < 700; ++x) { lo = std::min<int>(lo, up.data[x]); hi = std::max<int>(hi, up.data[x]); }
    CHECK(lo >= 20 && hi <= 235);
    br_image_free(&up);
    br_image_free(&src);
}

TEST(resize_format_conversion_and_extremes) {
    br_image src = brt::make_photo_image(1000, 700, BR_PIXEL_RGBA8, 8);
    const br_image_view v = br_image_as_view(&src);
    for (br_pixel_format pf : {BR_PIXEL_GRAY8, BR_PIXEL_RGB8, BR_PIXEL_BGR8, BR_PIXEL_BGRA8}) {
        br_image dst{};
        CHECK_OK(br_image_create(123, 77, pf, &dst));
        br_resize_options o = br_resize_options_default();
        CHECK_OK(br_resize(nullptr, &v, &dst, &o));
        br_image_free(&dst);
    }
    br_image tiny{};
    CHECK_OK(br_resize_to(nullptr, &v, 1, 1, nullptr, &tiny));
    br_image_free(&tiny);
    br_image big{};
    br_image one{};
    CHECK_OK(br_resize_to(nullptr, &v, 3, 2, nullptr, &one));
    const br_image_view ov = br_image_as_view(&one);
    CHECK_OK(br_resize_to(nullptr, &ov, 640, 480, nullptr, &big));
    br_image_free(&big);
    br_image_free(&one);
    br_image_free(&src);
}

TEST(resize_native_and_point_honour_color_transfer) {
    br_image src{};CHECK_OK(br_image_create(256,1,BR_PIXEL_RGB8,&src));
    for(uint32_t x=0;x<256;++x)for(uint32_t c=0;c<3;++c)src.data[x*3+c]=uint8_t(x);
    for(bool to_linear:{false,true})for(bool point:{false,true}){
        auto v=br_image_as_view(&src);v.color_space=to_linear?BR_COLOR_SRGB:BR_COLOR_LINEAR_SRGB;
        br_image dst{};CHECK_OK(br_image_create(point?512:256,1,BR_PIXEL_RGB8,&dst));
        dst.color_space=to_linear?BR_COLOR_LINEAR_SRGB:BR_COLOR_SRGB;
        auto o=br_resize_options_for(BR_RESIZE_UI_TEXT);o.filter=BR_FILTER_POINT;
        CHECK_OK(br_resize(nullptr,&v,&dst,&o));
        for(uint32_t x=0;x<dst.width;++x){
            const double value=double(point?x/2:x)/255;
            const double transformed=to_linear?(value<=0.04045?value/12.92:std::pow((value+0.055)/1.055,2.4)):
                (value<=0.0031308?12.92*value:1.055*std::pow(value,1/2.4)-0.055);
            const auto expected=uint8_t(std::lround(transformed*255));
            CHECK(dst.data[x*3]==expected&&dst.data[x*3+1]==expected&&dst.data[x*3+2]==expected);
        }
        br_image_free(&dst);
    }
    br_image_free(&src);
}

TEST(resize_point_honours_alpha_representation) {
    br_image src{};CHECK_OK(br_image_create(2,1,BR_PIXEL_RGBA8,&src));
    const uint8_t pixels[]{80,40,20,128,0,0,0,0};std::memcpy(src.data,pixels,sizeof(pixels));src.premultiplied_alpha=1;
    auto v=br_image_as_view(&src);br_image dst{};CHECK_OK(br_image_create(4,1,BR_PIXEL_BGRA8,&dst));
    auto o=br_resize_options_for(BR_RESIZE_UI_TEXT);o.filter=BR_FILTER_POINT;
    CHECK_OK(br_resize(nullptr,&v,&dst,&o));
    for(uint32_t x=0;x<2;++x){const auto* p=dst.data+x*4;CHECK(p[0]==40&&p[1]==80&&p[2]==159&&p[3]==128);}
    for(uint32_t x=2;x<4;++x){const auto* p=dst.data+x*4;CHECK(p[0]==0&&p[1]==0&&p[2]==0&&p[3]==0);}
    v.color_space=BR_COLOR_LINEAR_SRGB;
    CHECK_OK(br_resize(nullptr,&v,&dst,&o));
    // Перед умножением на альфа-канал назначения преобразует прямой линейный цвет в sRGB.
    const auto gamma=[](int value){const double z=double(value)/255;return int(std::lround(255*(z<=0.0031308?12.92*z:1.055*std::pow(z,1/2.4)-0.055)));};
    CHECK(dst.data[0]==gamma(40)&&dst.data[1]==gamma(80)&&dst.data[2]==gamma(159));
    dst.premultiplied_alpha=1;CHECK_OK(br_resize(nullptr,&v,&dst,&o));
    CHECK(dst.data[0]==(gamma(40)*128+127)/255&&dst.data[1]==(gamma(80)*128+127)/255&&dst.data[2]==(gamma(159)*128+127)/255);
    br_image_free(&dst);br_image_free(&src);
}

TEST(resize_alpha_transfer_matches_equivalent_straight_icon) {
    br_image straight{},premul{};CHECK_OK(br_image_create(4,3,BR_PIXEL_RGBA8,&straight));
    CHECK_OK(br_image_create(4,3,BR_PIXEL_RGBA8,&premul));premul.premultiplied_alpha=1;
    for(uint32_t y=0;y<3;++y)for(uint32_t x=0;x<4;++x){
        const uint32_t alpha=x*85;auto* s=straight.data+y*straight.stride+x*4;auto* p=premul.data+y*premul.stride+x*4;
        // Кратные трём значения точно представимы при альфа 1/3 и 2/3.
        s[0]=180;s[1]=120;s[2]=60;s[3]=uint8_t(alpha);
        for(uint32_t c=0;c<3;++c)p[c]=uint8_t((uint32_t(s[c])*alpha+127)/255);
        p[3]=uint8_t(alpha);
    }
    for(auto mode:{BR_RESIZE_QUALITY,BR_RESIZE_UI_TEXT})for(bool destination_linear:{false,true})
        for(bool destination_premul:{false,true}){
            br_image a{},b{};CHECK_OK(br_image_create(13,7,BR_PIXEL_BGRA8,&a));CHECK_OK(br_image_create(13,7,BR_PIXEL_BGRA8,&b));
            a.color_space=b.color_space=destination_linear?BR_COLOR_LINEAR_SRGB:BR_COLOR_SRGB;
            a.premultiplied_alpha=b.premultiplied_alpha=uint8_t(destination_premul);
            auto o=br_resize_options_for(mode);o.filter=BR_FILTER_TRIANGLE;
            auto sv=br_image_as_view(&straight),pv=br_image_as_view(&premul);
            CHECK_OK(br_resize(nullptr,&sv,&a,&o));CHECK_OK(br_resize(nullptr,&pv,&b,&o));
            CHECK(brt::images_equal(br_image_as_view(&a),br_image_as_view(&b)));
            if(destination_premul){
                for(uint32_t y=0;y<a.height;++y)for(uint32_t x=0;x<a.width;++x){const auto* p=a.data+y*a.stride+x*4;
                    CHECK(p[0]<=p[3]&&p[1]<=p[3]&&p[2]<=p[3]);}
            }
            br_image_free(&a);br_image_free(&b);
        }
    br_image_free(&straight);br_image_free(&premul);
}

TEST(fit_dimensions) {
    uint32_t w = 0, h = 0;
    br_fit_options f{};
    f.max_long_edge = 1568;
    br_fit_dimensions(3840, 2160, &f, &w, &h);
    CHECK(w == 1568 && h == 882);
    f = {};
    f.max_pixels = 1150000;
    br_fit_dimensions(1920, 1080, &f, &w, &h);
    CHECK(static_cast<uint64_t>(w) * h <= 1150000 && w > 1400);
    f = {};
    f.max_pixels = 1003520;
    f.multiple_of = 28;
    br_fit_dimensions(2560, 1600, &f, &w, &h);
    CHECK(w % 28 == 0 && h % 28 == 0 && static_cast<uint64_t>(w) * h <= 1003520);
    f = {};
    br_fit_dimensions(800, 600, &f, &w, &h);
    CHECK(w == 800 && h == 600);
    f.max_width = 2000;
    f.allow_upscale = 1;
    br_fit_dimensions(800, 600, &f, &w, &h);
    CHECK(w == 2000 && h == 1500);
}

TEST(zoom_and_transforms) {
    br_image src = brt::make_ui_image(1920, 1080, BR_PIXEL_BGRA8, 3);
    const br_image_view v = br_image_as_view(&src);
    br_image z{};
    br_transform t{};
    CHECK_OK(br_zoom(nullptr, &v, {100, 200, 300, 150}, 900, 0, nullptr, &z, &t));
    CHECK(z.width == 900 && z.height == 450);
    double x = 0, y = 0;
    br_transform_point(&t, 0, 0, &x, &y);
    CHECK(std::abs(x - 100) < 1e-9 && std::abs(y - 200) < 1e-9);
    br_transform_point(&t, 900, 450, &x, &y);
    CHECK(std::abs(x - 400) < 1e-9 && std::abs(y - 350) < 1e-9);
    const br_transform inv = br_transform_inverse(t);
    const br_transform id = br_transform_compose(t, inv);
    CHECK(std::abs(id.sx - 1) < 1e-12 && std::abs(id.tx) < 1e-9);
    const br_rect_i32 r = br_transform_rect(&t, {0, 0, 9, 9});
    CHECK(r.x == 100 && r.y == 200 && r.width == 3 && r.height == 3);
    br_image_free(&z);
    br_image_free(&src);
}
