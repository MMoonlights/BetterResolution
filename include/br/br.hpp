// RAII-обёртки C++ для C API.
#ifndef BETTERRESOLUTION_BR_HPP
#define BETTERRESOLUTION_BR_HPP

#include "br.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace br {

class Error : public std::runtime_error {
public:
    Error(br_status s, const std::string& what)
        : std::runtime_error(what + ": " + br_status_string(s) + detail()), status_(s) {}
    br_status status() const noexcept { return status_; }

private:
    static std::string detail() {
        const char* d = br_last_error();
        return d && *d ? std::string(" (") + d + ")" : std::string();
    }
    br_status status_;
};

inline void check(br_status s, const char* what) {
    if (s != BR_OK) throw Error(s, what);
}

struct Fit {
    br_fit_options o{};
    static Fit long_edge(uint32_t n) { Fit f; f.o.max_long_edge = n; return f; }
    static Fit box(uint32_t w, uint32_t h) { Fit f; f.o.max_width = w; f.o.max_height = h; return f; }
    static Fit pixels(uint64_t n) { Fit f; f.o.max_pixels = n; return f; }
    Fit& multiple_of(uint32_t m) { o.multiple_of = m; return *this; }
    Fit& upscale(bool on = true) { o.allow_upscale = on ? 1 : 0; return *this; }
};

struct Resize {
    br_resize_options o = br_resize_options_for(BR_RESIZE_UI_TEXT);
    static Resize mode(br_resize_mode m) { Resize r; r.o = br_resize_options_for(m); return r; }
    Resize& filter(br_filter f) { o.filter = f; return *this; }
    Resize& linear(bool on) { o.linear_light = on ? 1 : 0; return *this; }
    Resize& antiring(bool on) { o.antiring = on ? 1 : 0; return *this; }
};

struct Encode {
    br_encode_options o = br_encode_options_default(BR_ENCODE_PNG);
    static Encode png(int effort = 1) { Encode e; e.o.effort = effort; return e; }
    static Encode jpeg(int quality = 90, bool chroma_444 = true) {
        Encode e;
        e.o = br_encode_options_default(BR_ENCODE_JPEG);
        e.o.quality = quality;
        e.o.force_444 = chroma_444 ? 1 : 0;
        return e;
    }
    static Encode format(br_encoded_format f) { Encode e; e.o = br_encode_options_default(f); return e; }
};

class Image {
public:
    Image() = default;
    explicit Image(br_image raw) : img_(raw) {}
    Image(uint32_t w, uint32_t h, br_pixel_format f = BR_PIXEL_BGRA8) { check(br_image_create(w, h, f, &img_), "br_image_create"); }
    Image(const Image& o) { if (o.img_.data) { const br_image_view v = o.view(); check(br_image_clone(&v, BR_PIXEL_UNKNOWN, &img_), "br_image_clone"); } }
    Image(Image&& o) noexcept : img_(o.img_) { o.img_ = br_image{}; }
    Image& operator=(Image o) noexcept { std::swap(img_, o.img_); return *this; }
    ~Image() { br_image_free(&img_); }

    static Image load(const std::string& utf8_path, br_pixel_format f = BR_PIXEL_UNKNOWN) {
        Image i;
        check(br_load(utf8_path.c_str(), f, &i.img_), "br_load");
        return i;
    }
    static Image decode(const void* data, size_t size, br_pixel_format f = BR_PIXEL_UNKNOWN) {
        Image i;
        check(br_decode(data, size, f, &i.img_), "br_decode");
        return i;
    }

    uint32_t width() const noexcept { return img_.width; }
    uint32_t height() const noexcept { return img_.height; }
    br_pixel_format format() const noexcept { return img_.format; }
    uint8_t* data() noexcept { return img_.data; }
    const uint8_t* data() const noexcept { return img_.data; }
    ptrdiff_t stride() const noexcept { return img_.stride; }
    bool empty() const noexcept { return img_.data == nullptr; }
    br_image_view view() const noexcept { return br_image_as_view(&img_); }
    br_mut_image_view* raw() noexcept { return &img_; }

