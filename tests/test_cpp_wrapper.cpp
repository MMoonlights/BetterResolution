#include "test_framework.hpp"

#include <br/br.hpp>

TEST(cpp_wrapper_roundtrip) {
    try {
        br::Image img(400, 300, BR_PIXEL_BGRA8);
        br_image_fill(img.raw(), 0xffeeeeee);
        br_draw_text(img.raw(), 10, 10, "wrapper", 0xff000000, 0, 2);
        auto [small, to_src] = img.fit(br::Fit::long_edge(200));
        CHECK(small.width() == 200 && small.height() == 150);
        const auto p = br::map(to_src, 200, 150);
        CHECK(p.first == 400 && p.second == 300);
        const std::vector<uint8_t> jpg = small.encode(br::Encode::jpeg(85));
        CHECK(jpg.size() > 100 && jpg[0] == 0xff);
        br::Image back = br::Image::decode(jpg.data(), jpg.size(), BR_PIXEL_BGRA8);
        CHECK(back.width() == 200);
        br::Image copy = img;
        br_draw_rect(copy.raw(), {50, 50, 10, 10}, 0xffff0000, 0);
        const br::Diff d = br::diff(img, copy);
        CHECK(d.changed() && d.rects.size() == 1);
        auto [z, z2s] = img.zoom({10, 10, 100, 50}, 300);
        CHECK(z.width() == 300 && z.height() == 150 && z2s.tx == 10);
        bool threw = false;
        try {
            br::Image::load("does/not/exist.png");
        } catch (const br::Error& e) {
            threw = e.status() == BR_E_IO;
        }
        CHECK(threw);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        CHECK(false);
    }
}
