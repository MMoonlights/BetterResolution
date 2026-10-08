#include "test_framework.hpp"

#include <algorithm>

#include <cstring>

using brt::images_equal;

static void roundtrip_lossless(br_encoded_format fmt, br_pixel_format pf, bool photo) {
    br_image img = photo ? brt::make_photo_image(97, 61, pf, 3) : brt::make_ui_image(203, 117, pf, 7);
    const br_image_view v = br_image_as_view(&img);
    for (int effort : {0, 1, 4, 9}) {
        br_encode_options o = br_encode_options_default(fmt);
        o.effort = effort;
        uint8_t* data = nullptr;
        size_t size = 0;
        CHECK_OK(br_encode_alloc(&v, &o, &data, &size));
        br_image info_img{};
        br_image_info info{};
        CHECK_OK(br_probe(data, size, &info));
        CHECK(info.width == img.width && info.height == img.height);
        CHECK_OK(br_decode(data, size, pf, &info_img));
        const br_image_view d = br_image_as_view(&info_img);
        CHECK(images_equal(v, d));
        br_image_free(&info_img);
        br_free(data);
        if (fmt != BR_ENCODE_PNG) break;
    }
    br_image_free(&img);
}

TEST(png_roundtrip_formats) {
    for (br_pixel_format pf : {BR_PIXEL_GRAY8, BR_PIXEL_RGB8, BR_PIXEL_RGBA8, BR_PIXEL_BGRA8, BR_PIXEL_BGR8}) {
        roundtrip_lossless(BR_ENCODE_PNG, pf, false);
        roundtrip_lossless(BR_ENCODE_PNG, pf, true);
    }
}

TEST(png_alpha_and_palette) {
    br_image img{};
    CHECK_OK(br_image_create(40, 30, BR_PIXEL_RGBA8, &img));
    for (uint32_t y = 0; y < 30; ++y)
        for (uint32_t x = 0; x < 40; ++x) {
            uint8_t* p = img.data + y * img.stride + 4 * x;
            p[0] = static_cast<uint8_t>((x / 10) * 60);
            p[1] = static_cast<uint8_t>((y / 10) * 90);
            p[2] = 30;
            p[3] = static_cast<uint8_t>(x < 20 ? 255 : 128);
        }
    const br_image_view v = br_image_as_view(&img);
    br_encode_options o = br_encode_options_default(BR_ENCODE_PNG);
    uint8_t* data = nullptr;
    size_t size = 0;
    CHECK_OK(br_encode_alloc(&v, &o, &data, &size));
    // Блоки 4x3 с двумя уровнями альфа дают мало цветов и палитру с tRNS.
    CHECK(size > 8 && data[25] == 3);
    br_image back{};
    CHECK_OK(br_decode(data, size, BR_PIXEL_RGBA8, &back));
    const br_image_view bv = br_image_as_view(&back);
    CHECK(images_equal(v, bv));
    br_image_free(&back);
    br_free(data);
    br_image_free(&img);
}

TEST(png_palette_preserves_straight_alpha_for_premultiplied_icons) {
    for (br_pixel_format pf : {BR_PIXEL_RGBA8, BR_PIXEL_BGRA8}) {
        br_image icon{};
        CHECK_OK(br_image_create(17, 5, pf, &icon));
        icon.premultiplied_alpha = 1;
        const uint8_t pixels[][4] = {{32, 64, 96, 128}, {0, 0, 0, 0}, {12, 37, 49, 63}, {50, 40, 30, 255}};
        for (uint32_t y = 0; y < icon.height; ++y)
            for (uint32_t x = 0; x < icon.width; ++x)
                std::memcpy(icon.data + y * icon.stride + 4 * x, pixels[(x + y) % 4], 4);
        const auto view = br_image_as_view(&icon);
        br_image straight{};
        CHECK_OK(br_image_create(icon.width, icon.height, pf, &straight));
        CHECK_OK(br_convert(&view, &straight));
        for (int palette : {BR_PALETTE_AUTO, BR_PALETTE_OFF}) {
            auto options = br_encode_options_default(BR_ENCODE_PNG);
            options.png_palette = palette;
            uint8_t* data = nullptr;
            size_t size = 0;
            CHECK_OK(br_encode_alloc(&view, &options, &data, &size));
            CHECK(data[25] == (palette == BR_PALETTE_AUTO ? 3 : 6));
            br_image decoded{};
            CHECK_OK(br_decode(data, size, pf, &decoded));
            CHECK(!decoded.premultiplied_alpha);
            CHECK(images_equal(br_image_as_view(&straight), br_image_as_view(&decoded)));
            br_image_free(&decoded);
            br_free(data);
        }
        br_image_free(&straight);
        br_image_free(&icon);
    }
}

