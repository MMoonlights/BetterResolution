#if defined(_WIN32)

#include "platform/windows/session.hpp"

#include "analysis/diff.hpp"
#include "core/common.hpp"
#include "core/frame.hpp"

#include <algorithm>
#include <cstring>
#include <string>

// Для интерфейсов WinRT, объявленных вручную, необходимо сохранить порядок методов из vtable Windows SDK.
namespace br::win {
namespace wrt {

struct SizeInt32 { int32_t Width; int32_t Height; };
struct TimeSpan { int64_t Duration; };
struct EventToken { int64_t value; };

const IID IID_IGraphicsCaptureItem = {0x79c3f95b, 0x31f7, 0x4ec2, {0xa4, 0x64, 0x63, 0x2e, 0xf5, 0xd3, 0x07, 0x60}};
const IID IID_IGraphicsCaptureItemInterop = {0x3628e81b, 0x3cac, 0x4c60, {0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56}};
const IID IID_IDirect3D11CaptureFramePoolStatics2 = {0x589b103f, 0x6bbc, 0x5df5, {0xa9, 0x91, 0x02, 0xe2, 0x8b, 0x3b, 0x66, 0xd5}};
const IID IID_IGraphicsCaptureSession = {0x814e42a9, 0xf70f, 0x4ad7, {0x93, 0x9b, 0xfd, 0xdc, 0xc6, 0xeb, 0x88, 0x0d}};
const IID IID_IGraphicsCaptureSession2 = {0x2c39ae40, 0x7d2e, 0x5044, {0x80, 0x4e, 0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e}};
const IID IID_IGraphicsCaptureSession3 = {0xf2cdd966, 0x22ae, 0x5ea1, {0x95, 0x96, 0x3a, 0x28, 0x93, 0x44, 0xc3, 0xbe}};
const IID IID_IGraphicsCaptureSessionStatics = {0x2224a540, 0x5974, 0x49aa, {0xb2, 0x32, 0x08, 0x82, 0x53, 0x6f, 0x4c, 0xb5}};
const IID IID_IClosable = {0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};
const IID IID_IDirect3DDevice = {0xa37624ab, 0x8d5f, 0x4650, {0x9d, 0x3e, 0x9e, 0xae, 0x3d, 0x9b, 0xc6, 0x70}};
const IID IID_IDirect3DDxgiInterfaceAccess = {0xa9b3d012, 0x3df2, 0x4ee3, {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};

struct IGraphicsCaptureItem : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(SizeInt32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Closed(void* handler, EventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Closed(EventToken token) = 0;
};

struct IGraphicsCaptureItemInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID riid, void** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID riid, void** result) = 0;
};

struct IGraphicsCaptureSession : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE StartCapture() = 0;
};

struct IGraphicsCaptureSession2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsCursorCaptureEnabled(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsCursorCaptureEnabled(boolean value) = 0;
};

struct IGraphicsCaptureSession3 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsBorderRequired(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsBorderRequired(boolean value) = 0;
};

struct IGraphicsCaptureSessionStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE IsSupported(boolean* result) = 0;
};

struct IDirect3D11CaptureFrame : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Surface(IInspectable** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SystemRelativeTime(TimeSpan* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ContentSize(SizeInt32* value) = 0;
};

struct IDirect3D11CaptureFramePool : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Recreate(IInspectable* device, int32_t pixel_format, int32_t buffers, SizeInt32 size) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetNextFrame(IDirect3D11CaptureFrame** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_FrameArrived(void* handler, EventToken* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_FrameArrived(EventToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCaptureSession(IGraphicsCaptureItem* item, IGraphicsCaptureSession** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DispatcherQueue(IInspectable** value) = 0;
};

struct IDirect3D11CaptureFramePoolStatics2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateFreeThreaded(IInspectable* device, int32_t pixel_format, int32_t buffers,
                                                         SizeInt32 size, IDirect3D11CaptureFramePool** result) = 0;
};

struct IClosable : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

struct IDirect3DDxgiInterfaceAccess : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** p) = 0;
};

constexpr int32_t kPixelFormatB8G8R8A8 = 87;

HRESULT activation_factory(const wchar_t* cls, REFIID iid, void** out) {
    const Api& a = api();
    if (!a.RoGetActivationFactory || !a.WindowsCreateString || !a.WindowsDeleteString) return E_NOTIMPL;
    HSTRING s = nullptr;
    HRESULT hr = a.WindowsCreateString(cls, static_cast<UINT32>(wcslen(cls)), &s);
    if (FAILED(hr)) return hr;
    hr = a.RoGetActivationFactory(s, iid, out);
    a.WindowsDeleteString(s);
    return hr;
}

void ensure_winrt() {
    thread_local bool done = false;
    if (done) return;
    done = true;
    if (api().RoInitialize) api().RoInitialize(1 /* Многопоточная инициализация; S_FALSE и RPC_E_CHANGED_MODE допустимы. */);
}

template <class T>
void close_object(Com<T>& p) {
    if (!p) return;
    Com<IClosable> c;
    if (SUCCEEDED(p.as(IID_IClosable, c))) c->Close();
    p.reset();
}

}