    Image convert(br_pixel_format f) const {
        Image o;
        const br_image_view v = view();
        check(br_image_clone(&v, f, &o.img_), "br_image_clone");
        return o;
    }
    Image crop(br_rect_i32 r) const {
        const br_image_view v = view();
        br_image_view c;
        check(br_image_crop(&v, r, &c), "br_image_crop");
        Image o;
        check(br_image_clone(&c, BR_PIXEL_UNKNOWN, &o.img_), "br_image_clone");
        return o;
    }
    Image resize(uint32_t w, uint32_t h, const Resize& r = Resize(), br_context* ctx = nullptr) const {
        Image o;
        const br_image_view v = view();
        check(br_resize_to(ctx, &v, w, h, &r.o, &o.img_), "br_resize_to");
        return o;
    }
    // Возвращает изображение и преобразование из выходных координат в исходные.
    std::pair<Image, br_transform> fit(const Fit& f, const Resize& r = Resize(), br_context* ctx = nullptr) const {
        Image o;
        br_transform t{};
        const br_image_view v = view();
        check(br_resize_fit(ctx, &v, &f.o, &r.o, &o.img_, &t), "br_resize_fit");
        return {std::move(o), br_transform_inverse(t)};
    }
    std::pair<Image, br_transform> zoom(br_rect_i32 region, uint32_t w, uint32_t h = 0, const Resize& r = Resize(),
                                        br_context* ctx = nullptr) const {
        Image o;
        br_transform t{};
        const br_image_view v = view();
        check(br_zoom(ctx, &v, region, w, h, &r.o, &o.img_, &t), "br_zoom");
        return {std::move(o), t};
    }
    std::vector<uint8_t> encode(const Encode& e = Encode()) const {
        uint8_t* data = nullptr;
        size_t size = 0;
        const br_image_view v = view();
        check(br_encode_alloc(&v, &e.o, &data, &size), "br_encode_alloc");
        std::vector<uint8_t> out(data, data + size);
        br_free(data);
        return out;
    }
    void save(const std::string& utf8_path) const {
        const br_image_view v = view();
        check(br_save(utf8_path.c_str(), &v, nullptr), "br_save");
    }
    void save(const std::string& utf8_path, const Encode& e) const {
        const br_image_view v = view();
        check(br_save(utf8_path.c_str(), &v, &e.o), "br_save");
    }
    Image& grid(const br_grid_options& g = br_grid_options_default()) { check(br_draw_grid(&img_, &g), "br_draw_grid"); return *this; }
    Image& marks(const std::vector<br_rect_i32>& r, uint32_t first = 1) {
        check(br_draw_marks(&img_, r.data(), r.size(), first, 0), "br_draw_marks");
        return *this;
    }

private:
    br_image img_{};
};

struct Diff {
    br_diff_result result{};
    std::vector<br_rect_i32> rects;
    bool changed() const noexcept { return result.changed_pixels > 0; }
};

inline Diff diff(const Image& before, const Image& after, br_diff_options o = br_diff_options_default()) {
    Diff d;
    d.rects.resize(1024);
    const br_image_view a = before.view(), b = after.view();
    check(br_diff(&a, &b, &o, d.rects.data(), d.rects.size(), &d.result), "br_diff");
    d.rects.resize(d.result.rect_count < d.rects.size() ? d.result.rect_count : d.rects.size());
    return d;
}

struct Frame {
    Image image;
    br_frame_info info{};
    std::vector<br_rect_i32> dirty;
};

class Capture {
public:
    explicit Capture(const br_capture_options& o = br_capture_options_default(), br_context* ctx = nullptr) {
        check(br_capture_open(ctx, &o, &cap_), "br_capture_open");
    }
    static br_capture_options window(const std::string& utf8_title_part, bool client_only = false) {
        br_window_info w{};
        check(br_find_window(utf8_title_part.c_str(), &w), "br_find_window");
        br_capture_options o = br_capture_options_default();
        o.target = BR_TARGET_WINDOW;
        o.window = w.handle;
        o.client_area = client_only ? 1 : 0;
        return o;
    }
    static br_capture_options monitor(uint32_t index = 0) {
        br_capture_options o = br_capture_options_default();
        o.monitor = index;
        return o;
    }
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    Capture(Capture&& o) noexcept : cap_(o.cap_) { o.cap_ = nullptr; }
    ~Capture() { br_capture_destroy(cap_); }

    // Последний доступный кадр; изображение может браться из кэша, если не менялось.
    Frame grab(uint32_t timeout_ms = 1000) {
        Frame f;
        f.dirty.resize(256);
        check(br_capture_grab(cap_, timeout_ms, f.image.raw(), &f.info, f.dirty.data(), f.dirty.size()), "br_capture_grab");
        f.dirty.resize(f.info.dirty_count < f.dirty.size() ? f.info.dirty_count : f.dirty.size());
        return f;
    }
    // image_to_screen учитывает изменение размера.
    Frame grab_fit(const Fit& fit, const Resize& r = Resize(), uint32_t timeout_ms = 1000) {
        Frame f;
        check(br_capture_grab_fit(cap_, timeout_ms, &fit.o, &r.o, f.image.raw(), &f.info), "br_capture_grab_fit");
        return f;
    }

private:
    br_capture* cap_{};
};

inline std::pair<double, double> map(const br_transform& t, double x, double y) {
    return {x * t.sx + t.tx, y * t.sy + t.ty};
}

} // пространство имён br

#endif