TEST(png_palette_all_depths_decode_directly_to_requested_format) {
    for (uint32_t count : {2u, 3u, 13u, 33u, 193u}) {
        br_image icon{};
        CHECK_OK(br_image_create(33, 17, BR_PIXEL_RGBA8, &icon));
        for (uint32_t y = 0; y < icon.height; ++y)
            for (uint32_t x = 0; x < icon.width; ++x) {
                const uint8_t i = static_cast<uint8_t>((x + y * icon.width) % count);
                const uint8_t color[] = {i, static_cast<uint8_t>(i ^ 93u), static_cast<uint8_t>(i * 7u),
                                         static_cast<uint8_t>(i ? 255 : 17)};
                std::memcpy(icon.data + y * icon.stride + 4 * x, color, 4);
            }
        const auto view = br_image_as_view(&icon);
        const auto options = br_encode_options_default(BR_ENCODE_PNG);
        uint8_t* data = nullptr;
        size_t size = 0;
        CHECK_OK(br_encode_alloc(&view, &options, &data, &size));
        CHECK(data[25] == 3);
        CHECK(data[24] == (count <= 2 ? 1 : count <= 4 ? 2 : count <= 16 ? 4 : 8));
        for (br_pixel_format pf : {BR_PIXEL_GRAY8, BR_PIXEL_RGB8, BR_PIXEL_BGR8, BR_PIXEL_RGBA8, BR_PIXEL_BGRA8}) {
            br_image expected{}, decoded{};
            CHECK_OK(br_image_clone(&view, pf, &expected));
            CHECK_OK(br_decode(data, size, pf, &decoded));
            CHECK(decoded.format == pf);
            CHECK(images_equal(br_image_as_view(&expected), br_image_as_view(&decoded)));
            br_image_free(&expected);
            br_image_free(&decoded);
        }
        br_image natural{};
        CHECK_OK(br_decode(data, size, BR_PIXEL_UNKNOWN, &natural));
        CHECK(natural.format == BR_PIXEL_RGBA8);
        CHECK(images_equal(view, br_image_as_view(&natural)));
        br_image_free(&natural);
        br_free(data);
        br_image_free(&icon);
    }
}

TEST(png_gray_auto_palette_boundary_preserves_pixels_and_truecolor_bytes) {
    for (uint32_t count : {16u, 17u, 256u}) {
        br_image gray{};
        CHECK_OK(br_image_create(67, 19, BR_PIXEL_GRAY8, &gray));
        for (uint32_t y = 0; y < gray.height; ++y)
            for (uint32_t x = 0; x < gray.width; ++x) {
                const uint32_t i = (x + y * gray.width) % count;
                gray.data[y * gray.stride + x] = static_cast<uint8_t>(i * 255u / (count - 1));
            }
        const auto gray_view = br_image_as_view(&gray);
        for (br_pixel_format pf : {BR_PIXEL_GRAY8, BR_PIXEL_RGB8, BR_PIXEL_BGR8, BR_PIXEL_RGBA8, BR_PIXEL_BGRA8}) {
            br_image source{};
            CHECK_OK(br_image_clone(&gray_view, pf, &source));
            const auto source_view = br_image_as_view(&source);
            for (int effort : {0, 1, 4, 9}) {
                auto options = br_encode_options_default(BR_ENCODE_PNG);
                options.effort = effort;
                uint8_t* data = nullptr;
                size_t size = 0;
                CHECK_OK(br_encode_alloc(&source_view, &options, &data, &size));
                CHECK(data[25] == (count == 16 ? 3 : 0));
                CHECK(data[24] == (count == 16 ? 4 : 8));
                br_image decoded{};
                CHECK_OK(br_decode(data, size, BR_PIXEL_GRAY8, &decoded));
                CHECK(images_equal(gray_view, br_image_as_view(&decoded)));
                br_image_free(&decoded);
                if (count > 16) {
                    // Отброшенная палитра AUTO не должна влиять на фактический поток PNG:
                    // сравнивает все байты с вариантом GRAY8/OFF.
                    options.png_palette = BR_PALETTE_OFF;
                    uint8_t* plain = nullptr;
                    size_t plain_size = 0;
                    CHECK_OK(br_encode_alloc(&source_view, &options, &plain, &plain_size));
                    CHECK(size == plain_size);
                    CHECK(size == plain_size && std::memcmp(data, plain, size) == 0);
                    br_free(plain);
                }
                br_free(data);
            }
            br_image_free(&source);
        }
        br_image_free(&gray);
    }
}