namespace {

class WgcSession final : public CachedSession {
public:
    WgcSession() { backend_ = BR_BACKEND_WGC; }
    ~WgcSession() override {
        wrt::close_object(session_);
        wrt::close_object(pool_);
        item_.reset();
        free_image(next_);
    }

    br_status init(HWND hwnd, HMONITOR mon, const br_rect_i32& mon_rect, bool client, const br_rect_i32* crop, bool cursor, bool border) {
        wrt::ensure_winrt();
        hwnd_ = hwnd;
        mon_rect_ = mon_rect;
        client_ = client;
        if (crop) { crop_ = *crop; has_crop_ = true; }
        include_cursor_ = false; // По запросу WGC самостоятельно добавляет указатель в кадр.
        const Api& a = api();
        if (!a.CreateDirect3D11DeviceFromDXGIDevice || !a.RoGetActivationFactory)
            return fail(BR_E_UNSUPPORTED, "Windows.Graphics.Capture is not available (Windows 10 1903+ required)");
        {
            Com<wrt::IGraphicsCaptureSessionStatics> statics;
            HRESULT hr = wrt::activation_factory(L"Windows.Graphics.Capture.GraphicsCaptureSession", wrt::IID_IGraphicsCaptureSessionStatics,
                                                 statics.put_void());
            boolean supported = 0;
            if (FAILED(hr) || FAILED(statics->IsSupported(&supported)) || !supported)
                return fail(BR_E_UNSUPPORTED, "Windows.Graphics.Capture is not supported on this system");
        }
        br_status st = create_device(nullptr, dev_, ctx_);
        if (st != BR_OK) return st;
        Com<IDXGIDevice> dxgi;
        HRESULT hr = dev_.as(__uuidof(IDXGIDevice), dxgi);
        if (FAILED(hr)) return hr_fail(BR_E_UNSUPPORTED, "IDXGIDevice", hr);
        Com<IInspectable> insp;
        hr = a.CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put());
        if (FAILED(hr)) return hr_fail(BR_E_UNSUPPORTED, "CreateDirect3D11DeviceFromDXGIDevice", hr);
        hr = insp.as(wrt::IID_IDirect3DDevice, winrt_device_);
        if (FAILED(hr)) return hr_fail(BR_E_UNSUPPORTED, "IDirect3DDevice", hr);

        Com<wrt::IGraphicsCaptureItemInterop> interop;
        hr = wrt::activation_factory(L"Windows.Graphics.Capture.GraphicsCaptureItem", wrt::IID_IGraphicsCaptureItemInterop, interop.put_void());
        if (FAILED(hr)) return hr_fail(BR_E_UNSUPPORTED, "GraphicsCaptureItem factory", hr);
        hr = hwnd ? interop->CreateForWindow(hwnd, wrt::IID_IGraphicsCaptureItem, item_.put_void())
                  : interop->CreateForMonitor(mon, wrt::IID_IGraphicsCaptureItem, item_.put_void());
        if (FAILED(hr)) return hr_fail(BR_E_UNSUPPORTED, hwnd ? "GraphicsCaptureItem.CreateForWindow" : "GraphicsCaptureItem.CreateForMonitor", hr);
        hr = item_->get_Size(&pool_size_);
        if (FAILED(hr) || pool_size_.Width <= 0 || pool_size_.Height <= 0) return hr_fail(BR_E_UNSUPPORTED, "GraphicsCaptureItem.Size", hr);

        Com<wrt::IDirect3D11CaptureFramePoolStatics2> pool_statics;
        hr = wrt::activation_factory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", wrt::IID_IDirect3D11CaptureFramePoolStatics2,
                                     pool_statics.put_void());
        if (FAILED(hr)) return hr_fail(BR_E_UNSUPPORTED, "Direct3D11CaptureFramePool factory", hr);
        hr = pool_statics->CreateFreeThreaded(winrt_device_.get(), wrt::kPixelFormatB8G8R8A8, 2, pool_size_, pool_.put());
        if (FAILED(hr)) return hr_fail(BR_E_UNSUPPORTED, "CreateFreeThreaded", hr);
        hr = pool_->CreateCaptureSession(item_.get(), session_.put());
        if (FAILED(hr)) return hr_fail(BR_E_UNSUPPORTED, "CreateCaptureSession", hr);
        Com<wrt::IGraphicsCaptureSession2> s2;
        if (SUCCEEDED(session_.as(wrt::IID_IGraphicsCaptureSession2, s2))) s2->put_IsCursorCaptureEnabled(cursor ? 1 : 0);
        Com<wrt::IGraphicsCaptureSession3> s3;
        if (SUCCEEDED(session_.as(wrt::IID_IGraphicsCaptureSession3, s3))) s3->put_IsBorderRequired(border ? 1 : 0);
        hr = session_->StartCapture();
        if (FAILED(hr)) return hr_fail(BR_E_UNSUPPORTED, "StartCapture", hr);
        return BR_OK;
    }

