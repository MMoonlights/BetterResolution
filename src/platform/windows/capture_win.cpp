#if defined(_WIN32)

#include "platform/windows/enum.hpp"
#include "platform/windows/session.hpp"

#include "core/common.hpp"
#include "core/frame.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace br::win {

CachedSession::~CachedSession() { free_image(cache_); }

void CachedSession::ensure_cache(uint32_t w, uint32_t h) {
    if (cache_.data && cache_.width == w && cache_.height == h) return;
    free_image(cache_);
    cache_ = alloc_image(w, h, BR_PIXEL_BGRA8, true);
    have_ = false;
}

br_status CachedSession::size(uint32_t& w, uint32_t& h) {
    if (have_) {
        w = cache_.width;
        h = cache_.height;
        return BR_OK;
    }
    expected_size(w, h);
    if (w && h) return BR_OK;
    Update u;
    const br_status st = update(1000, false, u);
    if (st != BR_OK) return st;
    w = cache_.width;
    h = cache_.height;
    return have_ ? BR_OK : fail(BR_E_TIMEOUT, "no frame available yet");
}

void CachedSession::fill_info(br_frame_info& info, bool changed, bool dirty_known) const {
    info = {};
    info.frame_id = frame_id_;
    info.timestamp_us = frame_time_;
    info.screen_rect = screen_rect_;
    info.image_to_screen = {scale_x_, scale_y_, static_cast<double>(screen_rect_.x), static_cast<double>(screen_rect_.y)};
    info.backend = backend_;
    info.dirty_known = dirty_known ? 1 : 0;
    info.changed = changed ? 1 : 0;
}

br_status CachedSession::grab(uint32_t timeout_ms, br_mut_image_view& img, br_frame_info& info, std::vector<br_rect_i32>& dirty) {
    DpiScope dpi;
    Update u;
    const br_status st = update(timeout_ms, false, u);
    if (st != BR_OK) return st;
    if (!have_) return fail(BR_E_TIMEOUT, "no frame was delivered within the timeout");
    if (img.data && (img.width != cache_.width || img.height != cache_.height || img.format != BR_PIXEL_BGRA8)) free_image(img);
    if (!img.data) img = alloc_image(cache_.width, cache_.height, BR_PIXEL_BGRA8, false);
    for (uint32_t y = 0; y < cache_.height; ++y)
        std::memcpy(row_ptr(img, y), row_ptr(cache_, y), static_cast<size_t>(cache_.width) * 4);
    img.color_space = BR_COLOR_SRGB;
    img.premultiplied_alpha = 0;
    if (include_cursor_) composite_cursor(img);
    fill_info(info, u.updated, u.dirty_known);
    dirty = std::move(u.dirty);
    return BR_OK;
}

br_status CachedSession::next(uint32_t timeout_ms, const br_mut_image_view& dst, std::vector<br_rect_i32>& dirty) {
    DpiScope dpi;
    Update u;
    const bool first = !have_;
    const br_status st = update(timeout_ms, true, u);
    if (st != BR_OK) return st;
    if (!u.updated && !first) return fail(BR_E_TIMEOUT, "no new frame within the timeout");
    if (!have_) return fail(BR_E_TIMEOUT, "no frame was delivered within the timeout");
    if (!validate_image(dst) || dst.format != BR_PIXEL_BGRA8 || dst.width != cache_.width || dst.height != cache_.height)
        return fail(BR_E_INVALID_ARGUMENT, "destination must be BGRA8 with the capture size (see br_capture_size)");
    for (uint32_t y = 0; y < cache_.height; ++y)
        std::memcpy(row_ptr(dst, y), row_ptr(cache_, y), static_cast<size_t>(cache_.width) * 4);
    if (include_cursor_) composite_cursor(dst);
    dirty = std::move(u.dirty);
    return BR_OK;
}