TEST(png_quantize_is_close) {
    br_image img = brt::make_photo_image(128, 96, BR_PIXEL_RGB8, 5);
    const br_image_view v = br_image_as_view(&img);
    br_encode_options o = br_encode_options_default(BR_ENCODE_PNG);
    o.png_palette = BR_PALETTE_QUANTIZE;
    o.max_colors = 256;
    uint8_t* data = nullptr;
    size_t size = 0;
    CHECK_OK(br_encode_alloc(&v, &o, &data, &size));
    br_image back{};
    CHECK_OK(br_decode(data, size, BR_PIXEL_RGB8, &back));
    const br_image_view bv = br_image_as_view(&back);
    CHECK(brt::psnr(v, bv) > 28.0);
    br_image_free(&back);
    br_free(data);
    br_image_free(&img);
}

TEST(jpeg_roundtrip_quality) {
    for (int sub : {1, 0}) {
        br_image img = brt::make_photo_image(161, 99, BR_PIXEL_BGRA8, 11);
        const br_image_view v = br_image_as_view(&img);
        br_encode_options o = br_encode_options_default(BR_ENCODE_JPEG);
        o.quality = 92;
        o.force_444 = static_cast<uint8_t>(sub);
        uint8_t* data = nullptr;
        size_t size = 0;
        CHECK_OK(br_encode_alloc(&v, &o, &data, &size));
        CHECK(size > 100 && data[0] == 0xff && data[1] == 0xd8 && data[size - 2] == 0xff && data[size - 1] == 0xd9);
        br_image back{};
        CHECK_OK(br_decode(data, size, BR_PIXEL_BGRA8, &back));
        const br_image_view bv = br_image_as_view(&back);
        CHECK(brt::psnr(v, bv) > (sub ? 34.0 : 30.0));
        br_image_free(&back);
        br_free(data);
        br_image_free(&img);
    }
}

TEST(jpeg_gray_content_is_single_component) {
    br_image img = brt::make_photo_image(64, 48, BR_PIXEL_GRAY8, 2);
    const br_image_view v = br_image_as_view(&img);
    br_encode_options o = br_encode_options_default(BR_ENCODE_JPEG);
    uint8_t* data = nullptr;
    size_t size = 0;
    CHECK_OK(br_encode_alloc(&v, &o, &data, &size));
    br_image_info info{};
    CHECK_OK(br_probe(data, size, &info));
    CHECK(info.channels == 1);
    br_free(data);
    br_image_free(&img);
}

TEST(jpeg_gray_roundtrip_requested_formats) {
    br_image source = brt::make_photo_image(97, 61, BR_PIXEL_GRAY8, 74);
    const auto view = br_image_as_view(&source);
    auto options = br_encode_options_default(BR_ENCODE_JPEG);
    options.quality = 92;
    uint8_t* data = nullptr;
    size_t size = 0;
    CHECK_OK(br_encode_alloc(&view, &options, &data, &size));
    br_image natural{};
    CHECK_OK(br_decode(data, size, BR_PIXEL_UNKNOWN, &natural));
    CHECK(natural.format == BR_PIXEL_GRAY8 && natural.width == 97 && natural.height == 61);
    const auto natural_view = br_image_as_view(&natural);
    CHECK(brt::psnr(view, natural_view) > 35.0);
    for (br_pixel_format pf : {BR_PIXEL_GRAY8, BR_PIXEL_RGB8, BR_PIXEL_BGR8, BR_PIXEL_RGBA8, BR_PIXEL_BGRA8}) {
        br_image expected{}, decoded{};
        CHECK_OK(br_image_clone(&natural_view, pf, &expected));
        CHECK_OK(br_decode(data, size, pf, &decoded));
        CHECK(images_equal(br_image_as_view(&expected), br_image_as_view(&decoded)));
        br_image_free(&expected);
        br_image_free(&decoded);
    }
    br_image_free(&natural);
    br_free(data);
    br_image_free(&source);
}