protected:
    void expected_size(uint32_t& w, uint32_t& h) override {
        br_rect_i32 r;
        if (target_rect(r)) { w = static_cast<uint32_t>(r.width); h = static_cast<uint32_t>(r.height); }
        else { w = h = 0; }
    }

    br_status update(uint32_t timeout_ms, bool wait_for_new, Update& u) override {
        wrt::ensure_winrt();
        if (hwnd_ && !IsWindow(hwnd_)) return fail(BR_E_DEVICE_LOST, "the captured window was closed");
        const uint64_t deadline = monotonic_us() + static_cast<uint64_t>((wait_for_new || !have_) ? timeout_ms : 0) * 1000;
        Com<wrt::IDirect3D11CaptureFrame> frame;
        for (;;) {
            // Забирает все кадры из очереди и оставляет самый новый.
            for (;;) {
                Com<wrt::IDirect3D11CaptureFrame> f;
                if (FAILED(pool_->TryGetNextFrame(f.put())) || !f) break;
                if (frame) wrt::close_object(frame);
                frame = std::move(f);
            }
            if (frame || monotonic_us() >= deadline) break;
            precise_sleep_ms(1);
        }
        u.dirty_known = true;
        if (!frame) return BR_OK; // Кадр не изменился или ещё не получен первый кадр.
        const br_status st = consume(frame.get(), u);
        wrt::close_object(frame);
        return st;
    }

