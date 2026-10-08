#if defined(_WIN32)

#include "platform/windows/d3d11_scaler.hpp"
#include "platform/windows/session.hpp"

#include "core/common.hpp"
#include "core/frame.hpp"
#include "resize/kernels.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace br::win {
namespace {

// Хранит полученный кадр до следующего acquire; переносит изменённые и перемещённые области в last_.
class DxgiSession final : public CachedSession {
public:
    DxgiSession() { backend_ = BR_BACKEND_DXGI; }
    ~DxgiSession() override {
        if (holding_ && dup_) dup_->ReleaseFrame();
    }

    br_status init(uint32_t adapter, uint32_t output, const br_rect_i32* crop, bool cursor) {
        adapter_idx_ = adapter;
        output_idx_ = output;
        include_cursor_ = cursor;
        if (crop) { want_crop_ = *crop; has_crop_ = true; }
        return reinit();
    }

    br_status next_d3d11(uint32_t timeout_ms, void** texture, uint32_t& w, uint32_t& h, std::vector<br_rect_i32>& dirty) override {
        DpiScope dpi;
        bool got = false;
        const bool first = !gpu_valid_;
        br_status st = acquire(timeout_ms, got);
        if (st != BR_OK) return st;
        if (!got && !first) return fail(BR_E_TIMEOUT, "no new frame within the timeout");
        if (!gpu_valid_) return fail(BR_E_TIMEOUT, "no frame was delivered within the timeout");
        last_->AddRef();
        *texture = last_.get();
        w = static_cast<uint32_t>(crop_.width);
        h = static_cast<uint32_t>(crop_.height);
        dirty = last_dirty_;
        return BR_OK;
    }

    br_status grab_scaled(uint32_t timeout_ms, uint32_t out_w, uint32_t out_h, const br_resize_options& options, br_mut_image_view& img,
                          br_frame_info& info) override {
        DpiScope dpi;
        if (!out_w || !out_h) return fail(BR_E_INVALID_ARGUMENT, "invalid output size");
        const double sx=double(out_w)/crop_.width, sy=double(out_h)/crop_.height;
        const br_filter f=resize::choose_filter(options.filter,options.mode,sx,sy);
        if(f<BR_FILTER_BOX||f>BR_FILTER_POINT)return fail(BR_E_INVALID_ARGUMENT,"unknown resize filter");
        if(options.multistage && f!=BR_FILTER_BOX && std::min(sx,sy)<=0.25)
            return fail(BR_E_UNSUPPORTED,"large reduction uses CPU multistage path");
        bool got = false;
        br_status st = acquire(gpu_valid_ ? 0 : timeout_ms, got);
        if (st != BR_OK) return st;
        if (!gpu_valid_) return fail(BR_E_TIMEOUT, "no frame was delivered within the timeout");
        if (!scaler_) {
            scaler_ = std::make_unique<GpuScaler>();
            st = scaler_->init(dev_.get());
            if (st != BR_OK) { scaler_.reset(); scaler_failed_ = true; }
        }
        if (scaler_failed_ || !scaler_) return fail(BR_E_UNSUPPORTED, "GPU scaler unavailable");
        st = scaler_->run(ctx_.get(), last_.get(), static_cast<uint32_t>(crop_.width), static_cast<uint32_t>(crop_.height),
                          out_w, out_h, f, options.linear_light != 0, options.antiring != 0, img);
        if (st != BR_OK) return st;
        fill_info(info, got, false);
        info.image_to_screen = {static_cast<double>(crop_.width) / out_w, static_cast<double>(crop_.height) / out_h,
                                static_cast<double>(crop_.x), static_cast<double>(crop_.y)};
        return BR_OK;
    }

protected:
    void expected_size(uint32_t& w, uint32_t& h) override {
        w = static_cast<uint32_t>(crop_.width);
        h = static_cast<uint32_t>(crop_.height);
    }