TEST(simple_formats_roundtrip) {
    for (br_encoded_format f : {BR_ENCODE_BMP, BR_ENCODE_QOI, BR_ENCODE_PNM}) {
        for (br_pixel_format pf : {BR_PIXEL_RGB8, BR_PIXEL_RGBA8}) {
            if (f == BR_ENCODE_PNM && pf == BR_PIXEL_RGBA8) continue;
            br_image img = brt::make_ui_image(77, 41, pf, 9);
            if (pf == BR_PIXEL_RGBA8) img.data[3] = 7; // Создаёт полупрозрачный пиксель.
            const br_image_view v = br_image_as_view(&img);
            br_encode_options o = br_encode_options_default(f);
            uint8_t* data = nullptr;
            size_t size = 0;
            CHECK_OK(br_encode_alloc(&v, &o, &data, &size));
            br_image back{};
            CHECK_OK(br_decode(data, size, pf, &back));
            const br_image_view bv = br_image_as_view(&back);
            CHECK(images_equal(v, bv));
            br_image_free(&back);
            br_free(data);
            br_image_free(&img);
        }
    }
}

TEST(decoder_rejects_garbage) {
    uint8_t junk[64];
    for (int i = 0; i < 64; ++i) junk[i] = static_cast<uint8_t>(i * 37);
    br_image img{};
    CHECK(br_decode(junk, sizeof(junk), BR_PIXEL_UNKNOWN, &img) != BR_OK);
    // Усечённые PNG/JPEG должны завершаться ошибкой без аварийного завершения.
    br_image src = brt::make_photo_image(50, 40, BR_PIXEL_RGB8, 1);
    const br_image_view v = br_image_as_view(&src);
    for (br_encoded_format f : {BR_ENCODE_PNG, BR_ENCODE_JPEG}) {
        br_encode_options o = br_encode_options_default(f);
        uint8_t* data = nullptr;
        size_t size = 0;
        CHECK_OK(br_encode_alloc(&v, &o, &data, &size));
        for (size_t cut : {size / 3, size / 2, size - 3}) {
            br_image tmp{};
            const br_status st = br_decode(data, cut, BR_PIXEL_UNKNOWN, &tmp);
            if (st == BR_OK) br_image_free(&tmp); // Декодеры JPEG могут вернуть частичное изображение.
        }
        
        brt::Rng rng(42);
        for (int i = 0; i < 50; ++i) {
            const size_t pos = 20 + rng.next() % (size - 20);
            data[pos] ^= static_cast<uint8_t>(1u << (rng.next() % 8));
            br_image tmp{};
            if (br_decode(data, size, BR_PIXEL_UNKNOWN, &tmp) == BR_OK) br_image_free(&tmp);
        }
        br_free(data);
    }
    br_image_free(&src);
}

TEST(base64) {
    const char* in = "BetterResolution";
    char out[64];
    const size_t n = br_base64_encode(in, std::strlen(in), out, sizeof(out));
    CHECK(n == 25);
    CHECK(std::strcmp(out, "QmV0dGVyUmVzb2x1dGlvbg==") == 0);
    CHECK(br_base64_encode("", 0, out, sizeof(out)) == 1 && out[0] == 0);
}

#include "codecs/deflate.hpp"
#include "core/common.hpp"

namespace {
void png_test_chunk(br::Bytes& dst, const char* name, const uint8_t* data, size_t size) {
    dst.be32(static_cast<uint32_t>(size));
    const size_t from=dst.size();dst.append(name,4);dst.append(data,size);
    dst.be32(br::crc32(dst.data()+from,dst.size()-from));
}
void png_test_header(br::Bytes& dst, uint8_t depth, uint8_t color_type, uint8_t interlace = 0) {
    const uint8_t sig[] = {137,80,78,71,13,10,26,10};
    dst.append(sig, sizeof(sig));
    const uint8_t ihdr[] = {0,0,0,1,0,0,0,1,depth,color_type,0,0,interlace};
    png_test_chunk(dst, "IHDR", ihdr, sizeof(ihdr));
}
void png_test_data(br::Bytes& dst, const uint8_t* row, size_t size) {
    br::Bytes compressed;
    br::deflate::Deflater z(compressed, 1, true);
    z.write(row, size);
    z.finish();
    png_test_chunk(dst, "IDAT", compressed.data(), compressed.size());
    png_test_chunk(dst, "IEND", nullptr, 0);
}
}