private:
    bool target_rect(br_rect_i32& r) const {
        if (hwnd_) {
            if (IsIconic(hwnd_)) return false;
            r = to_rect(client_ ? client_rect_screen(hwnd_) : window_frame(hwnd_));
        } else {
            r = mon_rect_;
        }
        if (has_crop_) {
            const int32_t x0 = std::max(r.x, crop_.x), y0 = std::max(r.y, crop_.y);
            const int32_t x1 = std::min(r.x + r.width, crop_.x + crop_.width), y1 = std::min(r.y + r.height, crop_.y + crop_.height);
            r = {x0, y0, x1 - x0, y1 - y0};
        }
        return r.width > 0 && r.height > 0;
    }

    br_status consume(wrt::IDirect3D11CaptureFrame* frame, Update& u) {
        wrt::SizeInt32 cs{};
        frame->get_ContentSize(&cs);
        Com<IInspectable> surface;
        HRESULT hr = frame->get_Surface(surface.put());
        if (FAILED(hr)) return hr_fail(BR_E_INTERNAL, "frame surface", hr);
        Com<wrt::IDirect3DDxgiInterfaceAccess> access;
        hr = surface.as(wrt::IID_IDirect3DDxgiInterfaceAccess, access);
        if (FAILED(hr)) return hr_fail(BR_E_INTERNAL, "IDirect3DDxgiInterfaceAccess", hr);
        Com<ID3D11Texture2D> tex;
        hr = access->GetInterface(__uuidof(ID3D11Texture2D), tex.put_void());
        if (FAILED(hr)) return hr_fail(BR_E_INTERNAL, "frame texture", hr);
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        if (cs.Width != pool_size_.Width || cs.Height != pool_size_.Height) {
            // Размер окна изменился; следующие кадры будут иметь новый размер.
            pool_size_ = cs;
            pool_->Recreate(winrt_device_.get(), wrt::kPixelFormatB8G8R8A8, 2, cs);
        }
        const int32_t content_w = std::min<int32_t>(cs.Width, static_cast<int32_t>(td.Width));
        const int32_t content_h = std::min<int32_t>(cs.Height, static_cast<int32_t>(td.Height));
        if (content_w <= 0 || content_h <= 0) return BR_OK;

        // Кадр содержит видимые границы объекта: рамку окна или монитор с началом в его исходной точке.
        const br_rect_i32 item = hwnd_ ? to_rect(window_frame(hwnd_)) : mon_rect_;
        br_rect_i32 target;
        if (!target_rect(target)) return BR_OK;
        const double fx = item.width > 0 ? static_cast<double>(content_w) / item.width : 1.0;
        const double fy = item.height > 0 ? static_cast<double>(content_h) / item.height : 1.0;
        int32_t sx0 = static_cast<int32_t>((target.x - item.x) * fx), sy0 = static_cast<int32_t>((target.y - item.y) * fy);
        int32_t sx1 = static_cast<int32_t>((target.x + target.width - item.x) * fx);
        int32_t sy1 = static_cast<int32_t>((target.y + target.height - item.y) * fy);
        sx0 = std::clamp(sx0, 0, content_w); sx1 = std::clamp(sx1, 0, content_w);
        sy0 = std::clamp(sy0, 0, content_h); sy1 = std::clamp(sy1, 0, content_h);
        const int32_t w = sx1 - sx0, h = sy1 - sy0;
        if (w <= 0 || h <= 0) return BR_OK;

        if (!staging_ || staging_w_ != w || staging_h_ != h) {
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = static_cast<UINT>(w);
            sd.Height = static_cast<UINT>(h);
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr = dev_->CreateTexture2D(&sd, nullptr, staging_.put());
            if (FAILED(hr)) return hr_fail(BR_E_OUT_OF_MEMORY, "CreateTexture2D(staging)", hr);
            staging_w_ = w;
            staging_h_ = h;
        }
        D3D11_BOX box{static_cast<UINT>(sx0), static_cast<UINT>(sy0), 0, static_cast<UINT>(sx1), static_cast<UINT>(sy1), 1};
        ctx_->CopySubresourceRegion(staging_.get(), 0, 0, 0, 0, tex.get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE ms{};
        hr = ctx_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &ms);
        if (FAILED(hr)) return hr_fail(BR_E_INTERNAL, "Map(staging)", hr);
        const uint32_t uw = static_cast<uint32_t>(w), uh = static_cast<uint32_t>(h);
        if (!next_.data || next_.width != uw || next_.height != uh) {
            free_image(next_);
            next_ = alloc_image(uw, uh, BR_PIXEL_BGRA8, false);
        }
        for (uint32_t y = 0; y < uh; ++y) {
            const uint32_t* s = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(ms.pData) + static_cast<size_t>(y) * ms.RowPitch);
            uint32_t* d = reinterpret_cast<uint32_t*>(row_ptr(next_, y));
            for (uint32_t x = 0; x < uw; ++x) d[x] = s[x] | 0xff000000u;
        }
        ctx_->Unmap(staging_.get(), 0);

        if (have_ && cache_.width == uw && cache_.height == uh) {
            br_diff_options o{16, 0, 8};
            br_diff_result res{};
            analysis::diff_images(as_view(cache_), as_view(next_), o, u.dirty, res);
            u.updated = res.changed_pixels > 0;
        } else {
            u.updated = true;
            u.dirty.assign(1, br_rect_i32{0, 0, w, h});
        }
        std::swap(cache_, next_);
        have_ = true;
        if (u.updated) {
            ++frame_id_;
            frame_time_ = monotonic_us();
        }
        screen_rect_ = target;
        scale_x_ = static_cast<double>(target.width) / w;
        scale_y_ = static_cast<double>(target.height) / h;
        return BR_OK;
    }

    HWND hwnd_{};
    br_rect_i32 mon_rect_{};
    bool client_{false};
    bool has_crop_{false};
    br_rect_i32 crop_{};
    Com<ID3D11Device> dev_;
    Com<ID3D11DeviceContext> ctx_;
    Com<IInspectable> winrt_device_;
    Com<wrt::IGraphicsCaptureItem> item_;
    Com<wrt::IDirect3D11CaptureFramePool> pool_;
    Com<wrt::IGraphicsCaptureSession> session_;
    wrt::SizeInt32 pool_size_{};
    Com<ID3D11Texture2D> staging_;
    int32_t staging_w_{0}, staging_h_{0};
    br_mut_image_view next_{};
};

}

br_status open_wgc_window(HWND hwnd, bool client, const br_rect_i32* crop, bool cursor, bool border,
                          std::unique_ptr<capture::Session>& out) {
    auto s = std::make_unique<WgcSession>();
    const br_status st = s->init(hwnd, nullptr, {}, client, crop, cursor, border);
    if (st != BR_OK) return st;
    out = std::move(s);
    return BR_OK;
}

br_status open_wgc_monitor(HMONITOR mon, const br_rect_i32& mon_rect, const br_rect_i32* crop, bool cursor, bool border,
                           std::unique_ptr<capture::Session>& out) {
    auto s = std::make_unique<WgcSession>();
    const br_status st = s->init(nullptr, mon, mon_rect, false, crop, cursor, border);
    if (st != BR_OK) return st;
    out = std::move(s);
    return BR_OK;
}

}

#endif