void CachedSession::composite_cursor(const br_mut_image_view& img) const {
    const CursorShape& c = cursor_;
    if (!c.visible || c.pixels.empty() || !c.width || !c.height) return;
    const bool mono = c.type == 1;
    const uint32_t h = mono ? c.height / 2 : c.height;
    for (uint32_t y = 0; y < h; ++y) {
        const int32_t iy = c.y + static_cast<int32_t>(y);
        if (iy < 0 || iy >= static_cast<int32_t>(img.height)) continue;
        uint8_t* row = row_ptr(img, static_cast<uint32_t>(iy));
        for (uint32_t x = 0; x < c.width; ++x) {
            const int32_t ix = c.x + static_cast<int32_t>(x);
            if (ix < 0 || ix >= static_cast<int32_t>(img.width)) continue;
            uint8_t* d = row + 4u * static_cast<uint32_t>(ix);
            if (mono) {
                const uint8_t and_bit = (c.pixels[static_cast<size_t>(y) * c.pitch + x / 8] >> (7 - x % 8)) & 1;
                const uint8_t xor_bit = (c.pixels[static_cast<size_t>(y + h) * c.pitch + x / 8] >> (7 - x % 8)) & 1;
                for (int k = 0; k < 3; ++k) {
                    uint8_t v = and_bit ? d[k] : 0;
                    if (xor_bit) v ^= 0xff;
                    d[k] = v;
                }
                continue;
            }
            const uint8_t* s = &c.pixels[static_cast<size_t>(y) * c.pitch + 4u * x];
            if (c.type == 2) { // Цвет с наложением по альфа-каналу.
                const uint32_t a = s[3];
                for (int k = 0; k < 3; ++k) d[k] = static_cast<uint8_t>((s[k] * a + d[k] * (255u - a) + 127u) / 255u);
            } else { // Цвет с маской: альфа 0 - заменить, 0xff - выполнить XOR.
                if (s[3] == 0) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
                else { d[0] ^= s[0]; d[1] ^= s[1]; d[2] ^= s[2]; }
            }
        }
    }
}

void copy_rect_from_mapped(const uint8_t* src, size_t pitch, int32_t sx, int32_t sy, const br_mut_image_view& dst,
                           const br_rect_i32& r) {
    for (int32_t y = 0; y < r.height; ++y) {
        const uint8_t* s = src + static_cast<size_t>(sy + y) * pitch + static_cast<size_t>(sx) * 4;
        std::memcpy(row_ptr(dst, static_cast<uint32_t>(r.y + y)) + static_cast<size_t>(r.x) * 4, s, static_cast<size_t>(r.width) * 4);
    }
}

namespace {
bool intersect(const br_rect_i32& a, const br_rect_i32& b, br_rect_i32& out) {
    const int32_t x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const int32_t x1 = std::min(a.x + a.width, b.x + b.width), y1 = std::min(a.y + a.height, b.y + b.height);
    out = {x0, y0, x1 - x0, y1 - y0};
    return out.width > 0 && out.height > 0;
}
bool contains(const br_rect_i32& outer, const br_rect_i32& inner) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}
}

}

namespace br::win {
br_status prewarm_graphics() {
    const Api& a = api();
    Com<ID3D11Device> dev;
    Com<ID3D11DeviceContext> ctx;
    if (!a.D3D11CreateDevice) return fail(BR_E_UNSUPPORTED, "Direct3D 11 is not available");
    return create_device(nullptr, dev, ctx);
}
}