TEST(png_rejects_undefined_palette_pixels_and_invalid_chunk_structure) {
    const uint8_t palette[] = {10,20,30,40,50,60};
    const uint8_t invalid_pixel[] = {0,2};
    for (uint8_t interlace : {uint8_t(0), uint8_t(1)}) {
        br::Bytes png;
        png_test_header(png, 8, 3, interlace);
        png_test_chunk(png, "PLTE", palette, sizeof(palette));
        png_test_data(png, invalid_pixel, sizeof(invalid_pixel));
        br_image decoded{};
        CHECK(br_decode(png.data(), png.size(), BR_PIXEL_BGRA8, &decoded) == BR_E_DECODE);
        CHECK(decoded.data == nullptr);
    }
    const uint8_t valid_pixel[] = {0,0};
    for (int invalid : {0,1,2,3,4}) {
        br::Bytes png;
        png_test_header(png, 1, 3);
        if (invalid == 0) png_test_chunk(png, "PLTE", nullptr, 0);
        else png_test_chunk(png, "PLTE", palette, sizeof(palette));
        if (invalid == 1) png_test_chunk(png, "PLTE", palette, sizeof(palette));
        if (invalid == 2) {
            const uint8_t transparency[] = {0,1,2};
            png_test_chunk(png, "tRNS", transparency, sizeof(transparency));
        }
        if (invalid == 3) {
            const uint8_t ihdr[] = {0,0,0,1,0,0,0,1,1,3,0,0,0};
            png_test_chunk(png, "IHDR", ihdr, sizeof(ihdr));
        }
        if (invalid == 4) png_test_chunk(png, "ABCD", nullptr, 0);
        png_test_data(png, valid_pixel, sizeof(valid_pixel));
        br_image decoded{};
        CHECK(br_decode(png.data(), png.size(), BR_PIXEL_UNKNOWN, &decoded) == (invalid == 4 ? BR_E_UNSUPPORTED : BR_E_DECODE));
        CHECK(decoded.data == nullptr);
    }
}

TEST(png_probe_rejects_bad_header_checksum) {
    const uint8_t row[] = {0,0};
    br::Bytes png;
    png_test_header(png, 8, 0);
    png_test_data(png, row, sizeof(row));
    png[29] ^= 1;
    br_image_info info{};
    CHECK(br_probe(png.data(), png.size(), &info) == BR_E_DECODE);
}

TEST(png_idat_chunks_must_be_consecutive) {
    const uint8_t row[] = {0,31,47,61};
    const uint8_t text[] = {'k',0,'v'};
    br::Bytes stream;
    br::deflate::zlib_compress(row, sizeof(row), 1, stream);
    for (bool interrupt : {false, true}) {
        br::Bytes png;
        png_test_header(png, 8, 2);
        png_test_chunk(png, "tEXt", text, sizeof(text)); // Вспомогательные данные перед IDAT допустимы.
        const size_t split = stream.size() / 2;
        png_test_chunk(png, "IDAT", stream.data(), split);
        if (interrupt) png_test_chunk(png, "tEXt", text, sizeof(text));
        png_test_chunk(png, "IDAT", stream.data() + split, stream.size() - split);
        png_test_chunk(png, "IEND", nullptr, 0);
        br_image decoded{};
        const br_status status = br_decode(png.data(), png.size(), BR_PIXEL_RGB8, &decoded);
        if (interrupt) CHECK(status == BR_E_DECODE && decoded.data == nullptr);
        else {
            CHECK(status == BR_OK && decoded.data != nullptr);
            if (decoded.data) CHECK(std::memcmp(decoded.data, row + 1, 3) == 0);
        }
        br_image_free(&decoded);
    }
}
TEST(png_does_not_hide_inflate_failure_after_full_output) {
    const uint8_t row[]={0, 30,40,50, 10,20,30}; // Два пикселя RGB, фильтр 0.
    br::Bytes stream;
    stream.push(0x78);stream.push(0x01);
    br::deflate::Deflater z(stream,1,false);
    z.write(row,sizeof(row));z.finish_partial();
    stream.push(7); // Последний блок с зарезервированным типом 3 после полного изображения.
    stream.be32(br::adler32_update(1,row,sizeof(row)));
    br::Bytes png;const uint8_t sig[]={137,80,78,71,13,10,26,10};png.append(sig,8);
    const uint8_t ihdr[]={0,0,0,2,0,0,0,1,8,2,0,0,0};
    png_test_chunk(png,"IHDR",ihdr,sizeof(ihdr));
    png_test_chunk(png,"IDAT",stream.data(),stream.size());
    png_test_chunk(png,"IEND",nullptr,0);
    br_image out{};
    CHECK(br_decode(png.data(),png.size(),BR_PIXEL_RGBA8,&out)==BR_E_DECODE);
    CHECK(out.data==nullptr);
}

