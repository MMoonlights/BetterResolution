#if defined(_WIN32)

#include "platform/windows/session.hpp"

#include "analysis/diff.hpp"
#include "core/common.hpp"
#include "core/frame.hpp"

#include <algorithm>
#include <cstring>

namespace br::win {
namespace {

class GdiSession final : public CachedSession {
public:
    explicit GdiSession(GdiMode mode) : mode_(mode) {
        backend_ = mode == GdiMode::Print || mode == GdiMode::PrintFull ? BR_BACKEND_GDI_PRINT : mode == GdiMode::Screen ? BR_BACKEND_GDI_SCREEN : BR_BACKEND_GDI;
    }
    ~GdiSession() override {
        free_dib();
        free_image(next_);
    }

    br_status init_region(const br_rect_i32& r, bool cursor) {
        region_ = r;
        include_cursor_ = false; // Указатель уже наложен функцией capture_into.
        draw_cursor_ = cursor;
        return BR_OK;
    }
    br_status init_window(HWND h, bool client, const br_rect_i32* crop, bool cursor) {
        hwnd_ = h;
        client_ = client;
        if (crop) { crop_ = *crop; has_crop_ = true; }
        include_cursor_ = false;
        draw_cursor_ = cursor;
        return BR_OK;
    }

protected:
    void expected_size(uint32_t& w, uint32_t& h) override {
        br_rect_i32 r;
        if (target_rect(r)) { w = static_cast<uint32_t>(r.width); h = static_cast<uint32_t>(r.height); }
        else { w = h = 0; }
    }

    br_status update(uint32_t, bool wait_for_new, Update& u) override {
        br_rect_i32 r;
        if (hwnd_ && !IsWindow(hwnd_)) return fail(BR_E_DEVICE_LOST, "the captured window was closed");
        if (!target_rect(r)) return fail(BR_E_UNSUPPORTED, "capture area is empty (window minimized or off-screen?)");
        const uint32_t w = static_cast<uint32_t>(r.width), h = static_cast<uint32_t>(r.height);
        const br_status st = capture_into(r);
        if (st != BR_OK) return st;
        if (!next_.data || next_.width != w || next_.height != h) {
            free_image(next_);
            next_ = alloc_image(w, h, BR_PIXEL_BGRA8, false);
        }
        // Копирует строки DIB в next_ и задаёт альфу 255 (GDI оставляет её неопределённой или нулевой).
        const uint8_t* bits = static_cast<const uint8_t*>(bits_);
        for (uint32_t y = 0; y < h; ++y) {
            const uint32_t* s = reinterpret_cast<const uint32_t*>(bits + static_cast<size_t>(y) * dib_w_ * 4);
            uint32_t* d = reinterpret_cast<uint32_t*>(row_ptr(next_, y));
            for (uint32_t x = 0; x < w; ++x) d[x] = s[x] | 0xff000000u;
        }
        const bool same_size = have_ && cache_.width == w && cache_.height == h;
        u.dirty_known = true;
        if (same_size) {
            br_diff_options o{16, 0, 8};
            br_diff_result res{};
            analysis::diff_images(as_view(cache_), as_view(next_), o, u.dirty, res);
            u.updated = res.changed_pixels > 0;
        } else {
            u.updated = true;
            u.dirty.assign(1, br_rect_i32{0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)});
        }
        if (wait_for_new && !u.updated && have_) return BR_OK;
        std::swap(cache_, next_);
        have_ = true;
        if (u.updated) {
            ++frame_id_;
            frame_time_ = monotonic_us();
        }
        screen_rect_ = r;
        return BR_OK;
    }

private:
    bool target_rect(br_rect_i32& r) const {
        if (!hwnd_) {
            r = region_;
            return r.width > 0 && r.height > 0;
        }
        if (IsIconic(hwnd_)) return false;
        if (mode_ == GdiMode::PrintFull) {
            RECT bounds{};
            if (!GetWindowRect(hwnd_, &bounds)) return false;
            r = to_rect(bounds);
        } else r = to_rect(client_ ? client_rect_screen(hwnd_) : window_frame(hwnd_));
        if (has_crop_) {
            const int32_t x0 = std::max(r.x, crop_.x), y0 = std::max(r.y, crop_.y);
            const int32_t x1 = std::min(r.x + r.width, crop_.x + crop_.width), y1 = std::min(r.y + r.height, crop_.y + crop_.height);
            r = {x0, y0, x1 - x0, y1 - y0};
        }
        return r.width > 0 && r.height > 0;
    }