namespace br::capture {

using namespace br::win;

br_status list_monitors(std::vector<br_monitor_info>& out) {
    std::vector<MonitorEntry> m;
    const br_status st = enum_monitors(m);
    if (st != BR_OK) return st;
    out.clear();
    for (const auto& e : m) out.push_back(e.info);
    return BR_OK;
}

br_status list_windows(std::vector<br_window_info>& out) { return enum_windows(out); }

br_status open_dxgi_output(uint32_t adapter, uint32_t output, std::unique_ptr<Session>& out) {
    DpiScope dpi;
    return open_dxgi(adapter, output, nullptr, false, out);
}

br_status open_window(void* hwnd, std::unique_ptr<Session>& out) {
    DpiScope dpi;
    HWND h = static_cast<HWND>(hwnd);
    if (!IsWindow(h)) return fail(BR_E_NOT_FOUND, "invalid window handle");
    if (open_wgc_window(h, false, nullptr, false, false, out) == BR_OK) return BR_OK;
    return open_gdi_window(h, false, nullptr, false, GdiMode::Auto, out);
}

br_status open_native(const br_capture_options& o, bool cold_start_first, std::unique_ptr<Session>& out) {
    DpiScope dpi;
    const br_rect_i32* crop = o.region.width > 0 && o.region.height > 0 ? &o.region : nullptr;
    const bool cursor = o.include_cursor != 0;
    const bool border = o.border != 0;
    std::string errors;
    auto note = [&](const char* backend, br_status st) {
        if (st == BR_OK) return;
        if (!errors.empty()) errors += "; ";
        errors += backend;
        errors += ": ";
        errors += br::last_error();
    };
    auto finish = [&]() {
        set_errorf("no capture backend could be started (%s)", errors.c_str());
        return BR_E_UNSUPPORTED;
    };

    switch (o.target) {
    case BR_TARGET_MONITOR: {
        std::vector<MonitorEntry> mons;
        br_status st = enum_monitors(mons);
        if (st != BR_OK) return st;
        if (o.monitor >= mons.size()) return fail(BR_E_NOT_FOUND, "monitor index out of range");
        const MonitorEntry& m = mons[o.monitor];
        br_rect_i32 area = m.info.bounds;
        if (crop && !intersect(*crop, m.info.bounds, area)) return fail(BR_E_INVALID_ARGUMENT, "region does not intersect the monitor");
        const br_rect_i32* c = crop ? &area : nullptr;
        if (o.backend == BR_BACKEND_GDI_PRINT) return fail(BR_E_UNSUPPORTED, "PrintWindow captures a window; use BR_BACKEND_GDI_SCREEN for a monitor");
        if (o.backend == BR_BACKEND_AUTO && cold_start_first) {
            // Для одного кадра предпочитает GDI, чтобы не открывать графический сеанс.
            st = open_gdi_region(area, cursor, out);
            if (st == BR_OK) return st;
            note("gdi", st);
        }
        if ((o.backend == BR_BACKEND_AUTO || o.backend == BR_BACKEND_DXGI) && m.info.adapter_index != UINT32_MAX) {
            st = open_dxgi(m.info.adapter_index, m.info.output_index, c, cursor, out);
            if (st == BR_OK || o.backend == BR_BACKEND_DXGI) return st;
            note("dxgi", st);
        }
        if (o.backend == BR_BACKEND_AUTO || o.backend == BR_BACKEND_WGC) {
            st = open_wgc_monitor(m.handle, m.info.bounds, c, cursor, border, out);
            if (st == BR_OK || o.backend == BR_BACKEND_WGC) return st;
            note("wgc", st);
        }
        st = open_gdi_region(area, cursor, out);
        if (st == BR_OK) return st;
        note("gdi", st);
        return finish();
    }
    case BR_TARGET_WINDOW: {
        HWND h = reinterpret_cast<HWND>(static_cast<uintptr_t>(o.window));
        if (!o.window || !IsWindow(h)) return fail(BR_E_NOT_FOUND, "invalid window handle");
        if (IsIconic(h)) return fail(BR_E_UNSUPPORTED, "the window is minimized; restore it before capturing");
        const bool client = o.client_area != 0;
        br_status st;
        if (o.backend == BR_BACKEND_AUTO && cold_start_first) {
            // Копия экрана подходит, только пока цель ничем не перекрыта; проверяет это при каждом захвате.
            br_rect_i32 area = to_rect(client ? client_rect_screen(h) : window_frame(h));
            if (crop) intersect(*crop, area, area);
            const RECT ar{area.x, area.y, area.x + area.width, area.y + area.height};
            if (window_is_unobstructed(h, ar)) {
                st = open_gdi_window(h, client, crop, cursor, GdiMode::Auto, out);
                if (st == BR_OK) return st;
                note("gdi", st);
            }
        }
        if (o.backend == BR_BACKEND_AUTO || o.backend == BR_BACKEND_WGC) {
            st = open_wgc_window(h, client, crop, cursor, border, out);
            if (st == BR_OK || o.backend == BR_BACKEND_WGC) return st;
            note("wgc", st);
        }
        if (o.backend == BR_BACKEND_DXGI) {
            // Захват окна через DXGI обрезает изображение монитора в координатах рабочего стола.
            std::vector<MonitorEntry> mons;
            st = enum_monitors(mons);
            if (st != BR_OK) return st;
            br_rect_i32 wr = to_rect(client ? client_rect_screen(h) : window_frame(h));
            if (crop && !intersect(*crop, wr, wr)) return fail(BR_E_INVALID_ARGUMENT, "region does not intersect the window");
            for (const auto& m : mons)
                if (contains(m.info.bounds, wr) && m.info.adapter_index != UINT32_MAX)
                    return open_dxgi(m.info.adapter_index, m.info.output_index, &wr, cursor, out);
            return fail(BR_E_UNSUPPORTED, "DXGI window capture requires the window to lie on one monitor");
        }
        const GdiMode mode = o.backend == BR_BACKEND_GDI_SCREEN ? GdiMode::Screen
                             : o.backend == BR_BACKEND_GDI_PRINT ? GdiMode::Print : GdiMode::Auto;
        st = open_gdi_window(h, client, crop, cursor, mode, out);
        if (st == BR_OK) return st;
        note("gdi", st);
        return finish();
    }
    case BR_TARGET_REGION:
    case BR_TARGET_DESKTOP: {
        br_rect_i32 area = virtual_desktop();
        if (o.target == BR_TARGET_REGION) {
            if (!crop) return fail(BR_E_INVALID_ARGUMENT, "BR_TARGET_REGION requires a region");
            if (!intersect(*crop, area, area)) return fail(BR_E_INVALID_ARGUMENT, "region is outside the desktop");
        }
        br_status st;
        if (o.backend == BR_BACKEND_WGC)
            return fail(BR_E_UNSUPPORTED, "Windows.Graphics.Capture captures a window or a whole monitor, not a region or the desktop");
        if (o.backend == BR_BACKEND_GDI_PRINT) return fail(BR_E_UNSUPPORTED, "PrintWindow captures a window; use BR_BACKEND_GDI_SCREEN for a region");
        if (o.backend == BR_BACKEND_AUTO && cold_start_first) {
            st = open_gdi_region(area, cursor, out);
            if (st == BR_OK) return st;
            note("gdi", st);
        }
        if (o.backend == BR_BACKEND_AUTO || o.backend == BR_BACKEND_DXGI) {
            std::vector<MonitorEntry> mons;
            if (enum_monitors(mons) == BR_OK) {
                for (const auto& m : mons) {
                    if (!contains(m.info.bounds, area) || m.info.adapter_index == UINT32_MAX) continue;
                    st = open_dxgi(m.info.adapter_index, m.info.output_index, &area, cursor, out);
                    if (st == BR_OK) return st;
                    note("dxgi", st);
                    break;
                }
            }
            if (o.backend == BR_BACKEND_DXGI) return errors.empty() ? fail(BR_E_UNSUPPORTED, "region spans several monitors") : finish();
        }
        st = open_gdi_region(area, cursor, out);
        if (st == BR_OK) return st;
        note("gdi", st);
        return finish();
    }
    default:
        return fail(BR_E_INVALID_ARGUMENT, "unknown capture target");
    }
}

br_status prewarm() {
    DpiScope dpi;
    return prewarm_graphics();
}

void sleep_ms(uint32_t ms) { precise_sleep_ms(ms); }

void release_d3d11_texture(void* texture) {
    if (texture) static_cast<IUnknown*>(texture)->Release();
}

const char* features() noexcept {
    static const std::string f = [] {
        std::string s;
        const Api& a = api();
        if (a.CreateDXGIFactory1 && a.D3D11CreateDevice) s += "dxgi,";
        if (a.RoGetActivationFactory && a.CreateDirect3D11DeviceFromDXGIDevice) s += "wgc,";
        s += "gdi";
        if (a.D3DCompile) s += ",d3d11-scaler";
        return s;
    }();
    return f.c_str();
}

}

#endif
