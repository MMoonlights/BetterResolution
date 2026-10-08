#include <br/br.h>

#include "api/capture_internal.hpp"
#include "api/context.hpp"
#include "core/common.hpp"
#include "core/frame.hpp"
#include "platform/capture.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <cstring>
#include <memory>
#include <vector>

br_capture::~br_capture() { br::free_image(scratch); }

using namespace br;

namespace {
template <class T>
br_status copy_list(const std::vector<T>& v, T* out, size_t cap, size_t* count) {
    if (count) *count = v.size();
    if (out) std::copy_n(v.begin(), std::min(cap, v.size()), out);
    return BR_OK;
}

void copy_dirty(const std::vector<br_rect_i32>& d, br_rect_i32* out, size_t cap) {
    if (out) std::copy_n(d.begin(), std::min(cap, d.size()), out);
}

br_status make_capture(br_context* ctx, std::unique_ptr<capture::Session> s, br_capture** out) {
    auto* c = new br_capture();
    c->context = resolve_context(ctx);
    c->session = std::move(s);
    *out = c;
    return BR_OK;
}
}

namespace br {

br_status open_capture(br_context* ctx, const br_capture_options& o, bool cold_start_first, br_capture** out) {
    *out = nullptr;
    return guarded([&] {
        std::unique_ptr<capture::Session> s;
        const br_status st = capture::open(o, s, cold_start_first);
        if (st != BR_OK) return st;
        return make_capture(ctx, std::move(s), out);
    });
}

br_status grab_fit_timed(br_capture* c, uint32_t timeout_ms, const br_fit_options* fit, const br_resize_options* opt,
                         br_image* out, br_frame_info* info, GrabTimings* timings, double scale) {
    GrabTimings t;
    uint32_t sw = 0, sh = 0;
    br_status st = c->session->size(sw, sh);
    if (st != BR_OK) return st;
    uint32_t w = 0, h = 0;
    if (scale > 0.0 && scale != 1.0) {
        w = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(sw) * scale)));
        h = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(sh) * scale)));
    } else if (scale == 1.0) {
        w = sw;
        h = sh;
    } else {
        br_fit_dimensions(sw, sh, fit, &w, &h);
    }
    br_frame_info fi{};
    const br_resize_options o = opt ? *opt : br_resize_options_for(BR_RESIZE_UI_TEXT);
    const uint64_t t0 = monotonic_us();
    if (w != sw || h != sh) {
        st = c->session->grab_scaled(timeout_ms, w, h, o, *out, fi);
        if (st == BR_OK) {
            t.capture_us = monotonic_us() - t0;
            t.gpu_scaled = true;
            if (info) *info = fi;
            if (timings) *timings = t;
            return BR_OK;
        }
        if (st != BR_E_UNSUPPORTED) return st;
    }
    c->dirty.clear();
    st = c->session->grab(timeout_ms, c->scratch, fi, c->dirty);
    if (st != BR_OK) return st;
    const uint64_t t1 = monotonic_us();
    t.capture_us = t1 - t0;
    fi.dirty_count = static_cast<uint32_t>(c->dirty.size());
    // Во время изменения размера WGC size() может быть устаревшим; используются размеры полученного кадра.
    sw = c->scratch.width;
    sh = c->scratch.height;
    if (scale > 0.0) {
        w = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(sw) * scale)));
        h = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(sh) * scale)));
    } else {
        br_fit_dimensions(sw, sh, fit, &w, &h);
    }
    if (w == sw && h == sh) {
        // Меняет владение буферами без копирования пикселей; оба выделенных буфера можно использовать повторно.
        std::swap(*out, c->scratch);
    } else {
        const br_image_view src = as_view(c->scratch);
        if (out->data && (out->width != w || out->height != h || out->format != BR_PIXEL_BGRA8)) free_image(*out);
        if (!out->data) *out = alloc_image(w, h, BR_PIXEL_BGRA8, false);
        st = c->context->resizer.resize(src, *out, o, c->context->pool);
        if (st != BR_OK) return st;
        const br_transform down{static_cast<double>(sw) / w, static_cast<double>(sh) / h, 0.0, 0.0};
        fi.image_to_screen = br_transform_compose(down, fi.image_to_screen);
        t.resize_us = monotonic_us() - t1;
    }
    if (info) *info = fi;
    if (timings) *timings = t;
    return BR_OK;
}

}