    br_status update(uint32_t timeout_ms, bool wait_for_new, Update& u) override {
        bool got = false;
        br_status st = acquire((wait_for_new || !have_) ? timeout_ms : 0, got);
        if (st != BR_OK) return st;
        if (!gpu_valid_) {
            // Если первого кадра DXGI ещё нет, заполняет кэш через GDI.
            if (have_ || wait_for_new) return BR_OK;
            ensure_cache(static_cast<uint32_t>(crop_.width), static_cast<uint32_t>(crop_.height));
            st = gdi_copy_rect(crop_, cache_);
            if (st != BR_OK) return BR_OK; // Вызывающий код сообщает о тайм-ауте.
            have_ = true;
            ++frame_id_;
            frame_time_ = monotonic_us();
            u.updated = true;
            u.dirty_known = true;
            u.dirty.assign(1, br_rect_i32{0, 0, crop_.width, crop_.height});
            return BR_OK;
        }
        return sync_cpu(u);
    }

private:
    br_status reinit() {
        dup_.reset();
        holding_ = false;
        const Api& a = api();
        if (!a.CreateDXGIFactory1 || !a.D3D11CreateDevice) return fail(BR_E_UNSUPPORTED, "DXGI / D3D11 are not available");
        Com<IDXGIFactory1> factory;
        HRESULT hr = a.CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void());
        if (FAILED(hr)) return hr_fail(BR_E_UNSUPPORTED, "CreateDXGIFactory1", hr);
        Com<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapter_idx_, adapter.put()) == DXGI_ERROR_NOT_FOUND) return fail(BR_E_NOT_FOUND, "DXGI adapter not found");
        Com<IDXGIOutput> output;
        if (adapter->EnumOutputs(output_idx_, output.put()) == DXGI_ERROR_NOT_FOUND) return fail(BR_E_NOT_FOUND, "DXGI output not found");
        Com<IDXGIOutput1> output1;
        hr = output.as(__uuidof(IDXGIOutput1), output1);
        if (FAILED(hr)) return hr_fail(BR_E_UNSUPPORTED, "IDXGIOutput1 (Desktop Duplication needs Windows 8+)", hr);
        DXGI_OUTPUT_DESC od{};
        output->GetDesc(&od);
        if (od.Rotation != DXGI_MODE_ROTATION_IDENTITY && od.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
            return fail(BR_E_UNSUPPORTED, "rotated monitors are captured with the GDI or WGC backend");
        const br_rect_i32 desk = to_rect(od.DesktopCoordinates);
        br_rect_i32 crop = desk;
        if (has_crop_) {
            const int32_t x0 = std::max(desk.x, want_crop_.x), y0 = std::max(desk.y, want_crop_.y);
            const int32_t x1 = std::min(desk.x + desk.width, want_crop_.x + want_crop_.width);
            const int32_t y1 = std::min(desk.y + desk.height, want_crop_.y + want_crop_.height);
            crop = {x0, y0, x1 - x0, y1 - y0};
            if (crop.width <= 0 || crop.height <= 0) return fail(BR_E_INVALID_ARGUMENT, "crop does not intersect the monitor");
        }
        if (!dev_) {
            const br_status st = create_device(adapter.get(), dev_, ctx_);
            if (st != BR_OK) return st;
        }
        hr = output1->DuplicateOutput(dev_.get(), dup_.put());
        if (FAILED(hr)) {
            if (hr == E_ACCESSDENIED) return hr_fail(BR_E_DEVICE_LOST, "DuplicateOutput (secure desktop / UAC / lock screen?)", hr);
            if (hr == DXGI_ERROR_UNSUPPORTED) return hr_fail(BR_E_UNSUPPORTED, "DuplicateOutput (remote session or wrong GPU on a hybrid laptop?)", hr);
            if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) return hr_fail(BR_E_UNSUPPORTED, "DuplicateOutput (too many duplication clients)", hr);
            return hr_fail(BR_E_UNSUPPORTED, "DuplicateOutput", hr);
        }
        const bool geometry_changed = crop.x != crop_.x || crop.y != crop_.y || crop.width != crop_.width || crop.height != crop_.height;
        desk_ = desk;
        crop_ = crop;
        screen_rect_ = crop;
        if (geometry_changed || !last_) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(crop.width);
            td.Height = static_cast<UINT>(crop.height);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            hr = dev_->CreateTexture2D(&td, nullptr, last_.put());
            if (FAILED(hr)) return hr_fail(BR_E_OUT_OF_MEMORY, "CreateTexture2D(last)", hr);
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr = dev_->CreateTexture2D(&td, nullptr, staging_.put());
            if (FAILED(hr)) return hr_fail(BR_E_OUT_OF_MEMORY, "CreateTexture2D(staging)", hr);
        }
        gpu_valid_ = false;
        pending_full_ = true;
        pending_.clear();
        return BR_OK;
    }

    br_status acquire(uint32_t timeout_ms, bool& got) {
        got = false;
        if (!dup_) {
            const br_status st = reinit();
            if (st != BR_OK) return st;
        }
        if (holding_) {
            dup_->ReleaseFrame();
            holding_ = false;
        }
        DXGI_OUTDUPL_FRAME_INFO fi{};
        Com<IDXGIResource> res;
        HRESULT hr = dup_->AcquireNextFrame(timeout_ms, &fi, res.put());
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
            const br_status st = reinit();
            if (st != BR_OK) {
                if (st == BR_E_DEVICE_LOST) return st;
                const std::string why = br::last_error();
                set_errorf("desktop duplication lost and could not be restarted: %s", why.c_str());
                return BR_E_DEVICE_LOST;
            }
            hr = dup_->AcquireNextFrame(std::max<uint32_t>(timeout_ms, 100), &fi, res.put());
        }
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return BR_OK;
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            dup_.reset();
            return fail(BR_E_DEVICE_LOST, "desktop duplication access lost");
        }
        if (FAILED(hr)) return hr_fail(BR_E_INTERNAL, "AcquireNextFrame", hr);
        holding_ = true;

        if (fi.LastMouseUpdateTime.QuadPart) {
            cursor_.visible = fi.PointerPosition.Visible != 0;
            cursor_.x = fi.PointerPosition.Position.x + (desk_.x - crop_.x);
            cursor_.y = fi.PointerPosition.Position.y + (desk_.y - crop_.y);
            if (fi.PointerShapeBufferSize) {
                cursor_.pixels.resize(fi.PointerShapeBufferSize);
                UINT need = 0;
                DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
                if (SUCCEEDED(dup_->GetFramePointerShape(fi.PointerShapeBufferSize, cursor_.pixels.data(), &need, &si))) {
                    cursor_.type = si.Type;
                    cursor_.width = si.Width;
                    cursor_.height = si.Height;
                    cursor_.pitch = si.Pitch;
                }
            }
        }
        if (fi.LastPresentTime.QuadPart == 0 && gpu_valid_) return BR_OK; // Обновилось только положение указателя.

        Com<ID3D11Texture2D> tex;
        hr = res.as(__uuidof(ID3D11Texture2D), tex);
        if (FAILED(hr)) return hr_fail(BR_E_INTERNAL, "desktop texture", hr);

        // Изменённые прямоугольники в координатах обрезанной области.
        std::vector<br_rect_i32> rects;
        bool full = !gpu_valid_;
        if (!full && fi.TotalMetadataBufferSize) {
            meta_.resize(fi.TotalMetadataBufferSize);
            UINT used_move = 0, used_dirty = 0;
            hr = dup_->GetFrameMoveRects(fi.TotalMetadataBufferSize, reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(meta_.data()), &used_move);
            if (SUCCEEDED(hr))
                hr = dup_->GetFrameDirtyRects(fi.TotalMetadataBufferSize - used_move,
                                              reinterpret_cast<RECT*>(meta_.data() + used_move), &used_dirty);
            if (FAILED(hr)) {
                full = true;
            } else {
                const auto* mv = reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(meta_.data());
                for (UINT i = 0; i < used_move / sizeof(DXGI_OUTDUPL_MOVE_RECT); ++i) add_local(rects, mv[i].DestinationRect);
                const auto* dr = reinterpret_cast<const RECT*>(meta_.data() + used_move);
                for (UINT i = 0; i < used_dirty / sizeof(RECT); ++i) add_local(rects, dr[i]);
            }
        } else if (!full) {
            full = true; // При отсутствии метаданных обрабатывает весь кадр.
        }
        uint64_t area = 0;
        for (const auto& r : rects) area += static_cast<uint64_t>(r.width) * r.height;
        if (rects.size() > 128 || area * 10 > static_cast<uint64_t>(crop_.width) * crop_.height * 6) full = true;

        const int32_t ox = crop_.x - desk_.x, oy = crop_.y - desk_.y;
        if (full) {
            D3D11_BOX box{static_cast<UINT>(ox), static_cast<UINT>(oy), 0, static_cast<UINT>(ox + crop_.width),
                          static_cast<UINT>(oy + crop_.height), 1};
            ctx_->CopySubresourceRegion(last_.get(), 0, 0, 0, 0, tex.get(), 0, &box);
            pending_full_ = true;
            pending_.clear();
            last_dirty_.assign(1, br_rect_i32{0, 0, crop_.width, crop_.height});
        } else {
            for (const auto& r : rects) {
                D3D11_BOX box{static_cast<UINT>(ox + r.x), static_cast<UINT>(oy + r.y), 0, static_cast<UINT>(ox + r.x + r.width),
                              static_cast<UINT>(oy + r.y + r.height), 1};
                ctx_->CopySubresourceRegion(last_.get(), 0, static_cast<UINT>(r.x), static_cast<UINT>(r.y), 0, tex.get(), 0, &box);
            }
            if (!pending_full_) {
                pending_.insert(pending_.end(), rects.begin(), rects.end());
                if (pending_.size() > 256) pending_full_ = true;
            }
            last_dirty_ = rects;
        }
        gpu_valid_ = true;
        got = true;
        ++frame_id_;
        frame_time_ = monotonic_us();
        return BR_OK;
    }

    void add_local(std::vector<br_rect_i32>& out, const RECT& r) const {
        // Переводит координаты изображения рабочего стола (локальные для выхода) в координаты обрезки и обрезает границы.
        const int32_t ox = crop_.x - desk_.x, oy = crop_.y - desk_.y;
        const int32_t x0 = std::max<int32_t>(r.left - ox, 0), y0 = std::max<int32_t>(r.top - oy, 0);
        const int32_t x1 = std::min<int32_t>(r.right - ox, crop_.width), y1 = std::min<int32_t>(r.bottom - oy, crop_.height);
        if (x1 > x0 && y1 > y0) out.push_back({x0, y0, x1 - x0, y1 - y0});
    }

    br_status sync_cpu(Update& u) {
        u.dirty_known = true;
        const uint32_t w = static_cast<uint32_t>(crop_.width), h = static_cast<uint32_t>(crop_.height);
        if (!cache_.data || cache_.width != w || cache_.height != h) {
            ensure_cache(w, h);
            pending_full_ = true;
        }
        if (!pending_full_ && pending_.empty() && have_) return BR_OK; // Изображение не изменилось.
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (pending_full_ || !have_) {
            ctx_->CopyResource(staging_.get(), last_.get());
            const HRESULT hr = ctx_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &ms);
            if (FAILED(hr)) return hr_fail(BR_E_INTERNAL, "Map(staging)", hr);
            copy_rect_from_mapped(static_cast<const uint8_t*>(ms.pData), ms.RowPitch, 0, 0, cache_,
                                  {0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)});
            u.dirty.assign(1, br_rect_i32{0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)});
        } else {
            for (const auto& r : pending_) {
                D3D11_BOX box{static_cast<UINT>(r.x), static_cast<UINT>(r.y), 0, static_cast<UINT>(r.x + r.width),
                              static_cast<UINT>(r.y + r.height), 1};
                ctx_->CopySubresourceRegion(staging_.get(), 0, static_cast<UINT>(r.x), static_cast<UINT>(r.y), 0, last_.get(), 0, &box);
            }
            const HRESULT hr = ctx_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &ms);
            if (FAILED(hr)) return hr_fail(BR_E_INTERNAL, "Map(staging)", hr);
            for (const auto& r : pending_)
                copy_rect_from_mapped(static_cast<const uint8_t*>(ms.pData), ms.RowPitch, r.x, r.y, cache_, r);
            u.dirty = pending_;
        }
        ctx_->Unmap(staging_.get(), 0);
        // Альфа рабочего стола не определена; задаёт непрозрачность изменённых областей.
        for (const auto& r : u.dirty)
            for (int32_t y = r.y; y < r.y + r.height; ++y) {
                uint8_t* p = row_ptr(cache_, static_cast<uint32_t>(y)) + static_cast<size_t>(r.x) * 4 + 3;
                for (int32_t x = 0; x < r.width; ++x) p[4 * x] = 255;
            }
        pending_.clear();
        pending_full_ = false;
        have_ = true;
        u.updated = true;
        return BR_OK;
    }

    uint32_t adapter_idx_{0}, output_idx_{0};
    bool has_crop_{false};
    br_rect_i32 want_crop_{};
    br_rect_i32 desk_{}, crop_{};
    Com<ID3D11Device> dev_;
    Com<ID3D11DeviceContext> ctx_;
    Com<IDXGIOutputDuplication> dup_;
    Com<ID3D11Texture2D> last_, staging_;
    bool holding_{false};
    bool gpu_valid_{false};
    bool pending_full_{true};
    std::vector<br_rect_i32> pending_, last_dirty_;
    std::vector<uint8_t> meta_;
    std::unique_ptr<GpuScaler> scaler_;
    bool scaler_failed_{false};
};

}

br_status open_dxgi(uint32_t adapter, uint32_t output, const br_rect_i32* crop, bool cursor, std::unique_ptr<capture::Session>& out) {
    auto s = std::make_unique<DxgiSession>();
    const br_status st = s->init(adapter, output, crop, cursor);
    if (st != BR_OK) return st;
    out = std::move(s);
    return BR_OK;
}

}

#endif