    void free_dib() {
        if (mem_dc_) DeleteDC(mem_dc_);
        if (dib_) DeleteObject(dib_);
        mem_dc_ = nullptr;
        dib_ = nullptr;
        bits_ = nullptr;
        dib_w_ = dib_h_ = 0;
    }

    br_status ensure_dib(int32_t w, int32_t h) {
        if (dib_ && dib_w_ >= w && dib_h_ >= h && dib_w_ == w) return BR_OK;
        free_dib();
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // Строки сверху вниз.
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        mem_dc_ = CreateCompatibleDC(screen);
        dib_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!mem_dc_ || !dib_ || !bits_) {
            free_dib();
            return fail(BR_E_OUT_OF_MEMORY, "CreateDIBSection failed");
        }
        SelectObject(mem_dc_, dib_);
        dib_w_ = w;
        dib_h_ = h;
        return BR_OK;
    }

    br_status capture_into(const br_rect_i32& r) {
        bool done = false;
        br_capture_backend used = BR_BACKEND_GDI_SCREEN;
        const RECT area{r.x, r.y, r.x + r.width, r.y + r.height};
        // В режиме Auto использует пиксели экрана, только если цель ничем не перекрыта.
        const bool use_print = hwnd_ && mode_ != GdiMode::Screen && (mode_ == GdiMode::Print || mode_ == GdiMode::PrintFull || !window_is_unobstructed(hwnd_, area));
        if (use_print) {
            RECT wr{};
            GetWindowRect(hwnd_, &wr);
            const int32_t ww = wr.right - wr.left, wh = wr.bottom - wr.top;
            if (ww > 0 && wh > 0) {
                br_status st = ensure_dib(ww, wh);
                if (st != BR_OK) return st;
                if (PrintWindow(hwnd_, mem_dc_, 2 /* PW_RENDERFULLCONTENT */)) {
                    // Сдвигает нужную подпрямоугольную область к началу DIB.
                    const int32_t ox = r.x - wr.left, oy = r.y - wr.top;
                    if (ox || oy) {
                        uint8_t* b = static_cast<uint8_t*>(bits_);
                        for (int32_t y = 0; y < r.height; ++y)
                            std::memmove(b + static_cast<size_t>(y) * dib_w_ * 4,
                                         b + (static_cast<size_t>(y + oy) * dib_w_ + static_cast<size_t>(ox)) * 4,
                                         static_cast<size_t>(r.width) * 4);
                    }
                    done = !looks_blank(r.width, r.height);
                    if (done) used = BR_BACKEND_GDI_PRINT;
                    // При сбое PrintWindow не допускает выдачу пикселей перекрывающего окна.
                    else if (mode_ == GdiMode::Auto && !window_is_unobstructed(hwnd_, area))
                        return fail(BR_E_UNSUPPORTED, "PrintWindow returned a blank image and the window is covered by another one; "
                                                      "bring it to the front or use the WGC backend");
                }
            }
        }
        if (!done) {
            if (mode_ == GdiMode::Print || mode_ == GdiMode::PrintFull)
                return fail(BR_E_UNSUPPORTED, "PrintWindow failed or returned a blank image; explicit PrintWindow never reads screen pixels");
            if (use_print && !window_is_unobstructed(hwnd_, area))
                return fail(BR_E_UNSUPPORTED, "PrintWindow failed and the target is covered; screen fallback is unavailable");
            br_status st = ensure_dib(r.width, r.height);
            if (st != BR_OK) return st;
            HDC screen = GetDC(nullptr);
            const BOOL ok = BitBlt(mem_dc_, 0, 0, r.width, r.height, screen, r.x, r.y, SRCCOPY | CAPTUREBLT);
            ReleaseDC(nullptr, screen);
            if (!ok) return fail(BR_E_UNSUPPORTED, "BitBlt from the screen failed (secure desktop or no interactive session?)");
        }
        if (mode_ == GdiMode::Auto) backend_ = used; // Запоминает способ, которым получен этот кадр.
        if (draw_cursor_) {
            CURSORINFO ci{};
            ci.cbSize = sizeof(ci);
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor) {
                ICONINFO ii{};
                int hx = 0, hy = 0;
                if (GetIconInfo(ci.hCursor, &ii)) {
                    hx = static_cast<int>(ii.xHotspot);
                    hy = static_cast<int>(ii.yHotspot);
                    if (ii.hbmMask) DeleteObject(ii.hbmMask);
                    if (ii.hbmColor) DeleteObject(ii.hbmColor);
                }
                DrawIconEx(mem_dc_, ci.ptScreenPos.x - hx - r.x, ci.ptScreenPos.y - hy - r.y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
            }
        }
        GdiFlush();
        return BR_OK;
    }