extern "C" {

br_status br_monitors(br_monitor_info* out, size_t cap, size_t* count) {
    if (count) *count = 0;
    return guarded([&] {
        std::vector<br_monitor_info> v;
        const br_status st = capture::list_monitors(v);
        if (st != BR_OK) return st;
        return copy_list(v, out, cap, count);
    });
}

br_status br_windows(br_window_info* out, size_t cap, size_t* count) {
    if (count) *count = 0;
    return guarded([&] {
        std::vector<br_window_info> v;
        const br_status st = capture::list_windows(v);
        if (st != BR_OK) return st;
        return copy_list(v, out, cap, count);
    });
}

br_status br_find_window(const char* needle, br_window_info* out) {
    if (!needle || !out) return fail(BR_E_INVALID_ARGUMENT, "null argument");
    return guarded([&] {
        std::vector<br_window_info> v;
        const br_status st = capture::list_windows(v);
        if (st != BR_OK) return st;
    // Без учёта регистра: ASCII и распространённые символы кириллицы в UTF-8.
        auto fold = [](const char* s) {
            std::string r;
            const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
            while (*p) {
                if (*p < 0x80) { r.push_back(static_cast<char>(std::tolower(*p))); ++p; continue; }
                if ((p[0] == 0xd0) && p[1] >= 0x90 && p[1] <= 0xaf) {       // А-Я -> а-я
                    const unsigned cp = 0x410u + (p[1] - 0x90u) + 0x20u;
                    r.push_back(static_cast<char>(0xc0 | (cp >> 6)));
                    r.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
                    p += 2;
                    continue;
                }
                if (p[0] == 0xd0 && p[1] == 0x81) { r += "\xd1\x91"; p += 2; continue; } // Ё -> ё
                r.push_back(static_cast<char>(*p++));
            }
            return r;
        };
        const std::string n = fold(needle);
        for (const auto& w : v) {
            if (!w.visible || w.minimized) continue;
            if (fold(w.title).find(n) != std::string::npos) {
                *out = w;
                return BR_OK;
            }
        }
        return fail(BR_E_NOT_FOUND, "no visible window title contains the given text");
    });
}

br_capture_options br_capture_options_default(void) {
    br_capture_options o{};
    o.target = BR_TARGET_MONITOR;
    o.monitor = 0;
    o.backend = BR_BACKEND_AUTO;
    o.include_cursor = 0;
    o.client_area = 0;
    o.border = 0;
    return o;
}

br_status br_capture_open(br_context* ctx, const br_capture_options* opt, br_capture** out) {
    if (!out) return fail(BR_E_INVALID_ARGUMENT, "null output");
    *out = nullptr;
    const br_capture_options o = opt ? *opt : br_capture_options_default();
    return guarded([&] {
        std::unique_ptr<capture::Session> s;
        const br_status st = capture::open(o, s);
        if (st != BR_OK) return st;
        return make_capture(ctx, std::move(s), out);
    });
}

br_status br_capture_create_monitor(br_context* ctx, uint32_t adapter, uint32_t output, br_capture** out) {
    if (!out) return fail(BR_E_INVALID_ARGUMENT, "null output");
    *out = nullptr;
    return guarded([&] {
        std::unique_ptr<capture::Session> s;
        const br_status st = capture::open_dxgi_output(adapter, output, s);
        if (st != BR_OK) return st;
        return make_capture(ctx, std::move(s), out);
    });
}

br_status br_capture_create_window(br_context* ctx, void* hwnd, br_capture** out) {
    if (!out || !hwnd) return fail(BR_E_INVALID_ARGUMENT, "null argument");
    *out = nullptr;
    return guarded([&] {
        std::unique_ptr<capture::Session> s;
        const br_status st = capture::open_window(hwnd, s);
        if (st != BR_OK) return st;
        return make_capture(ctx, std::move(s), out);
    });
}

br_status br_capture_size(br_capture* c, uint32_t* w, uint32_t* h) {
    if (!c || !c->session) return fail(BR_E_INVALID_ARGUMENT, "null capture");
    return guarded([&] {
        uint32_t cw = 0, ch = 0;
        const br_status st = c->session->size(cw, ch);
        if (w) *w = cw;
        if (h) *h = ch;
        return st;
    });
}

br_status br_capture_grab(br_capture* c, uint32_t timeout_ms, br_image* out, br_frame_info* info, br_rect_i32* dirty,
                          size_t dirty_cap) {
    if (!c || !c->session || !out) return fail(BR_E_INVALID_ARGUMENT, "null argument");
    return guarded([&] {
        br_frame_info fi{};
        c->dirty.clear();
        const br_status st = c->session->grab(timeout_ms, *out, fi, c->dirty);
        if (st != BR_OK) return st;
        fi.dirty_count = static_cast<uint32_t>(c->dirty.size());
        copy_dirty(c->dirty, dirty, dirty_cap);
        if (info) *info = fi;
        return BR_OK;
    });
}

br_status br_capture_grab_fit(br_capture* c, uint32_t timeout_ms, const br_fit_options* fit, const br_resize_options* opt,
                              br_image* out, br_frame_info* info) {
    if (!c || !c->session || !out) return fail(BR_E_INVALID_ARGUMENT, "null argument");
    return guarded([&] { return grab_fit_timed(c, timeout_ms, fit, opt, out, info, nullptr); });
}

br_status br_screenshot(const br_capture_options* opt, br_image* out, br_frame_info* info) {
    if (!out) return fail(BR_E_INVALID_ARGUMENT, "null output");
    br_capture* c = nullptr;
    // Для одного кадра выбирает способ захвата с наименьшими затратами запуска (см. capture::open).
    br_status st = open_capture(nullptr, opt ? *opt : br_capture_options_default(), true, &c);
    if (st != BR_OK) return st;
    st = br_capture_grab(c, 1000, out, info, nullptr, 0);
    br_capture_destroy(c);
    return st;
}

br_status br_capture_next(br_capture* c, uint32_t timeout_ms, br_mut_image_view* dst, br_rect_i32* dirty, size_t cap,
                          size_t* count) {
    if (!c || !c->session || !dst) return fail(BR_E_INVALID_ARGUMENT, "null argument");
    return guarded([&] {
        c->dirty.clear();
        const br_status st = c->session->next(timeout_ms, *dst, c->dirty);
        if (st != BR_OK) return st;
        if (count) *count = c->dirty.size();
        copy_dirty(c->dirty, dirty, cap);
        return BR_OK;
    });
}

br_status br_capture_next_d3d11(br_capture* c, uint32_t timeout_ms, void** tex, uint32_t* w, uint32_t* h, br_rect_i32* dirty,
                                size_t cap, size_t* count) {
    if (!c || !c->session || !tex) return fail(BR_E_INVALID_ARGUMENT, "null argument");
    return guarded([&] {
        uint32_t tw = 0, th = 0;
        c->dirty.clear();
        const br_status st = c->session->next_d3d11(timeout_ms, tex, tw, th, c->dirty);
        if (st != BR_OK) return st;
        if (w) *w = tw;
        if (h) *h = th;
        if (count) *count = c->dirty.size();
        copy_dirty(c->dirty, dirty, cap);
        return BR_OK;
    });
}

void br_d3d11_texture_release(void* texture) { capture::release_d3d11_texture(texture); }

void br_capture_destroy(br_capture* c) { delete c; }

}
