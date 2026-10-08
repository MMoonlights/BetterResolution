#pragma once

#include <br/br.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace brt {

struct Case {
    const char* name;
    void (*fn)();
};

std::vector<Case>& registry();
extern int g_failures;

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

void check(bool ok, const char* expr, const char* file, int line);
void check_status(br_status st, const char* expr, const char* file, int line);

// Детерминированный генератор псевдослучайных чисел (xorshift).
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<uint32_t>(s >> 11);
    }
};

// Синтетическое изображение «интерфейса»: плоские панели, тонкие линии и небольшие штрихи, похожие на символы.
br_image make_ui_image(uint32_t w, uint32_t h, br_pixel_format fmt, uint64_t seed);
// Плавный градиент с шумом (изображение, похожее на фотографию).
br_image make_photo_image(uint32_t w, uint32_t h, br_pixel_format fmt, uint64_t seed);
bool images_equal(const br_image_view& a, const br_image_view& b);
double psnr(const br_image_view& a, const br_image_view& b);

} 

#define TEST(name)                                              \
    static void name();                                         \
    static brt::Registrar name##_registrar(#name, &name);       \
    static void name()

#define CHECK(x) brt::check(!!(x), #x, __FILE__, __LINE__)
#define CHECK_OK(x) brt::check_status((x), #x, __FILE__, __LINE__)