    // Для некоторых окон с GPU-отрисовкой PrintWindow возвращает чёрный кадр; проверяет, не состоит ли он целиком из чёрных пикселей.
    bool looks_blank(int32_t w, int32_t h) const {
        const uint8_t* b = static_cast<const uint8_t*>(bits_);
        const int32_t step = std::max(1, (w * h) / 4096);
        for (int64_t i = 0; i < static_cast<int64_t>(w) * h; i += step) {
            const int64_t y = i / w, x = i % w;
            const uint8_t* p = b + (static_cast<size_t>(y) * dib_w_ + static_cast<size_t>(x)) * 4;
            if (p[0] | p[1] | p[2]) return false;
        }
        return true;
    }

    GdiMode mode_{GdiMode::Auto};
    HWND hwnd_{};
    bool client_{false};
    bool has_crop_{false};
    br_rect_i32 crop_{};
    br_rect_i32 region_{};
    bool draw_cursor_{false};
    HDC mem_dc_{};
    HBITMAP dib_{};
    void* bits_{};
    int32_t dib_w_{0}, dib_h_{0};
    br_mut_image_view next_{};
};

}

br_status gdi_copy_rect(const br_rect_i32& r, const br_mut_image_view& dst) {
    if (r.width <= 0 || r.height <= 0 || dst.width != static_cast<uint32_t>(r.width) || dst.height != static_cast<uint32_t>(r.height))
        return fail(BR_E_INVALID_ARGUMENT, "gdi_copy_rect: size mismatch");
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = r.width;
    bi.bmiHeader.biHeight = -r.height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    br_status st = BR_OK;
    if (!mem || !dib || !bits) {
        st = fail(BR_E_OUT_OF_MEMORY, "CreateDIBSection failed");
    } else {
        HGDIOBJ old = SelectObject(mem, dib);
        if (!BitBlt(mem, 0, 0, r.width, r.height, screen, r.x, r.y, SRCCOPY | CAPTUREBLT)) {
            st = fail(BR_E_UNSUPPORTED, "BitBlt from the screen failed");
        } else {
            GdiFlush();
            for (int32_t y = 0; y < r.height; ++y) {
                const uint32_t* s = static_cast<const uint32_t*>(bits) + static_cast<size_t>(y) * static_cast<size_t>(r.width);
                uint32_t* d = reinterpret_cast<uint32_t*>(row_ptr(dst, static_cast<uint32_t>(y)));
                for (int32_t x = 0; x < r.width; ++x) d[x] = s[x] | 0xff000000u;
            }
        }
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return st;
}

br_status open_gdi_region(const br_rect_i32& r, bool cursor, std::unique_ptr<capture::Session>& out) {
    if (r.width <= 0 || r.height <= 0) return fail(BR_E_INVALID_ARGUMENT, "empty capture region");
    auto s = std::make_unique<GdiSession>(GdiMode::Screen);
    const br_status st = s->init_region(r, cursor);
    if (st != BR_OK) return st;
    out = std::move(s);
    return BR_OK;
}

br_status open_gdi_window(HWND h, bool client, const br_rect_i32* crop, bool cursor, GdiMode mode,
                          std::unique_ptr<capture::Session>& out) {
    auto s = std::make_unique<GdiSession>(mode);
    const br_status st = s->init_window(h, client, crop, cursor);
    if (st != BR_OK) return st;
    out = std::move(s);
    return BR_OK;
}

}

#endif
