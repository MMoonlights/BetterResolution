#include "test_framework.hpp"

#include <algorithm>

#include <cstring>

TEST(diff_finds_exact_rectangles) {
    br_image a = brt::make_ui_image(800, 600, BR_PIXEL_BGRA8, 5);
    br_image b{};
    const br_image_view av = br_image_as_view(&a);
    CHECK_OK(br_image_clone(&av, BR_PIXEL_UNKNOWN, &b));
    br_draw_rect(&b, {100, 50, 30, 20}, 0xff00ff00, 0);      // Изменение 1.
    br_draw_rect(&b, {500, 400, 3, 7}, 0xffff0000, 0);       // Изменение 2 (небольшое).
    br_draw_rect(&b, {137, 52, 10, 10}, 0xff0000ff, 0);      // Рядом с изменением 1, объединяется (зазор 8).
    const br_image_view bv = br_image_as_view(&b);
    br_rect_i32 rects[8];
    br_diff_result res{};
    br_diff_options o = br_diff_options_default();
    CHECK_OK(br_diff(&av, &bv, &o, rects, 8, &res));
    CHECK(res.rect_count == 2);
    CHECK(rects[0].x == 100 && rects[0].y == 50 && rects[0].width == 47 && rects[0].height == 20);
    CHECK(rects[1].x == 500 && rects[1].y == 400 && rects[1].width == 3 && rects[1].height == 7);
    CHECK(res.bounds.x == 100 && res.bounds.y == 50);
    CHECK_OK(br_diff(&av, &av, &o, rects, 8, &res));
    CHECK(res.rect_count == 0 && res.changed_pixels == 0);
    br_image_free(&a);
    br_image_free(&b);
}

TEST(diff_threshold_and_formats) {
    br_image a{}, b{};
    CHECK_OK(br_image_create(64, 64, BR_PIXEL_RGB8, &a));
    CHECK_OK(br_image_create(64, 64, BR_PIXEL_BGRA8, &b));
    br_image_fill(&a, 0xff808080);
    br_image_fill(&b, 0xff828282);
    const br_image_view av = br_image_as_view(&a), bv = br_image_as_view(&b);
    br_diff_options o = br_diff_options_default();
    br_diff_result res{};
    CHECK_OK(br_diff(&av, &bv, &o, nullptr, 0, &res));
    CHECK(res.changed_pixels == 64 * 64 && res.rect_count == 1);
    o.threshold = 2;
    CHECK_OK(br_diff(&av, &bv, &o, nullptr, 0, &res));
    CHECK(res.changed_pixels == 0);
    br_image_free(&a);
    br_image_free(&b);
}

TEST(tile_hashes) {
    br_image a = brt::make_ui_image(256, 128, BR_PIXEL_RGBA8, 1);
    const br_image_view av = br_image_as_view(&a);
    uint64_t h1[64], h2[64];
    size_t n = 0;
    const uint64_t c1 = br_hash_image_tiles(&av, 32, 32, h1, 64, &n);
    CHECK(n == 32);
    a.data[40 * a.stride + 4 * 40] ^= 1;
    const uint64_t c2 = br_hash_image_tiles(&av, 32, 32, h2, 64, &n);
    CHECK(c1 != c2);
    int changed = 0;
    for (size_t i = 0; i < n; ++i) changed += h1[i] != h2[i];
    CHECK(changed == 1 && h1[9] != h2[9]);
    br_image_free(&a);
}

TEST(draw_grid_text_marks) {
    br_image img{};
    CHECK_OK(br_image_create(640, 360, BR_PIXEL_BGRA8, &img));
    br_image_fill(&img, 0xffffffff);
    br_grid_options g = br_grid_options_default();
    g.label_transform = {2.0, 2.0, 100.0, 0.0}; // Пиксели изображения -> пиксели экрана.
    CHECK_OK(br_draw_grid(&img, &g));
    br_rect_i32 r[2] = {{10, 40, 50, 20}, {200, 200, 80, 40}};
    CHECK_OK(br_draw_marks(&img, r, 2, 1, 0));
    CHECK_OK(br_draw_text(&img, 300, 100, "Hello, 1C! 42%", 0xff000000, 0x80ffff00, 2));
    uint32_t w = 0, h = 0;
    br_measure_text("abc\nde", 2, &w, &h);
    CHECK(w == 34 && h == 30);
    
    size_t non_white = 0;
    for (uint32_t i = 0; i < 640u * 360u; ++i) non_white += img.data[4 * i] != 255;
    CHECK(non_white > 1000);
    br_image_free(&img);
}

TEST(brf_container) {
    uint8_t px[16] = {};
    br_image_view v{px, 2, 2, 8, BR_PIXEL_RGBA8, BR_COLOR_SRGB, 0};
    size_t need = 0;
    CHECK_OK(br_brf_pack_raw(&v, nullptr, 0, nullptr, 0, &need));
    CHECK(need > 16);
    uint8_t out[256];
    CHECK_OK(br_brf_pack_raw(&v, nullptr, 0, out, sizeof(out), &need));
    CHECK(out[0] == 'B' && out[1] == 'R' && out[2] == 'F' && out[3] == '1');
    CHECK(br_brf_pack_raw(&v, nullptr, 0, out, 4, &need) == BR_E_BUFFER_TOO_SMALL);
}