TEST(png_checks_both_chunk_crc_and_zlib_adler) {
    br_image src=brt::make_photo_image(39,17,BR_PIXEL_RGB8,81);
    const auto view=br_image_as_view(&src);
    auto opt=br_encode_options_default(BR_ENCODE_PNG);opt.png_palette=BR_PALETTE_OFF;
    uint8_t* data=nullptr;size_t size=0;CHECK_OK(br_encode_alloc(&view,&opt,&data,&size));
    std::vector<uint8_t> clean(data,data+size);br_free(data);br_image_free(&src);
    for(size_t pos=8;pos+12<=size;) {
        const size_t n=br::load_be32(clean.data()+pos);
        if(std::memcmp(clean.data()+pos+4,"IDAT",4)==0) {
            auto bad=clean;bad[pos+8+n-1]^=1;
            // Сначала неверная CRC, затем правильная CRC, но неверная внутренняя сумма Adler-32.
            for(int repair=0;repair<2;++repair) {
                if(repair) {
                    const uint32_t crc=br::crc32(bad.data()+pos+4,n+4);
                    for(int k=0;k<4;++k)bad[pos+8+n+static_cast<size_t>(k)]=static_cast<uint8_t>(crc>>(24-8*k));
                }
                br_image out{};
                CHECK(br_decode(bad.data(),bad.size(),BR_PIXEL_UNKNOWN,&out)==BR_E_DECODE);
                CHECK(out.data==nullptr);
            }
            break;
        }
        pos+=n+12;
    }
    for(size_t cut=size-12;cut<size;++cut) {
        br_image out{};CHECK(br_decode(clean.data(),cut,BR_PIXEL_UNKNOWN,&out)==BR_E_DECODE);
        CHECK(out.data==nullptr);
    }
}


TEST(bmp_top_bit_masks_do_not_shift_by_32) {
    std::vector<uint8_t> bmp(74,0);
    auto put32 = [&](size_t pos,uint32_t value) {
        for(int k=0;k<4;++k) bmp[pos+static_cast<size_t>(k)]=static_cast<uint8_t>(value>>(8*k));
    };
    bmp[0]='B';bmp[1]='M';put32(2,74);put32(10,70);put32(14,56);
    put32(18,1);put32(22,1);bmp[26]=1;bmp[28]=32;put32(30,3);
    put32(54,0xff000000u);put32(58,0x00ff0000u);put32(62,0x0000ff00u);put32(66,0x000000ffu);
    put32(70,0x11223344u);
    br_image img{};
    CHECK_OK(br_decode(bmp.data(),bmp.size(),BR_PIXEL_RGBA8,&img));
    const uint8_t expected[]={0x11,0x22,0x33,0x44};
    CHECK(img.data && std::memcmp(img.data,expected,4)==0);br_image_free(&img);
    // Полная 32-битная выборка проверяет, что countr_one(0xffffffff) возвращает 32.
    put32(54,0xffffffffu);put32(58,0);put32(62,0);put32(66,0);put32(70,0xffffffffu);
    CHECK_OK(br_decode(bmp.data(),bmp.size(),BR_PIXEL_RGB8,&img));
    CHECK(img.data && img.data[0]==255 && img.data[1]==0 && img.data[2]==0);br_image_free(&img);
}
